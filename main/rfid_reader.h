#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * MFRC522 RFID reader.
 *
 * Wired and live: SPI3 (VSPI) at 4 MHz, SDA=5, SCK=18, MOSI=23, MISO=19,
 * RST=4. Two authorized 4-byte NUIDs are hard-coded in rfid_reader.c.
 * The polling task drives six power-control GPIOs (4 MOSFETs + SSR +
 * relay on pins 12, 14, 27, 26, 25, 13) on authorized presence and
 * de-energizes them on absence or unauthorized cards.
 *
 * Battery coordination:
 *   The battery state machine in battery_sensor.c calls
 *   rfid_reader_set_ports_allowed() (declared in router_globals.h) to
 *   force the power outputs off in Critical state regardless of card
 *   presence. When re-allowed, the next valid card poll re-energizes
 *   them — there is no separate "resume" call.
 *
 * Compile-time disable:
 *   Set RFID_ENABLED to 0 at the top of rfid_reader.c to compile out the
 *   polling task. While disabled, rfid_reader_card_present() returns
 *   false and rfid_reader_start() is a logging no-op.
 */

typedef void (*rfid_presence_cb_t)(bool card_present);

/* Starts the SPI bus, configures the MFRC522, and launches the polling
 * task. Returns ESP_FAIL if the chip does not respond on VersionReg
 * (typically wiring or power). No-op when RFID_ENABLED is 0. */
esp_err_t rfid_reader_start(void);

/* Whether an authorized card is currently considered present. */
bool rfid_reader_card_present(void);

/* Register a callback invoked on presence edge transitions. */
void rfid_reader_set_presence_callback(rfid_presence_cb_t cb);

#ifdef __cplusplus
}
#endif
