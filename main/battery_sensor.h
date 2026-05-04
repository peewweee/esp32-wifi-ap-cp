#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Battery voltage sensor.
 *
 * Reads the 12 V battery rail through a resistor divider on GPIO 32
 * (ADC1 channel 4). Computes battery voltage from the ADC reading and
 * estimates state-of-charge (percent) using a 12 V lead-acid curve.
 * Periodic task pushes readings to Supabase.
 *
 * Wiring:
 *   12V battery + ----[ R1 (top) ]---- ADC node ----[ R2 (bottom) ]---- GND
 *                                            |
 *                                       ESP32 GPIO 32
 *
 * BATTERY_VOLTAGE_DIVIDER_RATIO is (R1 + R2) / R2.
 * Example for R1 = 30 kohm, R2 = 10 kohm:
 *   ratio = 40k / 10k = 4.0
 *   12 V battery -> 3.0 V at GPIO 32 (within the 0-3.3 V ADC range).
 *
 * Adjust the macro below to match your physical resistor values.
 */

#ifndef BATTERY_SENSOR_ENABLED
#define BATTERY_SENSOR_ENABLED 1
#endif

#ifndef BATTERY_SENSOR_SUPABASE_SYNC_ENABLED
#define BATTERY_SENSOR_SUPABASE_SYNC_ENABLED 1
#endif

/* GPIO 32 = ADC1 channel 4 on ESP32. */
#define BATTERY_SENSOR_GPIO              32

/* (R1 + R2) / R2. Current PCB uses 100 kohm (top) + 22 kohm (bottom),
 * giving 122 / 22 = 5.5454. 12 V battery -> ~2.16 V at GPIO 32, with
 * headroom up to ~18 V before the ADC pin clips at 3.3 V.
 * If you change the resistors, recompute and update this value. */
#ifndef BATTERY_VOLTAGE_DIVIDER_RATIO
#define BATTERY_VOLTAGE_DIVIDER_RATIO    5.5454f
#endif

/* Lead-acid 12 V curve. Override these if you're using LiFePO4
 * (4S = 12.8 V nominal, full ~14.4 V, empty ~10.0 V) or another chemistry. */
#ifndef BATTERY_VOLTAGE_FULL_V
#define BATTERY_VOLTAGE_FULL_V           12.7f
#endif
#ifndef BATTERY_VOLTAGE_EMPTY_V
#define BATTERY_VOLTAGE_EMPTY_V          10.5f
#endif

#ifndef BATTERY_SENSOR_SYNC_INTERVAL_MS
#define BATTERY_SENSOR_SYNC_INTERVAL_MS  10000   /* 10 s */
#endif

#ifndef BATTERY_SENSOR_SAMPLES
#define BATTERY_SENSOR_SAMPLES           16      /* averaged per read for noise reduction */
#endif

typedef struct {
    float    voltage_v;          /* battery voltage at the terminals, V */
    float    percent;            /* estimated state-of-charge, 0.0 - 100.0 */
    int      raw_mv;             /* raw ADC voltage at GPIO 32, mV (post-calibration) */
    bool     valid;
} battery_reading_t;

esp_err_t battery_sensor_init(void);
esp_err_t battery_sensor_read(battery_reading_t *out);
esp_err_t battery_sensor_start(void);

#ifdef __cplusplus
}
#endif
