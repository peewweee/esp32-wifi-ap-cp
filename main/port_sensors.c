#include "port_sensors.h"

#include "esp_log.h"
#include <string.h>

static const char *TAG = "port_sensors";

typedef struct {
    port_sensor_id_t  id;
    const char       *port_key;
    uint8_t           i2c_address;
    int               mosfet_gpio;
} port_sensor_descriptor_t;

static const port_sensor_descriptor_t s_descriptors[PORT_SENSOR_COUNT] = {
    [PORT_SENSOR_USB_C_1] = {
        .id          = PORT_SENSOR_USB_C_1,
        .port_key    = "usb_c_1",
        .i2c_address = PORT_SENSORS_INA219_ADDR_USB_C_1,
        .mosfet_gpio = PORT_SENSORS_MOSFET_GPIO_USB_C_1,
    },
    [PORT_SENSOR_USB_C_2] = {
        .id          = PORT_SENSOR_USB_C_2,
        .port_key    = "usb_c_2",
        .i2c_address = PORT_SENSORS_INA219_ADDR_USB_C_2,
        .mosfet_gpio = PORT_SENSORS_MOSFET_GPIO_USB_C_2,
    },
    [PORT_SENSOR_USB_A_1] = {
        .id          = PORT_SENSOR_USB_A_1,
        .port_key    = "usb_a_1",
        .i2c_address = PORT_SENSORS_INA219_ADDR_USB_A_1,
        .mosfet_gpio = PORT_SENSORS_MOSFET_GPIO_USB_A_1,
    },
    [PORT_SENSOR_USB_A_2] = {
        .id          = PORT_SENSOR_USB_A_2,
        .port_key    = "usb_a_2",
        .i2c_address = PORT_SENSORS_INA219_ADDR_USB_A_2,
        .mosfet_gpio = PORT_SENSORS_MOSFET_GPIO_USB_A_2,
    },
};

static bool s_initialized = false;

/* Fill *out with the descriptor identity fields and a NOT_READY status.
 * Used both by the disabled stub path and as a starting point for the
 * enabled read path before hardware data lands. */
static void fill_not_ready_reading(port_sensor_id_t id, port_sensor_reading_t *out)
{
    if (out == NULL) {
        return;
    }
    if (id >= PORT_SENSOR_COUNT) {
        memset(out, 0, sizeof(*out));
        out->status = PORT_SENSOR_STATUS_NOT_READY;
        return;
    }
    const port_sensor_descriptor_t *d = &s_descriptors[id];
    out->id            = d->id;
    out->port_key      = d->port_key;
    out->i2c_address   = d->i2c_address;
    out->mosfet_gpio   = d->mosfet_gpio;
    out->current_ma    = 0.0f;
    out->bus_voltage_v = 0.0f;
    out->status        = PORT_SENSOR_STATUS_NOT_READY;
}

#if PORT_SENSORS_ENABLED
#include "driver/i2c.h"

#define PORT_SENSORS_I2C_PORT  I2C_NUM_0

/* ----------------------------------------------------------------------------
 * INA219 driver — minimal, NOT YET IMPLEMENTED.
 *
 * Notes for whoever fills these in during hardware bring-up:
 *
 *   Registers (all 16-bit, MSB first):
 *     0x00 Configuration  — write to set BRNG / PG / SADC / BADC / mode
 *     0x01 Shunt Voltage  — signed; 10 µV per LSB
 *     0x02 Bus Voltage    — top 13 bits used; 4 mV per LSB; bit 1 = CNVR, bit 0 = OVF
 *     0x03 Power          — depends on calibration
 *     0x04 Current        — depends on calibration
 *     0x05 Calibration    — must match the shunt resistor on the carrier board
 *
 *   Read transaction:
 *     I2C write the 1-byte register pointer, then I2C read 2 bytes (MSB, LSB).
 *
 *   Common Adafruit/clone modules use a 0.1 Ω shunt; calibration value 0x1000
 *   gives ~3.2 A range and 100 µA per LSB on the Current register. Verify the
 *   actual shunt on your board before trusting any of these constants.
 *
 *   Watch for negative shunt voltages — current is signed. The Current
 *   register can also be near-zero when no load is attached.
 * -------------------------------------------------------------------------- */

