#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Eco Achievement and Green Metrics (EAGM) — preparation module
 * ============================================================================
 *
 * This module is intentionally INACTIVE. The compile-time feature flag
 * ECO_METRICS_ENABLED defaults to 0, so:
 *   - eco_metrics_init()                is a no-op that returns ESP_OK.
 *   - eco_metrics_record_sample()       is a no-op.
 *   - eco_metrics_today_energy_wh()     returns 0.
 *   - eco_metrics_today_co2_saved_g()   returns 0.
 *   - No accumulators run.
 *   - The captive portal, /ports test UI, and Supabase upserts continue
 *     to work exactly as they did before.
 *
 * The PWA dashboard currently estimates CO2 savings client-side from the
 * existing port_state rows that the /ports test UI populates. This module
 * exists so that future hardware bring-up can drop in real energy data
 * without disturbing the rest of the firmware.
 *
 * When INA219 hardware is wired and PORT_SENSORS_ENABLED is flipped to 1:
 *   1. Define ECO_METRICS_ENABLED to 1 (here or via build flag).
 *   2. Have a periodic task call eco_metrics_record_sample(...) for each
 *      port using the INA219 readings (current_ma, bus_voltage_v) and the
 *      seconds elapsed since the previous sample for that port.
 *   3. Periodically read eco_metrics_today_co2_saved_g() and push it to
 *      Supabase (likely a new column on station_state, or a dedicated
 *      eco_state table — schema is intentionally NOT changed here).
 *   4. Call eco_metrics_reset_today() at midnight UTC rollover.
 *
 * Out of scope (intentionally not implemented):
 *   - AC outlet energy. The current schematic has no current sensor on
 *     the AC outlet. Skip recording samples for the "outlet" port_key
 *     until that hardware exists.
 *   - Battery accounting. The current schematic has no fuel gauge or
 *     voltage divider on the battery, so there is no source of truth for
 *     a battery-side energy figure yet.
 * ========================================================================= */

#ifndef ECO_METRICS_ENABLED
#define ECO_METRICS_ENABLED 0
#endif

/* Average grid emissions intensity in kg CO2 per kWh. The Philippines grid
 * runs around 0.65–0.75 kg/kWh depending on the year and source mix. 0.70
 * is a reasonable default for thesis-level estimates. Reviewers can tune
 * this constant from a single place. */
#ifndef GRID_EMISSION_FACTOR_KG_PER_KWH
#define GRID_EMISSION_FACTOR_KG_PER_KWH  0.70f
#endif

/* Initialize the eco metrics module. No-op when ECO_METRICS_ENABLED is 0.
 * Safe to call from app_main() at any time. */
esp_err_t eco_metrics_init(void);

/* True if ECO_METRICS_ENABLED is 1 and init has succeeded. */
bool eco_metrics_is_enabled(void);

/* Record a single sample for one port.
 *
 *   port_key       — matches Supabase port_state.port_key, e.g. "usb_a_1".
 *                    A NULL or unknown key is silently ignored.
 *   current_ma     — instantaneous current draw in milliamps.
 *   bus_voltage_v  — instantaneous bus voltage in volts (~5.0 for USB).
 *   dt_seconds     — seconds elapsed since the previous sample for this
 *                    port. Used to integrate energy over time.
 *
 * No-op when disabled. */
void eco_metrics_record_sample(const char *port_key,
                               float current_ma,
                               float bus_voltage_v,
                               float dt_seconds);

/* Total energy delivered today across all USB ports, in watt-hours.
 * Returns 0 when disabled or when the day has just rolled over. */
float eco_metrics_today_energy_wh(void);

/* Estimated CO2 savings today, in grams.
 *   energy_kWh   = energy_Wh / 1000
 *   co2_saved_g  = energy_kWh * GRID_EMISSION_FACTOR_KG_PER_KWH * 1000
 * Returns 0 when disabled. */
float eco_metrics_today_co2_saved_g(void);

/* Reset the daily accumulators. Should be called at midnight rollover.
 * For now, also called from eco_metrics_init(). No-op when disabled. */
void eco_metrics_reset_today(void);

#ifdef __cplusplus
}
#endif
