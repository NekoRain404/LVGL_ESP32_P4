#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include "esp_lvgl_port_priv.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lvgl_port_disp.h"
#include "draw/sw/lv_draw_sw_utils.h"


#define LVGL_PORT_HANDLE_FLUSH_READY    1
static const char *TAG = "LVGL";

/*******************************************************************************
* 类型定义
*******************************************************************************/

typedef struct {
    lvgl_port_disp_type_t     disp_type;        /* 显示设备类型 */
    esp_lcd_panel_io_handle_t io_handle;        /* LCD 面板 I/O 句柄 */
    esp_lcd_panel_handle_t    panel_handle;     /* LCD 面板句柄 */
    esp_lcd_panel_handle_t    control_handle;   /* LCD 面板控制句柄 */
    lvgl_port_rotation_cfg_t  rotation;         /* 屏幕旋转的默认值 */
    lv_display_t              *disp;            /* LVGL 显示对象 */
    lv_color_t                *trans_buf;       /* 发送到驱动的缓冲区 */
    uint32_t                  trans_size;       /* 单次传输的最大大小 */
    SemaphoreHandle_t         trans_sem;        /* 空闲传输互斥锁 */
    void                      *buf1;            /* LVGL buffer 1 */
    void                      *buf2;            /* LVGL buffer 2 */
    void                      *buf3;            /* LVGL buffer 3 */
    lv_draw_buf_t             draw_buf_3;       /* 第三个绘图缓冲的描述符 */
    bool                      sw_rotate;        /* 是否使用LVGL软件旋转 */
} lvgl_port_display_ctx_t;

/*******************************************************************************
* 函数声明
*******************************************************************************/
static lv_display_t *lvgl_port_add_disp_priv(const lvgl_port_display_cfg_t *disp_cfg, const lvgl_port_disp_priv_cfg_t *priv_cfg);
static lvgl_port_display_ctx_t *lvgl_port_get_display_ctx(lv_display_t *disp);
static void lvgl_port_disp_resolution_evt_cb(lv_event_t *e);
#if LVGL_PORT_HANDLE_FLUSH_READY
static bool lvgl_port_flush_io_ready_callback(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx);
static bool lvgl_port_flush_rgb_vsync_ready_callback(esp_lcd_panel_handle_t panel_io, const esp_lcd_rgb_panel_event_data_t *edata, void *user_ctx);
static bool lvgl_port_flush_dpi_panel_ready_callback(esp_lcd_panel_handle_t panel_io, esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx);
static bool lvgl_port_flush_dpi_vsync_ready_callback(esp_lcd_panel_handle_t panel_io, esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx);
#endif
static void lvgl_port_flush_callback(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map);
static void lvgl_port_update_callback(lv_display_t *disp);

/*******************************************************************************
* 公共函数
*******************************************************************************/

/**
 * 添加一个显示设备，并配置其参数。
 * 
 * 此函数通过调用 `lvgl_port_add_disp_priv` 函数来添加显示设备，并设置显示设备的上下文。如果定义了 `LVGL_PORT_HANDLE_FLUSH_READY` 宏，
 * 还会注册一个颜色传输完成的回调函数。
 * 
 * @param disp_cfg 指向显示设备配置结构体的指针。
 * @return 成功时返回指向 `lv_disp_t` 对象的指针，失败时返回 NULL。
 */
lv_display_t *lvgl_port_add_disp(const lvgl_port_display_cfg_t *disp_cfg)
{
    /* 调用私有函数添加显示设备 */
    lv_display_t *disp = lvgl_port_add_disp_priv(disp_cfg, NULL);

    /* 检查显示设备是否成功添加 */
    if (disp != NULL) {
        /* 获取显示设备上下文 */
        lvgl_port_display_ctx_t *disp_ctx = lvgl_port_get_display_ctx(disp);
        /* 设置显示设备类型 */
        disp_ctx->disp_type = LVGL_PORT_DISP_TYPE_OTHER;

        /* 断言 IO 句柄不为空 */
        assert(disp_ctx->io_handle != NULL);

#if LVGL_PORT_HANDLE_FLUSH_READY
        /* 定义颜色传输完成的回调函数 */
        const esp_lcd_panel_io_callbacks_t cbs = {
            .on_color_trans_done = lvgl_port_flush_io_ready_callback,
        };
        /* 注册回调函数 */
        esp_lcd_panel_io_register_event_callbacks(disp_ctx->io_handle, &cbs, disp);
#endif
    }

    /* 返回显示设备对象指针 */
    return disp;
}

/**
 * @brief 添加DSI（显示串行接口）显示设备
 * 
 * 此函数根据配置参数添加一个DSI显示设备。它负责设置DSI显示的基本参数，
 * 配置回调函数，并将这些回调函数注册到显示驱动程序中。
 * 
 * @param disp_cfg 基本显示配置参数，用于设置显示设备。
 * @param dsi_cfg DSI特定的配置参数，包含DSI接口和模式设置。
 * @return lv_display_t* 返回一个表示显示设备的lv_display_t对象指针，
 *         如果添加失败则返回NULL。
 */