static esp_err_t ina219_read_u16(uint8_t addr, uint8_t reg, uint16_t *out_value)
{
    (void)addr; (void)reg; (void)out_value;
    /* TODO: i2c_master_write_read_device(PORT_SENSORS_I2C_PORT,
     *                                    addr,
     *                                    &reg, 1,
     *                                    rx, 2,
     *                                    pdMS_TO_TICKS(50));
     *       *out_value = (rx[0] << 8) | rx[1];
     */
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t ina219_configure(uint8_t addr)
{
    (void)addr;
    /* TODO: write CONFIG (0x00) and CALIBRATION (0x05) appropriate to the
     * shunt resistor on the carrier board. */
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t i2c_bus_install(void)
{
    i2c_config_t cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = PORT_SENSORS_I2C_PIN_SDA,
        .scl_io_num       = PORT_SENSORS_I2C_PIN_SCL,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = PORT_SENSORS_I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_param_config(PORT_SENSORS_I2C_PORT, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    return i2c_driver_install(PORT_SENSORS_I2C_PORT, cfg.mode, 0, 0, 0);
}
#endif /* PORT_SENSORS_ENABLED */

esp_err_t port_sensors_init(void)
{
#if PORT_SENSORS_ENABLED
    if (s_initialized) {
        return ESP_OK;
    }
    esp_err_t err = i2c_bus_install();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C install failed: %s", esp_err_to_name(err));
        return err;
    }
    for (size_t i = 0; i < PORT_SENSOR_COUNT; i++) {
        esp_err_t cfg_err = ina219_configure(s_descriptors[i].i2c_address);
        if (cfg_err != ESP_OK) {
            ESP_LOGW(TAG, "INA219 0x%02X (%s) did not respond: %s",
                     s_descriptors[i].i2c_address,
                     s_descriptors[i].port_key,
                     esp_err_to_name(cfg_err));
        }
    }
    s_initialized = true;
    ESP_LOGI(TAG, "port sensors initialized (PORT_SENSORS_ENABLED=1)");
    return ESP_OK;
#else
    ESP_LOGI(TAG,
             "port sensors disabled at compile time (PORT_SENSORS_ENABLED=0); "
             "manual /ports test UI remains the source of port_state");
    return ESP_OK;
#endif
}

bool port_sensors_is_enabled(void)
{
#if PORT_SENSORS_ENABLED
    return s_initialized;
#else
    return false;
#endif
}

esp_err_t port_sensors_read(port_sensor_id_t id, port_sensor_reading_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (id >= PORT_SENSOR_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

#if PORT_SENSORS_ENABLED
    if (!s_initialized) {
        fill_not_ready_reading(id, out);
        return ESP_ERR_INVALID_STATE;
    }

    fill_not_ready_reading(id, out);
    out->status = PORT_SENSOR_STATUS_FAULT;

    const port_sensor_descriptor_t *d = &s_descriptors[id];

    uint16_t bus_raw = 0;
    uint16_t cur_raw = 0;
    esp_err_t err = ina219_read_u16(d->i2c_address, 0x02, &bus_raw);
    if (err != ESP_OK) {
        return err;
    }
    err = ina219_read_u16(d->i2c_address, 0x04, &cur_raw);
    if (err != ESP_OK) {
        return err;
    }

    /* TODO: convert raw -> mA / V using the calibration constants once the
     * shunt and CALIBRATION register are finalized. The expressions below
     * are placeholders — do not trust them until the math is verified
     * against the actual shunt resistor on the carrier board. */
    out->bus_voltage_v = ((float)((bus_raw >> 3) & 0x1FFF)) * 0.004f;
    out->current_ma    = (float)((int16_t)cur_raw) * 0.1f;  /* assumes 100 µA/LSB */

    out->status = (out->current_ma >= PORT_IN_USE_THRESHOLD_MA)
                      ? PORT_SENSOR_STATUS_IN_USE
                      : PORT_SENSOR_STATUS_AVAILABLE;
    return ESP_OK;
#else
    fill_not_ready_reading(id, out);
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t port_sensors_read_all(port_sensor_reading_t out[PORT_SENSOR_COUNT])
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
#if PORT_SENSORS_ENABLED
    esp_err_t first_err = ESP_OK;
    for (size_t i = 0; i < PORT_SENSOR_COUNT; i++) {
        esp_err_t e = port_sensors_read((port_sensor_id_t)i, &out[i]);
        if (e != ESP_OK && first_err == ESP_OK) {
            first_err = e;
        }
    }
    return first_err;
#else
    for (size_t i = 0; i < PORT_SENSOR_COUNT; i++) {
        fill_not_ready_reading((port_sensor_id_t)i, &out[i]);
    }
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

const char *port_sensors_port_key(port_sensor_id_t id)
{
    if (id >= PORT_SENSOR_COUNT) {
        return NULL;
    }
    return s_descriptors[id].port_key;
}

const char *port_sensors_status_string(port_sensor_status_t status)
{
    switch (status) {
        case PORT_SENSOR_STATUS_AVAILABLE: return "available";
        case PORT_SENSOR_STATUS_IN_USE:    return "in_use";
        case PORT_SENSOR_STATUS_FAULT:     return "fault";
        case PORT_SENSOR_STATUS_NOT_READY:
        default:                           return "offline";
    }
}
