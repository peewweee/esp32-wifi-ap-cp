#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * USB charging port sensors (INA219 over I2C)
 * ============================================================================
 *
 * Safe default:
 *   PORT_SENSORS_ENABLED defaults to 0. In that state this module does not
 *   initialize I2C, does not configure GPIOs, does not read hardware, and does
 *   not write Supabase. The manual /ports test UI remains the source of
 *   port_state updates.
 *
 * Enable for hardware bring-up:
 *   - Set PORT_SENSORS_ENABLED=1 at build time, or in .env if using the
 *     project CMake bridge.
 *   - Optional: set PORT_SENSORS_SUPABASE_SYNC_ENABLED=0 to scan/read sensors
 *     without pushing detected statuses to Supabase.
 *
 * Out of scope for this module:
 *   - AC outlet usage detection. The schematic has no AC current sensor yet.
 *   - Battery percentage. There is no voltage divider, fuel gauge, or BMS data
 *     source wired into firmware yet.
 *   - MOSFET/relay control. Pins are recorded here for future power switching,
 *     but this module does not drive them.
 * ========================================================================= */

#ifndef PORT_SENSORS_ENABLED
#define PORT_SENSORS_ENABLED 0
#endif

#ifndef PORT_SENSORS_SUPABASE_SYNC_ENABLED
#define PORT_SENSORS_SUPABASE_SYNC_ENABLED PORT_SENSORS_ENABLED
#endif

#ifndef PORT_SENSORS_SYNC_INTERVAL_MS
#define PORT_SENSORS_SYNC_INTERVAL_MS 10000
#endif

#ifndef PORT_SENSORS_SYNC_STACK_BYTES
#define PORT_SENSORS_SYNC_STACK_BYTES 6144
#endif

/* I2C wiring: all four INA219 devices share this bus. */
#define PORT_SENSORS_I2C_PIN_SDA   21
#define PORT_SENSORS_I2C_PIN_SCL   22
#define PORT_SENSORS_I2C_FREQ_HZ   400000

/* INA219 7-bit addresses per port. Each physical module must be unique. */
#define PORT_SENSORS_INA219_ADDR_USB_C_1   0x40
#define PORT_SENSORS_INA219_ADDR_USB_C_2   0x41
#define PORT_SENSORS_INA219_ADDR_USB_A_1   0x44
#define PORT_SENSORS_INA219_ADDR_USB_A_2   0x45

/* MOSFET GPIOs recorded for future station_power work. Not driven here. */
#define PORT_SENSORS_MOSFET_GPIO_USB_C_1   14
#define PORT_SENSORS_MOSFET_GPIO_USB_C_2   27
#define PORT_SENSORS_MOSFET_GPIO_USB_A_1   26
#define PORT_SENSORS_MOSFET_GPIO_USB_A_2   25
#define PORT_SENSORS_RELAY_GPIO            13

/* Current threshold for classifying a port as in_use vs available.
 * Keep this configurable; phones near full charge may idle below/above 50 mA. */
#ifndef PORT_IN_USE_THRESHOLD_MA
#define PORT_IN_USE_THRESHOLD_MA 50.0f
#endif

typedef enum {
    PORT_SENSOR_USB_C_1 = 0,
    PORT_SENSOR_USB_C_2,
    PORT_SENSOR_USB_A_1,
    PORT_SENSOR_USB_A_2,
    PORT_SENSOR_COUNT
} port_sensor_id_t;

typedef enum {
    PORT_SENSOR_STATUS_NOT_READY = 0,
    PORT_SENSOR_STATUS_AVAILABLE,
    PORT_SENSOR_STATUS_IN_USE,
    PORT_SENSOR_STATUS_FAULT
} port_sensor_status_t;

typedef struct {
    port_sensor_id_t      id;
    const char           *port_key;     /* Supabase port_state.port_key */
    uint8_t               i2c_address;  /* INA219 address from A0/A1 jumpers */
    int                   mosfet_gpio;  /* Future use only; not driven here */
    float                 current_ma;
    float                 bus_voltage_v;
    port_sensor_status_t  status;
} port_sensor_reading_t;

/* Initialize the I2C bus, scan it, and configure the four expected INA219s.
 * With PORT_SENSORS_ENABLED=0 this is a no-op returning ESP_OK. */
esp_err_t port_sensors_init(void);

/* True when PORT_SENSORS_ENABLED=1 and the I2C bus initialized. */
bool port_sensors_is_enabled(void);

/* Diagnostic I2C scanner. Logs all responding addresses.
 * Returns ESP_ERR_NOT_SUPPORTED when PORT_SENSORS_ENABLED=0. */
esp_err_t port_sensors_scan_i2c(uint8_t *addresses,
                                size_t max_addresses,
                                size_t *out_count);

/* Read one USB port. With PORT_SENSORS_ENABLED=0, fills NOT_READY and returns
 * ESP_ERR_NOT_SUPPORTED. */
esp_err_t port_sensors_read(port_sensor_id_t id, port_sensor_reading_t *out);

/* Read all four USB ports. */
esp_err_t port_sensors_read_all(port_sensor_reading_t out[PORT_SENSOR_COUNT]);

/* Start the optional periodic Supabase sync task.
 * No-op unless PORT_SENSORS_ENABLED=1 and PORT_SENSORS_SUPABASE_SYNC_ENABLED=1. */
esp_err_t port_sensors_start_supabase_sync(void);

const char *port_sensors_port_key(port_sensor_id_t id);
const char *port_sensors_status_string(port_sensor_status_t status);

#ifdef __cplusplus
}
#endif
