#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * USB charging port sensors (INA219 over I2C) — preparation module
 * ============================================================================
 *
 * This module is intentionally INACTIVE. The compile-time feature flag
 * PORT_SENSORS_ENABLED defaults to 0, which means:
 *   - port_sensors_init()        is a no-op that logs "disabled".
 *   - port_sensors_read*()       returns NOT_READY without touching hardware.
 *   - The I2C bus is NOT initialized.
 *   - The MOSFET GPIOs are NOT configured.
 *   - No runtime behavior changes; the captive portal and the /ports test
 *     UI continue to work exactly as before.
 *
 * It exists so that future hardware bring-up only needs to:
 *   1. Define PORT_SENSORS_ENABLED to 1 (here or via build flag).
 *   2. Fill in the INA219 register-read TODOs in port_sensors.c.
 *   3. Verify each module's physical A0/A1 jumper matches the address
 *      below; otherwise multiple INA219s on the bus will collide on 0x40.
 *   4. Wire calls to port_sensors_read_all() into a periodic task that
 *      pushes statuses to Supabase (replacing the manual /ports toggles
 *      at that point — but keep the toggles available as an override).
 *
 * Out of scope (intentionally not implemented):
 *   - AC outlet usage detection. The current schematic has no current
 *     sensor on the AC outlet; "outlet" port_state stays driven by the
 *     manual /ports toggle until that hardware is added.
 *   - Battery percentage. The current schematic has no voltage divider
 *     or fuel gauge; battery_percent stays driven by the manual slider.
 *   - MOSFET / relay control. Pins are recorded here for reference, but
 *     active power gating belongs in a separate station_power module.
 * ========================================================================= */

#ifndef PORT_SENSORS_ENABLED
#define PORT_SENSORS_ENABLED 0
#endif

/* ----------------------------------------------------------------------------
 * I2C wiring (shared bus across all four INA219s)
 * -------------------------------------------------------------------------- */
#define PORT_SENSORS_I2C_PIN_SDA   21   /* ESP32 GPIO21 -> SDA */
#define PORT_SENSORS_I2C_PIN_SCL   22   /* ESP32 GPIO22 -> SCL */
#define PORT_SENSORS_I2C_FREQ_HZ   400000  /* INA219 supports up to 2.94 MHz; 400 kHz is the safe default */

/* ----------------------------------------------------------------------------
 * INA219 7-bit addresses per port.
 * IMPORTANT: each physical INA219 module must be jumpered to a unique address.
 * Defaults to 0x40 from the factory; multiple defaults on one bus will collide.
 * -------------------------------------------------------------------------- */
#define PORT_SENSORS_INA219_ADDR_USB_C_1   0x40
#define PORT_SENSORS_INA219_ADDR_USB_C_2   0x41
#define PORT_SENSORS_INA219_ADDR_USB_A_1   0x44
#define PORT_SENSORS_INA219_ADDR_USB_A_2   0x45

/* ----------------------------------------------------------------------------
 * MOSFET GPIOs (recorded for the future station_power module).
 * Not driven by this module.
 * -------------------------------------------------------------------------- */
#define PORT_SENSORS_MOSFET_GPIO_USB_C_1   14   /* MOSFET_1 */
#define PORT_SENSORS_MOSFET_GPIO_USB_C_2   27   /* MOSFET_2 */
#define PORT_SENSORS_MOSFET_GPIO_USB_A_1   26   /* MOSFET_3 */
#define PORT_SENSORS_MOSFET_GPIO_USB_A_2   25   /* MOSFET_4 */
#define PORT_SENSORS_RELAY_GPIO            13   /* AC inverter / main relay; not a port sensor */

/* ----------------------------------------------------------------------------
 * Threshold for classifying a port as in_use vs available.
 * Anything at or above the threshold is treated as a device drawing current.
 * -------------------------------------------------------------------------- */
#define PORT_IN_USE_THRESHOLD_MA   50.0f

/* ----------------------------------------------------------------------------
 * Public types
 * -------------------------------------------------------------------------- */
typedef enum {
    PORT_SENSOR_USB_C_1 = 0,
    PORT_SENSOR_USB_C_2,
    PORT_SENSOR_USB_A_1,
    PORT_SENSOR_USB_A_2,
    PORT_SENSOR_COUNT
} port_sensor_id_t;

typedef enum {
    PORT_SENSOR_STATUS_NOT_READY = 0,  /* module disabled or no successful read yet */
    PORT_SENSOR_STATUS_AVAILABLE,
    PORT_SENSOR_STATUS_IN_USE,
    PORT_SENSOR_STATUS_FAULT
} port_sensor_status_t;

typedef struct {
    port_sensor_id_t      id;
    const char           *port_key;     /* matches Supabase port_state.port_key */
    uint8_t               i2c_address;  /* INA219 address — must match A0/A1 jumpers */
    int                   mosfet_gpio;  /* recorded for future station_power use; not driven here */
    float                 current_ma;
    float                 bus_voltage_v;
    port_sensor_status_t  status;
} port_sensor_reading_t;

/* ----------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/* Initialize the I2C bus and probe each INA219.
 * When PORT_SENSORS_ENABLED is 0 this is a harmless no-op that returns ESP_OK.
 * Safe to call from app_main() at any time. */
esp_err_t port_sensors_init(void);

/* True if PORT_SENSORS_ENABLED is 1 AND init has succeeded. */
bool port_sensors_is_enabled(void);

/* Read one port. When disabled, fills *out with NOT_READY and returns
 * ESP_ERR_NOT_SUPPORTED. */
esp_err_t port_sensors_read(port_sensor_id_t id, port_sensor_reading_t *out);

/* Read all four ports. When disabled, fills the array with NOT_READY
 * entries and returns ESP_ERR_NOT_SUPPORTED. */
esp_err_t port_sensors_read_all(port_sensor_reading_t out[PORT_SENSOR_COUNT]);

/* Helpers — also valid in the disabled state. */
const char *port_sensors_port_key(port_sensor_id_t id);
const char *port_sensors_status_string(port_sensor_status_t status);

#ifdef __cplusplus
}
#endif
