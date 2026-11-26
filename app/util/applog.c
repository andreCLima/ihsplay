#include "applog.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL.h>

#include "logging.h"

#define APPLOG_MAX_BYTES (2 * 1024 * 1024)
#define APPLOG_PENDING_MAX (16 * 1024)
#define APPLOG_LINE_MAX 1280
#define APPLOG_FLUSH_INTERVAL_MS 1000

/* Provided by the linker for -Wl,--wrap=commons_log_vprintf: the original PmLog/stdio implementation. */
void __real_commons_log_vprintf(commons_log_level level, const char *tag, const char *fmt, va_list arg);

void __wrap_commons_log_vprintf(commons_log_level level, const char *tag, const char *fmt, va_list arg);

/* Logs come from the session, receive, HID, timer and UI threads, so everything below is under this lock. */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *file = NULL;
static bool open_attempted = false;
static bool closed = false;
static char path[1024], old_path[1024], marker[1024];
static size_t written = 0;
static commons_log_level file_level = COMMONS_LOG_LEVEL_INFO;
static int64_t last_flush_ms = 0;

static char pending[APPLOG_PENDING_MAX];
static size_t pending_len = 0;

static int64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int64_t start_ms(void) {
    static int64_t start = -1;
    if (start < 0) {
        start = mono_ms();
    }
    return start;
}

/* One line: wall clock (so the user can match it with what they saw), ms since start (for intervals), level, tag. */
static int format_line(char *out, size_t size, size_t *body_offset, commons_log_level level, const char *tag,
                       const char *fmt, va_list arg) {
    static const char level_chars[] = "FEWIDV";
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    struct tm tm_now;
    time_t secs = now.tv_sec;
    localtime_r(&secs, &tm_now);
    int64_t since_start = mono_ms() - start_ms();
    int n = snprintf(out, size, "%04d-%02d-%02d %02d:%02d:%02d.%03d [%8lld.%03lld] %c %s: ", tm_now.tm_year + 1900,
                     tm_now.tm_mon + 1, tm_now.tm_mday, tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec,
                     (int) (now.tv_nsec / 1000000), (long long) (since_start / 1000), (long long) (since_start % 1000),
                     level >= 0 && level <= COMMONS_LOG_LEVEL_VERBOSE ? level_chars[level] : '?', tag);
    if (n < 0 || (size_t) n >= size) {
        return 0;
    }
    *body_offset = (size_t) n;
    int m = vsnprintf(out + n, size - n - 1, fmt, arg);
    if (m < 0) {
        m = 0;
    }
    size_t len = (size_t) n + (size_t) m;
    if (len > size - 2) {
        len = size - 2;
    }
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r')) {
        len--;
    }
    out[len++] = '\n';
    out[len] = '\0';
    return (int) len;
}

static void rotate_locked(void) {
    if (file != NULL) {
        fclose(file);
        file = NULL;
    }
    rename(path, old_path);
    file = fopen(path, "w");
    written = 0;
    if (file != NULL) {
        setvbuf(file, NULL, _IOFBF, 8192);
    }
}

/*
 * The disk use is bounded no matter what: two files of at most APPLOG_MAX_BYTES each (4 MiB in all), because a file
 * that would overflow is rotated first and the older one is replaced. On top of that, two limiters keep a noisy
 * source from rolling the useful history out of those 4 MiB in minutes:
 *  - a message identical to the previous one (same tag and text) within a second is folded into a
 *    "repeated N times" line;
 *  - a tag may write APPLOG_TAG_LINES_PER_SEC lines per second (errors excepted), the rest is counted and reported.
 */
#define APPLOG_TAG_LINES_PER_SEC 20
#define APPLOG_REPEAT_WINDOW_MS 1000
#define APPLOG_TAG_SLOTS 32
#define APPLOG_TAG_LEN 32

typedef struct tag_budget_t {
    char tag[APPLOG_TAG_LEN];
    int64_t window_start_ms;
    int lines;
    int dropped;
} tag_budget_t;

static tag_budget_t budgets[APPLOG_TAG_SLOTS];

static char last_tag[APPLOG_TAG_LEN];
static char last_body[APPLOG_LINE_MAX];
static commons_log_level last_level = COMMONS_LOG_LEVEL_INFO;
static int64_t last_line_ms = 0;
static int repeat_count = 0;

