#include "battery_sensor.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rfid_reader.h"
#include "router_globals.h"
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
#define BATTERY_ADC_ATTEN      ADC_ATTEN_DB_11 /* full 0-3.3V range on ESP-IDF 5.1 */

#if BATTERY_SENSOR_ENABLED

static bool s_initialized;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t          s_cali;
static bool                       s_have_cali;

/* State-machine bookkeeping. Owned by battery_task; no concurrent access. */
static battery_state_t s_state = BATTERY_STATE_NORMAL;
static int s_dbc_to_warning;     /* Normal  -> Warning  (falling V) */
static int s_dbc_to_critical;    /* Warning -> Critical (falling V) */
static int s_dbc_to_charging;    /* Wake Up / Critical -> Charging On (rising V) */
static int s_dbc_to_normal;      /* Charging On / Warning -> Normal (rising V) */

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

    /* Dashboard reads `battery_percent`; the extra fields are station-wide
     * battery diagnostics. Keep the matching station_state columns in
     * Supabase, or PostgREST will reject the upsert with HTTP 400. */
    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"station_id\":\"" BATTERY_STATION_ID
             "\",\"battery_percent\":%.0f"
             ",\"battery_voltage_v\":%.2f"
             ",\"battery_raw_mv\":%d"
             ",\"battery_state\":\"%s\"}",
             (double)r->percent,
             (double)r->voltage_v,
             r->raw_mv,
             battery_state_name(r->state));

    esp_err_t err = supabase_post_upsert(
        "/rest/v1/station_state?on_conflict=station_id",
        payload);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Supabase sync failed: %s", esp_err_to_name(err));
    }
    return err;
}
#endif

/* Apply the side-effects associated with a state.
 *   Normal       : user AP on,  ports on
 *   Warning      : user AP off, ports on
 *   Critical     : user AP off, ports off
 *   Wake Up      : user AP off, ports off (post-shutdown recovery)
 *   Charging On  : user AP off, ports on  (recovering from Critical/Wake Up)
 *
 * Telemetry STA Wi-Fi stays up in every non-Normal state so the dashboard
 * can keep showing what's going on.
 *
 * In dev mode (BATTERY_SENSOR_ENFORCE_THRESHOLDS=0), the side-effects are
 * skipped entirely so a 12 V bench supply doesn't drop the AP. Telemetry,
 * state-machine evaluation, and Supabase upserts continue to run. */
static void apply_actions_for_state(battery_state_t state)
{
#if BATTERY_SENSOR_ENFORCE_THRESHOLDS
    bool ap_on    = (state == BATTERY_STATE_NORMAL);
    bool ports_on = (state == BATTERY_STATE_NORMAL ||
                     state == BATTERY_STATE_WARNING ||
                     state == BATTERY_STATE_CHARGING_ON);

    set_user_ap_enabled(ap_on);
    rfid_reader_set_ports_allowed(ports_on);
#else
    ESP_LOGI(TAG,
             "dev mode (BATTERY_SENSOR_ENFORCE_THRESHOLDS=0): "
             "skipping AP / RFID action for state=%s",
             battery_state_name(state));
#endif
}

static void transition_to(battery_state_t new_state, float voltage_v)
{
    if (new_state == s_state) {
        return;
    }
    ESP_LOGI(TAG, "battery state: %s -> %s (V=%.2f)",
             battery_state_name(s_state),
             battery_state_name(new_state),
             (double)voltage_v);
    s_state = new_state;
    s_dbc_to_warning = 0;
    s_dbc_to_critical = 0;
    s_dbc_to_charging = 0;
    s_dbc_to_normal = 0;
    apply_actions_for_state(new_state);
}

/* Per-sample evaluation: count consecutive samples that meet a transition
 * predicate. Reset the counter the moment the predicate fails — that's the
 * "30 second sustained" requirement. */
