#include "ui.h"

#include <math.h>
#include <stdint.h>
#include <time.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lcd.h"
#include "lvgl.h"

#define UI_CH_NUM               8
#define UI_POINTS               48
#define UI_REFRESH_MS           100
#define UI_ROW_H                138
#define UI_LEFT_W               200
#define UI_CLR_W                76
#define UI_XDIV_COUNT           6
#define UI_YDIV_COUNT           6
#define UI_UPDATE_CH_PER_TICK   1

typedef enum {
    UNIT_MV = 0,
    UNIT_V = 1,
} unit_t;

typedef enum {
    ACT_UNIT = 0,
    ACT_GAIN,
    ACT_Y_MINUS,
    ACT_Y_PLUS,
    ACT_X_MINUS,
    ACT_X_PLUS,
    ACT_CLEAR,
} action_t;

typedef struct {
    lv_color_t scr_bg;
    lv_color_t panel_bg;
    lv_color_t border;
    lv_color_t text;
    lv_color_t grid;
    lv_color_t major_grid;
} ui_theme_t;

typedef struct {
    unit_t unit;
    uint8_t gain_idx;
    uint8_t y_div_idx;
    uint8_t x_div_idx;
    uint8_t push_divider;
    uint8_t push_count;
    uint8_t clear_burst;
    bool pending_apply;
    uint64_t last_action_us;
    int16_t last_mv;
} ch_cfg_t;

typedef struct {
    lv_obj_t *scr;
    lv_obj_t *top;
    lv_obj_t *top_brt_panel;
    lv_obj_t *top_theme_panel;
    lv_obj_t *brt_slider;
    lv_obj_t *theme_label;
    lv_obj_t *time_label;
    lv_obj_t *brightness_label;
    lv_obj_t *theme_sw;
    lv_obj_t *value_label[UI_CH_NUM];
    lv_obj_t *unit_btn_label[UI_CH_NUM];
    lv_obj_t *gain_btn_label[UI_CH_NUM];
    lv_obj_t *axis_label[UI_CH_NUM];
    lv_obj_t *chart[UI_CH_NUM];
    lv_chart_series_t *series[UI_CH_NUM];
    lv_timer_t *timer;
    ch_cfg_t ch[UI_CH_NUM];
    bool dark_theme;
    bool brt_dragging;
    uint8_t brightness_pct;
    uint8_t brightness_last_good;
    uint8_t time_tick_div;
    uint16_t phase;
} ui_ctx_t;

static ui_ctx_t s_ui;
static const char *TAG = "ui_adc";

static const ui_theme_t THEME_DARK = {
    .scr_bg = LV_COLOR_MAKE(0x12, 0x14, 0x18),
    .panel_bg = LV_COLOR_MAKE(0x0f, 0x11, 0x15),
    .border = LV_COLOR_MAKE(0xe9, 0xed, 0xf2),
    .text = LV_COLOR_MAKE(0xf1, 0xf5, 0xf9),
    .grid = LV_COLOR_MAKE(0x64, 0x72, 0x82),
    .major_grid = LV_COLOR_MAKE(0x9a, 0xa9, 0xbd),
};

static const ui_theme_t THEME_LIGHT = {
    .scr_bg = LV_COLOR_MAKE(0xf3, 0xf6, 0xfa),
    .panel_bg = LV_COLOR_MAKE(0xff, 0xff, 0xff),
    .border = LV_COLOR_MAKE(0x1f, 0x29, 0x37),
    .text = LV_COLOR_MAKE(0x0f, 0x17, 0x2a),
    .grid = LV_COLOR_MAKE(0x8e, 0x9d, 0xb0),
    .major_grid = LV_COLOR_MAKE(0x4a, 0x5c, 0x73),
};

static const uint8_t GAIN_LIST[] = {1, 2, 4, 8};
static const uint16_t Y_DIV_MV_LIST[] = {100, 200, 500, 1000, 2000, 5000};
static const uint16_t X_DIV_MS_LIST[] = {20, 50, 100, 200, 500, 1000};

