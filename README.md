# Smart Solar Hub ESP32 Firmware

Last updated: 2026-05-06

Thesis title: `Development and Implementation of Solar-Powered Smart Charging Station with Integrated Connectivity`

Station brand / SSID: `SOLAR CONNECT`

This repository contains the ESP32 firmware for the embedded control and connectivity layer of the Smart Solar Hub. The firmware connects the station hardware to Supabase and the external PWA at `https://solarconnect.live`.

The codebase is the source of truth. This README separates:
- expected thesis behavior
- behavior implemented in the current firmware
- known gaps between the two

## Repository Boundary

This repo owns:
- ESP32 Wi-Fi AP/STA setup
- captive portal and terms acceptance
- per-device Wi-Fi session timing
- RFID reader integration
- charging-port power GPIO control
- USB INA219 sensing
- AC outlet PZEM sensing
- battery ADC sensing and state machine
- Supabase telemetry publishing
- local admin/test pages

This repo does not own:
- the Vercel PWA source code
- the live Supabase project configuration outside the SQL files in `supabase/`
- the physical solar charger, BMS, inverter, or wiring outside firmware assumptions
- PWA-side CO2 calculation and dashboard rendering

## Expected Thesis Behavior

At system level, the station is expected to:
- require a valid RFID card before enabling user-facing station functionality
- disable charging ports and user Wi-Fi when no valid RFID card is present
- keep MCU telemetry Wi-Fi available while the battery has enough energy
- provide user internet through `SOLAR CONNECT` after terms acceptance
- limit each user device to 1 hour of Wi-Fi use per day
- publish user session state to Supabase for the PWA
- publish USB-A, USB-C, and AC outlet availability/in-use state to Supabase
- publish battery percentage/state to Supabase
- publish raw energy inputs so the PWA can estimate CO2 savings

Battery threshold target:

| Battery state | Trend | Charging ports | User Wi-Fi | MCU telemetry Wi-Fi |
|---|---|---:|---:|---:|
| 100% to 26% | any | on | on | on |
| 25% | falling | on | off | on |
| 15% | falling | off | off | on |
| 10% | falling | off | off | off |
| 13% | recovering | off | off | on |
| 20% | recovering | on | off | on |
| 30% | recovering | on | on | on |

## Current Firmware Behavior

### Implemented

- Wi-Fi AP named `SOLAR CONNECT`, with optional STA uplink for internet and telemetry.
- Captive portal at `http://192.168.4.1/` with terms acceptance.
- Per-client access gating using session state, `client_acl`, DNS interception, DHCP option 114, and an lwIP IPv4 input hook.
- 1-hour daily quota per MAC/device identity.
- Supabase session creation, heartbeat, disconnect, and expiry updates.
- MFRC522 RFID reader over SPI3 with two authorized 4-byte UIDs.
- RFID-controlled power GPIOs for charging hardware.
- Battery override hook that can force RFID-controlled outputs off.
- Battery ADC on GPIO 32 with Supabase `station_state` updates every 5 seconds.
- USB INA219 current sensing with event-driven `port_state` status updates.
- PZEM-004T v3 Modbus AC reader with Supabase AC telemetry and outlet status updates.
- Local `/ports`, `/port-occupied`, and `/config` admin/test surfaces.
- PWA one-time linking handoff through `https://solarconnect.live/?session_token=<token>`.

### Important Current Config

The local `.env` currently sets:

```env
PORT_SENSORS_ENABLED=1
PORT_SENSORS_SUPABASE_SYNC_ENABLED=1
PORT_IN_USE_THRESHOLD_MA=50.0
BATTERY_SENSOR_ENFORCE_THRESHOLDS=0
RFID_ENFORCE_AUTH=1
```

Meaning:
- RFID authorization is enforced.
- Port sensor reads and Supabase port sync are enabled.
- Battery telemetry is enabled.
- Battery threshold side effects are disabled in this local dev build, so the battery state machine does not currently toggle user AP or RFID port allow/block.

## Key Mismatches To Track

1. RFID currently gates charging power GPIOs only. It does not gate the user Wi-Fi AP. The expected thesis behavior says invalid/no card should disable both charging ports and user Wi-Fi.

