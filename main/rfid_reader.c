#include "rfid_reader.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

#include <string.h>

#define RFID_ENABLED 1

/* Wiring (matches the team's Arduino bring-up sketch):
 *   MFRC522 <-> ESP32
 *     SDA/SS -> GPIO 5
 *     SCK    -> GPIO 18
 *     MOSI   -> GPIO 23
 *     MISO   -> GPIO 19
 *     RST    -> GPIO 4
 *     IRQ    -> not connected
 * VSPI on ESP32 == SPI3_HOST in ESP-IDF. */
#define RFID_PIN_SS    5
#define RFID_PIN_RST   4
#define RFID_PIN_SCK   18
#define RFID_PIN_MOSI  23
#define RFID_PIN_MISO  19
#define RFID_SPI_HOST  SPI3_HOST
#define RFID_SPI_HZ    (4 * 1000 * 1000)

#if RFID_ENABLED
/* All 6 power-control pins from the schematic: 4 MOSFETs + SSR + relay.
 * GPIO 12 is a strapping pin (MTDI / flash-voltage select) — we initialize
 * it LOW after boot to match the Arduino sketch. */
static const gpio_num_t s_power_pins[] = {12, 14, 27, 26, 25, 13};
#define RFID_NUM_POWER_PINS (sizeof(s_power_pins) / sizeof(s_power_pins[0]))

/* Authorized 4-byte NUIDs (MIFARE Classic). */
static const uint8_t s_valid_uids[][4] = {
    {0x43, 0x79, 0xAD, 0x38},
    {0xF3, 0x95, 0xCF, 0x1E},
};
#define RFID_NUM_UIDS (sizeof(s_valid_uids) / sizeof(s_valid_uids[0]))
#endif /* RFID_ENABLED */

#define RFID_PRESENCE_TIMEOUT_MS  1000  /* matches Arduino loop */
#define RFID_POLL_MS              50    /* fast enough for hold/lift detection */

static const char *TAG = "rfid";
static rfid_presence_cb_t s_cb = NULL;
static bool s_card_present = false;

#if RFID_ENABLED

/* MFRC522 register subset (datasheet §9). */
#define REG_COMMAND      0x01
#define REG_COMIRQ       0x04
#define REG_ERROR        0x06
#define REG_FIFO_DATA    0x09
#define REG_FIFO_LEVEL   0x0A
#define REG_CONTROL      0x0C
#define REG_BIT_FRAMING  0x0D
#define REG_COLL         0x0E
#define REG_MODE         0x11
#define REG_TX_CONTROL   0x14
#define REG_TX_ASK       0x15
#define REG_T_MODE       0x2A
#define REG_T_PRESCALER  0x2B
#define REG_T_RELOAD_H   0x2C
#define REG_T_RELOAD_L   0x2D
#define REG_VERSION      0x37

#define CMD_IDLE         0x00
#define CMD_TRANSCEIVE   0x0C
#define CMD_SOFT_RESET   0x0F

#define PICC_REQA        0x26
#define PICC_SEL_CL1     0x93

static spi_device_handle_t s_spi;
static bool s_hardware_ok = false;
static bool s_ports_active = false;
static bool s_invalid_logged = false;

static esp_err_t mfrc522_write(uint8_t reg, uint8_t value)
{
    uint8_t tx[2] = { (uint8_t)((reg << 1) & 0x7E), value };
    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = tx,
    };
    return spi_device_polling_transmit(s_spi, &t);
}

static esp_err_t mfrc522_read(uint8_t reg, uint8_t *out)
{
    uint8_t tx[2] = { (uint8_t)(0x80 | ((reg << 1) & 0x7E)), 0x00 };
    uint8_t rx[2] = {0};
    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    esp_err_t err = spi_device_polling_transmit(s_spi, &t);
    if (err == ESP_OK) {
        *out = rx[1];
    }
    return err;
}

