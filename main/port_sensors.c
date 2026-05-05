#include "port_sensors.h"

#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

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

static void fill_not_ready_reading(port_sensor_id_t id, port_sensor_reading_t *out)
{
    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->status = PORT_SENSOR_STATUS_NOT_READY;

    if (id >= PORT_SENSOR_COUNT) {
        return;
    }

    const port_sensor_descriptor_t *d = &s_descriptors[id];
    out->id          = d->id;
    out->port_key    = d->port_key;
    out->i2c_address = d->i2c_address;
    out->mosfet_gpio = d->mosfet_gpio;
}

#if PORT_SENSORS_ENABLED

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "supabase_client.h"

#define PORT_SENSORS_I2C_PORT I2C_NUM_0
#define PORT_SENSORS_I2C_TIMEOUT_MS 50

#define INA219_REG_CONFIG      0x00
#define INA219_REG_BUS_VOLTAGE 0x02
#define INA219_REG_CURRENT     0x04
#define INA219_REG_CALIBRATION 0x05

/* Common INA219 module setup for a 0.1 ohm shunt:
 * - 32 V bus range
 * - +/-320 mV shunt range
 * - 12-bit bus and shunt ADC, one sample
 * - continuous shunt + bus measurements
 *
 * Calibration 4096 gives a current LSB of 100 uA, so Current register LSB is
 * 0.1 mA. Verify the actual shunt resistor before relying on absolute current.
 */
#ifndef PORT_SENSORS_INA219_CONFIG
#define PORT_SENSORS_INA219_CONFIG 0x399F
#endif

#ifndef PORT_SENSORS_INA219_CALIBRATION
#define PORT_SENSORS_INA219_CALIBRATION 4096
#endif

#ifndef PORT_SENSORS_INA219_CURRENT_LSB_MA
#define PORT_SENSORS_INA219_CURRENT_LSB_MA 0.1f
#endif

#define INA219_BUS_VOLTAGE_LSB_V 0.004f

#ifndef PORT_SENSORS_STATION_ID
#define PORT_SENSORS_STATION_ID "solar-hub-01"
#endif

static bool s_initialized;
static bool s_i2c_installed;
static bool s_sensor_present[PORT_SENSOR_COUNT];
#if PORT_SENSORS_SUPABASE_SYNC_ENABLED
static bool s_sync_task_started;
/* Per-port event-driven sync state. Reset to NOT_READY/0 so the very first
 * good reading always counts as a status change and gets pushed. */
static port_sensor_status_t s_last_pushed_status[PORT_SENSOR_COUNT];
static int64_t s_last_pushed_ts_ms[PORT_SENSOR_COUNT];

/* Per-port "seconds in_use today" accumulator. Used by the dashboard to
 * estimate energy delivered per port (Pavg × hours_in_use). Reset at UTC
 * midnight to match the dashboard's "today" boundary. Lives in RAM only —
 * an ESP32 reboot mid-day will reset to 0; documented as a known limit. */
static uint32_t s_daily_in_use_seconds[PORT_SENSOR_COUNT];
static int64_t  s_last_accum_ts_ms[PORT_SENSOR_COUNT];
static int      s_utc_day_anchor = -1;   /* epoch_days of the active day */
#endif

/* Read SDA/SCL with the I2C driver detached. With internal pull-ups
 * enabled, both lines should read HIGH when idle. If a line reads LOW
 * here, the bus is held low by something (stuck device, PCB short to
 * GND, or damaged GPIO). Logged for diagnostic clarity. */
