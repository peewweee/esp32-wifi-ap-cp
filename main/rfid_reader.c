#include "rfid_reader.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

/* Flip to 1 when the MFRC522 hardware is wired and ready for bring-up. */
#define RFID_ENABLED 0

/* Default wiring (SPI) — adjust to match your actual connections. */
#define RFID_PIN_RST   22
#define RFID_PIN_SS    21
#define RFID_PIN_SCK   18
#define RFID_PIN_MOSI  23
#define RFID_PIN_MISO  19

#define RFID_POLL_MS       500  /* how often to poll the reader */
#define RFID_ABSENT_READS  3    /* consecutive empty reads before declaring absent */

static const char *TAG = "rfid";
static bool s_card_present = false;
static rfid_presence_cb_t s_cb = NULL;

#if RFID_ENABLED
#include "driver/spi_master.h"
#include "driver/gpio.h"

static spi_device_handle_t s_spi;

/* TODO: implement MFRC522 register-level read/write and PICC select.
 * Reference: NXP MFRC522 datasheet, and any MFRC522 Arduino/ESP-IDF port.
 */
static void mfrc522_init(void) { /* SPI init, antenna gain, self test */ }

static bool mfrc522_poll_card(uint8_t uid_out[10], uint8_t *uid_len_out) {
    (void)uid_out; (void)uid_len_out;
    /* TODO: PICC_REQA → anticollision → select. Return true if a card
     * responded; populate uid_out / uid_len_out with its UID. */
    return false;
}

static void rfid_task(void *arg) {
    uint8_t absent_count = 0;
    uint8_t uid[10];
    uint8_t uid_len = 0;

    while (1) {
        bool now_present = mfrc522_poll_card(uid, &uid_len);

        if (now_present) {
            absent_count = 0;
            if (!s_card_present) {
                s_card_present = true;
                ESP_LOGI(TAG, "card present");
                if (s_cb) s_cb(true);
            }
        } else if (s_card_present) {
            if (++absent_count >= RFID_ABSENT_READS) {
                s_card_present = false;
                absent_count = 0;
                ESP_LOGI(TAG, "card absent");
                if (s_cb) s_cb(false);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(RFID_POLL_MS));
    }
}
#endif /* RFID_ENABLED */

esp_err_t rfid_reader_start(void)
{
#if RFID_ENABLED
    mfrc522_init();
    BaseType_t ok = xTaskCreate(rfid_task, "rfid", 4096, NULL, 4, NULL);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
#else
    ESP_LOGI(TAG, "rfid_reader_start: compiled out (RFID_ENABLED=0)");
    return ESP_OK;
#endif
}

bool rfid_reader_card_present(void)
{
#if RFID_ENABLED
    return s_card_present;
#else
    /* Hardware not wired yet — pretend the card is always inserted so
     * the rest of the system behaves as if the station is powered. */
    return true;
#endif
}

void rfid_reader_set_presence_callback(rfid_presence_cb_t cb)
{
    s_cb = cb;
}