static void mfrc522_set_bits(uint8_t reg, uint8_t mask)
{
    uint8_t v;
    if (mfrc522_read(reg, &v) == ESP_OK) {
        mfrc522_write(reg, v | mask);
    }
}

static void mfrc522_clear_bits(uint8_t reg, uint8_t mask)
{
    uint8_t v;
    if (mfrc522_read(reg, &v) == ESP_OK) {
        mfrc522_write(reg, (uint8_t)(v & ~mask));
    }
}

static void pcd_antenna_on(void)
{
    uint8_t v;
    if (mfrc522_read(REG_TX_CONTROL, &v) == ESP_OK && (v & 0x03) != 0x03) {
        mfrc522_write(REG_TX_CONTROL, v | 0x03);
    }
}

static void pcd_antenna_off(void)
{
    mfrc522_clear_bits(REG_TX_CONTROL, 0x03);
}

static void pcd_hard_reset(void)
{
    gpio_set_level(RFID_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(RFID_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

static esp_err_t pcd_init_chip(void)
{
    pcd_hard_reset();

    mfrc522_write(REG_COMMAND, CMD_SOFT_RESET);
    for (int i = 0; i < 10; i++) {
        uint8_t cmd;
        if (mfrc522_read(REG_COMMAND, &cmd) == ESP_OK && (cmd & 0x10) == 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    /* Internal timer: TAuto=1, prescaler=0x0A9 + 0x03E8 reload (~25ms). */
    mfrc522_write(REG_T_MODE,      0x80);
    mfrc522_write(REG_T_PRESCALER, 0xA9);
    mfrc522_write(REG_T_RELOAD_H,  0x03);
    mfrc522_write(REG_T_RELOAD_L,  0xE8);

    mfrc522_write(REG_TX_ASK, 0x40);  /* 100% ASK */
    mfrc522_write(REG_MODE,   0x3D);  /* CRC preset 0x6363 */

    pcd_antenna_on();

    uint8_t version = 0;
    esp_err_t err = mfrc522_read(REG_VERSION, &version);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "VersionReg read failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "MFRC522 firmware version 0x%02X", version);
    if (version == 0x00 || version == 0xFF) {
        ESP_LOGE(TAG, "MFRC522 not responding (check wiring/power)");
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

/* Send a frame and wait for response. tx_last_bits=7 sends a short frame
 * (REQA/WUPA); tx_last_bits=0 sends whole bytes. */
static esp_err_t pcd_transceive(const uint8_t *tx, uint8_t tx_len,
                                uint8_t *rx, uint8_t rx_size,
                                uint8_t *rx_len, uint8_t tx_last_bits)
{
    *rx_len = 0;

    mfrc522_write(REG_COMMAND, CMD_IDLE);
    mfrc522_write(REG_COMIRQ,  0x7F);
    mfrc522_write(REG_FIFO_LEVEL, 0x80);

    for (uint8_t i = 0; i < tx_len; i++) {
        mfrc522_write(REG_FIFO_DATA, tx[i]);
    }

    mfrc522_write(REG_BIT_FRAMING, (uint8_t)(tx_last_bits & 0x07));
    mfrc522_write(REG_COMMAND, CMD_TRANSCEIVE);
    mfrc522_set_bits(REG_BIT_FRAMING, 0x80);

    int deadline_ms = 36;
    bool done = false;
    while (deadline_ms-- > 0) {
        uint8_t irq;
        if (mfrc522_read(REG_COMIRQ, &irq) != ESP_OK) {
            return ESP_FAIL;
        }
        if (irq & 0x30) {  /* RxIRq | IdleIRq */
            done = true;
            break;
        }
        if (irq & 0x01) {  /* TimerIRq — no card answered in time */
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (!done) {
        return ESP_ERR_TIMEOUT;
    }

    mfrc522_clear_bits(REG_BIT_FRAMING, 0x80);

    uint8_t err_reg = 0;
    if (mfrc522_read(REG_ERROR, &err_reg) == ESP_OK && (err_reg & 0x13)) {
        return ESP_FAIL;
    }

    uint8_t fifo_level = 0;
    if (mfrc522_read(REG_FIFO_LEVEL, &fifo_level) != ESP_OK) {
        return ESP_FAIL;
    }
    if (fifo_level > rx_size) {
        fifo_level = rx_size;
    }

    for (uint8_t i = 0; i < fifo_level; i++) {
        if (mfrc522_read(REG_FIFO_DATA, &rx[i]) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    *rx_len = fifo_level;
    return ESP_OK;
}

static bool picc_request_a(void)
{
    uint8_t buf[2];
    uint8_t len = 0;
    uint8_t cmd = PICC_REQA;

    mfrc522_write(REG_COLL, 0x80);
    esp_err_t err = pcd_transceive(&cmd, 1, buf, sizeof(buf), &len, 7);
    return err == ESP_OK && len == 2;
}

/* Cascade-level-1 anticollision. Returns 4-byte NUID (sufficient for the
 * Mifare Classic cards used by the project). */
static bool picc_anticoll(uint8_t uid[4])
{
    uint8_t tx[2] = { PICC_SEL_CL1, 0x20 };
    uint8_t rx[5] = {0};
    uint8_t len = 0;

    mfrc522_write(REG_COLL, 0x80);
    esp_err_t err = pcd_transceive(tx, sizeof(tx), rx, sizeof(rx), &len, 0);
    if (err != ESP_OK || len != 5) {
        return false;
    }
    if ((rx[0] ^ rx[1] ^ rx[2] ^ rx[3]) != rx[4]) {
        return false;
    }
    memcpy(uid, rx, 4);
    return true;
}

static bool uid_is_authorized(const uint8_t uid[4])
{
    for (size_t i = 0; i < RFID_NUM_UIDS; i++) {
        if (memcmp(uid, s_valid_uids[i], 4) == 0) {
            return true;
        }
    }
    return false;
}

static void power_pins_init(void)
{
    for (size_t i = 0; i < RFID_NUM_POWER_PINS; i++) {
        gpio_reset_pin(s_power_pins[i]);
        gpio_set_direction(s_power_pins[i], GPIO_MODE_OUTPUT);
        gpio_set_level(s_power_pins[i], 0);
    }
    s_ports_active = false;
}

/* Master gate set by the battery state machine. When false, the RFID
 * task is forbidden from energizing the power pins regardless of card
 * presence. Default true so RFID works normally on a healthy battery. */
static volatile bool s_ports_allowed_by_battery = true;

static void set_power_state(bool on)
{
    if (on && !s_ports_allowed_by_battery) {
        on = false;  /* battery says no, override to off */
    }
    if (s_ports_active == on) {
        return;
    }
    s_ports_active = on;
    for (size_t i = 0; i < RFID_NUM_POWER_PINS; i++) {
        gpio_set_level(s_power_pins[i], on ? 1 : 0);
    }
    if (on) {
        ESP_LOGI(TAG, "AUTHORIZED CARD PRESENT: charging ports + SSR + relay ON");
    } else {
        ESP_LOGI(TAG, "CARD ABSENT/UNAUTHORIZED: all power outputs OFF");
    }
}

void rfid_reader_set_ports_allowed(bool allowed)
{
    if (s_ports_allowed_by_battery == allowed) {
        return;
    }
    s_ports_allowed_by_battery = allowed;
    ESP_LOGI(TAG, "battery override: ports %s",
             allowed ? "ALLOWED (RFID controls)" : "BLOCKED (force off)");
    if (!allowed && s_ports_active) {
        set_power_state(false);
    }
    /* When re-allowed, the RFID poll loop will turn ports back on the
     * next time it sees the authorized card; nothing to do here. */
}

static void rfid_task(void *arg)
{
    (void)arg;
    TickType_t last_valid_tick = 0;
    const TickType_t timeout_ticks = pdMS_TO_TICKS(RFID_PRESENCE_TIMEOUT_MS);

    for (;;) {
        TickType_t now = xTaskGetTickCount();

        if (s_ports_active && (now - last_valid_tick) > timeout_ticks) {
            set_power_state(false);
        }

        if (picc_request_a()) {
            uint8_t uid[4] = {0};
            if (picc_anticoll(uid)) {
                if (uid_is_authorized(uid)) {
                    last_valid_tick = now;
                    s_invalid_logged = false;
                    set_power_state(true);
                    if (!s_card_present) {
                        s_card_present = true;
                        ESP_LOGI(TAG, "card present UID=%02X%02X%02X%02X",
                                 uid[0], uid[1], uid[2], uid[3]);
                        if (s_cb) s_cb(true);
                    }
                } else {
                    set_power_state(false);
                    if (!s_invalid_logged) {
                        ESP_LOGW(TAG,
                                 "UNAUTHORIZED UID=%02X%02X%02X%02X — access denied",
                                 uid[0], uid[1], uid[2], uid[3]);
                        s_invalid_logged = true;
                    }
                    if (s_card_present) {
                        s_card_present = false;
                        if (s_cb) s_cb(false);
                    }
                }
            }

            /* Antenna bounce — physically resets a resting card so the next
             * REQA sees it as a fresh "new card" instead of HALT state.
             * Intentionally does NOT use PICC_HaltA(). */
            pcd_antenna_off();
            vTaskDelay(pdMS_TO_TICKS(15));
            pcd_antenna_on();
            vTaskDelay(pdMS_TO_TICKS(15));
        } else if (s_card_present && (now - last_valid_tick) > timeout_ticks) {
            s_card_present = false;
            s_invalid_logged = false;
            ESP_LOGI(TAG, "card absent");
            if (s_cb) s_cb(false);
        }

        vTaskDelay(pdMS_TO_TICKS(RFID_POLL_MS));
    }
}

#endif /* RFID_ENABLED */

esp_err_t rfid_reader_start(void)
{
#if RFID_ENABLED
    /* Initialize power pins to LOW first so the bench is safe even if the
     * MFRC522 fails to come up. */
    power_pins_init();

    spi_bus_config_t bus = {
        .mosi_io_num = RFID_PIN_MOSI,
        .miso_io_num = RFID_PIN_MISO,
        .sclk_io_num = RFID_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32,
    };
    esp_err_t err = spi_bus_initialize(RFID_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    spi_device_interface_config_t dev = {
        .clock_speed_hz = RFID_SPI_HZ,
        .mode = 0,
        .spics_io_num = RFID_PIN_SS,
        .queue_size = 4,
    };
    err = spi_bus_add_device(RFID_SPI_HOST, &dev, &s_spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        return err;
    }

    gpio_reset_pin(RFID_PIN_RST);
    gpio_set_direction(RFID_PIN_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(RFID_PIN_RST, 1);

    if (pcd_init_chip() != ESP_OK) {
        ESP_LOGW(TAG, "MFRC522 init failed; presence task will not start");
        s_hardware_ok = false;
        return ESP_FAIL;
    }
    s_hardware_ok = true;

    BaseType_t ok = xTaskCreate(rfid_task, "rfid", 4096, NULL, 4, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(rfid) failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "RFID reader started; %u authorized UIDs, presence timeout %u ms",
             (unsigned)RFID_NUM_UIDS, (unsigned)RFID_PRESENCE_TIMEOUT_MS);
    return ESP_OK;
#else
    ESP_LOGI(TAG, "rfid_reader_start: compiled out (RFID_ENABLED=0)");
    return ESP_OK;
#endif
}

bool rfid_reader_card_present(void)
{
    return s_card_present;
}

void rfid_reader_set_presence_callback(rfid_presence_cb_t cb)
{
    s_cb = cb;
}
