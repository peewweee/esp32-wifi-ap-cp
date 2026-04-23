#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * MFRC522 RFID reader — SKELETON.
 *
 * Hardware is not wired yet. rfid_reader_start() is a no-op at the moment
 * so the firmware can be flashed and the existing Wi-Fi / captive-portal
 * functionality tested without the MFRC522 module attached.
 *
 * To enable once the module is wired:
 *   1. Set RFID_ENABLED to 1 at the top of rfid_reader.c.
 *   2. Confirm the RFID_PIN_* defines match your wiring.
 *   3. Call rfid_reader_start() from app_main() after nvs_flash_init().
 *   4. Register a presence callback to drive the station state machine.
 *
 * While disabled, rfid_reader_card_present() returns true so the rest of
 * the system behaves as if the card is always inserted.
 */

typedef void (*rfid_presence_cb_t)(bool card_present);

/* Starts the polling task if RFID_ENABLED is 1; otherwise a no-op. */
esp_err_t rfid_reader_start(void);

/* Whether a card is currently considered present. */
bool rfid_reader_card_present(void);

/* Register a callback invoked on presence edge transitions. */
void rfid_reader_set_presence_callback(rfid_presence_cb_t cb);

#ifdef __cplusplus
}
#endif