static void log_bus_idle_state(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PORT_SENSORS_I2C_PIN_SDA) |
                        (1ULL << PORT_SENSORS_I2C_PIN_SCL),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    /* Brief settle so internal pull-ups have time to drag the line up. */
    vTaskDelay(pdMS_TO_TICKS(2));

    int sda = gpio_get_level(PORT_SENSORS_I2C_PIN_SDA);
    int scl = gpio_get_level(PORT_SENSORS_I2C_PIN_SCL);
    ESP_LOGI(TAG, "bus idle state with internal pull-ups: SDA=%d SCL=%d",
             sda, scl);
    if (sda == 0) {
        ESP_LOGW(TAG, "SDA is stuck LOW — bus shorted to GND, held by a "
                      "stuck device, or GPIO%d damaged",
                 PORT_SENSORS_I2C_PIN_SDA);
    }
    if (scl == 0) {
        ESP_LOGW(TAG, "SCL is stuck LOW — bus shorted to GND, held by a "
                      "stuck device, or GPIO%d damaged",
                 PORT_SENSORS_I2C_PIN_SCL);
    }
}

static esp_err_t i2c_bus_install(void)
{
    if (s_i2c_installed) {
        return ESP_OK;
    }

    log_bus_idle_state();

    /* Internal pull-ups disabled: this board has strong external pull-ups
     * (the INA219 breakouts pull SDA/SCL up to their VCC = 5 V rail).
     * Adding the ESP32's internal 45 kohm pull-up to 3.3 V on top would
     * just leak current through the GPIO clamp diodes. */
    i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PORT_SENSORS_I2C_PIN_SDA,
        .scl_io_num = PORT_SENSORS_I2C_PIN_SCL,
        .sda_pullup_en = GPIO_PULLUP_DISABLE,
        .scl_pullup_en = GPIO_PULLUP_DISABLE,
        .master.clk_speed = PORT_SENSORS_I2C_FREQ_HZ,
    };

    esp_err_t err = i2c_param_config(PORT_SENSORS_I2C_PORT, &cfg);
    if (err != ESP_OK) {
        return err;
    }

    err = i2c_driver_install(PORT_SENSORS_I2C_PORT, cfg.mode, 0, 0, 0);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "I2C driver was already installed on port %d", PORT_SENSORS_I2C_PORT);
        err = ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    s_i2c_installed = true;
    ESP_LOGI(TAG, "I2C master ready: SDA=GPIO%d SCL=GPIO%d freq=%d Hz",
             PORT_SENSORS_I2C_PIN_SDA,
             PORT_SENSORS_I2C_PIN_SCL,
             PORT_SENSORS_I2C_FREQ_HZ);
    return ESP_OK;
}

static esp_err_t i2c_probe_address(uint8_t addr)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = i2c_master_start(cmd);
    if (err == ESP_OK) {
        err = i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    }
    if (err == ESP_OK) {
        err = i2c_master_stop(cmd);
    }
    if (err == ESP_OK) {
        err = i2c_master_cmd_begin(PORT_SENSORS_I2C_PORT,
                                   cmd,
                                   pdMS_TO_TICKS(PORT_SENSORS_I2C_TIMEOUT_MS));
    }

    i2c_cmd_link_delete(cmd);
    return err;
}

static esp_err_t ina219_write_u16(uint8_t addr, uint8_t reg, uint16_t value)
{
    const uint8_t tx[3] = {
        reg,
        (uint8_t)(value >> 8),
        (uint8_t)(value & 0xFF),
    };

    return i2c_master_write_to_device(PORT_SENSORS_I2C_PORT,
                                      addr,
                                      tx,
                                      sizeof(tx),
                                      pdMS_TO_TICKS(PORT_SENSORS_I2C_TIMEOUT_MS));
}