static int16_t sim_mv(uint8_t ch, uint16_t t)
{
    float base = 550.0f + 260.0f * (float)ch;
    float a = 160.0f + 18.0f * (float)ch;
    float b = 95.0f + 8.0f * (float)ch;
    float v = base
              + a * sinf(0.052f * (float)t + 0.39f * (float)ch)
              + b * sinf(0.123f * (float)t + 0.21f * (float)ch);
    int32_t noise = (int32_t)(esp_random() % 56U) - 28;
    int32_t out = (int32_t)v + noise;
    if (out < 0) out = 0;
    if (out > 3300) out = 3300;
    return (int16_t)out;
}

static void ui_update_channel_ctrl_text(uint8_t ch)
{
    uint8_t gain = GAIN_LIST[s_ui.ch[ch].gain_idx];
    lv_label_set_text(s_ui.unit_btn_label[ch], (s_ui.ch[ch].unit == UNIT_V) ? "V" : "mV");
    lv_label_set_text_fmt(s_ui.gain_btn_label[ch], "x%u", (unsigned)gain);
}

static void ui_update_axis_label(uint8_t ch)
{
    uint16_t x_div = X_DIV_MS_LIST[s_ui.ch[ch].x_div_idx];
    uint16_t y_div_mv = Y_DIV_MV_LIST[s_ui.ch[ch].y_div_idx];
    uint32_t y_max_mv = (uint32_t)y_div_mv * UI_YDIV_COUNT;

    if (s_ui.ch[ch].unit == UNIT_V) {
        lv_label_set_text_fmt(s_ui.axis_label[ch], "X:%ums/div  Y:%.3fV/div", (unsigned)x_div, (double)y_div_mv / 1000.0);
    } else {
        lv_label_set_text_fmt(s_ui.axis_label[ch], "X:%ums/div  Y:%umV/div", (unsigned)x_div, (unsigned)y_div_mv);
    }

    lv_chart_set_axis_range(s_ui.chart[ch], LV_CHART_AXIS_PRIMARY_Y, 0, (int32_t)y_max_mv);
}

static void ui_update_value_label(uint8_t ch, int32_t mv)
{
    if (s_ui.ch[ch].unit == UNIT_V) {
        lv_label_set_text_fmt(s_ui.value_label[ch], "CH%u: %.6fV", (unsigned)(ch + 1), (double)mv / 1000.0);
    } else {
        lv_label_set_text_fmt(s_ui.value_label[ch], "CH%u: %ldmV", (unsigned)(ch + 1), (long)mv);
    }
}

static void ui_recalc_x_divider(uint8_t ch)
{
    uint16_t x_div_ms = X_DIV_MS_LIST[s_ui.ch[ch].x_div_idx];
    uint32_t window_ms = (uint32_t)x_div_ms * 10U;
    uint32_t sample_ms = window_ms / UI_POINTS;
    uint32_t div = (sample_ms + (UI_REFRESH_MS / 2U)) / UI_REFRESH_MS;
    if (div == 0U) div = 1U;
    if (div > 20U) div = 20U;
    s_ui.ch[ch].push_divider = (uint8_t)div;
}

