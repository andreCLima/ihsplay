#pragma once

/*
 * Log file next to the app's other data (SDL_GetPrefPath), written in addition to PmLog, which a TV owner can't read
 * without root. Every message that goes through commons_log_* ends up here: the build wraps commons_log_vprintf
 * (-Wl,--wrap), so SDL, LVGL, SS4S and ihslib messages are included without touching the commons submodule.
 *
 * Files: ihsplay.log (current run) and ihsplay.1.log (the run before, or the part that overflowed).
 * Level: Info; Debug if an empty file `debug` exists. On webOS SDL_GetPrefPath has no trailing slash, so the names
 * are glued to the app folder's: applications/org.mariotaku.ihsplayihsplay.log and org.mariotaku.ihsplaydebug.
 */

#include <stdbool.h>

/** Open the log. Messages logged before this are held back (up to 16 KiB) and written first. */
void applog_open(void);

/** Flush and close. Messages logged afterwards go to PmLog only. */
void applog_close(void);

/** Full path of the current log file, or NULL if no file could be opened. */
const char *applog_path(void);

/** Full path of the file that turns on Debug (read at start), or NULL if there is no log directory. */
const char *applog_debug_marker_path(void);

/** True if this run logs at Debug level. */
bool applog_is_debug(void);