lv_display_t *lvgl_port_add_disp_dsi(const lvgl_port_display_cfg_t *disp_cfg, const lvgl_port_display_dsi_cfg_t *dsi_cfg)
{
    /* 确保DSI配置不为空 */
    assert(dsi_cfg != NULL);

    /* 初始化私有配置结构体 */
    const lvgl_port_disp_priv_cfg_t priv_cfg = {
        .avoid_tearing = dsi_cfg->flags.avoid_tearing,
    };

    /* 调用私有函数添加显示设备 */
    lv_display_t *disp = lvgl_port_add_disp_priv(disp_cfg, &priv_cfg);

    if (disp != NULL)
    {
        /* 获取显示上下文 */
        lvgl_port_display_ctx_t *disp_ctx = lvgl_port_get_display_ctx(disp);
#if LVGL_PORT_HANDLE_FLUSH_READY
        /* 设置显示类型为DSI */
        disp_ctx->disp_type = LVGL_PORT_DISP_TYPE_DSI;

        /* 初始化显示面板事件回调结构体 */
        esp_lcd_dpi_panel_event_callbacks_t cbs = {0};

        /* 根据是否避免撕裂设置不同的回调函数 */
        if (dsi_cfg->flags.avoid_tearing)
        {
            cbs.on_refresh_done = lvgl_port_flush_dpi_vsync_ready_callback;
        }
        else
        {
            cbs.on_color_trans_done = lvgl_port_flush_dpi_panel_ready_callback;
        }

        /* 注册完成回调 */
        esp_lcd_dpi_panel_register_event_callbacks(disp_ctx->panel_handle, &cbs, disp);
#endif
    }

    /* 返回显示设备指针 */
    return disp;
}

/**
 * 添加并配置一个RGB显示设备
 * 
 * @param disp_cfg 显示设备的基本配置参数
 * @param rgb_cfg RGB显示设备的特定配置参数
 * @return 返回创建的显示设备对象指针
 */
lv_display_t *lvgl_port_add_disp_rgb(const lvgl_port_display_cfg_t *disp_cfg, const lvgl_port_display_rgb_cfg_t *rgb_cfg)
{
    /* 确保RGB显示设备的配置参数不为空 */
    assert(rgb_cfg != NULL);

    /* 初始化显示设备的私有配置结构体，并根据配置设置防撕裂功能 */
    const lvgl_port_disp_priv_cfg_t priv_cfg = {
        .avoid_tearing = rgb_cfg->flags.avoid_tearing,
    };

    /* 调用底层函数添加显示设备并获取显示设备对象 */
    lv_display_t *disp = lvgl_port_add_disp_priv(disp_cfg, &priv_cfg);

    /* 如果显示设备对象成功创建，进一步配置它 */
    if (disp != NULL)
    {
        /* 获取显示设备上下文以进行后续配置 */
        lvgl_port_display_ctx_t *disp_ctx = lvgl_port_get_display_ctx(disp);

        /* 设置显示设备类型为RGB */
        disp_ctx->disp_type = LVGL_PORT_DISP_TYPE_RGB;
#if LVGL_PORT_HANDLE_FLUSH_READY
        /* 配置RGB显示设备的事件回调函数 */
        /* 这里配置V-sync信号回调函数 */
        const esp_lcd_rgb_panel_event_callbacks_t vsync_cbs = {
            .on_vsync = lvgl_port_flush_rgb_vsync_ready_callback,
        };

        /* 根据IDF版本和是否启用弹跳缓冲模式，配置适当的回调函数 */
        const esp_lcd_rgb_panel_event_callbacks_t bb_cbs =
        {
            .on_color_trans_done = lvgl_port_flush_rgb_vsync_ready_callback,
        };

        /* 根据配置和IDF版本，决定是否注册弹跳缓冲模式的回调函数 */
        if (rgb_cfg->flags.bb_mode && (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 2)))
        {
            /* 注册弹跳缓冲模式的回调函数 */
            ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(disp_ctx->panel_handle, &bb_cbs, disp));
        }
        else
        {
            /* 注册V-sync信号的回调函数 */
            ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(disp_ctx->panel_handle, &vsync_cbs, disp));
        }
#endif
    }

    /* 返回创建的显示设备对象 */
    return disp;
}

/**
 * @brief 移除LVGL显示器端口
 * 
 * 本函数负责释放与LVGL显示器相关的所有资源，并从LVGL系统中移除该显示器
 * 它首先检查传输信号量是否存在，如果存在，则删除它接着，它会移除显示器驱动相关的缓冲区，
 * 并释放这些缓冲区所占用的内存最后，它会释放显示器上下文本身
 * 
 * @param disp 指向要移除的LVGL显示器的指针
 * @return esp_err_t 如果操作成功，返回ESP_OK；否则返回错误代码
 */
