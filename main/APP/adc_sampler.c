#include "adc_sampler.h"

#include "esp_adc/adc_oneshot.h"
#include "hal/adc_types.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "adc_sampler";
static adc_oneshot_unit_handle_t s_adc_handle;
static bool s_ready;
static const adc_channel_t s_channels[ADC_SAMPLER_CHANNELS] = {
    ADC_CHANNEL_0,
    ADC_CHANNEL_1,
    ADC_CHANNEL_2,
    ADC_CHANNEL_3,
    ADC_CHANNEL_4,
    ADC_CHANNEL_5,
    ADC_CHANNEL_6,
    ADC_CHANNEL_7,
};

bool adc_sampler_init(void)
{
    esp_err_t ret;

    if (s_ready) {
        return true;
    }

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ret = adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "adc_oneshot_new_unit failed: %s", esp_err_to_name(ret));
        return false;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    for (int i = 0; i < ADC_SAMPLER_CHANNELS; i++) {
        ret = adc_oneshot_config_channel(s_adc_handle, s_channels[i], &chan_cfg);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "config channel %d failed: %s", i, esp_err_to_name(ret));
            adc_oneshot_del_unit(s_adc_handle);
            s_adc_handle = NULL;
            return false;
        }
    }

    s_ready = true;
    return true;
}

void adc_sampler_deinit(void)
{
    if (!s_ready) {
        return;
    }
    adc_oneshot_del_unit(s_adc_handle);
    s_adc_handle = NULL;
    s_ready = false;
}

bool adc_sampler_read_mv(uint8_t channel, float *mv_out)
{
    int raw;
    esp_err_t ret;

    if (!s_ready || channel >= ADC_SAMPLER_CHANNELS || mv_out == NULL) {
        return false;
    }

    ret = adc_oneshot_read(s_adc_handle, s_channels[channel], &raw);
    if (ret != ESP_OK) {
        return false;
    }                                                                                                                                                                                                               

    /* Fallback conversion without calibration. */
    *mv_out = ((float)raw * 3300.0f) / 4095.0f;
    return true;
}
