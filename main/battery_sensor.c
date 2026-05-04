#include "battery_sensor.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "supabase_client.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "battery";

#ifndef BATTERY_STATION_ID
#define BATTERY_STATION_ID "solar-hub-01"
#endif

#define BATTERY_ADC_UNIT       ADC_UNIT_1
#define BATTERY_ADC_CHANNEL    ADC_CHANNEL_4   /* GPIO 32 */
#define BATTERY_ADC_BITWIDTH   ADC_BITWIDTH_12
#define BATTERY_ADC_ATTEN      ADC_ATTEN_DB_12 /* full 0-3.3V range */

#if BATTERY_SENSOR_ENABLED

static bool s_initialized;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t          s_cali;
static bool                       s_have_cali;

#if BATTERY_SENSOR_SUPABASE_SYNC_ENABLED
static bool s_sync_task_started;
#endif

static float battery_percent_from_voltage(float v)
{
    if (v >= BATTERY_VOLTAGE_FULL_V) {
        return 100.0f;
    }
    if (v <= BATTERY_VOLTAGE_EMPTY_V) {
        return 0.0f;
    }
    return (v - BATTERY_VOLTAGE_EMPTY_V) /
           (BATTERY_VOLTAGE_FULL_V - BATTERY_VOLTAGE_EMPTY_V) * 100.0f;
}

static esp_err_t battery_sample_mv(int *out_mv)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    int32_t raw_sum = 0;
    int     samples = 0;
    for (int i = 0; i < BATTERY_SENSOR_SAMPLES; i++) {
        int raw = 0;
        esp_err_t err = adc_oneshot_read(s_adc, BATTERY_ADC_CHANNEL, &raw);
        if (err != ESP_OK) {
            return err;
        }
        raw_sum += raw;
        samples++;
    }
    int raw_avg = (int)(raw_sum / samples);

    int mv = 0;
    if (s_have_cali) {
        esp_err_t err = adc_cali_raw_to_voltage(s_cali, raw_avg, &mv);
        if (err != ESP_OK) {
            return err;
        }
    } else {
        /* Fall-back: linear scaling assuming 0-3.3 V on a 12-bit ADC.
         * Less accurate, but still gives a usable reading. */
        mv = (raw_avg * 3300) / 4095;
    }
    *out_mv = mv;
    return ESP_OK;
}

#if BATTERY_SENSOR_SUPABASE_SYNC_ENABLED
static esp_err_t battery_sync_to_supabase(const battery_reading_t *r)
{
    if (r == NULL || !r->valid) {
        return ESP_ERR_INVALID_ARG;
    }

    char payload[192];
    snprintf(payload, sizeof(payload),
             "{\"station_id\":\"" BATTERY_STATION_ID
             "\",\"voltage_v\":%.2f,\"percent\":%.0f,\"raw_mv\":%d}",
             (double)r->voltage_v, (double)r->percent, r->raw_mv);

    esp_err_t err = supabase_post_upsert(
        "/rest/v1/battery_state?on_conflict=station_id",
        payload);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Supabase sync failed: %s", esp_err_to_name(err));
    }
    return err;
}

static void battery_task(void *pvParameters)
{
    (void)pvParameters;

    supabase_init();

    const TickType_t delay_ticks =
        pdMS_TO_TICKS(BATTERY_SENSOR_SYNC_INTERVAL_MS > 0
                          ? BATTERY_SENSOR_SYNC_INTERVAL_MS
                          : 10000);

    ESP_LOGI(TAG,
             "battery sync task started; interval=%d ms divider_ratio=%.2f curve=%.1fV..%.1fV",
             BATTERY_SENSOR_SYNC_INTERVAL_MS,
             (double)BATTERY_VOLTAGE_DIVIDER_RATIO,
             (double)BATTERY_VOLTAGE_EMPTY_V,
             (double)BATTERY_VOLTAGE_FULL_V);

    while (true) {
        battery_reading_t r;
        esp_err_t err = battery_sensor_read(&r);
        if (err == ESP_OK) {
            ESP_LOGI(TAG,
                     "V=%.2f V  SoC=%.0f%%  (raw=%d mV)",
                     (double)r.voltage_v, (double)r.percent, r.raw_mv);
            battery_sync_to_supabase(&r);
        } else {
            ESP_LOGW(TAG, "battery read failed: %s", esp_err_to_name(err));
        }
        vTaskDelay(delay_ticks);
    }
}
#endif /* BATTERY_SENSOR_SUPABASE_SYNC_ENABLED */

