#include "pzem_reader.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "supabase_client.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "pzem";

#ifndef PZEM_STATION_ID
#define PZEM_STATION_ID "solar-hub-01"
#endif

/* AC outlet "in use" detection: any draw above this is treated as a real
 * device. PZEM v3 power resolution is 0.1 W; the floor sits well above
 * standby leakage but low enough to catch a 5 V phone charger (~2-3 W). */
#ifndef PZEM_OUTLET_IN_USE_W
#define PZEM_OUTLET_IN_USE_W       1.0f
#endif

#define PZEM_UART_PORT             UART_NUM_2
#define PZEM_RX_BUFFER_BYTES       256
#define PZEM_RESPONSE_TIMEOUT_MS   300

/* PZEM-004T v3.0 Modbus-RTU. 0xF8 is the "general slave address" — every v3
 * module answers regardless of its programmed address, which is what we want
 * for a single-module setup. */
#define PZEM_V3_SLAVE_ADDR         0xF8
#define PZEM_V3_FUNC_READ_INPUT    0x04
#define PZEM_V3_REG_START          0x0000
#define PZEM_V3_REG_COUNT          0x000A   /* 10 registers (20 data bytes) */
#define PZEM_V3_REQUEST_BYTES      8        /* addr+func+addr16+count16+crc16 */
#define PZEM_V3_RESPONSE_BYTES     25       /* addr+func+bc+20 data+crc16 */

#if PZEM_ENABLED

static bool s_initialized;

/* Cache of the most recent successful read. Updated by pzem_task and
 * snapshotted by HTTP handlers. Spinlock so the snapshot is atomic
 * (struct copy under critical section). */
static pzem_reading_t s_last_reading;
static bool           s_last_reading_valid;
static portMUX_TYPE   s_last_reading_lock = portMUX_INITIALIZER_UNLOCKED;

/* Modbus-RTU CRC-16, poly 0xA001, seed 0xFFFF. */
static uint16_t pzem_modbus_crc16(const uint8_t *buf, int len)
{
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= (uint16_t)buf[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc & 1) ? ((crc >> 1) ^ 0xA001) : (crc >> 1);
        }
    }
    return crc;
}

/* One Modbus request reads the entire 10-register input block. PZEM v3 lays
 * the registers out as:
 *
 *   reg 0x00  voltage          16-bit, 0.1 V          -> resp[3..4]
 *   reg 0x01  current LOW      |
 *   reg 0x02  current HIGH     | 32-bit, 0.001 A      -> resp[5..8]
 *   reg 0x03  power LOW        |
 *   reg 0x04  power HIGH       | 32-bit, 0.1 W        -> resp[9..12]
 *   reg 0x05  energy LOW       |
 *   reg 0x06  energy HIGH      | 32-bit, 1 Wh         -> resp[13..16]
 *   reg 0x07  frequency        16-bit, 0.1 Hz         -> resp[17..18]
 *   reg 0x08  power factor     16-bit, 0.01           -> resp[19..20]
 *   reg 0x09  alarm status     16-bit                 -> resp[21..22]
 *
 * 32-bit values are little-endian by *register* (LOW reg first), big-endian
 * within each register — i.e. value = (HIGH_word << 16) | LOW_word. */