esp_err_t lvgl_port_remove_disp(lv_display_t *disp)
{
    assert(disp);
    lvgl_port_display_ctx_t *disp_ctx = lvgl_port_get_display_ctx(disp);

    if (disp_ctx->trans_sem)
    {
        vSemaphoreDelete(disp_ctx->trans_sem);
    }

    free(disp_ctx->buf1);
    free(disp_ctx->buf2);
    free(disp_ctx->buf3);
    free(disp_ctx->trans_buf);

    lv_display_set_user_data(disp, NULL);
    lv_display_delete(disp);
    free(disp_ctx);

    /* 操作成功，返回ESP_OK */
    return ESP_OK;
}

/**
 * @brief 通知LVGL库帧缓冲区的绘制操作已经完成
 * 
 * 此函数用于通知LVGL库，对帧缓冲区的绘制操作已经结束。它通过调用lv_disp_flush_ready函数，
 * 并传递显示设备的驱动结构体指针来实现。在调用此函数之前，进行了一些必要的断言检查，
 * 以确保传入的显示设备指针及其驱动结构体指针均有效。
 * 
 * @param disp 显示设备的指针，用于标识和操作特定的显示设备
 */
void lvgl_port_flush_ready(lv_display_t *disp)
{
    assert(disp);
    lv_display_flush_ready(disp);
}

/*******************************************************************************
* 私有函数
*******************************************************************************/

/**
 * 获取显示上下文
 * 
 * 本函数旨在从LVGL显示设备中提取特定于显示端口的上下文信息这是在显示驱动层面上完成的，
 * 通过访问display(driver)的user_data字段，其中包含了指向lvgl_port_display_ctx_t类型结构的指针
 * 
 * @param disp 显示设备的指针，不应为NULL
 * @return 返回指向显示上下文的指针如果传递的disp为NULL或disp的driver为NULL，则断言失败
 */
static lvgl_port_display_ctx_t *lvgl_port_get_display_ctx(lv_display_t *disp)
{
    assert(disp);
    return (lvgl_port_display_ctx_t *)lv_display_get_user_data(disp);
}

/* 
 * 初始化显示设备
 * 参数:
 * - disp_cfg: 显示配置结构体指针
 * - priv_cfg: 私有配置结构体指针
 * 返回值:
 * - 成功返回 lv_disp_t 指针，失败返回 NULL
 */
