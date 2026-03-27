#ifndef __ADC_SAMPLER_H
#define __ADC_SAMPLER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ADC_SAMPLER_CHANNELS 8

bool adc_sampler_init(void);
void adc_sampler_deinit(void);
bool adc_sampler_read_mv(uint8_t channel, float *mv_out);

#ifdef __cplusplus
}
#endif

#endif
