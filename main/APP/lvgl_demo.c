/**
 ******************************************************************************
 * @file        lvgl_demo.c
 * @version     V1.0
 * @brief       LVGL V8移植 实验
 ******************************************************************************
 * @attention   Waiken-Smart 慧勤智远
 * 
 * 实验平台:     慧勤智远 ESP32-P4 开发板
 ******************************************************************************
 */

#include "lvgl_demo.h"
#include "lcd.h"
#include "touch.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "demos/lv_demos.h"
#include "esp_lvgl_port.h"
#include "esp_lvgl_port_disp.h"
#include "ui.h"

typedef struct {
    uint16_t x;
    uint16_t y;
    lv_indev_state_t state;
} lvgl_touch_cache_t;

static lvgl_touch_cache_t s_touch_cache = {
    .x = 0,
    .y = 0,
    .state = LV_INDEV_STATE_RELEASED,
};

static portMUX_TYPE s_touch_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_touch_task_handle = NULL;

static void IRAM_ATTR touchpad_isr_handler(void *arg)
{
    BaseType_t need_yield = pdFALSE;

    if (s_touch_task_handle != NULL) {
        vTaskNotifyGiveFromISR(s_touch_task_handle, &need_yield);
    }

    if (need_yield == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void touchpad_poll_task(void *arg)
{
    (void)arg;

    while (1) {
        lvgl_touch_cache_t next = {
            .x = 0,
            .y = 0,
            .state = LV_INDEV_STATE_RELEASED,
        };
        uint32_t wait_ms;

        portENTER_CRITICAL(&s_touch_mux);
        wait_ms = (s_touch_cache.state == LV_INDEV_STATE_PRESSED) ? 4 : 8;
        portEXIT_CRITICAL(&s_touch_mux);

        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms) > 0 ? pdMS_TO_TICKS(wait_ms) : 1);

        tp_dev.scan(0);
        if (tp_dev.sta & TP_PRES_DOWN) {
            next.x = tp_dev.x[0];
            next.y = tp_dev.y[0];
            next.state = LV_INDEV_STATE_PRESSED;
        }

        portENTER_CRITICAL(&s_touch_mux);
        s_touch_cache = next;
        portEXIT_CRITICAL(&s_touch_mux);

        lvgl_port_task_wake(LVGL_PORT_EVENT_TOUCH, NULL);
    }
}


/**
 * @brief       初始化并注册显示设备
 * @param       无
 * @retval      lvgl显示设备指针
 */