static lv_display_t *lvgl_port_add_disp_priv(const lvgl_port_display_cfg_t *disp_cfg, const lvgl_port_disp_priv_cfg_t *priv_cfg)
{
    esp_err_t ret = ESP_OK;         /* 初始化返回值为成功 */
    lv_display_t *disp = NULL;      /* 初始化显示设备指针 */
    lv_color_t *buf1 = NULL;        /* 初始化第一个缓冲区指针 */
    lv_color_t *buf2 = NULL;        /* 初始化第二个缓冲区指针 */
    lv_color_t *buf3 = NULL;        /* 初始化第三个绘图缓冲区指针 */
    lv_color_t *trans_buf = NULL;   /* 初始化传输缓冲区指针 */
    uint32_t buffer_size = 0;       /* 初始化缓冲区大小 */
    uint32_t buffer_size_bytes = 0;
    SemaphoreHandle_t trans_sem = NULL;     /* 初始化传输信号量指针 */
    lv_display_render_mode_t render_mode = LV_DISPLAY_RENDER_MODE_PARTIAL;
    assert(disp_cfg != NULL);               /* 断言显示配置不为空 */
    assert(disp_cfg->panel_handle != NULL); /* 断言面板句柄不为空 */
    assert(disp_cfg->buffer_size > 0);      /* 断言缓冲区大小大于0 */
    assert(disp_cfg->hres > 0);             /* 断言水平分辨率大于0 */
    assert(disp_cfg->vres > 0);             /* 断言垂直分辨率大于0 */

    /* 分配显示上下文内存 */
    lvgl_port_display_ctx_t *disp_ctx = malloc(sizeof(lvgl_port_display_ctx_t));
    ESP_GOTO_ON_FALSE(disp_ctx, ESP_ERR_NO_MEM, err, TAG, "Not enough memory for display context allocation!"); /* 内存分配失败则跳转到错误处理 */
    memset(disp_ctx, 0, sizeof(lvgl_port_display_ctx_t));       /* 清零显示上下文 */
    disp_ctx->io_handle = disp_cfg->io_handle;                  /* 设置IO句柄 */
    disp_ctx->panel_handle = disp_cfg->panel_handle;            /* 设置面板句柄 */
    disp_ctx->control_handle = disp_cfg->control_handle;        /* 设置控制句柄 */
    disp_ctx->rotation.swap_xy = disp_cfg->rotation.swap_xy;    /* 设置旋转参数 */
    disp_ctx->rotation.mirror_x = disp_cfg->rotation.mirror_x;  /* 设置X轴镜像 */
    disp_ctx->rotation.mirror_y = disp_cfg->rotation.mirror_y;  /* 设置Y轴镜像 */
    disp_ctx->trans_size = disp_cfg->trans_size;                /* 设置传输大小 */
    disp_ctx->sw_rotate = disp_cfg->flags.sw_rotate;            /* 设置软件旋转标志 */

    buffer_size = disp_cfg->buffer_size;                        /* 设置缓冲区大小 */

    /* 如果需要避免撕裂效果 */
    if (priv_cfg && priv_cfg->avoid_tearing)
    {
        buffer_size = disp_cfg->hres * disp_cfg->vres; /* 计算全屏缓冲区大小 */
#if LVGL_PORT_HANDLE_FLUSH_READY
        if (disp_cfg->io_handle != NULL)                /* 如果是MIPI屏幕 */
        {
            ESP_GOTO_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(disp_cfg->panel_handle, 2, (void *)&buf1, (void *)&buf2), err, TAG, "Get RGB buffers failed"); /* 获取RGB缓冲区 */
        }
        else /* 如果是RGB屏幕 */
        {
            ESP_GOTO_ON_ERROR(esp_lcd_rgb_panel_get_frame_buffer(disp_cfg->panel_handle, 2, (void *)&buf1, (void *)&buf2), err, TAG, "Get RGB buffers failed"); /* 获取RGB缓冲区 */
        }
#endif
        trans_sem = xSemaphoreCreateCounting(1, 0); /* 创建计数信号量 */
        ESP_GOTO_ON_FALSE(trans_sem, ESP_ERR_NO_MEM, err, TAG, "Failed to create transport counting Semaphore"); /* 信号量创建失败则跳转到错误处理 */
        disp_ctx->trans_sem = trans_sem;            /* 设置传输信号量 */
    }
    else
    {
        uint32_t buff_caps = MALLOC_CAP_DEFAULT;    /* 初始化缓冲区分配标志 */
        if (disp_cfg->flags.buff_dma && disp_cfg->flags.buff_spiram && (0 == disp_cfg->trans_size)) /* 检查DMA和SPIRAM标志 */
        {
            ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "Alloc DMA capable buffer in SPIRAM is not supported!"); /* 不支持在SPIRAM中分配DMA缓冲区 */
        }
        else if (disp_cfg->flags.buff_dma)          /* 如果需要DMA缓冲区 */
        {
            buff_caps = MALLOC_CAP_DMA;             /* 设置DMA分配标志 */
        }
        else if (disp_cfg->flags.buff_spiram)       /* 如果需要SPIRAM缓冲区 */
        {
            buff_caps = MALLOC_CAP_SPIRAM;          /* 设置SPIRAM分配标志 */
        }

        if (disp_cfg->trans_size)                   /* 如果需要传输缓冲区 */
        {
            uint32_t trans_caps = disp_cfg->flags.buff_spiram ? MALLOC_CAP_SPIRAM : MALLOC_CAP_DEFAULT;
            if (disp_cfg->flags.buff_dma) {
                trans_caps = MALLOC_CAP_DMA;
            }
            trans_buf = heap_caps_aligned_alloc(CONFIG_LV_DRAW_BUF_ALIGN, disp_cfg->trans_size * sizeof(lv_color_t), trans_caps); /* 分配传输缓冲区 */
            ESP_GOTO_ON_FALSE(trans_buf, ESP_ERR_NO_MEM, err, TAG, "Not enough memory for buffer(transport) allocation!"); /* 缓冲区分配失败则跳转到错误处理 */
            disp_ctx->trans_buf = trans_buf;        /* 设置传输缓冲区 */

            trans_sem = xSemaphoreCreateCounting(1, 0);     /* 创建计数信号量 */
            ESP_GOTO_ON_FALSE(trans_sem, ESP_ERR_NO_MEM, err, TAG, "Failed to create transport counting Semaphore"); /* 信号量创建失败则跳转到错误处理 */
            disp_ctx->trans_sem = trans_sem;                /* 设置传输信号量 */
        }

        /* 分配LVGL绘图缓冲区 */
        buf1 = heap_caps_aligned_alloc(CONFIG_LV_DRAW_BUF_ALIGN, buffer_size * sizeof(lv_color_t), buff_caps);
        ESP_GOTO_ON_FALSE(buf1, ESP_ERR_NO_MEM, err, TAG, "Not enough memory for LVGL buffer (buf1) allocation!"); /* 缓冲区分配失败则跳转到错误处理 */
        disp_ctx->buf1 = buf1;
        
        if (disp_cfg->double_buffer) /* 如果需要双缓冲 */
        {
            buf2 = heap_caps_aligned_alloc(CONFIG_LV_DRAW_BUF_ALIGN, buffer_size * sizeof(lv_color_t), buff_caps); /* 分配第二个缓冲区 */
            ESP_GOTO_ON_FALSE(buf2, ESP_ERR_NO_MEM, err, TAG, "Not enough memory for LVGL buffer (buf2) allocation!"); /* 缓冲区分配失败则跳转到错误处理 */
            disp_ctx->buf2 = buf2;
        }

        if (disp_cfg->flags.triple_buffer) /* 如果需要三缓冲 */
        {
            ESP_GOTO_ON_FALSE(disp_cfg->double_buffer, ESP_ERR_INVALID_ARG, err, TAG, "Triple buffer requires double_buffer enabled");
            buf3 = heap_caps_aligned_alloc(CONFIG_LV_DRAW_BUF_ALIGN, buffer_size * sizeof(lv_color_t), buff_caps); /* 分配第三个缓冲区 */
            ESP_GOTO_ON_FALSE(buf3, ESP_ERR_NO_MEM, err, TAG, "Not enough memory for LVGL buffer (buf3) allocation!");
            disp_ctx->buf3 = buf3;
        }
    }

    ESP_GOTO_ON_FALSE(!disp_cfg->monochrome, ESP_ERR_NOT_SUPPORTED, err, TAG, "Monochrome mode is not supported on LVGL9 port");

    if (disp_cfg->flags.direct_mode) {
        ESP_GOTO_ON_FALSE((disp_cfg->hres * disp_cfg->vres == buffer_size), ESP_ERR_INVALID_ARG, err, TAG, "Direct mode must use full buffer");
        render_mode = LV_DISPLAY_RENDER_MODE_DIRECT;
    } else if (disp_cfg->flags.full_refresh) {
        ESP_GOTO_ON_FALSE((disp_cfg->hres * disp_cfg->vres == buffer_size), ESP_ERR_INVALID_ARG, err, TAG, "Full refresh must use full buffer");
        render_mode = LV_DISPLAY_RENDER_MODE_FULL;
    }

    disp = lv_display_create(disp_cfg->hres, disp_cfg->vres);
    ESP_GOTO_ON_FALSE(disp, ESP_ERR_NO_MEM, err, TAG, "Create LVGL display failed");
    disp_ctx->disp = disp;
    lv_display_set_user_data(disp, disp_ctx);
    lv_display_set_color_format(disp, disp_cfg->color_format);

    buffer_size_bytes = buffer_size * lv_color_format_get_size(disp_cfg->color_format);
    lv_display_set_buffers(disp, buf1, buf2, buffer_size_bytes, render_mode);
    if (buf3 != NULL) {
        const uint32_t buf3_lines = buffer_size / disp_cfg->hres;
        ESP_GOTO_ON_FALSE((buf3_lines > 0) && (buf3_lines * disp_cfg->hres == buffer_size), ESP_ERR_INVALID_ARG, err, TAG, "Third buffer requires line-aligned partial buffer size");
        ESP_GOTO_ON_FALSE(lv_draw_buf_init(&disp_ctx->draw_buf_3, disp_cfg->hres, buf3_lines, disp_cfg->color_format, LV_STRIDE_AUTO, buf3, buffer_size_bytes) == LV_RESULT_OK,
                          ESP_FAIL, err, TAG, "Init third draw buffer failed");
        lv_display_set_3rd_draw_buffer(disp, &disp_ctx->draw_buf_3);
    }
    lv_display_set_flush_cb(disp, lvgl_port_flush_callback);
    lv_display_add_event_cb(disp, lvgl_port_disp_resolution_evt_cb, LV_EVENT_RESOLUTION_CHANGED, NULL);

    lvgl_port_update_callback(disp);

