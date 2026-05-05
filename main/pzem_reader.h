#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * PZEM-004T v3.0 AC power monitor (Modbus-RTU protocol).
 *
 * One read transaction = one Modbus "read input registers" frame to slave
 * 0xF8 (general broadcast), returning 10 registers (voltage, current,
 * power, energy, frequency, power factor, alarm). All four user-visible
 * fields are populated from a single 25-byte response.
 *
 * Wiring:
 *   ESP32 GPIO 17 (TX2) -> PZEM RX
 *   ESP32 GPIO 16 (RX2) -> PZEM TX
 *   PZEM 5V             -> LM2596 5V (with shared GND to ESP32)
 */

#ifndef PZEM_ENABLED
#define PZEM_ENABLED 1
#endif

#define PZEM_UART_PIN_TX        17    /* ESP32 TX2 -> PZEM RX */
#define PZEM_UART_PIN_RX        16    /* ESP32 RX2 -> PZEM TX */
#define PZEM_UART_BAUD          9600

#ifndef PZEM_READ_INTERVAL_MS
#define PZEM_READ_INTERVAL_MS   5000
#endif

typedef struct {
    float    voltage_v;      /* AC line voltage, V        (0.1 V resolution) */
    float    current_a;      /* AC line current, A        (0.01 A resolution) */
    float    power_w;        /* Active power, W           (1 W resolution) */
    uint32_t energy_wh;      /* Cumulative energy, Wh     (1 Wh resolution) */
    bool     valid;
} pzem_reading_t;

esp_err_t pzem_reader_init(void);
esp_err_t pzem_reader_read(pzem_reading_t *out);
esp_err_t pzem_reader_start(void);

/* Copy the most recent successful PZEM reading into *out.
 *
 * Returns true iff the periodic pzem_task has completed at least one
 * successful read since boot. Designed for HTTP / diagnostics callers
 * that should NOT issue their own UART queries (concurrent reads with
 * the periodic task would interleave bytes on UART2 and corrupt
 * responses).
 *
 * The reading is at most PZEM_READ_INTERVAL_MS old (5 s by default). */
bool pzem_reader_get_last(pzem_reading_t *out);

#ifdef __cplusplus
}
#endif
