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

/* Enforce the battery-driven side effects on the user AP and the
 * RFID-controlled charging ports.
 *   1 (default): full thesis behavior — at <13.2 V the user AP is
 *                disabled, at <11.8 V the charging ports are blocked,
 *                etc.
 *   0 (dev):     telemetry still runs (voltage, percent, state, and
 *                Supabase upsert all work), but the AP and RFID-port
 *                gate are NEVER touched by the state machine. Use this
 *                when running on a 12 V bench supply that would
 *                otherwise leave the AP off.
 *
 * Override via top-level .env:
 *   BATTERY_SENSOR_ENFORCE_THRESHOLDS=0
 */
#ifndef BATTERY_SENSOR_ENFORCE_THRESHOLDS
#define BATTERY_SENSOR_ENFORCE_THRESHOLDS 1
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

/* Battery curve calibrated to the project's threshold table:
 *   Normal     13.6 V – 12.8 V  (100% – 26%)
 *   Warning    ~12.6 V          (25%, falling)   user AP off
 *   Critical   ~11.8 V          (15%, falling)   ports off
 *   Shutdown   11.6 V           (10%, hardware MPPT cut)
 *   Wake Up    ~12.4 V          (13%, recovering after MPPT recovery)
 *   Charging   ~12.8 V          (20%, recovering, ports back on)
 *   Full Svc   13.2 V           (30%, recovering, user AP back on)
 *
 * Percentage is linearly mapped between FULL_V and EMPTY_V so the
 * dashboard's "%" matches the table above. */
#ifndef BATTERY_VOLTAGE_FULL_V
#define BATTERY_VOLTAGE_FULL_V           13.9f
#endif
#ifndef BATTERY_VOLTAGE_EMPTY_V
#define BATTERY_VOLTAGE_EMPTY_V          11.6f
#endif

/* State-machine voltage thresholds (volts).
 * Falling thresholds are LOWER than rising ones for the same feature so
 * the system has hysteresis and doesn't flap when voltage hovers. */
#define BATTERY_V_WARNING_FALL           12.6f   /* Normal -> Warning */
#define BATTERY_V_CRITICAL_FALL          11.8f   /* Warning -> Critical */
#define BATTERY_V_WAKEUP_BOOT_LOW        12.4f   /* boot threshold for Wake Up */
#define BATTERY_V_CHARGING_RISE          12.8f   /* Critical/Wake Up -> Charging On */
#define BATTERY_V_NORMAL_RISE            13.2f   /* Charging On / Warning -> Normal */

/* How many consecutive samples must agree before we change state.
 * 6 samples × 5 s = 30 s of debouncing — a phone plugging in dips
 * voltage briefly, but won't sustain that for half a minute. */
#ifndef BATTERY_DEBOUNCE_SAMPLES
#define BATTERY_DEBOUNCE_SAMPLES         6
#endif

/* Minimum voltage for a reading to be considered real and pushed to
 * Supabase. Below this we assume the ESP32 is either still booting
 * (caps not charged yet) or has just been unplugged and the input rail
 * is decaying — in either case the ADC samples noise that the linear
 * curve would map to "0 %", and we don't want that landing in
 * station_state.battery_percent.
 *
 * 5 V was picked because:
 *   - Real LiFePO4 readings live in 11.6-13.6 V land
 *   - Even a deeply-discharged 12 V battery does not fall to 5 V
 *   - The ESP32 itself runs off 3.3 V LDO; any rail capable of running
 *     the chip will keep the divider node well above 5 V too
 *
 * Combined with the dashboard's staleness check, this gives a clean
 * "Offline" UX when the ESP32 is unplugged. */
#ifndef BATTERY_MIN_VALID_VOLTAGE_V
#define BATTERY_MIN_VALID_VOLTAGE_V      5.0f
#endif

/* On boot, take this many quick samples (with shorter delay) before
 * deciding which state to enter. Avoids reacting to a single noisy
 * reading at power-up. */
#ifndef BATTERY_BOOT_SAMPLES
#define BATTERY_BOOT_SAMPLES             5
#endif
#ifndef BATTERY_BOOT_SAMPLE_DELAY_MS
#define BATTERY_BOOT_SAMPLE_DELAY_MS     500
#endif

#ifndef BATTERY_SENSOR_SYNC_INTERVAL_MS
#define BATTERY_SENSOR_SYNC_INTERVAL_MS  5000    /* 5 s — matches debounce design */
#endif

#ifndef BATTERY_SENSOR_SAMPLES
#define BATTERY_SENSOR_SAMPLES           16      /* averaged per read for noise reduction */
#endif

typedef enum {
    BATTERY_STATE_NORMAL = 0,
    BATTERY_STATE_WARNING,
    BATTERY_STATE_CRITICAL,
    BATTERY_STATE_WAKE_UP,
    BATTERY_STATE_CHARGING_ON,
} battery_state_t;

typedef struct {
    float           voltage_v;   /* battery voltage at the terminals, V */
    float           percent;     /* estimated state-of-charge, 0.0 - 100.0 */
    int             raw_mv;      /* raw ADC voltage at GPIO 32, mV (post-calibration) */
    battery_state_t state;       /* current state-machine label */
    bool            valid;
} battery_reading_t;

esp_err_t battery_sensor_init(void);
esp_err_t battery_sensor_read(battery_reading_t *out);
esp_err_t battery_sensor_start(void);

const char *battery_state_name(battery_state_t state);
battery_state_t battery_sensor_current_state(void);

#ifdef __cplusplus
}
#endif