err:
    if (ret != ESP_OK)  /* 如果发生错误 */
    {
        if (buf1)       /* 释放第一个缓冲区 */
        {
            free(buf1);
        }
        if (buf2)       /* 释放第二个缓冲区 */
        {
            free(buf2);
        }
        if (buf3)       /* 释放第三个缓冲区 */
        {
            free(buf3);
        }
        if (trans_buf)  /* 释放传输缓冲区 */
        {
            free(trans_buf);
        }
        if (trans_sem) /* 删除传输信号量 */
        {
            vSemaphoreDelete(trans_sem);
        }
        if (disp_ctx)   /* 释放显示上下文 */
        {
            free(disp_ctx);
        }
        if (disp) {
            lv_display_delete(disp);
        }
    }

    return disp;        /* 返回显示设备指针 */
}


#if LVGL_PORT_HANDLE_FLUSH_READY
/**
 * @brief LVGL显示端口刷新完成的回调函数
 * 
 * 该函数在LVGL图形库的显示端口完成一帧数据的刷新后调用，主要用于通知系统或用户刷新操作已经完成。
 * 它负责在中断服务例程（ISR）环境中处理显示刷新完成的事件，通过释放信号量来通知等待的任务可以继续执行。
 * 
 * @param panel_io 显示面板的IO句柄，用于识别和操作特定的显示设备
 * @param edata 事件数据指针，包含与事件相关的附加信息
 * @param user_ctx 用户上下文，通常是由用户在初始化时设置的私有数据指针
 * @return bool 返回值指示是否需要进一步处理该事件，false表示无需进一步处理
 */
IRAM_ATTR static bool lvgl_port_flush_io_ready_callback(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    /* 初始化任务唤醒标志为false */
    BaseType_t taskAwake = pdFALSE;

    lv_display_t *disp = (lv_display_t *)user_ctx;
    assert(disp != NULL);
    lvgl_port_display_ctx_t *disp_ctx = lvgl_port_get_display_ctx(disp);
    /* 断言显示端口上下文非空，进一步确保操作的安全性 */
    assert(disp_ctx != NULL);
    /* 通知LVGL显示驱动一帧数据的刷新已完成 */
    lv_display_flush_ready(disp);

    /* 如果有传输的数据且信号量存在，则在中断中释放信号量 */
    if (disp_ctx->trans_size && disp_ctx->trans_sem)
    {
        xSemaphoreGiveFromISR(disp_ctx->trans_sem, &taskAwake);
    }

    /* 返回false，表示不需要进一步处理该事件 */
    return false;
}