static void ui_apply_theme(void)
{
    const ui_theme_t *th = s_ui.dark_theme ? &THEME_DARK : &THEME_LIGHT;
    lv_obj_t *root = lv_obj_get_child(s_ui.scr, 1);

    lv_obj_set_style_bg_color(s_ui.scr, th->scr_bg, 0);
    lv_obj_set_style_bg_opa(s_ui.scr, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_ui.top, th->panel_bg, 0);
    lv_obj_set_style_bg_opa(s_ui.top, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_ui.top, th->border, 0);
    lv_obj_set_style_border_width(s_ui.top, 2, 0);
    lv_obj_set_style_radius(s_ui.top, 4, 0);

    lv_obj_set_style_bg_color(s_ui.top_brt_panel, th->scr_bg, 0);
    lv_obj_set_style_bg_opa(s_ui.top_brt_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_ui.top_brt_panel, th->border, 0);
    lv_obj_set_style_border_width(s_ui.top_brt_panel, 1, 0);
    lv_obj_set_style_radius(s_ui.top_brt_panel, 4, 0);

    lv_obj_set_style_bg_color(s_ui.top_theme_panel, th->scr_bg, 0);
    lv_obj_set_style_bg_opa(s_ui.top_theme_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_ui.top_theme_panel, th->border, 0);
    lv_obj_set_style_border_width(s_ui.top_theme_panel, 1, 0);
    lv_obj_set_style_radius(s_ui.top_theme_panel, 4, 0);

    lv_obj_set_style_text_color(s_ui.time_label, th->text, 0);
    lv_obj_set_style_text_color(s_ui.brightness_label, th->text, 0);
    lv_obj_set_style_text_color(s_ui.theme_label, th->text, 0);
    lv_obj_set_style_bg_color(s_ui.brt_slider, th->major_grid, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_ui.brt_slider, th->grid, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.brt_slider, th->text, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s_ui.brt_slider, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.brt_slider, LV_OPA_90, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_ui.brt_slider, LV_OPA_100, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_ui.brt_slider, 2, LV_PART_KNOB);

    lv_obj_set_style_bg_color(s_ui.theme_sw, th->grid, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.theme_sw, th->major_grid, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_ui.theme_sw, th->text, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s_ui.theme_sw, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.theme_sw, LV_OPA_90, LV_PART_INDICATOR);
    if (root) {
        lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    }

    for (uint8_t ch = 0; ch < UI_CH_NUM; ch++) {
        lv_obj_t *row = lv_obj_get_child(root, ch);
        lv_obj_t *box_l = lv_obj_get_child(row, 0);
        lv_obj_t *box_r = lv_obj_get_child(row, 1);
        lv_obj_t *box_c = lv_obj_get_child(row, 2);

        lv_obj_set_style_bg_color(box_l, th->panel_bg, 0);
        lv_obj_set_style_bg_opa(box_l, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(box_l, th->border, 0);
        lv_obj_set_style_border_width(box_l, 2, 0);

        lv_obj_set_style_bg_color(box_r, th->panel_bg, 0);
        lv_obj_set_style_bg_opa(box_r, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(box_r, th->border, 0);
        lv_obj_set_style_border_width(box_r, 2, 0);
        if (box_c) {
            lv_obj_set_style_bg_color(box_c, th->panel_bg, 0);
            lv_obj_set_style_bg_opa(box_c, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(box_c, th->border, 0);
            lv_obj_set_style_border_width(box_c, 2, 0);
        }

        lv_obj_set_style_text_color(s_ui.value_label[ch], th->text, 0);
        lv_obj_set_style_text_color(s_ui.axis_label[ch], th->text, 0);
        lv_obj_set_style_bg_color(s_ui.chart[ch], th->panel_bg, 0);
        lv_obj_set_style_bg_opa(s_ui.chart[ch], LV_OPA_COVER, 0);
        lv_obj_set_style_line_color(s_ui.chart[ch], th->grid, LV_PART_MAIN);
        lv_obj_set_style_line_opa(s_ui.chart[ch], LV_OPA_90, LV_PART_MAIN);
        lv_obj_set_style_border_color(s_ui.chart[ch], th->major_grid, LV_PART_INDICATOR);
    }
}

static void ui_apply_brightness(void)
{
    lcd_set_backlight(s_ui.brightness_pct);
    lv_label_set_text_fmt(s_ui.brightness_label, "BRT %u%%", (unsigned)s_ui.brightness_pct);
}

static void brightness_event_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        s_ui.brt_dragging = true;
        s_ui.brightness_last_good = s_ui.brightness_pct;
        return;
    }

    if (code == LV_EVENT_VALUE_CHANGED) {
        uint8_t v = (uint8_t)lv_slider_get_value(slider);
        if (v < 60U) v = 60U;

        /* 过滤松手瞬间可能出现的跳变到最小值 */
        if (s_ui.brt_dragging && v <= 62U && s_ui.brightness_last_good >= 65U) {
            lv_slider_set_value(slider, s_ui.brightness_last_good, LV_ANIM_OFF);
            return;
        }

        s_ui.brightness_pct = v;
        s_ui.brightness_last_good = v;
        ui_apply_brightness();
        return;
    }

    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_ui.brt_dragging = false;
        if (s_ui.brightness_last_good < 60U) {
            s_ui.brightness_last_good = 60U;
        }
        s_ui.brightness_pct = s_ui.brightness_last_good;
        lv_slider_set_value(slider, s_ui.brightness_pct, LV_ANIM_OFF);
        ui_apply_brightness();
    }
}

static void theme_event_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    s_ui.dark_theme = lv_obj_has_state(sw, LV_STATE_CHECKED);
    ui_apply_theme();
}