2. Battery threshold actions are compiled but disabled by `.env` through `BATTERY_SENSOR_ENFORCE_THRESHOLDS=0`. Telemetry still runs.

3. Battery thresholds are implemented as voltage thresholds, not direct percentage thresholds. The code linearly maps 11.6 V to 0% and 13.9 V to 100%, which does not match the thesis percentage table or a real LiFePO4 discharge curve.

4. The 10% complete shutdown is not a firmware-controlled shutdown. The code relies on hardware/MPPT power loss.

5. USB sensor status is sent to Supabase, but `current_ma` and `bus_voltage_v` are not included in the current `port_sensors.c` upsert payload even though the schema has those columns and the local API exposes them.

6. Active sessions are RAM-only. Daily quota is NVS-backed, but `DEV_RESET_QUOTA_ON_BOOT=1` clears quota records on every boot for dev testing. If the ESP32 loses power due to battery shutdown and boots again, quota state clears while this flag remains `1`.

7. USB-C INA219 hardware is flagged as faulty on the current PCB revision. The firmware can attempt to read four sensors, but the live `/port-occupied` page intentionally focuses on USB-A readings.

8. Admin credentials default to `admin / admin123` unless overridden at build time.

## Eco Metrics Split

Eco metrics are estimated CO2 savings. The firmware does not need to calculate the final CO2 value.

Firmware publishes raw inputs:
- per-port `daily_in_use_seconds` in `port_state`
- `ac_energy_wh_today` in `station_state`

The PWA computes:

```text
(10 * USB-A hours + 15 * USB-C hours + acEnergyWhToday) * 0.70 = grams CO2 saved
```

Where:
- USB-A hours are derived from USB-A `daily_in_use_seconds`
- USB-C hours are derived from USB-C `daily_in_use_seconds`
- `acEnergyWhToday` comes from PZEM AC telemetry

`main/eco_metrics.c` remains a disabled/stub firmware-side accumulator and is not required for the current architecture.

## Hardware Map

### RFID MFRC522

| MFRC522 | ESP32 |
|---|---:|
| SDA/SS | GPIO 5 |
| SCK | GPIO 18 |
| MOSI | GPIO 23 |
| MISO | GPIO 19 |
| RST | GPIO 4 |
| VCC | 3.3 V |
| GND | GND |

### RFID-Controlled Power Pins

`main/rfid_reader.c` drives these pins together when an authorized card is present and the battery override allows ports:

```text
GPIO 12, GPIO 14, GPIO 27, GPIO 26, GPIO 25, GPIO 13
```

The code treats them as six power-control outputs: 4 MOSFETs, 1 SSR, and 1 relay.

### USB INA219 Sensors

I2C bus:
- SDA: GPIO 21
- SCL: GPIO 22
- frequency: 100 kHz

Current code address map:

| Port | INA219 address | MOSFET GPIO recorded in `port_sensors.h` |
|---|---:|---:|
| USB-C 1 | `0x44` | GPIO 14 |
| USB-C 2 | `0x41` | GPIO 27 |
| USB-A 1 | `0x40` | GPIO 26 |
| USB-A 2 | `0x45` | GPIO 25 |

### Battery ADC

- GPIO 32 / ADC1 channel 4
- divider ratio: `5.5454`
- current code full voltage: `13.9 V`
- current code empty voltage: `11.6 V`
- sample cadence: 5 seconds
- state transition debounce: 30 seconds

### AC PZEM

Current code uses PZEM-004T v3.0 Modbus RTU:

| Signal | ESP32 |
|---|---:|
| PZEM RX | GPIO 17 / TX2 |
| PZEM TX | GPIO 16 / RX2 |
| Baud | 9600 |
| Slave address | `0xF8` |

The older docs that mention PZEM-004T v1 and "logs only" are obsolete.

## Supabase Data Published By Firmware

### `sessions`

Published from `main/http_server.c` through `main/supabase_client.c`.

Important fields:
- `session_token`
- `device_hash`
- `remaining_seconds`
- `status`
- `ap_connected`
- `last_heartbeat`
- `session_start`
- `session_end`

### `port_state`