/**
 * @brief DPI面板准备就绪回调函数
 * 
 * 该函数在DPI面板准备就绪时被调用，主要用于通知LVGL图形库的显示驱动
 * 它是IRAM属性的，表示该函数代码驻留在IRAM中，以加快执行速度
 * 
 * @param panel_io 面板IO句柄，用于操作LCD面板
 * @param edata 面板事件数据指针，包含事件相关信息
 * @param user_ctx 用户上下文，通常是指向显示驱动结构的指针
 * @return bool 返回false，表示不在此回调中处理事件
 */
IRAM_ATTR static bool lvgl_port_flush_dpi_panel_ready_callback(esp_lcd_panel_handle_t panel_io, esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx)
{
    /* 初始化任务唤醒标志为false */
    BaseType_t taskAwake = pdFALSE;

    /* 将用户上下文转换为显示驱动结构体指针，并进行非空断言 */
    lv_display_t *disp = (lv_display_t *)user_ctx;
    assert(disp != NULL);

    /* 获取显示驱动的用户数据，即显示上下文，并进行非空断言 */
    lvgl_port_display_ctx_t *disp_ctx = lvgl_port_get_display_ctx(disp);
    assert(disp_ctx != NULL);

    /* 通知LVGL显示驱动，帧缓冲区已准备好 */
    lv_display_flush_ready(disp);

    /* 如果有传输数据且传输信号量存在，则释放信号量以通知其他任务 */
    if (disp_ctx->trans_size && disp_ctx->trans_sem)
    {
        xSemaphoreGiveFromISR(disp_ctx->trans_sem, &taskAwake);
    }

    /* 返回false，表示不在此回调中处理事件 */
    return false;
}

/**
 * @brief LVGL显示接口VSYNC就绪回调函数
 * 
 * 该函数是一个回调函数，用于在显示面板的VSYNC（垂直同步）信号就绪时通知LVGL框架。
 * 它主要通过释放一个信号量来同步LVGL的渲染过程与显示面板的刷新率，确保渲染性能和显示效果的稳定性。
 * 
 * @param panel_io 显示面板的句柄，用于识别和操作特定的显示设备
 * @param edata 包含 DPI（直接并行接口）面板事件的数据指针，此处未使用
 * @param user_ctx 用户上下文，通常用于传递额外的配置或数据，在本函数中被转换为LVGL显示驱动结构体指针
 * @return bool 指示是否需要在中断服务例程（ISR）中执行上下文切换
 * 
 * 注意：此函数标记为IRAM_ATTR，表示它被优化存储在指令RAM中，这对于快速中断处理是必要的。
 */
IRAM_ATTR static bool lvgl_port_flush_dpi_vsync_ready_callback(esp_lcd_panel_handle_t panel_io, esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx)
{
    /* 初始化变量，用于指示是否需要在ISR中执行上下文切换 */
    BaseType_t need_yield = pdFALSE;

    /* 将用户上下文转换为LVGL显示驱动结构体指针 */
    lv_display_t *disp = (lv_display_t *)user_ctx;
    assert(disp != NULL);
    lvgl_port_display_ctx_t *disp_ctx = lvgl_port_get_display_ctx(disp);
    /* 断言确保显示上下文不为空 */
    assert(disp_ctx != NULL);

    /* 如果传输信号量存在，则释放信号量以通知等待的线程VSYNC事件已发生 */
    if (disp_ctx->trans_sem)
    {
        xSemaphoreGiveFromISR(disp_ctx->trans_sem, &need_yield);
    }

    /* 返回是否需要在ISR中执行上下文切换的指示 */
    return (need_yield == pdTRUE);
}

/**
 * @brief LVGL显示驱动的RGB同步准备回调函数
 * 
 * 本函数用于在LVGL的RGB显示驱动中，当一帧数据传输完成后调用，
 * 以通知LVGL可以进行下一次数据传输。这在实现高效的图形更新和
 * 屏幕刷新中起到关键作用。
 * 
 * @param panel_io LCD面板句柄，用于操作LCD面板
 * @param edata RGB面板事件数据，包含事件相关信息
 * @param user_ctx 用户上下文，通常为LVGL显示驱动结构体指针
 * @return bool 指示是否需要在中断服务例程中切换任务
 * 
 * 注意：此函数通常由LVGL框架内部调用，用于处理显示数据的传输同步。
 */
IRAM_ATTR static bool lvgl_port_flush_rgb_vsync_ready_callback(esp_lcd_panel_handle_t panel_io, const esp_lcd_rgb_panel_event_data_t *edata, void *user_ctx)
{
    /* 初始化任务切换标志为FALSE */
    BaseType_t need_yield = pdFALSE;

    /* 将用户上下文转换为LVGL显示驱动结构体指针 */
    lv_display_t *disp = (lv_display_t *)user_ctx;
    assert(disp != NULL);
    lvgl_port_display_ctx_t *disp_ctx = lvgl_port_get_display_ctx(disp);
    /* 断言显示驱动的自定义上下文不为空 */
    assert(disp_ctx != NULL);

    /* 如果传输信号量存在，则释放信号量以通知LVGL可以进行下一次数据传输 */
    if (disp_ctx->trans_sem)
    {
        xSemaphoreGiveFromISR(disp_ctx->trans_sem, &need_yield);
    }

    /* 返回是否需要在中断服务例程中切换任务 */
    return (need_yield == pdTRUE);
}