static void ch_action_event_cb(lv_event_t *e)
{
    uintptr_t tag = (uintptr_t)lv_event_get_user_data(e);
    uint8_t ch = (uint8_t)(tag >> 8);
    action_t act = (action_t)(tag & 0xFFU);
    uint64_t now_us = (uint64_t)esp_timer_get_time();

    if (ch >= UI_CH_NUM) {
        return;
    }

    /* 触控防抖：80ms内同通道重复点击直接忽略，避免触发重绘风暴 */
    if ((now_us - s_ui.ch[ch].last_action_us) < 80000ULL) {
        return;
    }
    s_ui.ch[ch].last_action_us = now_us;

    switch (act) {
        case ACT_UNIT:
            s_ui.ch[ch].unit = (s_ui.ch[ch].unit == UNIT_V) ? UNIT_MV : UNIT_V;
            s_ui.ch[ch].pending_apply = true;
            break;
        case ACT_GAIN:
            s_ui.ch[ch].gain_idx = (uint8_t)((s_ui.ch[ch].gain_idx + 1U) % (sizeof(GAIN_LIST) / sizeof(GAIN_LIST[0])));
            s_ui.ch[ch].pending_apply = true;
            break;
        case ACT_Y_MINUS:
            if (s_ui.ch[ch].y_div_idx > 0U) s_ui.ch[ch].y_div_idx--;
            s_ui.ch[ch].pending_apply = true;
            break;
        case ACT_Y_PLUS:
            if (s_ui.ch[ch].y_div_idx + 1U < (sizeof(Y_DIV_MV_LIST) / sizeof(Y_DIV_MV_LIST[0]))) s_ui.ch[ch].y_div_idx++;
            s_ui.ch[ch].pending_apply = true;
            break;
        case ACT_X_MINUS:
            if (s_ui.ch[ch].x_div_idx > 0U) s_ui.ch[ch].x_div_idx--;
            s_ui.ch[ch].pending_apply = true;
            break;
        case ACT_X_PLUS:
            if (s_ui.ch[ch].x_div_idx + 1U < (sizeof(X_DIV_MS_LIST) / sizeof(X_DIV_MS_LIST[0]))) s_ui.ch[ch].x_div_idx++;
            s_ui.ch[ch].pending_apply = true;
            break;
        case ACT_CLEAR:
            s_ui.ch[ch].clear_burst = 10;
            s_ui.ch[ch].last_mv = 0;
            ui_update_value_label(ch, 0);
            break;
    }
}

static lv_obj_t *mk_btn(lv_obj_t *parent, const char *txt, uintptr_t tag)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_t *lab = lv_label_create(btn);
    lv_label_set_text(lab, txt);
    lv_obj_set_size(btn, 36, 24);
    lv_obj_center(lab);
    lv_obj_add_event_cb(btn, ch_action_event_cb, LV_EVENT_CLICKED, (void *)tag);
    return lab;
}