static void write_locked(commons_log_level level, const char *line, size_t len) {
    if (file == NULL) {
        return;
    }
    if (written + len > APPLOG_MAX_BYTES) {
        rotate_locked();
        if (file == NULL) {
            return;
        }
    }
    fwrite(line, 1, len, file);
    written += len;
    // Errors and warnings are what is looked at after a crash, so they go to disk at once; the rest at most a
    // second later, which keeps a Debug stream from hitting the flash for every line.
    int64_t now = mono_ms();
    if (level <= COMMONS_LOG_LEVEL_WARN || now - last_flush_ms >= APPLOG_FLUSH_INTERVAL_MS) {
        fflush(file);
        last_flush_ms = now;
    }
}

static void notice_locked(commons_log_level level, const char *tag, const char *fmt, ...)
__attribute__ ((format (printf, 3, 4)));

/* A line the limiters themselves add. */
static void notice_locked(commons_log_level level, const char *tag, const char *fmt, ...) {
    char line[APPLOG_LINE_MAX];
    size_t body_offset;
    va_list args;
    va_start(args, fmt);
    int len = format_line(line, sizeof(line), &body_offset, level, tag, fmt, args);
    va_end(args);
    if (len > 0) {
        write_locked(level, line, (size_t) len);
    }
}

static void flush_repeats_locked(void) {
    if (repeat_count > 0) {
        notice_locked(last_level, last_tag, "last message repeated %d times", repeat_count);
        repeat_count = 0;
    }
}

/* Returns false if the budget of this tag for the current second is used up. */
static bool tag_allowed_locked(commons_log_level level, const char *tag, int64_t now) {
    unsigned hash = 5381;
    for (const char *c = tag; *c != 0; c++) {
        hash = hash * 33 + (unsigned char) *c;
    }
    // Linear probing on the whole (truncated) tag, so two tags that hash alike keep separate budgets. If every slot
    // is taken by other tags, the one with the oldest window is given up.
    tag_budget_t *b = NULL;
    tag_budget_t *oldest = &budgets[hash % APPLOG_TAG_SLOTS];
    for (unsigned i = 0; i < APPLOG_TAG_SLOTS; i++) {
        tag_budget_t *candidate = &budgets[(hash + i) % APPLOG_TAG_SLOTS];
        if (strncmp(candidate->tag, tag, APPLOG_TAG_LEN - 1) == 0 && candidate->tag[0] != 0) {
            b = candidate;
            break;
        }
        if (candidate->tag[0] == 0) {
            b = candidate;
            memset(b, 0, sizeof(*b));
            strncpy(b->tag, tag, APPLOG_TAG_LEN - 1);
            b->window_start_ms = now;
            break;
        }
        if (candidate->window_start_ms < oldest->window_start_ms) {
            oldest = candidate;
        }
    }
    if (b == NULL) {
        b = oldest;
        if (b->dropped > 0) {
            notice_locked(COMMONS_LOG_LEVEL_WARN, b->tag, "%d messages dropped (rate limit)", b->dropped);
        }
        memset(b, 0, sizeof(*b));
        strncpy(b->tag, tag, APPLOG_TAG_LEN - 1);
        b->window_start_ms = now;
    }
    if (now - b->window_start_ms >= 1000) {
        if (b->dropped > 0) {
            notice_locked(COMMONS_LOG_LEVEL_WARN, b->tag, "%d messages dropped (rate limit)", b->dropped);
        }
        b->window_start_ms = now;
        b->lines = 0;
        b->dropped = 0;
    }
    if (level <= COMMONS_LOG_LEVEL_ERROR) {
        return true;
    }
    if (b->lines >= APPLOG_TAG_LINES_PER_SEC) {
        b->dropped++;
        return false;
    }
    b->lines++;
    return true;
}

static void log_locked(commons_log_level level, const char *tag, const char *line, size_t len, size_t body_offset) {
    int64_t now = mono_ms();
    const char *body = line + body_offset;
    size_t body_len = len - body_offset;
    bool same = repeat_count >= 0 && body_len < sizeof(last_body) && strcmp(last_tag, tag) == 0 &&
                strncmp(last_body, body, body_len) == 0 && last_body[body_len] == '\0';
    if (same && now - last_line_ms < APPLOG_REPEAT_WINDOW_MS) {
        repeat_count++;
        return;
    }
    flush_repeats_locked();
    if (!tag_allowed_locked(level, tag, now)) {
        return;
    }
    write_locked(level, line, len);
    strncpy(last_tag, tag, APPLOG_TAG_LEN - 1);
    last_tag[APPLOG_TAG_LEN - 1] = '\0';
    if (body_len < sizeof(last_body)) {
        memcpy(last_body, body, body_len);
        last_body[body_len] = '\0';
    } else {
        last_body[0] = '\0';
    }
    last_level = level;
    last_line_ms = now;
}