#endif

/**
 * @brief       用于刷新显示屏的回调函数，将屏幕的图像数据从内存刷新到显示硬件
 * @param       drv          显示驱动的结构体指针，包含显示器的配置信息，用于执行刷新操作
 * @param       area         刷新区域的坐标和尺寸，表示需要更新的显示区域
 * @param       color_map    显示区域的像素数据，包含要写入显示器的颜色信息
 * @retval      无
 */
static void lvgl_port_flush_callback(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    assert(disp != NULL);
    lvgl_port_display_ctx_t *disp_ctx = lvgl_port_get_display_ctx(disp);
    /* 再次检查获取的上下文是否为空 */
    assert(disp_ctx != NULL);
    lv_display_render_mode_t render_mode = lv_display_get_render_mode(disp);
    bool swap_bytes = (lv_display_get_color_format(disp) == LV_COLOR_FORMAT_RGB565_SWAPPED);

    /* 定义绘制区域的坐标变量 */
    int x_draw_start;
    int x_draw_end;
    int y_draw_start;
    int y_draw_end;

    /* 定义临时变量用于计算 */
    int y_start_tmp;
    int y_end_tmp;

    /* 定义传输计数和行数变量 */
    int trans_count;
    int trans_line;
    int max_line;

    /* 获取绘制区域的起始和结束坐标 */
    const int x_start = area->x1;
    const int x_end = area->x2;
    const int y_start = area->y1;
    const int y_end = area->y2;
    /* 计算绘制区域的宽度和高度 */
    const int width = x_end - x_start + 1;
    const int height = y_end - y_start + 1;

    /* 获取颜色映射的起始地址 */
    uint8_t *from = px_map;
    uint8_t *to = NULL;
    uint32_t px_size = lv_color_format_get_size(lv_display_get_color_format(disp));

    if (disp_ctx->sw_rotate)
    {
        const lv_color_format_t cf = lv_display_get_color_format(disp);
        const uint32_t src_stride = lv_draw_buf_width_to_stride(width, cf);
        const uint32_t dest_stride = lv_draw_buf_width_to_stride(height, cf);
        const int phys_hres = lv_display_get_vertical_resolution(disp);
        lv_area_t rotated_area = {
            .x1 = phys_hres - 1 - y_end,
            .y1 = x_start,
            .x2 = phys_hres - 1 - y_start,
            .y2 = x_end,
        };

        assert(disp_ctx->trans_buf != NULL);
        lv_draw_sw_rotate(px_map, disp_ctx->trans_buf, width, height, src_stride, dest_stride, LV_DISPLAY_ROTATION_90, cf);
        esp_lcd_panel_draw_bitmap(disp_ctx->panel_handle,
                                  rotated_area.x1,
                                  rotated_area.y1,
                                  rotated_area.x2 + 1,
                                  rotated_area.y2 + 1,
                                  disp_ctx->trans_buf);
        return;
    }

    /* 如果传输大小为0，则直接绘制 */
    if (disp_ctx->trans_size == 0)
    {
        /* 如果显示类型为RGB或DSI，并且支持直接模式或全刷新，则进行绘制 */
        if ((disp_ctx->disp_type == LVGL_PORT_DISP_TYPE_RGB || disp_ctx->disp_type == LVGL_PORT_DISP_TYPE_DSI) &&
            (render_mode == LV_DISPLAY_RENDER_MODE_DIRECT || render_mode == LV_DISPLAY_RENDER_MODE_FULL))
        {
            /* 如果是最后一个绘制区域，则进行绘制 */
            if (lv_display_flush_is_last(disp))
            {
                if (swap_bytes) {
                    lv_draw_sw_rgb565_swap(px_map, width * height);
                }
                /* 如果接口是I80或SPI，此步骤不能用于绘图 */
                esp_lcd_panel_draw_bitmap(disp_ctx->panel_handle, x_start, y_start, x_end + 1, y_end + 1, px_map);
                /* RGB直刷需要等待VSYNC；DSI在回调里通知flush ready */
                if (disp_ctx->disp_type == LVGL_PORT_DISP_TYPE_RGB && disp_ctx->trans_sem) {
                    xSemaphoreTake(disp_ctx->trans_sem, 0);
                    xSemaphoreTake(disp_ctx->trans_sem, portMAX_DELAY);
                }
            }
        }
        else
        {
            if (swap_bytes) {
                lv_draw_sw_rgb565_swap(px_map, width * height);
            }
            esp_lcd_panel_draw_bitmap(disp_ctx->panel_handle, x_start, y_start, x_end + 1, y_end + 1, px_map);
        }

        if (disp_ctx->disp_type == LVGL_PORT_DISP_TYPE_RGB ||
            (disp_ctx->disp_type == LVGL_PORT_DISP_TYPE_DSI &&
             !((render_mode == LV_DISPLAY_RENDER_MODE_DIRECT || render_mode == LV_DISPLAY_RENDER_MODE_FULL))))
        {
            lv_display_flush_ready(disp);
        }
    }
    else
    {
        /* 如果传输大小不为0，计算每次传输的行数和总传输次数 */
        y_start_tmp = y_start;
        max_line = ((disp_ctx->trans_size / width) > height) ? (height) : (disp_ctx->trans_size / width);
        trans_count = height / max_line + (height % max_line ? (1) : (0));

        /* 根据计算结果分多次传输绘制 */
        for (int i = 0; i < trans_count; i++)
        {
            /* 计算当前传输的行数和结束坐标 */
            trans_line = (y_end - y_start_tmp + 1) > max_line ? max_line : (y_end - y_start_tmp + 1);
            y_end_tmp = (y_end - y_start_tmp + 1) > max_line ? (y_start_tmp + max_line - 1) : y_end;

            /* 将颜色数据从源复制到目标缓冲区 */
            to = (uint8_t *)disp_ctx->trans_buf;
            for (int y = 0; y < trans_line; y++)
            {
                memcpy(to + y * width * px_size, from + y * width * px_size, width * px_size);
            }

            /* 设置绘制区域的坐标并进行绘制 */
            x_draw_start = x_start;
            x_draw_end = x_end;
            y_draw_start = y_start_tmp;
            y_draw_end = y_end_tmp;
            if (swap_bytes) {
                lv_draw_sw_rgb565_swap(to, width * trans_line);
            }
            esp_lcd_panel_draw_bitmap(disp_ctx->panel_handle, x_draw_start, y_draw_start, x_draw_end + 1, y_draw_end + 1, to);

            /* 更新源地址和下一次传输的起始坐标 */
            from += max_line * width * px_size;
            y_start_tmp += max_line;
            xSemaphoreTake(disp_ctx->trans_sem, portMAX_DELAY);
        }
    }
}