static void ui_tick(lv_timer_t *timer)
{
    LV_UNUSED(timer);

    uint8_t base = (uint8_t)((s_ui.phase * UI_UPDATE_CH_PER_TICK) % UI_CH_NUM);
    for (uint8_t n = 0; n < UI_UPDATE_CH_PER_TICK; n++) {
        uint8_t ch = (uint8_t)((base + n) % UI_CH_NUM);
        if (s_ui.ch[ch].pending_apply) {
            s_ui.ch[ch].pending_apply = false;
            ui_recalc_x_divider(ch);
            ui_update_channel_ctrl_text(ch);
            ui_update_axis_label(ch);
        }

        if (s_ui.ch[ch].clear_burst > 0U) {
            for (uint8_t k = 0; k < 8; k++) {
                lv_chart_set_next_value(s_ui.chart[ch], s_ui.series[ch], 0);
            }
            s_ui.ch[ch].clear_burst--;
            s_ui.ch[ch].last_mv = 0;
            ui_update_value_label(ch, 0);
            continue;
        }

        int32_t mv = (int32_t)sim_mv(ch, s_ui.phase) * (int32_t)GAIN_LIST[s_ui.ch[ch].gain_idx];
        int32_t y_max = (int32_t)Y_DIV_MV_LIST[s_ui.ch[ch].y_div_idx] * UI_YDIV_COUNT;
        if (mv > y_max) mv = y_max;

        s_ui.ch[ch].last_mv = (int16_t)mv;
        ui_update_value_label(ch, mv);

        s_ui.ch[ch].push_count++;
        if (s_ui.ch[ch].push_count >= s_ui.ch[ch].push_divider) {
            s_ui.ch[ch].push_count = 0;
            lv_chart_set_next_value(s_ui.chart[ch], s_ui.series[ch], mv);
        } else {
            lv_chart_set_next_value(s_ui.chart[ch], s_ui.series[ch], s_ui.ch[ch].last_mv);
        }
    }

    s_ui.time_tick_div++;
    if (s_ui.time_tick_div >= 20U) { /* ~2s when UI_REFRESH_MS=100ms */
        s_ui.time_tick_div = 0;
        {
            time_t now = time(NULL);
            struct tm tm_now;
            if (now > 1700000000 && localtime_r(&now, &tm_now) != NULL) {
                lv_label_set_text_fmt(s_ui.time_label, "%02d:%02d:%02d", tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
            } else {
                uint32_t sec = (uint32_t)(esp_timer_get_time() / 1000000ULL);
                lv_label_set_text_fmt(s_ui.time_label, "UP %02u:%02u:%02u",
                                      (unsigned)((sec / 3600U) % 100U),
                                      (unsigned)((sec / 60U) % 60U),
                                      (unsigned)(sec % 60U));
            }
        }
    }

    s_ui.phase++;
}

void ui_init(void)
{
    static const lv_color_t line_color[UI_CH_NUM] = {
        LV_COLOR_MAKE(0x6a, 0xcc, 0xff), LV_COLOR_MAKE(0xff, 0x9f, 0x55),
        LV_COLOR_MAKE(0x7a, 0xe5, 0x7a), LV_COLOR_MAKE(0xdb, 0xa2, 0xff),
        LV_COLOR_MAKE(0x6d, 0xe2, 0xe2), LV_COLOR_MAKE(0xff, 0x95, 0xb7),
        LV_COLOR_MAKE(0xff, 0xd6, 0x6b), LV_COLOR_MAKE(0xb4, 0xc8, 0xff),
    };
    lv_obj_t *top;
    lv_obj_t *brt_title;
    lv_obj_t *theme_title;
    lv_obj_t *content;
    lv_obj_t *brt_slider;

    ESP_LOGI(TAG, "ui_init start");
    if (s_ui.timer != NULL) {
        lv_timer_delete(s_ui.timer);
    }
    s_ui = (ui_ctx_t){0};
    s_ui.scr = lv_screen_active();
    s_ui.dark_theme = true;
    s_ui.brt_dragging = false;
    s_ui.brightness_pct = 100;
    s_ui.brightness_last_good = 100;
    s_ui.time_tick_div = 0;

    lv_obj_clean(s_ui.scr);
    lv_obj_set_style_pad_all(s_ui.scr, 8, 0);
    lv_obj_set_layout(s_ui.scr, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_ui.scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_ui.scr, 8, 0);

    top = lv_obj_create(s_ui.scr);
    s_ui.top = top;
    lv_obj_set_size(top, lv_pct(100), 52);
    lv_obj_set_style_pad_left(top, 10, 0);
    lv_obj_set_style_pad_right(top, 10, 0);
    lv_obj_set_style_pad_top(top, 8, 0);
    lv_obj_set_style_pad_bottom(top, 8, 0);
    lv_obj_set_style_pad_column(top, 10, 0);
    lv_obj_set_layout(top, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_ui.top_brt_panel = lv_obj_create(top);
    lv_obj_set_size(s_ui.top_brt_panel, 420, lv_pct(100));
    lv_obj_set_style_pad_left(s_ui.top_brt_panel, 10, 0);
    lv_obj_set_style_pad_right(s_ui.top_brt_panel, 10, 0);
    lv_obj_set_style_pad_top(s_ui.top_brt_panel, 6, 0);
    lv_obj_set_style_pad_bottom(s_ui.top_brt_panel, 6, 0);
    lv_obj_set_style_pad_column(s_ui.top_brt_panel, 8, 0);
    lv_obj_set_layout(s_ui.top_brt_panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_ui.top_brt_panel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_ui.top_brt_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    brt_title = lv_label_create(s_ui.top_brt_panel);
    lv_label_set_text(brt_title, "Brightness");
    lv_obj_set_style_text_font(brt_title, &lv_font_montserrat_14, 0);

    s_ui.brightness_label = lv_label_create(s_ui.top_brt_panel);
    lv_obj_set_style_text_font(s_ui.brightness_label, &lv_font_montserrat_14, 0);

    brt_slider = lv_slider_create(s_ui.top_brt_panel);
    s_ui.brt_slider = brt_slider;
    lv_obj_set_size(brt_slider, 220, 20);
    lv_slider_set_range(brt_slider, 60, 100);
    lv_slider_set_value(brt_slider, 100, LV_ANIM_OFF);
    lv_obj_add_event_cb(brt_slider, brightness_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(brt_slider, brightness_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(brt_slider, brightness_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(brt_slider, brightness_event_cb, LV_EVENT_PRESS_LOST, NULL);

    s_ui.top_theme_panel = lv_obj_create(top);
    lv_obj_set_size(s_ui.top_theme_panel, 220, lv_pct(100));
    lv_obj_set_style_pad_left(s_ui.top_theme_panel, 10, 0);
    lv_obj_set_style_pad_right(s_ui.top_theme_panel, 10, 0);
    lv_obj_set_style_pad_top(s_ui.top_theme_panel, 6, 0);
    lv_obj_set_style_pad_bottom(s_ui.top_theme_panel, 6, 0);
    lv_obj_set_style_pad_column(s_ui.top_theme_panel, 8, 0);
    lv_obj_set_layout(s_ui.top_theme_panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_ui.top_theme_panel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_ui.top_theme_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    theme_title = lv_label_create(s_ui.top_theme_panel);
    s_ui.theme_label = theme_title;
    lv_label_set_text(theme_title, "Theme");
    lv_obj_set_style_text_font(theme_title, &lv_font_montserrat_14, 0);

    s_ui.theme_sw = lv_switch_create(s_ui.top_theme_panel);
    lv_obj_set_size(s_ui.theme_sw, 58, 30);
    lv_obj_add_state(s_ui.theme_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_ui.theme_sw, theme_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_ui.time_label = lv_label_create(top);
    lv_obj_set_style_text_font(s_ui.time_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_ui.time_label, "--:--:--");
    lv_obj_set_flex_grow(s_ui.time_label, 1);
    lv_obj_set_style_text_align(s_ui.time_label, LV_TEXT_ALIGN_RIGHT, 0);

    content = lv_obj_create(s_ui.scr);
    lv_obj_set_size(content, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_style_pad_row(content, 6, 0);
    lv_obj_set_layout(content, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);

    for (uint8_t ch = 0; ch < UI_CH_NUM; ch++) {
        lv_obj_t *row = lv_obj_create(content);
        lv_obj_t *box_l;
        lv_obj_t *ctl;
        lv_obj_t *box_r;
        lv_obj_t *box_c;

        s_ui.ch[ch].unit = UNIT_V;
        s_ui.ch[ch].gain_idx = 0;
        s_ui.ch[ch].y_div_idx = 2;
        s_ui.ch[ch].x_div_idx = 2;
        s_ui.ch[ch].pending_apply = true;
        ui_recalc_x_divider(ch);

        lv_obj_set_size(row, lv_pct(100), UI_ROW_H);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_column(row, 10, 0);
        lv_obj_set_layout(row, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);

        box_l = lv_obj_create(row);
        lv_obj_set_size(box_l, UI_LEFT_W, lv_pct(100));
        lv_obj_set_style_radius(box_l, 0, 0);
        lv_obj_set_style_pad_left(box_l, 12, 0);
        lv_obj_set_style_pad_right(box_l, 8, 0);
        lv_obj_set_style_pad_top(box_l, 6, 0);
        lv_obj_set_style_pad_bottom(box_l, 6, 0);
        lv_obj_set_layout(box_l, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(box_l, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(box_l, 6, 0);

        s_ui.value_label[ch] = lv_label_create(box_l);
        lv_obj_set_style_text_font(s_ui.value_label[ch], &lv_font_montserrat_14, 0);
        lv_obj_set_style_transform_zoom(s_ui.value_label[ch], 320, 0); /* 256=1.0x, 320=1.25x */

        ctl = lv_obj_create(box_l);
        lv_obj_set_size(ctl, lv_pct(100), 58);
        lv_obj_set_style_bg_opa(ctl, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(ctl, 0, 0);
        lv_obj_set_style_pad_all(ctl, 0, 0);
        lv_obj_set_style_pad_column(ctl, 4, 0);
        lv_obj_set_style_pad_row(ctl, 4, 0);
        lv_obj_set_layout(ctl, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(ctl, LV_FLEX_FLOW_ROW_WRAP);

        s_ui.unit_btn_label[ch] = mk_btn(ctl, "V", ((uintptr_t)ch << 8) | ACT_UNIT);
        s_ui.gain_btn_label[ch] = mk_btn(ctl, "x1", ((uintptr_t)ch << 8) | ACT_GAIN);
        mk_btn(ctl, "Y-", ((uintptr_t)ch << 8) | ACT_Y_MINUS);
        mk_btn(ctl, "Y+", ((uintptr_t)ch << 8) | ACT_Y_PLUS);
        mk_btn(ctl, "X-", ((uintptr_t)ch << 8) | ACT_X_MINUS);
        mk_btn(ctl, "X+", ((uintptr_t)ch << 8) | ACT_X_PLUS);

        box_r = lv_obj_create(row);
        lv_obj_set_size(box_r, lv_pct(100), lv_pct(100));
        lv_obj_set_flex_grow(box_r, 1);
        lv_obj_set_style_radius(box_r, 0, 0);
        lv_obj_set_style_pad_all(box_r, 4, 0);

        s_ui.chart[ch] = lv_chart_create(box_r);
        lv_obj_set_size(s_ui.chart[ch], lv_pct(100), lv_pct(100));
        lv_obj_set_style_border_width(s_ui.chart[ch], 0, 0);
        lv_obj_set_style_line_width(s_ui.chart[ch], 2, LV_PART_ITEMS);
        lv_obj_set_style_line_opa(s_ui.chart[ch], LV_OPA_70, LV_PART_ITEMS);
        lv_obj_set_style_size(s_ui.chart[ch], 0, 0, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(s_ui.chart[ch], LV_OPA_TRANSP, LV_PART_INDICATOR);
        lv_obj_set_style_border_width(s_ui.chart[ch], 0, LV_PART_INDICATOR);
        lv_chart_set_type(s_ui.chart[ch], LV_CHART_TYPE_LINE);
        lv_chart_set_point_count(s_ui.chart[ch], UI_POINTS);
        lv_chart_set_div_line_count(s_ui.chart[ch], 2, 3);
        lv_chart_set_update_mode(s_ui.chart[ch], LV_CHART_UPDATE_MODE_CIRCULAR);
        s_ui.series[ch] = lv_chart_add_series(s_ui.chart[ch], line_color[ch], LV_CHART_AXIS_PRIMARY_Y);
        lv_chart_set_all_values(s_ui.chart[ch], s_ui.series[ch], 0);

        s_ui.axis_label[ch] = lv_label_create(box_r);
        lv_obj_set_style_text_font(s_ui.axis_label[ch], &lv_font_montserrat_14, 0);
        lv_obj_align(s_ui.axis_label[ch], LV_ALIGN_BOTTOM_RIGHT, -4, -2);

        box_c = lv_obj_create(row);
        lv_obj_set_size(box_c, UI_CLR_W, lv_pct(100));
        lv_obj_set_style_radius(box_c, 0, 0);
        lv_obj_set_style_pad_all(box_c, 6, 0);
        lv_obj_set_layout(box_c, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(box_c, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(box_c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        {
            lv_obj_t *btn = lv_button_create(box_c);
            lv_obj_t *lab = lv_label_create(btn);
            lv_label_set_text(lab, "Clear");
            lv_obj_set_size(btn, 62, 32);
            lv_obj_center(lab);
            lv_obj_add_event_cb(btn, ch_action_event_cb, LV_EVENT_CLICKED, (void *)(((uintptr_t)ch << 8) | ACT_CLEAR));
        }

        ui_update_channel_ctrl_text(ch);
        ui_update_axis_label(ch);
        ui_update_value_label(ch, 0);
    }

    ui_apply_theme();
    ui_apply_brightness();

    s_ui.timer = lv_timer_create(ui_tick, UI_REFRESH_MS, NULL);
    ESP_LOGI(TAG, "ui_init done, source=sim-8ch-advanced");
}
