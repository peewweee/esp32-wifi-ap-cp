#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * PZEM-004T v1.0 AC power monitor (legacy Peacefair binary protocol).
 *
 * The v1 module does NOT speak Modbus. It uses 7-byte command / 7-byte
 * response framing with a 4-byte IP-style device address (default
 * 192.168.1.1). Each metric (voltage, current, power, energy) requires a
 * separate query, so a single pzem_reader_read() issues four sequential
 * frames over UART2.
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

#ifdef __cplusplus
}
#endif
