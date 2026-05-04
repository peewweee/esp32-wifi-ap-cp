#include "pzem_reader.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "pzem";

#define PZEM_UART_PORT             UART_NUM_2
#define PZEM_RX_BUFFER_BYTES       256
#define PZEM_RESPONSE_TIMEOUT_MS   200
#define PZEM_INTER_CMD_DELAY_MS    30

#if PZEM_ENABLED

static bool s_initialized;

/* PZEM-004T v1 default device address: 192.168.1.1 (4-byte IP-style). */
static uint8_t s_v1_addr[4] = { 0xC0, 0xA8, 0x01, 0x01 };

#define PZEM_V1_CMD_VOLTAGE   0xB0
#define PZEM_V1_CMD_CURRENT   0xB1
#define PZEM_V1_CMD_POWER     0xB2
#define PZEM_V1_CMD_ENERGY    0xB3

#define PZEM_V1_RESP_CODE(cmd) ((uint8_t)((cmd) - 0x10))

/* Sum the first 6 bytes mod 256. Same algorithm for TX and RX frames. */
static uint8_t pzem_v1_checksum(const uint8_t f[6])
{
    uint16_t sum = 0;
    for (int i = 0; i < 6; i++) {
        sum += f[i];
    }
    return (uint8_t)(sum & 0xFF);
}

/* Send one 7-byte command and wait for its 7-byte reply.
 * Logs diagnostic info if no response (for debugging detection issues). */
static esp_err_t pzem_v1_query(uint8_t cmd, uint8_t resp[7])
{
    uint8_t tx[7];
    tx[0] = cmd;
    tx[1] = s_v1_addr[0];
    tx[2] = s_v1_addr[1];
    tx[3] = s_v1_addr[2];
    tx[4] = s_v1_addr[3];
    tx[5] = 0x00;
    tx[6] = pzem_v1_checksum(tx);

    uart_flush_input(PZEM_UART_PORT);

    if (uart_write_bytes(PZEM_UART_PORT, (const char *)tx, sizeof(tx))
        != (int)sizeof(tx)) {
        return ESP_FAIL;
    }
    uart_wait_tx_done(PZEM_UART_PORT, pdMS_TO_TICKS(50));

    int n = uart_read_bytes(PZEM_UART_PORT, resp, 7,
                            pdMS_TO_TICKS(PZEM_RESPONSE_TIMEOUT_MS));
    if (n <= 0) {
        ESP_LOGW(TAG, "v1: cmd 0x%02X got 0 bytes (PZEM unpowered, "
                      "wrong protocol, or RX wire broken)", cmd);
        return ESP_ERR_TIMEOUT;
    }
    if (n < 7) {
        ESP_LOGW(TAG,
                 "v1: cmd 0x%02X short response %d/7: %02X %02X %02X %02X %02X %02X %02X",
                 cmd, n,
                 n > 0 ? resp[0] : 0, n > 1 ? resp[1] : 0,
                 n > 2 ? resp[2] : 0, n > 3 ? resp[3] : 0,
                 n > 4 ? resp[4] : 0, n > 5 ? resp[5] : 0,
                 n > 6 ? resp[6] : 0);
        return ESP_FAIL;
    }

    uint8_t expected = PZEM_V1_RESP_CODE(cmd);
    if (resp[0] != expected) {
        ESP_LOGW(TAG, "v1: cmd 0x%02X got resp code 0x%02X (expected 0x%02X). "
                      "Frame: %02X %02X %02X %02X %02X %02X %02X",
                 cmd, resp[0], expected,
                 resp[0], resp[1], resp[2], resp[3], resp[4], resp[5], resp[6]);
        return ESP_FAIL;
    }

    uint8_t want = pzem_v1_checksum(resp);
    if (resp[6] != want) {
        ESP_LOGW(TAG, "v1: checksum mismatch: got 0x%02X want 0x%02X",
                 resp[6], want);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void pzem_task(void *pvParameters)
{
    (void)pvParameters;

    while (true) {
        pzem_reading_t r;
        esp_err_t err = pzem_reader_read(&r);
        if (err == ESP_OK) {
            ESP_LOGI(TAG,
                     "V=%.1f V  I=%.2f A  P=%.0f W  E=%u Wh",
                     (double)r.voltage_v,
                     (double)r.current_a,
                     (double)r.power_w,
                     (unsigned)r.energy_wh);
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
             "PZEM-004T v1 UART2 ready: TX=GPIO%d RX=GPIO%d baud=%d addr=%u.%u.%u.%u",
             PZEM_UART_PIN_TX, PZEM_UART_PIN_RX, PZEM_UART_BAUD,
             s_v1_addr[0], s_v1_addr[1], s_v1_addr[2], s_v1_addr[3]);
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

    uint8_t resp[7];

    /* Voltage (0xB0 -> 0xA0). Resolution 0.1 V. */
    esp_err_t err = pzem_v1_query(PZEM_V1_CMD_VOLTAGE, resp);
    if (err != ESP_OK) {
        return err;
    }
    out->voltage_v = (float)((resp[1] << 8) | resp[2]) + (float)resp[3] / 10.0f;
    vTaskDelay(pdMS_TO_TICKS(PZEM_INTER_CMD_DELAY_MS));

    /* Current (0xB1 -> 0xA1). Resolution 0.01 A. */
    err = pzem_v1_query(PZEM_V1_CMD_CURRENT, resp);
    if (err != ESP_OK) {
        return err;
    }
    out->current_a = (float)((resp[1] << 8) | resp[2]) + (float)resp[3] / 100.0f;
    vTaskDelay(pdMS_TO_TICKS(PZEM_INTER_CMD_DELAY_MS));

    /* Power (0xB2 -> 0xA2). Resolution 1 W. */
    err = pzem_v1_query(PZEM_V1_CMD_POWER, resp);
    if (err != ESP_OK) {
        return err;
    }
    out->power_w = (float)((resp[1] << 8) | resp[2]);
    vTaskDelay(pdMS_TO_TICKS(PZEM_INTER_CMD_DELAY_MS));

    /* Energy (0xB3 -> 0xA3). Resolution 1 Wh, 24-bit. */
    err = pzem_v1_query(PZEM_V1_CMD_ENERGY, resp);
    if (err != ESP_OK) {
        return err;
    }
    out->energy_wh = ((uint32_t)resp[1] << 16)
                   | ((uint32_t)resp[2] << 8)
                   |  (uint32_t)resp[3];

    out->valid = true;
    return ESP_OK;
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

    BaseType_t ok = xTaskCreate(pzem_task, "pzem_task", 4096, NULL, 4, NULL);
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