static esp_err_t pzem_v3_read(pzem_reading_t *out)
{
    uint8_t tx[PZEM_V3_REQUEST_BYTES];
    tx[0] = PZEM_V3_SLAVE_ADDR;
    tx[1] = PZEM_V3_FUNC_READ_INPUT;
    tx[2] = (uint8_t)(PZEM_V3_REG_START >> 8);
    tx[3] = (uint8_t)(PZEM_V3_REG_START & 0xFF);
    tx[4] = (uint8_t)(PZEM_V3_REG_COUNT >> 8);
    tx[5] = (uint8_t)(PZEM_V3_REG_COUNT & 0xFF);
    uint16_t crc = pzem_modbus_crc16(tx, 6);
    tx[6] = (uint8_t)(crc & 0xFF);
    tx[7] = (uint8_t)(crc >> 8);

    uart_flush_input(PZEM_UART_PORT);
    if (uart_write_bytes(PZEM_UART_PORT, (const char *)tx, sizeof(tx))
        != (int)sizeof(tx)) {
        return ESP_FAIL;
    }
    uart_wait_tx_done(PZEM_UART_PORT, pdMS_TO_TICKS(50));

    uint8_t resp[PZEM_V3_RESPONSE_BYTES];
    int n = uart_read_bytes(PZEM_UART_PORT, resp, sizeof(resp),
                            pdMS_TO_TICKS(PZEM_RESPONSE_TIMEOUT_MS));
    if (n <= 0) {
        ESP_LOGW(TAG, "v3: 0 bytes (PZEM unpowered or RX wire broken)");
        return ESP_ERR_TIMEOUT;
    }
    if (n != PZEM_V3_RESPONSE_BYTES) {
        ESP_LOGW(TAG, "v3: short response %d/%d", n, PZEM_V3_RESPONSE_BYTES);
        return ESP_FAIL;
    }
    if (resp[1] != PZEM_V3_FUNC_READ_INPUT || resp[2] != 20) {
        ESP_LOGW(TAG, "v3: bad framing addr=0x%02X func=0x%02X bc=0x%02X",
                 resp[0], resp[1], resp[2]);
        return ESP_FAIL;
    }
    uint16_t got_crc  = (uint16_t)resp[23] | ((uint16_t)resp[24] << 8);
    uint16_t want_crc = pzem_modbus_crc16(resp, 23);
    if (got_crc != want_crc) {
        ESP_LOGW(TAG, "v3: CRC mismatch got 0x%04X want 0x%04X",
                 got_crc, want_crc);
        return ESP_FAIL;
    }

    uint16_t v_raw  = ((uint16_t)resp[3]  << 8) | resp[4];
    uint32_t i_low  = ((uint32_t)resp[5]  << 8) | resp[6];
    uint32_t i_high = ((uint32_t)resp[7]  << 8) | resp[8];
    uint32_t p_low  = ((uint32_t)resp[9]  << 8) | resp[10];
    uint32_t p_high = ((uint32_t)resp[11] << 8) | resp[12];
    uint32_t e_low  = ((uint32_t)resp[13] << 8) | resp[14];
    uint32_t e_high = ((uint32_t)resp[15] << 8) | resp[16];

    out->voltage_v = v_raw / 10.0f;
    out->current_a = ((i_high << 16) | i_low) / 1000.0f;
    out->power_w   = ((p_high << 16) | p_low) / 10.0f;
    out->energy_wh = (e_high << 16) | e_low;
    out->valid     = true;
    return ESP_OK;
}

/* Upsert AC voltage / current / power / energy onto the existing
 * station_state row. The four ac_* columns must exist in Supabase or
 * PostgREST will reject the request with HTTP 400. */
static void pzem_sync_station_state(const pzem_reading_t *r)
{
    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"station_id\":\"" PZEM_STATION_ID
             "\",\"ac_voltage_v\":%.1f"
             ",\"ac_current_a\":%.3f"
             ",\"ac_power_w\":%.1f"
             ",\"ac_energy_wh\":%u}",
             (double)r->voltage_v,
             (double)r->current_a,
             (double)r->power_w,
             (unsigned)r->energy_wh);
    esp_err_t err = supabase_post_upsert(
        "/rest/v1/station_state?on_conflict=station_id", payload);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "station_state AC sync failed: %s", esp_err_to_name(err));
    }
}

/* Mirror the outlet's in-use state onto port_state so the dashboard's
 * existing outlet tile updates automatically when a load is detected.
 * Other port_state columns (current_ma, bus_voltage_v) are left alone —
 * they're sized for USB/INA219 readings, not 220 V mains. */