static void evaluate_transitions(float v)
{
    bool below_warning  = (v < BATTERY_V_WARNING_FALL);
    bool below_critical = (v < BATTERY_V_CRITICAL_FALL);
    bool above_charging = (v > BATTERY_V_CHARGING_RISE);
    bool above_normal   = (v > BATTERY_V_NORMAL_RISE);

    switch (s_state) {
        case BATTERY_STATE_NORMAL:
            if (below_warning) {
                if (++s_dbc_to_warning >= BATTERY_DEBOUNCE_SAMPLES) {
                    transition_to(BATTERY_STATE_WARNING, v);
                }
            } else {
                s_dbc_to_warning = 0;
            }
            break;

        case BATTERY_STATE_WARNING:
            if (below_critical) {
                if (++s_dbc_to_critical >= BATTERY_DEBOUNCE_SAMPLES) {
                    transition_to(BATTERY_STATE_CRITICAL, v);
                }
            } else {
                s_dbc_to_critical = 0;
            }
            if (above_normal) {
                if (++s_dbc_to_normal >= BATTERY_DEBOUNCE_SAMPLES) {
                    transition_to(BATTERY_STATE_NORMAL, v);
                }
            } else {
                s_dbc_to_normal = 0;
            }
            break;

        case BATTERY_STATE_CRITICAL:
            /* Falling further triggers hardware MPPT shutdown — ESP loses
             * power and we never see it. Only transition we care about
             * here is recovery to Charging On. */
            if (above_charging) {
                if (++s_dbc_to_charging >= BATTERY_DEBOUNCE_SAMPLES) {
                    transition_to(BATTERY_STATE_CHARGING_ON, v);
                }
            } else {
                s_dbc_to_charging = 0;
            }
            break;

        case BATTERY_STATE_WAKE_UP:
            /* Just-booted post-shutdown state. Move forward only. */
            if (above_charging) {
                if (++s_dbc_to_charging >= BATTERY_DEBOUNCE_SAMPLES) {
                    transition_to(BATTERY_STATE_CHARGING_ON, v);
                }
            } else {
                s_dbc_to_charging = 0;
            }
            break;

        case BATTERY_STATE_CHARGING_ON:
            if (above_normal) {
                if (++s_dbc_to_normal >= BATTERY_DEBOUNCE_SAMPLES) {
                    transition_to(BATTERY_STATE_NORMAL, v);
                }
            } else {
                s_dbc_to_normal = 0;
            }
            break;
    }
}

static battery_state_t determine_boot_state(float voltage)
{
    if (voltage > BATTERY_V_NORMAL_RISE) {
        return BATTERY_STATE_NORMAL;
    } else if (voltage > BATTERY_V_CHARGING_RISE) {
        return BATTERY_STATE_CHARGING_ON;
    } else if (voltage > BATTERY_V_WAKEUP_BOOT_LOW) {
        return BATTERY_STATE_WAKE_UP;
    } else if (voltage > BATTERY_V_CRITICAL_FALL) {
        return BATTERY_STATE_WARNING;
    } else {
        return BATTERY_STATE_CRITICAL;
    }
}

static void battery_task(void *pvParameters)
{
    (void)pvParameters;

    supabase_init();

    /* Boot averaging: take a few samples before deciding the initial
     * state so we don't react to one noisy power-on reading. */
    float boot_sum = 0.0f;
    int   boot_n   = 0;
    for (int i = 0; i < BATTERY_BOOT_SAMPLES; i++) {
        battery_reading_t r;
        if (battery_sensor_read(&r) == ESP_OK && r.valid) {
            boot_sum += r.voltage_v;
            boot_n++;
        }
        vTaskDelay(pdMS_TO_TICKS(BATTERY_BOOT_SAMPLE_DELAY_MS));
    }
    float boot_v = boot_n > 0 ? (boot_sum / boot_n) : 0.0f;
    s_state = determine_boot_state(boot_v);
    apply_actions_for_state(s_state);
    ESP_LOGI(TAG,
             "boot battery state: %s (avg V=%.2f over %d samples)",
             battery_state_name(s_state), (double)boot_v, boot_n);

    const TickType_t delay_ticks =
        pdMS_TO_TICKS(BATTERY_SENSOR_SYNC_INTERVAL_MS > 0
                          ? BATTERY_SENSOR_SYNC_INTERVAL_MS
                          : 5000);

    while (true) {
        battery_reading_t r;
        esp_err_t err = battery_sensor_read(&r);
        if (err == ESP_OK) {
            evaluate_transitions(r.voltage_v);
            r.state = s_state;
            ESP_LOGI(TAG,
                     "V=%.2f V  SoC=%.0f%%  state=%s",
                     (double)r.voltage_v, (double)r.percent,
                     battery_state_name(r.state));
#if BATTERY_SENSOR_SUPABASE_SYNC_ENABLED
            if (r.voltage_v >= BATTERY_MIN_VALID_VOLTAGE_V) {
                battery_sync_to_supabase(&r);
            } else {
                ESP_LOGW(TAG,
                         "skipping Supabase upsert: V=%.2f below %.1f V floor "
                         "(power-loss artifact or boot transient)",
                         (double)r.voltage_v,
                         (double)BATTERY_MIN_VALID_VOLTAGE_V);
            }
#endif
        } else {
            ESP_LOGW(TAG, "battery read failed: %s", esp_err_to_name(err));
        }
        vTaskDelay(delay_ticks);
    }
}

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
    out->state     = s_state;
    out->valid     = true;
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

const char *battery_state_name(battery_state_t state)
{
    switch (state) {
        case BATTERY_STATE_NORMAL:      return "normal";
        case BATTERY_STATE_WARNING:     return "warning";
        case BATTERY_STATE_CRITICAL:    return "critical";
        case BATTERY_STATE_WAKE_UP:     return "wake_up";
        case BATTERY_STATE_CHARGING_ON: return "charging_on";
        default:                        return "unknown";
    }
}

battery_state_t battery_sensor_current_state(void)
{
#if BATTERY_SENSOR_ENABLED
    return s_state;
#else
    return BATTERY_STATE_NORMAL;
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