static esp_err_t ina219_read_u16(uint8_t addr, uint8_t reg, uint16_t *out_value)
{
    uint8_t rx[2] = {0};

    if (out_value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = i2c_master_write_read_device(PORT_SENSORS_I2C_PORT,
                                                 addr,
                                                 &reg,
                                                 1,
                                                 rx,
                                                 sizeof(rx),
                                                 pdMS_TO_TICKS(PORT_SENSORS_I2C_TIMEOUT_MS));
    if (err != ESP_OK) {
        return err;
    }

    *out_value = ((uint16_t)rx[0] << 8) | rx[1];
    return ESP_OK;
}

static esp_err_t ina219_configure(uint8_t addr)
{
    esp_err_t err = ina219_write_u16(addr,
                                     INA219_REG_CONFIG,
                                     PORT_SENSORS_INA219_CONFIG);
    if (err != ESP_OK) {
        return err;
    }

    err = ina219_write_u16(addr,
                           INA219_REG_CALIBRATION,
                           PORT_SENSORS_INA219_CALIBRATION);
    if (err != ESP_OK) {
        return err;
    }

    uint16_t config_readback = 0;
    err = ina219_read_u16(addr, INA219_REG_CONFIG, &config_readback);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG,
             "INA219 0x%02X configured: config=0x%04X cal=0x%04X current_lsb=%.3f mA",
             addr,
             config_readback,
             PORT_SENSORS_INA219_CALIBRATION,
             (double)PORT_SENSORS_INA219_CURRENT_LSB_MA);
    return ESP_OK;
}

static esp_err_t ensure_sensor_ready(port_sensor_id_t id)
{
    const port_sensor_descriptor_t *d = &s_descriptors[id];

    if (s_sensor_present[id]) {
        return ESP_OK;
    }

    esp_err_t err = i2c_probe_address(d->i2c_address);
    if (err != ESP_OK) {
        return err;
    }

    err = ina219_configure(d->i2c_address);
    if (err == ESP_OK) {
        s_sensor_present[id] = true;
    }
    return err;
}

static void log_reading(const port_sensor_reading_t *reading)
{
    if (reading == NULL) {
        return;
    }

    ESP_LOGI(TAG,
             "%s addr=0x%02X current=%.1f mA bus=%.3f V -> %s",
             reading->port_key,
             reading->i2c_address,
             (double)reading->current_ma,
             (double)reading->bus_voltage_v,
             port_sensors_status_string(reading->status));
}

