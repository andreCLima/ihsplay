#include "wiki.h"
#include "util/applog.h"

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *parent);

const lv_fragment_class_t wiki_fragment_class = {
        .create_obj_cb = create_obj,
        .instance_size = sizeof(lv_fragment_t)
};

static lv_obj_t *create_obj(lv_fragment_t *self, lv_obj_t *parent) {
    lv_obj_t *content = lv_obj_create(parent);
    lv_obj_set_size(content, LV_PCT(100), LV_PCT(100));
    lv_obj_set_layout(content, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *hint = lv_label_create(content);
    lv_obj_set_size(hint, LV_PCT(100), LV_SIZE_CONTENT);
    const char *url = "https://github.com/mariotaku/ihsplay/wiki";

    lv_label_set_text_fmt(hint, "Open %s for guides and frequently asked questions. Or scan the QR code below.", url);

    lv_obj_t *qrcode = lv_qrcode_create(content, LV_DPX(200), lv_color_black(), lv_color_white());
    lv_obj_set_style_bg_opa(qrcode, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(qrcode, lv_color_white(), 0);

    lv_qrcode_update(qrcode, url, strlen(url));

    /* The marker file is the only switch for the detailed log, and it can't be guessed: on webOS its
     * name is glued to the app folder's (see applog.h). */
    const char *log_path = applog_path();
    const char *marker = applog_debug_marker_path();
    if (log_path != NULL && marker != NULL) {
        lv_obj_t *debug_hint = lv_label_create(content);
        lv_obj_set_size(debug_hint, LV_PCT(100), LV_SIZE_CONTENT);
        lv_label_set_text_fmt(debug_hint,
                              "Detailed log, for reporting a problem\n"
                              "The app writes its log to:\n%s\n"
                              "To make it detailed, create an empty file at:\n%s\n"
                              "(over SSH: touch %s) and restart the app. Delete the file to go back to "
                              "the normal log, which stays smaller.\n"
                              "Detailed log is %s for this run.",
                              log_path, marker, marker, applog_is_debug() ? "ON" : "OFF");

        lv_obj_t *debug_hint_pt = lv_label_create(content);
        lv_obj_set_size(debug_hint_pt, LV_PCT(100), LV_SIZE_CONTENT);
        lv_label_set_text_fmt(debug_hint_pt,
                              "Log detalhado, para relatar um problema\n"
                              "Crie um arquivo vazio em %s (por SSH: touch %s) e reinicie o app. "
                              "Apague o arquivo para voltar ao log normal, que fica menor.\n"
                              "O log detalhado está %s nesta execução.",
                              marker, marker, applog_is_debug() ? "LIGADO" : "DESLIGADO");
    }

    return content;
}