static void pzem_sync_outlet_status(bool in_use)
{
    char payload[160];
    snprintf(payload, sizeof(payload),
             "{\"station_id\":\"" PZEM_STATION_ID
             "\",\"port_key\":\"outlet\""
             ",\"status\":\"%s\"}",
             in_use ? "in_use" : "available");
    esp_err_t err = supabase_post_upsert(
        "/rest/v1/port_state?on_conflict=station_id,port_key", payload);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "outlet port_state sync failed: %s", esp_err_to_name(err));
    }
}

static void pzem_task(void *pvParameters)
{
    (void)pvParameters;

    supabase_init();

    /* Track the last published outlet status so we only POST port_state
     * when it actually changes — avoids 12 redundant upserts per minute. */
    bool last_outlet_in_use     = false;
    bool last_outlet_published  = false;

    while (true) {
        pzem_reading_t r;
        esp_err_t err = pzem_reader_read(&r);
        if (err == ESP_OK) {
            ESP_LOGI(TAG,
                     "V=%.1f V  I=%.3f A  P=%.1f W  E=%u Wh",
                     (double)r.voltage_v,
                     (double)r.current_a,
                     (double)r.power_w,
                     (unsigned)r.energy_wh);
            portENTER_CRITICAL(&s_last_reading_lock);
            s_last_reading = r;
            s_last_reading_valid = true;
            portEXIT_CRITICAL(&s_last_reading_lock);

            pzem_sync_station_state(&r);

            bool in_use_now = (r.power_w > PZEM_OUTLET_IN_USE_W);
            if (!last_outlet_published || in_use_now != last_outlet_in_use) {
                pzem_sync_outlet_status(in_use_now);
                last_outlet_in_use    = in_use_now;
                last_outlet_published = true;
            }
        } else {
            ESP_LOGW(TAG, "PZEM read failed: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(PZEM_READ_INTERVAL_MS));
    }
}

#endif /* PZEM_ENABLED */

esp_err_t pzem_reader_init(void)
{
#if PZEM_ENABLED
    if (s_initialized) {
        return ESP_OK;
    }

    uart_config_t cfg = {
        .baud_rate  = PZEM_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(PZEM_UART_PORT,
                                        PZEM_RX_BUFFER_BYTES,
                                        0,    /* TX buffer: blocking writes */
                                        0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_param_config(PZEM_UART_PORT, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_set_pin(PZEM_UART_PORT,
                       PZEM_UART_PIN_TX,
                       PZEM_UART_PIN_RX,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG,
             "PZEM-004T v3 UART2 ready: TX=GPIO%d RX=GPIO%d baud=%d slave=0x%02X",
             PZEM_UART_PIN_TX, PZEM_UART_PIN_RX, PZEM_UART_BAUD,
             PZEM_V3_SLAVE_ADDR);
    return ESP_OK;
#else
    ESP_LOGI(TAG, "PZEM disabled at compile time (PZEM_ENABLED=0)");
    return ESP_OK;
#endif
}

esp_err_t pzem_reader_read(pzem_reading_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

#if PZEM_ENABLED
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return pzem_v3_read(out);
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t pzem_reader_start(void)
{
#if PZEM_ENABLED
    esp_err_t err = pzem_reader_init();
    if (err != ESP_OK) {
        return err;
    }

    /* 8 KB stack: TLS handshake to Supabase needs more than the default 4 KB. */
    BaseType_t ok = xTaskCreate(pzem_task, "pzem_task", 8192, NULL, 4, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to start PZEM task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
#else
    ESP_LOGI(TAG, "pzem_reader_start: compiled out (PZEM_ENABLED=0)");
    return ESP_OK;
#endif
}

bool pzem_reader_get_last(pzem_reading_t *out)
{
    if (out == NULL) {
        return false;
    }

#if PZEM_ENABLED
    bool valid;
    portENTER_CRITICAL(&s_last_reading_lock);
    valid = s_last_reading_valid;
    if (valid) {
        *out = s_last_reading;
    }
    portEXIT_CRITICAL(&s_last_reading_lock);
    if (!valid) {
        memset(out, 0, sizeof(*out));
    }
    return valid;
#else
    memset(out, 0, sizeof(*out));
    return false;
#endif
}