void __wrap_commons_log_vprintf(commons_log_level level, const char *tag, const char *fmt, va_list arg) {
    va_list copy;
    va_copy(copy, arg);
    __real_commons_log_vprintf(level, tag, fmt, arg);

    // Decide before formatting whether the file wants this level at all: Debug lines are the bulk when only Info is kept
    pthread_mutex_lock(&lock);
    bool wanted = !closed && level <= (open_attempted ? file_level : COMMONS_LOG_LEVEL_INFO);
    pthread_mutex_unlock(&lock);
    if (!wanted) {
        va_end(copy);
        return;
    }

    char line[APPLOG_LINE_MAX];
    size_t body_offset = 0;
    int len = format_line(line, sizeof(line), &body_offset, level, tag, fmt, copy);
    va_end(copy);
    if (len <= 0) {
        return;
    }
    pthread_mutex_lock(&lock);
    if (closed) {
        // Nothing to do
    } else if (!open_attempted) {
        // Before applog_open (e.g. SDL is not initialised yet): keep the start of the log
        if (level <= COMMONS_LOG_LEVEL_INFO && pending_len + len <= APPLOG_PENDING_MAX) {
            memcpy(pending + pending_len, line, len);
            pending_len += len;
        }
    } else if (level <= file_level) {
        log_locked(level, tag, line, (size_t) len, body_offset);
    }
    pthread_mutex_unlock(&lock);
}
void applog_open(void) {
    start_ms();
    char *dir = SDL_GetPrefPath("mariotaku", "ihsplay");
    pthread_mutex_lock(&lock);
    if (open_attempted) {
        pthread_mutex_unlock(&lock);
        SDL_free(dir);
        return;
    }
    open_attempted = true;
    marker[0] = '\0';
    if (dir != NULL) {
        /* On webOS, SDL_GetPrefPath gives the app's own folder with no trailing slash, so these
         * names land next to it (applications/org.mariotaku.ihsplayihsplay.log), as does
         * host_keys.txt. Left as is on purpose: there they have survived every reinstall, and
         * inside the app's folder an update could remove them, and the pairing with them. */
        snprintf(path, sizeof(path), "%sihsplay.log", dir);
        snprintf(old_path, sizeof(old_path), "%sihsplay.1.log", dir);
        snprintf(marker, sizeof(marker), "%sdebug", dir);
        FILE *marker_file = fopen(marker, "r");
        if (marker_file != NULL) {
            fclose(marker_file);
            file_level = COMMONS_LOG_LEVEL_DEBUG;
        }
        // The previous run becomes ihsplay.1.log (replacing the one before it)
        rename(path, old_path);
        file = fopen(path, "w");
        if (file != NULL) {
            setvbuf(file, NULL, _IOFBF, 8192);
            if (pending_len > 0) {
                fwrite(pending, 1, pending_len, file);
                written += pending_len;
            }
            fflush(file);
        } else {
            path[0] = '\0';
        }
    } else {
        path[0] = '\0';
    }
    pending_len = 0;
    pthread_mutex_unlock(&lock);
    SDL_free(dir);
    if (path[0] != '\0') {
        commons_log_info("AppLog", "Logging to %s (level %s; an empty file %s turns on Debug)", path,
                         file_level == COMMONS_LOG_LEVEL_DEBUG ? "Debug" : "Info", marker);
    } else {
        commons_log_warn("AppLog", "Can't open a log file, logging to PmLog only");
    }
}

void applog_close(void) {
    pthread_mutex_lock(&lock);
    flush_repeats_locked();
    if (file != NULL) {
        fflush(file);
        fclose(file);
        file = NULL;
    }
    closed = true;
    pthread_mutex_unlock(&lock);
}

const char *applog_path(void) {
    return path[0] != '\0' ? path : NULL;
}

const char *applog_debug_marker_path(void) {
    return marker[0] != '\0' ? marker : NULL;
}

bool applog_is_debug(void) {
    return file_level == COMMONS_LOG_LEVEL_DEBUG;
}
