#include "debuglog.h"
#include "util/applog.h"

/*
 * How to turn on the detailed log, in English and Portuguese. A page of its own, with no QR code:
 * under the wiki's QR code the text ran off the screen, and the remote can't scroll a page with
 * nothing to focus. The marker file is the only switch, and its name can't be guessed on webOS
 * (glued to the app folder's, see applog.h), so the page shows the full path the app reads.
 */

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *parent);

const lv_fragment_class_t debuglog_fragment_class = {
        .create_obj_cb = create_obj,
        .instance_size = sizeof(lv_fragment_t)
};

static lv_obj_t *add_label(lv_obj_t *parent) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_size(label, LV_PCT(100), LV_SIZE_CONTENT);
    return label;
}

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *parent) {
    (void) self;
    lv_obj_t *content = lv_obj_create(parent);
    lv_obj_set_size(content, LV_PCT(100), LV_PCT(100));
    lv_obj_set_layout(content, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, LV_DPX(12), 0);

    const char *log_path = applog_path();
    const char *marker = applog_debug_marker_path();
    if (log_path == NULL || marker == NULL) {
        lv_label_set_text(add_label(content), "No log file could be opened. / Não foi possível abrir o arquivo de log.");
        return content;
    }
    bool on = applog_is_debug();

    lv_label_set_text_fmt(add_label(content), "Log file / Arquivo de log:\n%s", log_path);
    lv_label_set_text_fmt(add_label(content), "Detailed log switch / Chave do log detalhado:\n%s", marker);
    lv_label_set_text_fmt(add_label(content),
                          "Create that file, empty (over SSH, with touch), and restart the app. Delete it to go back "
                          "to the normal log, which stays smaller. Detailed log is %s now.", on ? "ON" : "OFF");
    lv_label_set_text_fmt(add_label(content),
                          "Crie esse arquivo, vazio (por SSH, com touch), e reinicie o app. Apague-o para voltar ao "
                          "log normal, que fica menor. O log detalhado está %s agora.", on ? "LIGADO" : "DESLIGADO");
    lv_label_set_text(add_label(content),
                      "While streaming, the red, green, yellow and blue keys mark the log. / Durante o jogo, as "
                      "teclas vermelha, verde, amarela e azul marcam o log.");
    return content;
}
