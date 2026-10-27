// Boot splash: the AeroRelics artwork (splash_img.cpp, made by tools/make_branding.py)
// with the firmware version where the artwork's own version number was.
#include <stdio.h>
#include <string.h>

#include "ui_priv.h"

extern const uint16_t SPLASH_IMG[800 * 480];
extern const int SPLASH_VERSION_Y, SPLASH_VERSION_END;

static lv_obj_t *s_splash;

void ui_splash_show(const char *fw_version) {
    static lv_img_dsc_t dsc;
    dsc.header.always_zero = 0;
    dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    dsc.header.w = 800;
    dsc.header.h = 480;
    dsc.data_size = sizeof(SPLASH_IMG);
    dsc.data = (const uint8_t *)SPLASH_IMG;
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    s_splash = lv_img_create(scr);
    lv_img_set_src(s_splash, &dsc);
    lv_obj_set_pos(s_splash, 0, 0);

    const char *v = fw_version ? fw_version : "";
    if (!strncmp(v, "mfd7 ", 5)) v += 5;
    char t[32];
    snprintf(t, sizeof(t), "Version %s", v);
    lv_obj_t *l = ui_label(s_splash, FONT_14, lv_color_hex(0x8696A5), t);
    lv_obj_update_layout(l);
    lv_obj_set_pos(l, SPLASH_VERSION_END + 1 - lv_obj_get_width(l), SPLASH_VERSION_Y - lv_obj_get_height(l) / 2);
}

void ui_splash_hide() {
    if (s_splash) lv_obj_del(s_splash);
    s_splash = nullptr;
}