static esp_err_t sync_reading_to_supabase(const port_sensor_reading_t *reading,
                                          uint32_t daily_in_use_seconds)
{
    char payload[224];
    const char *status;

    if (reading == NULL || reading->port_key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    status = port_sensors_status_string(reading->status);
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(payload,
             sizeof(payload),
             "{\"station_id\":\"" PORT_SENSORS_STATION_ID
             "\",\"port_key\":\"%s\",\"status\":\"%s\""
             ",\"daily_in_use_seconds\":%u}",
             reading->port_key,
             status,
             (unsigned)daily_in_use_seconds);

    ESP_LOGI(TAG,
             "Supabase port_state sync: %s status=%s current=%.1f mA",
             reading->port_key,
             status,
             (double)reading->current_ma);

    esp_err_t err = supabase_post_upsert(
        "/rest/v1/port_state?on_conflict=station_id,port_key",
        payload);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Supabase sync failed for %s (%s): %s",
                 reading->port_key,
                 status,
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Supabase sync ok for %s", reading->port_key);
    return ESP_OK;
}

#if PORT_SENSORS_SUPABASE_SYNC_ENABLED
/* Roll the daily counters over at UTC midnight. time(NULL) is wall-clock
 * UTC seconds once SNTP has synced; for the first ~5 s after boot it can
 * still be in 1970, in which case epoch_days is 0 — close enough not to
 * matter (we'll pick up the real day on the next sync). */
static void update_daily_anchor_and_maybe_reset(void)
{
    time_t now = time(NULL);
    int day_now = (int)(now / 86400);
    if (day_now == s_utc_day_anchor) {
        return;
    }
    if (s_utc_day_anchor < 0) {
        ESP_LOGI(TAG, "daily counters anchored to UTC day %d", day_now);
    } else {
        ESP_LOGI(TAG,
                 "UTC midnight rollover: %d -> %d, resetting per-port in_use seconds",
                 s_utc_day_anchor, day_now);
        for (size_t i = 0; i < PORT_SENSOR_COUNT; i++) {
            s_daily_in_use_seconds[i] = 0;
        }
    }
    s_utc_day_anchor = day_now;
}

static void port_sensors_sync_task(void *pvParameters)
{
    (void)pvParameters;

    supabase_init();

    for (size_t i = 0; i < PORT_SENSOR_COUNT; i++) {
        s_last_pushed_status[i]  = PORT_SENSOR_STATUS_NOT_READY;
        s_last_pushed_ts_ms[i]   = 0;
        s_daily_in_use_seconds[i] = 0;
        s_last_accum_ts_ms[i]    = 0;
    }

    const TickType_t poll_delay_ticks =
        pdMS_TO_TICKS(PORT_SENSORS_POLL_INTERVAL_MS > 0
                          ? PORT_SENSORS_POLL_INTERVAL_MS
                          : 500);

    ESP_LOGI(TAG,
             "port sensor Supabase sync task started; poll=%d ms heartbeat=%d ms threshold=%.1f mA",
             PORT_SENSORS_POLL_INTERVAL_MS,
             PORT_SENSORS_HEARTBEAT_INTERVAL_MS,
             (double)PORT_IN_USE_THRESHOLD_MA);

    while (true) {
        port_sensor_reading_t readings[PORT_SENSOR_COUNT];
        port_sensors_read_all(readings);

        const int64_t now_ms = esp_timer_get_time() / 1000;
        update_daily_anchor_and_maybe_reset();

        for (size_t i = 0; i < PORT_SENSOR_COUNT; i++) {
            if (!s_sensor_present[i]) {
                continue;
            }
            if (readings[i].status != PORT_SENSOR_STATUS_AVAILABLE &&
                readings[i].status != PORT_SENSOR_STATUS_IN_USE) {
                continue;
            }

            /* Accumulate elapsed seconds while the port is in_use. dt is
             * computed from the previous poll's timestamp so a varying
             * poll interval (or a missed cycle) doesn't bias the total. */
            if (readings[i].status == PORT_SENSOR_STATUS_IN_USE &&
                s_last_accum_ts_ms[i] != 0) {
                int64_t dt_ms = now_ms - s_last_accum_ts_ms[i];
                if (dt_ms > 0 && dt_ms < 60000) {
                    s_daily_in_use_seconds[i] += (uint32_t)(dt_ms / 1000);
                }
            }
            s_last_accum_ts_ms[i] = now_ms;

            const bool changed =
                (readings[i].status != s_last_pushed_status[i]);
            const bool heartbeat_due =
                (now_ms - s_last_pushed_ts_ms[i]) >=
                PORT_SENSORS_HEARTBEAT_INTERVAL_MS;

            if (!changed && !heartbeat_due) {
                continue;
            }

            esp_err_t err = sync_reading_to_supabase(&readings[i],
                                                     s_daily_in_use_seconds[i]);
            if (err == ESP_OK) {
                s_last_pushed_status[i] = readings[i].status;
                s_last_pushed_ts_ms[i]  = now_ms;
            }
            /* On failure leave last_pushed_* alone so the next poll retries. */
        }

        vTaskDelay(poll_delay_ticks);
    }
}
#endif

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

    port_sensors_scan_i2c(NULL, 0, NULL);

    for (size_t i = 0; i < PORT_SENSOR_COUNT; i++) {
        const port_sensor_descriptor_t *d = &s_descriptors[i];
        esp_err_t probe_err = i2c_probe_address(d->i2c_address);
        if (probe_err != ESP_OK) {
            s_sensor_present[i] = false;
            ESP_LOGW(TAG,
                     "expected INA219 missing: %s at 0x%02X (%s)",
                     d->port_key,
                     d->i2c_address,
                     esp_err_to_name(probe_err));
            continue;
        }

        esp_err_t cfg_err = ina219_configure(d->i2c_address);
        s_sensor_present[i] = (cfg_err == ESP_OK);
        if (cfg_err != ESP_OK) {
            ESP_LOGW(TAG,
                     "INA219 configure failed: %s at 0x%02X (%s)",
                     d->port_key,
                     d->i2c_address,
                     esp_err_to_name(cfg_err));
        } else {
            ESP_LOGI(TAG,
                     "mapped %s -> INA219 0x%02X (MOSFET GPIO%d recorded only)",
                     d->port_key,
                     d->i2c_address,
                     d->mosfet_gpio);
        }
    }

    s_initialized = true;
    ESP_LOGI(TAG,
             "port sensors initialized; threshold=%.1f mA, Supabase periodic sync=%s",
             (double)PORT_IN_USE_THRESHOLD_MA,
             PORT_SENSORS_SUPABASE_SYNC_ENABLED ? "enabled" : "disabled");
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

esp_err_t port_sensors_scan_i2c(uint8_t *addresses,
                                size_t max_addresses,
                                size_t *out_count)
{
    if (out_count != NULL) {
        *out_count = 0;
    }

#if PORT_SENSORS_ENABLED
    esp_err_t err = i2c_bus_install();
    if (err != ESP_OK) {
        return err;
    }

    size_t count = 0;
    ESP_LOGI(TAG, "I2C scan starting on SDA=GPIO%d SCL=GPIO%d",
             PORT_SENSORS_I2C_PIN_SDA,
             PORT_SENSORS_I2C_PIN_SCL);

    for (uint8_t addr = 0x03; addr <= 0x77; addr++) {
        if (i2c_probe_address(addr) == ESP_OK) {
            ESP_LOGI(TAG, "I2C device detected at 0x%02X", addr);
            if (addresses != NULL && count < max_addresses) {
                addresses[count] = addr;
            }
            count++;
        }
    }

    if (out_count != NULL) {
        *out_count = count;
    }

    if (count == 0) {
        ESP_LOGW(TAG, "I2C scan finished: no devices detected");
    } else {
        ESP_LOGI(TAG, "I2C scan finished: %u device(s) detected", (unsigned)count);
    }

    return ESP_OK;
#else
    (void)addresses;
    (void)max_addresses;
    ESP_LOGI(TAG, "I2C scan skipped because PORT_SENSORS_ENABLED=0");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t port_sensors_read(port_sensor_id_t id, port_sensor_reading_t *out)
{
    if (out == NULL || id >= PORT_SENSOR_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    fill_not_ready_reading(id, out);

#if PORT_SENSORS_ENABLED
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    const port_sensor_descriptor_t *d = &s_descriptors[id];
    out->status = PORT_SENSOR_STATUS_FAULT;

    esp_err_t err = ensure_sensor_ready(id);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "%s INA219 0x%02X not ready: %s",
                 d->port_key,
                 d->i2c_address,
                 esp_err_to_name(err));
        log_reading(out);
        return err;
    }

    /* Re-write calibration before current reads. Some INA219 reset paths clear
     * CALIBRATION, which makes the Current register read as zero. */
    err = ina219_write_u16(d->i2c_address,
                           INA219_REG_CALIBRATION,
                           PORT_SENSORS_INA219_CALIBRATION);
    if (err != ESP_OK) {
        s_sensor_present[id] = false;
        log_reading(out);
        return err;
    }

    uint16_t bus_raw = 0;
    uint16_t current_raw = 0;
    err = ina219_read_u16(d->i2c_address, INA219_REG_BUS_VOLTAGE, &bus_raw);
    if (err != ESP_OK) {
        s_sensor_present[id] = false;
        log_reading(out);
        return err;
    }

    err = ina219_read_u16(d->i2c_address, INA219_REG_CURRENT, &current_raw);
    if (err != ESP_OK) {
        s_sensor_present[id] = false;
        log_reading(out);
        return err;
    }

    out->bus_voltage_v = (float)((bus_raw >> 3) & 0x1FFF) * INA219_BUS_VOLTAGE_LSB_V;
    out->current_ma = (float)((int16_t)current_raw) * PORT_SENSORS_INA219_CURRENT_LSB_MA;
    out->status = (out->current_ma > PORT_IN_USE_THRESHOLD_MA)
                      ? PORT_SENSOR_STATUS_IN_USE
                      : PORT_SENSOR_STATUS_AVAILABLE;

    if ((bus_raw & 0x0001) != 0) {
        ESP_LOGW(TAG, "%s INA219 math overflow flag is set", d->port_key);
    }
    if (out->current_ma < -PORT_IN_USE_THRESHOLD_MA) {
        ESP_LOGW(TAG,
                 "%s current is negative; check INA219 VIN+/VIN- orientation if this is unexpected",
                 d->port_key);
    }

    log_reading(out);
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t port_sensors_read_all(port_sensor_reading_t out[PORT_SENSOR_COUNT])
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t first_err = ESP_OK;
    for (size_t i = 0; i < PORT_SENSOR_COUNT; i++) {
        esp_err_t err = port_sensors_read((port_sensor_id_t)i, &out[i]);
        if (err != ESP_OK && first_err == ESP_OK) {
            first_err = err;
        }
    }
    return first_err;
}

esp_err_t port_sensors_sync_once(void)
{
#if PORT_SENSORS_ENABLED
    port_sensor_reading_t readings[PORT_SENSOR_COUNT];
    esp_err_t first_err = port_sensors_read_all(readings);

    for (size_t i = 0; i < PORT_SENSOR_COUNT; i++) {
        /* Only push real availability signals: AVAILABLE or IN_USE.
         * Skip ports whose INA219 wasn't detected at boot, ports that
         * read FAULT, and anything still NOT_READY — none of those
         * should overwrite a healthy row in Supabase. */
        if (!s_sensor_present[i]) {
            continue;
        }
        if (readings[i].status != PORT_SENSOR_STATUS_AVAILABLE &&
            readings[i].status != PORT_SENSOR_STATUS_IN_USE) {
            continue;
        }

        /* Manual one-shot from the admin /ports test UI: emit the current
         * accumulator value, so it doesn't overwrite the real counter
         * with 0 when SYNC_ENABLED is on. When SYNC_ENABLED is off the
         * counter stays at 0 and that's fine. */
#if PORT_SENSORS_SUPABASE_SYNC_ENABLED
        uint32_t seconds = s_daily_in_use_seconds[i];
#else
        uint32_t seconds = 0;
#endif
        esp_err_t err = sync_reading_to_supabase(&readings[i], seconds);
        if (err != ESP_OK && first_err == ESP_OK) {
            first_err = err;
        }
    }

    return first_err;
#else
    ESP_LOGI(TAG, "port sensor Supabase sync skipped because PORT_SENSORS_ENABLED=0");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t port_sensors_start_supabase_sync(void)
{
#if PORT_SENSORS_ENABLED && PORT_SENSORS_SUPABASE_SYNC_ENABLED
    if (s_sync_task_started) {
        return ESP_OK;
    }

    esp_err_t err = port_sensors_init();
    if (err != ESP_OK) {
        return err;
    }

    BaseType_t ok = xTaskCreate(port_sensors_sync_task,
                                "port_sync",
                                PORT_SENSORS_SYNC_STACK_BYTES,
                                NULL,
                                4,
                                NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to start port sensor Supabase sync task");
        return ESP_ERR_NO_MEM;
    }

    s_sync_task_started = true;
    return ESP_OK;
#else
    ESP_LOGI(TAG,
             "port sensor Supabase periodic sync not started "
             "(PORT_SENSORS_ENABLED=%d, PORT_SENSORS_SUPABASE_SYNC_ENABLED=%d)",
             PORT_SENSORS_ENABLED,
             PORT_SENSORS_SUPABASE_SYNC_ENABLED);
    return ESP_OK;
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
        case PORT_SENSOR_STATUS_AVAILABLE:
            return "available";
        case PORT_SENSOR_STATUS_IN_USE:
            return "in_use";
        case PORT_SENSOR_STATUS_FAULT:
            return "fault";
        case PORT_SENSOR_STATUS_NOT_READY:
        default:
            return "offline";
    }
}