/**
 * @brief       旋转屏幕
 * @param       drv: 显示驱动结构体指针，提供关于当前显示器的一些配置信息
 * @retval      无
 */
static void lvgl_port_update_callback(lv_display_t *disp)
{
    assert(disp);
    lvgl_port_display_ctx_t *disp_ctx = lvgl_port_get_display_ctx(disp);
    assert(disp_ctx != NULL);
    if (disp_ctx->sw_rotate)
    {
        return;
    }
    esp_lcd_panel_handle_t control_handle = (disp_ctx->control_handle ? disp_ctx->control_handle : disp_ctx->panel_handle);
    lv_display_rotation_t rotation = lv_display_get_rotation(disp);

    /* 根据显示旋转值，旋转LCD显示 */
    switch (rotation)
    {
        case LV_DISPLAY_ROTATION_0:
            /* 不旋转 */
            esp_lcd_panel_swap_xy(control_handle, disp_ctx->rotation.swap_xy);
            esp_lcd_panel_mirror(control_handle, disp_ctx->rotation.mirror_x, disp_ctx->rotation.mirror_y);
            break;
        case LV_DISPLAY_ROTATION_90:
            /* 旋转90度 */
            esp_lcd_panel_swap_xy(control_handle, !disp_ctx->rotation.swap_xy);
            if (disp_ctx->rotation.swap_xy)
            {
                /* 如果swap_xy为真，则旋转180度 */
                esp_lcd_panel_mirror(control_handle, !disp_ctx->rotation.mirror_x, disp_ctx->rotation.mirror_y);
            }
            else
            {
                /* 否则旋转90度 */
                esp_lcd_panel_mirror(control_handle, disp_ctx->rotation.mirror_x, !disp_ctx->rotation.mirror_y);
            }
            break;
        case LV_DISPLAY_ROTATION_180:
            /* 旋转180度 */
            esp_lcd_panel_swap_xy(control_handle, disp_ctx->rotation.swap_xy);
            esp_lcd_panel_mirror(control_handle, !disp_ctx->rotation.mirror_x, !disp_ctx->rotation.mirror_y);
            break;
        case LV_DISPLAY_ROTATION_270:
            /* 旋转270度 */
            esp_lcd_panel_swap_xy(control_handle, !disp_ctx->rotation.swap_xy);

            if (disp_ctx->rotation.swap_xy)
            {
                /* 如果swap_xy为真，则旋转270度 */
                esp_lcd_panel_mirror(control_handle, disp_ctx->rotation.mirror_x, !disp_ctx->rotation.mirror_y);
            }
            else
            {
                /* 否则旋转90度 */
                esp_lcd_panel_mirror(control_handle, !disp_ctx->rotation.mirror_x, disp_ctx->rotation.mirror_y);
            }

            break;
    }
}

static void lvgl_port_disp_resolution_evt_cb(lv_event_t *e)
{
    lv_display_t *disp = lv_event_get_target(e);
    lvgl_port_update_callback(disp);
}
