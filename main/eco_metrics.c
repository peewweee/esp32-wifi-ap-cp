#include "eco_metrics.h"

#include "esp_log.h"
#include <string.h>

static const char *TAG = "eco_metrics";

#if ECO_METRICS_ENABLED
/* Per-port energy accumulator in watt-hours. Only USB ports for now;
 * outlet/battery are intentionally excluded until sensors exist. */
typedef struct {
    const char *port_key;
    float       energy_wh;
} port_accumulator_t;

static port_accumulator_t s_accum[] = {
    { "usb_c_1", 0.0f },
    { "usb_c_2", 0.0f },
    { "usb_a_1", 0.0f },
    { "usb_a_2", 0.0f },
};

#define ECO_NUM_PORTS  (sizeof(s_accum) / sizeof(s_accum[0]))

static bool s_initialized = false;

static port_accumulator_t *find_accumulator(const char *port_key)
{
    if (port_key == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < ECO_NUM_PORTS; i++) {
        if (strcmp(s_accum[i].port_key, port_key) == 0) {
            return &s_accum[i];
        }
    }
    return NULL;
}
#endif /* ECO_METRICS_ENABLED */

esp_err_t eco_metrics_init(void)
{
#if ECO_METRICS_ENABLED
    eco_metrics_reset_today();
    s_initialized = true;
    ESP_LOGI(TAG, "eco metrics initialized; emission factor=%.3f kg/kWh",
             (double)GRID_EMISSION_FACTOR_KG_PER_KWH);
    return ESP_OK;
#else
    ESP_LOGI(TAG,
             "eco metrics disabled at compile time (ECO_METRICS_ENABLED=0); "
             "PWA estimates CO2 client-side from /ports test data");
    return ESP_OK;
#endif
}

bool eco_metrics_is_enabled(void)
{
#if ECO_METRICS_ENABLED
    return s_initialized;
#else
    return false;
#endif
}

void eco_metrics_record_sample(const char *port_key,
                               float current_ma,
                               float bus_voltage_v,
                               float dt_seconds)
{
#if ECO_METRICS_ENABLED
    if (!s_initialized) {
        return;
    }
    if (current_ma <= 0.0f || bus_voltage_v <= 0.0f || dt_seconds <= 0.0f) {
        return;
    }

    port_accumulator_t *acc = find_accumulator(port_key);
    if (acc == NULL) {
        /* Unknown port_key — outlet/battery and any future ports without
         * a real current sensor land here; skip silently. */
        return;
    }

    /* Energy in watt-hours: V * A * (hours).
     *   amps  = current_ma / 1000
     *   hours = dt_seconds / 3600
     *   Wh    = bus_voltage_v * amps * hours
     */
    const float amps  = current_ma / 1000.0f;
    const float hours = dt_seconds / 3600.0f;
    const float wh    = bus_voltage_v * amps * hours;

    acc->energy_wh += wh;
#else
    (void)port_key;
    (void)current_ma;
    (void)bus_voltage_v;
    (void)dt_seconds;
#endif
}

float eco_metrics_today_energy_wh(void)
{
#if ECO_METRICS_ENABLED
    if (!s_initialized) {
        return 0.0f;
    }
    float total = 0.0f;
    for (size_t i = 0; i < ECO_NUM_PORTS; i++) {
        total += s_accum[i].energy_wh;
    }
    return total;
#else
    return 0.0f;
#endif
}

float eco_metrics_today_co2_saved_g(void)
{
#if ECO_METRICS_ENABLED
    /*  energy_kWh  = energy_Wh / 1000
     *  co2_saved_g = energy_kWh * GRID_EMISSION_FACTOR_KG_PER_KWH * 1000
     *
     * Both /1000 and *1000 cancel, so:
     *  co2_saved_g = energy_Wh * GRID_EMISSION_FACTOR_KG_PER_KWH
     * Kept in two-step form below for documentation clarity.
     */
    const float energy_wh  = eco_metrics_today_energy_wh();
    const float energy_kwh = energy_wh / 1000.0f;
    const float co2_kg     = energy_kwh * GRID_EMISSION_FACTOR_KG_PER_KWH;
    return co2_kg * 1000.0f;
#else
    return 0.0f;
#endif
}

void eco_metrics_reset_today(void)
{
#if ECO_METRICS_ENABLED
    for (size_t i = 0; i < ECO_NUM_PORTS; i++) {
        s_accum[i].energy_wh = 0.0f;
    }
    ESP_LOGI(TAG, "eco metrics daily counters reset");
#endif
}