Published by:
- manual `/ports` test UI
- USB INA219 sync task
- PZEM outlet status sync

Current USB sensor payload includes:
- `station_id`
- `port_key`
- `status`
- `daily_in_use_seconds`

Current USB sensor payload does not include:
- `current_ma`
- `bus_voltage_v`

Those columns exist in SQL and live readings are available through local APIs, but they are not currently sent by the event-driven Supabase upsert.

PZEM outlet sync updates:
- `station_id`
- `port_key = "outlet"`
- `status = "available" | "in_use"`

### `station_state`

Battery task updates:
- `battery_percent`
- `battery_voltage_v`
- `battery_raw_mv`
- `battery_state`

PZEM task updates:
- `ac_voltage_v`
- `ac_current_a`
- `ac_power_w`
- `ac_energy_wh`
- `ac_energy_wh_today`

## Operational Threshold Implementation

Current code thresholds:

| Code event | Voltage | Approx percent using 11.6 V to 13.9 V linear map | Side effect when enforcement is enabled |
|---|---:|---:|---|
| Normal -> Warning | 12.6 V falling | 43% | user AP off, ports still allowed |
| Warning -> Critical | 11.8 V falling | 9% | user AP off, ports blocked |
| Hardware/MPPT cutoff target | 11.6 V falling | 0% | no firmware shutdown action |
| Boot low / Wake Up threshold | 12.4 V | 35% | wake-up state |
| Critical/Wake Up -> Charging On | 12.8 V rising | 52% | ports allowed, user AP still off |
| Charging On/Warning -> Normal | 13.2 V rising | 70% | user AP on, ports allowed |

This is not yet calibrated to the expected 25%, 15%, 10%, 13%, 20%, and 30% thesis thresholds.

## Build

### PlatformIO

```bash
pio run -e esp32dev
pio run -e esp32dev -t upload --upload-port COM5
pio device monitor -e esp32dev --port COM5 --baud 115200
```

### ESP-IDF

```bash
idf.py set-target esp32
idf.py build
idf.py flash monitor
```

## Boot Flow

Current `app_main()` flow:

1. initialize NVS
2. mount SPIFFS
3. load router config and port maps
4. initialize Wi-Fi AP or AP+STA
5. initialize port sensors and start port Supabase sync
6. start RFID reader
7. start PZEM reader
8. start battery sensor task
9. start network diagnostics and LED task
10. start DNS and HTTP servers when unlocked
11. initialize console commands
12. enter interactive console

## Important Routes

| Route | Purpose |
|---|---|
| `/` | captive portal / terms page |
| `/confirm` | starts or resumes a session |
| `/api/status` | JSON session status |
| `/ports` | manual port and battery test UI |
| `/port-occupied` | live USB-A port occupancy view |
| `/api/ports/sensors` | JSON sensor readings |
| `/api/ports/sensors/sync` | one-shot sensor sync |
| `/config` | admin Wi-Fi/config UI |

## Documentation Map

- `PROJECT_CONTEXT.md` - source-truth project context, current implementation, mismatches, roadmap
- `SYSTEM_ARCHITECTURE.md` - subsystem architecture and runtime flow
- `PWA_LINKING_CONTRACT.md` - PWA identity, session linking, telemetry contract, CO2 formula
- `supabase/*.sql` - schema and migration files

## Current Priorities

1. Wire RFID card presence into user AP/session eligibility if the final thesis demo requires RFID to gate user Wi-Fi.
2. Decide when to set `BATTERY_SENSOR_ENFORCE_THRESHOLDS=1` for demo/production.
3. Calibrate battery thresholds against the actual LiFePO4 battery under load.
4. Replace the linear voltage-to-percent estimator with a LiFePO4 lookup or another measured SoC method.
5. Add firmware-side shutdown/final telemetry behavior if hardware gives enough warning before MPPT cutoff.
6. Include `current_ma` and `bus_voltage_v` in USB `port_state` upserts if the PWA needs live current/voltage.
7. Set `DEV_RESET_QUOTA_ON_BOOT=0` when daily quota must survive power loss.
8. Replace default admin credentials before a public demo.

## License

This project is part of a thesis submission. See repository/license details as applicable.
