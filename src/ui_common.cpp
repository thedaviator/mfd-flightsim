// Widget and drawing helpers shared by all pages.
#include <stdio.h>
#include <string.h>

#include "ui_priv.h"

// ---------------------------------------------------------------- widgets

lv_obj_t *ui_box(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text);
    return l;
}

lv_obj_t *ui_button(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_coord_t w, lv_coord_t h,
                    lv_event_cb_t cb, void *user) {
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x2A3138), 0);
    lv_obj_set_style_bg_grad_color(b, lv_color_hex(0x15191E), 0);
    lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x46525E), LV_STATE_PRESSED);
    lv_obj_set_style_bg_grad_color(b, lv_color_hex(0x2A3138), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, COL_BORDER, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, COL_TEXT, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    return b;
}

lv_obj_t *ui_button_label(lv_obj_t *btn) { return lv_obj_get_child(btn, 0); }

void ui_button_look(lv_obj_t *btn, BtnLook look) {
    lv_color_t text = COL_TEXT, border = COL_BORDER;
    lv_coord_t bw = 1;
    switch (look) {
    case BTN_ACTIVE: text = COL_GREEN; border = COL_GREEN; bw = 2; break;
    case BTN_SELECTED: text = COL_CYAN; border = COL_CYAN; bw = 2; break;
    case BTN_ARMED: text = COL_TEXT; border = COL_TEXT; bw = 2; break;
    case BTN_DISABLED: text = lv_color_hex(0x505A64); break;
    default: break;
    }
    lv_obj_t *l = ui_button_label(btn);
    if (l) ui_set_color(l, text);
    if (lv_obj_get_style_border_width(btn, 0) != bw) lv_obj_set_style_border_width(btn, bw, 0);
    if (lv_obj_get_style_border_color(btn, 0).full != border.full) lv_obj_set_style_border_color(btn, border, 0);
    if (look == BTN_DISABLED) lv_obj_add_state(btn, LV_STATE_DISABLED);
    else lv_obj_clear_state(btn, LV_STATE_DISABLED);
}