lv_display_t *lv_port_disp_init(void)
{
    lv_display_t *lcd_disp_handle = NULL; 

    /* 初始化显示设备LCD */
    lcd_init();                 /* LCD初始化 */

    if (lcddev.id <= 0x7084)    /* RGB屏触摸屏 */
    {
        /* 初始化LVGL显示配置 */
        const lvgl_port_display_cfg_t rgb_disp_cfg = {
            .panel_handle = lcddev.lcd_panel_handle,
            .buffer_size = lcddev.width * lcddev.width,
            .double_buffer = 0,
            .hres = lcddev.width,
            .vres = lcddev.height,
            .monochrome = false,
#if LVGL_VERSION_MAJOR >= 9
            .color_format = LV_COLOR_FORMAT_RGB565,
#endif
            .rotation = {           /* 必须与RGBLCD配置一样 */
                .swap_xy = false,
                .mirror_x = false,
                .mirror_y = false,
            },
            .flags = {
                .buff_dma = false,
                .buff_spiram = false,
                .full_refresh = false,
                .direct_mode = true,
#if LVGL_VERSION_MAJOR >= 9
                .swap_bytes = false,
#endif
            }
        };
        const lvgl_port_display_rgb_cfg_t rgb_cfg = {
            .flags = {
                .bb_mode = true,
                .avoid_tearing = true,
            }
        };

        lcd_disp_handle = lvgl_port_add_disp_rgb(&rgb_disp_cfg, &rgb_cfg);
    }
    else                        /* MIPI屏触摸屏 */
    {
        /* 初始化LVGL显示配置 */
        const lvgl_port_display_cfg_t disp_cfg = {
            .io_handle = lcddev.lcd_dbi_io,         /* 设置io_handle为lcddev.lcd_dbi_io，用于处理显示设备的IO操作 */
            .panel_handle = lcddev.lcd_panel_handle,/* 设置panel_handle为lcddev.lcd_panel_handle，用于处理显示设备的面板操作 */
            .control_handle = NULL,
            .buffer_size = lcddev.width * lcddev.height,
            .double_buffer = true,
            .trans_size = 0,
            .hres = lcddev.width,
            .vres = lcddev.height,
            .monochrome = false,                    /* 设置monochrome为false，用于设置显示设备是否为单色 */
            .rotation = {                           /* MIPI路径不使用硬件mirror/swap_xy，保持默认值即可 */
                .swap_xy = false,
                .mirror_x = false,
                .mirror_y = false,
            },
#if LVGL_VERSION_MAJOR >= 9                     /* LVGL9？ */
#if CONFIG_BSP_LCD_COLOR_FORMAT_RGB888
            .color_format = LV_COLOR_FORMAT_RGB888,
#else
            .color_format = LV_COLOR_FORMAT_RGB565,
#endif
#endif
            .flags = {
                .buff_dma = false,                 /* 分配的LVGL缓冲区支持DMA */
                .buff_spiram = true,               /* 分配的LVGL缓冲区使用PSRAM */
#if LVGL_VERSION_MAJOR >= 9
                .swap_bytes = false,
#endif
                .triple_buffer = true,
                .sw_rotate = false,
                .full_refresh = false,
                .direct_mode = false,
            }
        };

        const lvgl_port_display_dsi_cfg_t  dpi_cfg = {
            .flags = {
                .avoid_tearing = false,
            }
        };

        lcd_disp_handle = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
    }

    return lcd_disp_handle;                    
}

/**
 * @brief       图形库的触摸屏读取回调函数
 * @param       indev_drv   : 触摸屏设备
 * @param       data        : 输入设备数据结构体
 * @retval      无
 */
void touchpad_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    assert(indev); /* 确保输入设备有效 */
    lvgl_touch_cache_t cache;

    portENTER_CRITICAL(&s_touch_mux);
    cache = s_touch_cache;
    portEXIT_CRITICAL(&s_touch_mux);

    data->point.x = cache.x;
    data->point.y = cache.y;
    data->state = cache.state;
}

/**
 * @brief       初始化并注册输入设备
 * @param       无
 * @retval      lvgl输入设备指针
 */
lv_indev_t *lv_port_indev_init(lv_display_t *disp)
{
    /* 初始化触摸屏 */
    tp_dev.init();

    lv_indev_t *indev = lv_indev_create();
    assert(indev);

    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touchpad_read);
    if (disp != NULL) {
        lv_indev_set_display(indev, disp);
    }

    xTaskCreatePinnedToCore(touchpad_poll_task, "touchPoll", 4096, NULL, 6, &s_touch_task_handle, 0);
    gpio_set_intr_type(GT9XXX_INT_GPIO_PIN, GPIO_INTR_ANYEDGE);
    esp_err_t isr_ret = gpio_install_isr_service(0);
    assert(isr_ret == ESP_OK || isr_ret == ESP_ERR_INVALID_STATE);
    gpio_isr_handler_add(GT9XXX_INT_GPIO_PIN, touchpad_isr_handler, NULL);

    return indev;
}

/**
 * @brief       lvgl_demo入口函数
 * @param       无
 * @retval      无
 */
void lvgl_demo(void)
{
    /* 初始化LVGL端口配置 */
    lvgl_port_cfg_t lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    /* 初始化LVGL端口 */
    lvgl_port_init(&lvgl_port_cfg);

    lv_display_t *disp = lv_port_disp_init();    /* lvgl显示接口初始化,放在lv_init()的后面 */
    lv_port_indev_init(disp);                    /* lvgl输入接口初始化,放在lv_init()的后面 */

    /* 锁定互斥锁，因为LVGL API不是线程安全的 */
    if (lvgl_port_lock(0))
    {
        ui_init();

        /* 释放互斥锁 */
        lvgl_port_unlock();  /* 释放互斥锁 */
    }
}