#endif /* BATTERY_SENSOR_ENABLED */

esp_err_t battery_sensor_init(void)
{
#if BATTERY_SENSOR_ENABLED
    if (s_initialized) {
        return ESP_OK;
    }

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = BATTERY_ADC_UNIT,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed: %s", esp_err_to_name(err));
        return err;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = BATTERY_ADC_BITWIDTH,
        .atten    = BATTERY_ADC_ATTEN,
    };
    err = adc_oneshot_config_channel(s_adc, BATTERY_ADC_CHANNEL, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_config_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Pick the calibration scheme available on this target.
     * Classic ESP32 has line fitting; S2/S3/C3 have curve fitting. */
    s_have_cali = false;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t curve_cfg = {
        .unit_id  = BATTERY_ADC_UNIT,
        .chan     = BATTERY_ADC_CHANNEL,
        .atten    = BATTERY_ADC_ATTEN,
        .bitwidth = BATTERY_ADC_BITWIDTH,
    };
    err = adc_cali_create_scheme_curve_fitting(&curve_cfg, &s_cali);
    s_have_cali = (err == ESP_OK);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t line_cfg = {
        .unit_id  = BATTERY_ADC_UNIT,
        .atten    = BATTERY_ADC_ATTEN,
        .bitwidth = BATTERY_ADC_BITWIDTH,
    };
    err = adc_cali_create_scheme_line_fitting(&line_cfg, &s_cali);
    s_have_cali = (err == ESP_OK);
#endif
    if (!s_have_cali) {
        ESP_LOGW(TAG, "ADC calibration unavailable; using linear 0-3.3V fallback");
    }

    s_initialized = true;
    ESP_LOGI(TAG,
             "battery sensor ready: GPIO%d, divider=%.2f, full=%.1fV empty=%.1fV",
             BATTERY_SENSOR_GPIO,
             (double)BATTERY_VOLTAGE_DIVIDER_RATIO,
             (double)BATTERY_VOLTAGE_FULL_V,
             (double)BATTERY_VOLTAGE_EMPTY_V);
    return ESP_OK;
#else
    ESP_LOGI(TAG, "battery sensor disabled at compile time");
    return ESP_OK;
#endif
}

esp_err_t battery_sensor_read(battery_reading_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

#if BATTERY_SENSOR_ENABLED
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    int adc_mv = 0;
    esp_err_t err = battery_sample_mv(&adc_mv);
    if (err != ESP_OK) {
        return err;
    }

    out->raw_mv    = adc_mv;
    out->voltage_v = (adc_mv / 1000.0f) * BATTERY_VOLTAGE_DIVIDER_RATIO;
    out->percent   = battery_percent_from_voltage(out->voltage_v);
    out->valid     = true;
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t battery_sensor_start(void)
{
#if BATTERY_SENSOR_ENABLED
    esp_err_t err = battery_sensor_init();
    if (err != ESP_OK) {
        return err;
    }

#if BATTERY_SENSOR_SUPABASE_SYNC_ENABLED
    if (s_sync_task_started) {
        return ESP_OK;
    }
    /* 8 KB stack — Supabase HTTPS upsert (TLS handshake + JSON) needs more
     * than the 4 KB the task originally had, which caused a stack overflow
     * panic shortly after the captive portal /confirm flow. */
    BaseType_t ok = xTaskCreate(battery_task, "battery_task",
                                8192, NULL, 4, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to start battery task");
        return ESP_ERR_NO_MEM;
    }
    s_sync_task_started = true;
#else
    ESP_LOGI(TAG, "battery Supabase sync compiled out");
#endif
    return ESP_OK;
#else
    return ESP_OK;
#endif
}