void ui_set_text(lv_obj_t *label, const char *text) {
    if (strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

void ui_set_color(lv_obj_t *label, lv_color_t c) {
    if (lv_obj_get_style_text_color(label, 0).full != c.full) lv_obj_set_style_text_color(label, c, 0);
}

lv_obj_t *ui_canvas(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h, lv_event_cb_t draw_cb) {
    lv_obj_t *o = ui_box(parent, x, y, w, h);
    lv_obj_add_event_cb(o, draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
    return o;
}

// ---------------------------------------------------------------- drawing

void dr_line(lv_draw_ctx_t *dc, float x1, float y1, float x2, float y2, lv_coord_t w, lv_color_t c, lv_opa_t opa) {
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = c;
    d.width = w;
    d.opa = opa;
    lv_point_t a = {(lv_coord_t)lroundf(x1), (lv_coord_t)lroundf(y1)};
    lv_point_t b = {(lv_coord_t)lroundf(x2), (lv_coord_t)lroundf(y2)};
    // Cheap reject when the line's box is outside the clip area.
    const lv_area_t *clip = dc->clip_area;
    lv_coord_t pad = w + 1;
    if (LV_MAX(a.x, b.x) + pad < clip->x1 || LV_MIN(a.x, b.x) - pad > clip->x2 ||
        LV_MAX(a.y, b.y) + pad < clip->y1 || LV_MIN(a.y, b.y) - pad > clip->y2)
        return;
    lv_draw_line(dc, &d, &a, &b);
}

void dr_rect(lv_draw_ctx_t *dc, lv_coord_t x1, lv_coord_t y1, lv_coord_t x2, lv_coord_t y2, lv_color_t bg,
             lv_opa_t bg_opa, lv_coord_t radius, lv_coord_t border_w, lv_color_t border) {
    lv_area_t a = {x1, y1, x2, y2};
    lv_area_t tmp;
    if (!_lv_area_intersect(&tmp, &a, dc->clip_area)) return;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = bg;
    d.bg_opa = bg_opa;
    d.radius = radius;
    d.border_width = border_w;
    d.border_color = border;
    d.border_opa = border_w ? LV_OPA_COVER : LV_OPA_TRANSP;
    lv_draw_rect(dc, &d, &a);
}

lv_coord_t text_width(const char *txt, const lv_font_t *font) {
    lv_point_t s;
    lv_txt_get_size(&s, txt, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return s.x;
}

void dr_text(lv_draw_ctx_t *dc, lv_coord_t x, lv_coord_t y, const char *txt, const lv_font_t *font, lv_color_t c,
             TextAlign align, lv_opa_t opa) {
    lv_coord_t lh = lv_font_get_line_height(font);
    lv_coord_t y1 = y - lh / 2;
    const lv_area_t *clip = dc->clip_area;
    if (y1 > clip->y2 || y1 + lh < clip->y1) return;
    lv_coord_t w = text_width(txt, font);
    lv_coord_t x1 = align == TA_LEFT ? x : align == TA_CENTER ? x - w / 2 : x - w;
    if (x1 > clip->x2 || x1 + w < clip->x1) return;
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.color = c;
    d.font = font;
    d.opa = opa;
    lv_area_t a = {x1, y1, (lv_coord_t)(x1 + w + 1), (lv_coord_t)(y1 + lh)};
    lv_draw_label(dc, &d, &a, txt, nullptr);
}

lv_coord_t digit_w(const lv_font_t *font) { return lv_font_get_glyph_width(font, '0', '0'); }

void dr_digits(lv_draw_ctx_t *dc, lv_coord_t x_right, lv_coord_t y, const char *txt, const lv_font_t *font,
               lv_color_t c) {
    lv_coord_t dw = digit_w(font);
    lv_coord_t x = x_right;
    char ch[2] = {0, 0};
    for (int i = (int)strlen(txt) - 1; i >= 0; i--) {
        ch[0] = txt[i];
        lv_coord_t cw = (txt[i] >= '0' && txt[i] <= '9') ? dw : text_width(ch, font);
        x -= cw;
        if (txt[i] != ' ') dr_text(dc, x + cw / 2, y, ch, font, c, TA_CENTER);
    }
}

void dr_poly(lv_draw_ctx_t *dc, const lv_point_t *pts, uint16_t n, lv_color_t c, lv_opa_t opa) {
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = c;
    d.bg_opa = opa;
    lv_draw_polygon(dc, &d, pts, n);
}

// Circles and arcs are drawn as ellipses XS times narrower than tall, so they come out
// round on the panel's non-square pixels.

void dr_circle(lv_draw_ctx_t *dc, lv_coord_t cx, lv_coord_t cy, lv_coord_t r, lv_color_t c, lv_coord_t width,
               lv_opa_t opa) {
    float rx = r * XS;
    lv_area_t a = {(lv_coord_t)(cx - rx - width), (lv_coord_t)(cy - r - width), (lv_coord_t)(cx + rx + width),
                   (lv_coord_t)(cy + r + width)};
    lv_area_t tmp;
    if (!_lv_area_intersect(&tmp, &a, dc->clip_area)) return;
    if (r <= 14) {
        // Small: a rounded rectangle is indistinguishable from the ellipse and is anti-aliased.
        lv_area_t e = {(lv_coord_t)lroundf(cx - rx), (lv_coord_t)(cy - r), (lv_coord_t)lroundf(cx + rx), (lv_coord_t)(cy + r)};
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.radius = LV_RADIUS_CIRCLE;
        if (width == 0) {
            d.bg_color = c;
            d.bg_opa = opa;
        } else {
            d.bg_opa = LV_OPA_TRANSP;
            d.border_color = c;
            d.border_width = width;
            d.border_opa = opa;
        }
        lv_draw_rect(dc, &d, &e);
        return;
    }
    if (width == 0) {
        // Filled: one horizontal span per row (only rows inside the clip area are drawn).
        lv_coord_t y0 = LV_MAX(cy - r, dc->clip_area->y1), y1 = LV_MIN(cy + r, dc->clip_area->y2);
        for (lv_coord_t y = y0; y <= y1; y++) {
            float t = (float)(y - cy) / r;
            float half = rx * sqrtf(LV_MAX(0.0f, 1.0f - t * t));
            if (half < 0.5f) continue;
            dr_rect(dc, (lv_coord_t)lroundf(cx - half), y, (lv_coord_t)lroundf(cx + half), y, c, opa);
        }
        return;
    }
    // Outline: short anti-aliased segments, finer for bigger ellipses.
    int n = LV_CLAMP(24, r / 2, 120);
    float px = cx + rx, py = cy;
    for (int i = 1; i <= n; i++) {
        float a2 = i * 2 * 3.14159265f / n;
        float x = cx + cosf(a2) * rx, y = cy + sinf(a2) * r;
        dr_line(dc, px, py, x, y, width, c, opa);
        px = x;
        py = y;
    }
}

void dr_arc(lv_draw_ctx_t *dc, lv_coord_t cx, lv_coord_t cy, lv_coord_t r, lv_coord_t w, float start, float end,
            lv_color_t c, bool rounded) {
    // LVGL convention: angles in degrees, 0 = 3 o'clock, clockwise; r is the outer radius.
    float a0 = wrap360(start), a1 = wrap360(end);
    if (fabsf(a1 - a0) < 0.01f) return;
    if (a1 < a0) a1 += 360;
    float rm = r - w / 2.0f;                         // centre line of the band
    // Segment length keeps the chord error under ~0.3 px.
    float step = LV_CLAMP(1.5f, 2 * acosf(1 - 0.3f / LV_MAX(rm, 2.0f)) * RAD2DEG, 10.0f);
    int n = (int)ceilf((a1 - a0) / step);
    step = (a1 - a0) / n;
    const lv_area_t *clip = dc->clip_area;
    if (cx + r < clip->x1 || cx - r > clip->x2 || cy + r < clip->y1 || cy - r > clip->y2) return;
    if (w <= 3) {
        for (int i = 0; i < n; i++) {
            float b0 = (a0 + i * step) * DEG2RAD, b1 = (a0 + (i + 1) * step) * DEG2RAD;
            dr_line(dc, cx + cosf(b0) * rm * XS, cy + sinf(b0) * rm, cx + cosf(b1) * rm * XS, cy + sinf(b1) * rm, w, c);
        }
        return;
    }
    float ro = r, ri = r - w;
    for (int i = 0; i < n; i++) {
        float b0 = (a0 + i * step) * DEG2RAD, b1 = (a0 + (i + 1) * step) * DEG2RAD;
        float c0 = cosf(b0), s0 = sinf(b0), c1 = cosf(b1), s1 = sinf(b1);
        lv_point_t q[4] = {{(lv_coord_t)lroundf(cx + c0 * ro * XS), (lv_coord_t)lroundf(cy + s0 * ro)},
                           {(lv_coord_t)lroundf(cx + c1 * ro * XS), (lv_coord_t)lroundf(cy + s1 * ro)},
                           {(lv_coord_t)lroundf(cx + c1 * ri * XS), (lv_coord_t)lroundf(cy + s1 * ri)},
                           {(lv_coord_t)lroundf(cx + c0 * ri * XS), (lv_coord_t)lroundf(cy + s0 * ri)}};
        lv_coord_t bx1 = LV_MIN(LV_MIN(q[0].x, q[1].x), LV_MIN(q[2].x, q[3].x)) - 1;
        lv_coord_t bx2 = LV_MAX(LV_MAX(q[0].x, q[1].x), LV_MAX(q[2].x, q[3].x)) + 1;
        lv_coord_t by1 = LV_MIN(LV_MIN(q[0].y, q[1].y), LV_MIN(q[2].y, q[3].y)) - 1;
        lv_coord_t by2 = LV_MAX(LV_MAX(q[0].y, q[1].y), LV_MAX(q[2].y, q[3].y)) + 1;
        if (bx2 < clip->x1 || bx1 > clip->x2 || by2 < clip->y1 || by1 > clip->y2) continue;
        dr_poly(dc, q, 4, c);
    }
    if (rounded) {
        lv_coord_t cr = w / 2;
        float e0 = a0 * DEG2RAD, e1 = a1 * DEG2RAD;
        dr_circle(dc, (lv_coord_t)lroundf(cx + cosf(e0) * rm * XS), (lv_coord_t)lroundf(cy + sinf(e0) * rm), cr, c, 0);
        dr_circle(dc, (lv_coord_t)lroundf(cx + cosf(e1) * rm * XS), (lv_coord_t)lroundf(cy + sinf(e1) * rm), cr, c, 0);
    }
}

bool clip_push(lv_draw_ctx_t *dc, lv_coord_t x1, lv_coord_t y1, lv_coord_t x2, lv_coord_t y2, const lv_area_t **saved,
               lv_area_t *storage) {
    lv_area_t a = {x1, y1, x2, y2};
    *saved = dc->clip_area;
    if (!_lv_area_intersect(storage, &a, dc->clip_area)) return false;
    dc->clip_area = storage;
    return true;
}

void clip_pop(lv_draw_ctx_t *dc, const lv_area_t *saved) { dc->clip_area = saved; }

// ---------------------------------------------------------------- geometry

void geo_offset_nm(double lat0, double lon0, double lat, double lon, float &east, float &north) {
    double dlon = lon - lon0;
    if (dlon > 180) dlon -= 360;
    if (dlon < -180) dlon += 360;
    east = (float)(dlon * cos(lat0 * 0.017453292519943295) * 60.0);
    north = (float)((lat - lat0) * 60.0);
}
