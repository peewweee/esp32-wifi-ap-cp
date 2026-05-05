# Project Context

Last reviewed: 2026-05-06

Review basis:
- current workspace contents
- root build/config files
- all authored firmware code under `main/` and `components/`
- Supabase SQL files under `supabase/`
- current local `.env` values, with secrets ignored

The codebase is the source of truth. This document records what the firmware actually does today, then calls out where that differs from the expected thesis-level system behavior.

## Thesis/Product Framing

Title:

`Development and Implementation of Solar-Powered Smart Charging Station with Integrated Connectivity`

User-facing brand:

`SOLAR CONNECT`

Product concept:

`The Smart Solar Hub`

System goal:

An off-grid solar charging station that provides managed Wi-Fi access, RFID-controlled charging access, live station telemetry, and a PWA dashboard backed by Supabase.

## Repo Boundary

This repository implements the ESP32 firmware layer:
- Wi-Fi AP/STA connectivity
- captive portal and terms acceptance
- per-device Wi-Fi session timing
- RFID reader logic
- charging hardware GPIO control
- USB current sensing with INA219
- AC outlet sensing with PZEM
- battery voltage sensing and state machine
- Supabase telemetry publishing

External to this repo:
- Vercel PWA source code at `C:\Users\Phoebe Rhone Gangoso\Downloads\solar-powered-charging-station-pwa`
- live Supabase project settings and policies outside the SQL files committed here
- physical solar/BMS/inverter/charging hardware
- PWA-side estimated CO2 calculation and dashboard UI

## Expected System Behavior

The expected thesis behavior is:

1. A valid RFID card activates full station service:
   - charging ports enabled
   - user Wi-Fi enabled
   - MCU telemetry remains enabled

2. No card or an invalid card disables user-facing station service:
   - charging ports disabled
   - user Wi-Fi disabled
   - MCU telemetry Wi-Fi remains active unless battery is below shutdown threshold

3. Captive portal controls internet access:
   - users connect to `SOLAR CONNECT`
   - users accept terms
   - internet is granted through the ESP32 router
   - each device gets 1 hour per day

4. Firmware publishes raw station inputs to Supabase:
   - sessions and remaining Wi-Fi time
   - per-port availability/in-use state
   - per-port daily in-use seconds
   - AC energy today
   - battery percent/state

5. PWA computes estimated CO2 savings:

```text
(10 * USB-A hours + 15 * USB-C hours + acEnergyWhToday) * 0.70 = grams CO2 saved
```

6. Battery thresholds enforce progressive degradation:

| Battery state | Trend | Charging ports | User Wi-Fi | MCU telemetry Wi-Fi |
|---|---|---:|---:|---:|
| 100% to 26% | any | on | on | on |
| 25% | falling | on | off | on |
| 15% | falling | off | off | on |
| 10% | falling | off | off | off |
| 13% | recovering | off | off | on |
| 20% | recovering | on | off | on |
| 30% | recovering | on | on | on |

## Current Implementation Summary

### Networking and Captive Portal

Implemented in:
- `main/esp32_nat_router.c`
- `main/http_server.c`
- `main/dns_server.c`
- `main/lwip_hooks.c`
- `main/client_acl.c`

Current behavior:
- default SoftAP SSID is `SOLAR CONNECT`
- default AP password is empty unless configured
- optional STA uplink provides internet and Supabase connectivity
- captive portal serves terms acceptance at `/`
- `/confirm` starts or resumes a session
- per-client ACL and lwIP hook block non-admitted egress
- DNS hijacking sends unauthenticated clients to the local portal
- DHCP option 114 advertises the captive portal URL
- NAPT is enabled after access is granted and can be disabled when idle

### Session Timing

Implemented in `main/http_server.c`.

Current behavior:
- daily quota is `3600` seconds per device/MAC
- active sessions are stored in RAM
- quota accounting is stored in NVS
- `DEV_RESET_QUOTA_ON_BOOT=1` clears quota records on every boot for dev testing
- Supabase receives session status and heartbeat updates

Important implication:
- if the ESP32 reboots because of power loss, quota is cleared while `DEV_RESET_QUOTA_ON_BOOT=1`
- set that flag to `0` for production/demo behavior where a reboot must not grant a fresh hour

### RFID

Implemented in `main/rfid_reader.c`.

Current behavior:
- MFRC522 over SPI3
- authorized UID list contains two 4-byte cards
- valid card presence turns on six power-control GPIOs
- absent or invalid card turns those GPIOs off
- battery override can force those GPIOs off regardless of card state
- `RFID_ENFORCE_AUTH=1` in the local `.env`

Current limitation:
- RFID card state does not currently enable/disable the user Wi-Fi AP
- `rfid_reader_set_presence_callback()` exists, but the current app does not wire RFID presence into `set_user_ap_enabled()`

### Battery State Machine

Implemented in:
- `main/battery_sensor.h`
- `main/battery_sensor.c`

Current behavior:
- reads GPIO 32 through ADC1 channel 4
- divider ratio is `5.5454`
- publishes battery telemetry to `station_state` every 5 seconds
- state machine has `normal`, `warning`, `critical`, `wake_up`, and `charging_on`
- when enforcement is enabled, it calls:
  - `set_user_ap_enabled()`
  - `rfid_reader_set_ports_allowed()`

Current local config:
- `.env` sets `BATTERY_SENSOR_ENFORCE_THRESHOLDS=0`
- therefore telemetry runs, but battery state does not currently control user AP or RFID port allow/block in this workspace build

Current thresholds:

| Code event | Voltage | Approx percent with 11.6 V to 13.9 V linear map |
|---|---:|---:|
| Normal -> Warning | 12.6 V falling | 43% |
| Warning -> Critical | 11.8 V falling | 9% |
| MPPT/hardware cutoff target | 11.6 V falling | 0% |
| Wake/boot-low threshold | 12.4 V | 35% |
| Critical/Wake Up -> Charging On | 12.8 V rising | 52% |
| Charging On/Warning -> Normal | 13.2 V rising | 70% |

Current limitation:
- no firmware-side complete shutdown state exists
- further voltage drop relies on hardware/MPPT cutoff

### USB Port Sensing

Implemented in:
- `main/port_sensors.h`
- `main/port_sensors.c`
- `main/admin_ports.c`

Current behavior:
- I2C0 on SDA 21 / SCL 22
- poll interval is 500 ms
- status changes are pushed to Supabase
- heartbeat updates occur every 30 seconds
- threshold is `PORT_IN_USE_THRESHOLD_MA=50.0`
- live readings are exposed by local admin APIs

Current address map:

| Port | INA219 address |
|---|---:|
| USB-C 1 | `0x44` |
| USB-C 2 | `0x41` |
| USB-A 1 | `0x40` |
| USB-A 2 | `0x45` |

Current Supabase USB payload:
- `station_id`
- `port_key`
- `status`
- `daily_in_use_seconds`

Current limitation:
- `current_ma` and `bus_voltage_v` are read locally but not included in the Supabase upsert payload
- USB-C INA219 hardware is flagged as faulty on the current PCB revision

### AC Outlet Sensing

Implemented in:
- `main/pzem_reader.h`
- `main/pzem_reader.c`
- `main/admin_ports.c`

Current behavior:
- PZEM-004T v3.0 Modbus RTU
- UART2 TX GPIO 17, RX GPIO 16, 9600 baud
- slave address `0xF8`
- reads voltage, current, power, cumulative energy, frequency, power factor, and alarm register block
- publishes AC fields to `station_state`
- publishes outlet availability/in-use status to `port_state`

Current `station_state` AC fields:
- `ac_voltage_v`
- `ac_current_a`
- `ac_power_w`
- `ac_energy_wh`
- `ac_energy_wh_today`

Current outlet rule:
- `port_state.outlet` is `in_use` when PZEM power is greater than `1.0 W`

Important correction:
- docs that say PZEM is v1 or logs only are obsolete

### Supabase

Implemented in:
- `main/supabase_client.c`
- `supabase/*.sql`

Firmware writes:
- session lifecycle rows
- USB port status and daily in-use seconds
- AC outlet status
- battery telemetry
- AC energy telemetry

Schema files include:
- `001_schema.sql`
- `002_port_state_sensor_metrics.sql`
- `003_station_state_battery_telemetry.sql`
- `004_station_state_ac_telemetry.sql`
- `005_eco_metrics_telemetry.sql`

### PWA Linking

Implemented firmware side in:
- `main/http_server.c`
- `main/supabase_client.c`

Current behavior:
- firmware creates `session_token`
- firmware creates stable `device_hash` when it can identify the client MAC
- connected page links to `https://solarconnect.live/?session_token=<token>`
- PWA is expected to bind `installation_id` to that token once
- future PWA loads should resolve by `installation_id -> device_hash -> latest session`

See `PWA_LINKING_CONTRACT.md`.

## Critical Mismatches

### 1. RFID does not yet gate user Wi-Fi

Expected:
- valid RFID card enables charging ports and user Wi-Fi
- absent/invalid card disables charging ports and user Wi-Fi

Current:
- RFID controls charging power GPIOs only
- user Wi-Fi AP is controlled by the battery state machine, not RFID
- in the local dev build, even battery AP control is disabled by `.env`

Impact:
- a user may still connect to `SOLAR CONNECT` and use the captive portal even when no valid RFID card is present, depending on battery config

### 2. Battery enforcement is disabled in local config

Expected:
- battery threshold state changes should disable/re-enable user Wi-Fi and charging ports

Current:
- `.env` sets `BATTERY_SENSOR_ENFORCE_THRESHOLDS=0`
- telemetry still publishes
- AP/port side effects are skipped

Impact:
- local bench/dev behavior is intentionally different from final threshold enforcement

### 3. Battery thresholds are voltage-based and not calibrated to percentages

Expected:
- transitions at 25%, 15%, 10%, 13%, 20%, and 30%

Current:
- transitions happen at fixed voltages
- percent is a linear estimate between 11.6 V and 13.9 V
- LiFePO4 voltage is flat through much of discharge, so this is approximate

Impact:
- dashboard percent can disagree with state transitions

### 4. No firmware-side complete shutdown

Expected:
- 10% falling disables MCU Wi-Fi and performs complete shutdown

Current:
- there is no explicit shutdown state/action in firmware
- the code relies on MPPT/hardware cutoff

Impact:
- final telemetry can be lost when power disappears

### 5. USB live electrical values are not sent to Supabase by the sensor task

Expected:
- PWA may want current and bus voltage for live dashboard detail

Current:
- firmware reads `current_ma` and `bus_voltage_v`
- schema includes those columns
- event-driven USB upsert does not send those columns

Impact:
- Supabase/PWA can see port status and daily seconds, but not live USB electrical values unless another path is added

### 6. Dev quota reset clears daily quota on every boot

Expected production/demo behavior:
- daily quota survives reboot and battery recovery

Current:
- `DEV_RESET_QUOTA_ON_BOOT=1`
- every boot clears quota state

Impact:
- power loss or reset grants devices a fresh hour during dev mode

### 7. Active sessions do not survive reboot

Expected:
- connected users' countdown/status should recover after a reboot if possible

Current:
- active sessions are RAM-only
- quota can persist only when dev reset is off

Impact:
- after reboot, active internet sessions are not restored exactly as before

### 8. Security defaults are demo-grade

Current:
- admin defaults to `admin / admin123`
- AP defaults open if no AP password is configured
- SQL grants in local schema are permissive for demo integration

Impact:
- harden before public deployment

## Eco Metrics Clarification

Eco metrics mean estimated CO2 savings.

The current intended split is:
- firmware publishes raw inputs
- PWA computes the estimate

Firmware raw inputs:
- `port_state.daily_in_use_seconds`
- `station_state.ac_energy_wh_today`

PWA formula:

```text
(10 * USB-A hours + 15 * USB-C hours + acEnergyWhToday) * 0.70 = grams CO2 saved
```

Therefore, disabled `main/eco_metrics.c` is not a blocker for the current architecture. It is only a possible future firmware-side accumulator.

## Runtime Boot Flow

Current `app_main()` sequence:

1. initialize NVS
2. mount SPIFFS
3. load router configuration
4. load saved port mappings
5. initialize Wi-Fi
6. initialize port sensors
7. start port sensor Supabase sync
8. start RFID reader
9. start PZEM reader
10. start battery sensor
11. start network diagnostics
12. start LED task
13. start DNS and HTTP servers if unlocked
14. initialize console
15. register CLI commands
16. enter console loop

## Main Authored Files

- `main/esp32_nat_router.c` - app entrypoint, Wi-Fi lifecycle, NAT, boot flow
- `main/http_server.c` - captive portal, session timing, admin/config UI, Supabase session sync
- `main/dns_server.c` - captive DNS and upstream forwarding
- `main/lwip_hooks.c` - per-client egress enforcement and DHCP captive portal option
- `main/client_acl.c` - admitted-client table
- `main/rfid_reader.c` - MFRC522 and power GPIO control
- `main/port_sensors.c` - INA219 reads and port status sync
- `main/pzem_reader.c` - PZEM v3 Modbus reads and AC telemetry sync
- `main/battery_sensor.c` - ADC reads, battery state machine, battery sync
- `main/admin_ports.c` - local test/admin APIs for ports, sensors, battery, AC snapshot
- `main/supabase_client.c` - REST calls to Supabase
- `components/cmd_router/cmd_router.c` - router CLI and NVS config helpers

## Roadmap

Highest value firmware work:

1. Wire RFID presence into user Wi-Fi/session eligibility if thesis demo requires RFID to gate the user network.
2. Turn on `BATTERY_SENSOR_ENFORCE_THRESHOLDS=1` when moving from bench/dev to real threshold testing.
3. Calibrate battery voltage thresholds to the actual LiFePO4 battery under load.
4. Replace linear voltage-to-percent with a LiFePO4 lookup or measured SoC method.
5. Add firmware-side shutdown/final telemetry if the hardware gives enough warning before cutoff.
6. Add `current_ma` and `bus_voltage_v` to USB `port_state` upserts if the PWA needs them.
7. Set `DEV_RESET_QUOTA_ON_BOOT=0` for production/demo quota behavior.
8. Persist or reconstruct active sessions across reboot if exact countdown recovery is required.
9. Harden admin credentials and Supabase access before a public demo.

## Mental Model

Think of the project as five cooperating systems:

- hardware interface layer: RFID, GPIOs, INA219, PZEM, ADC
- power policy layer: battery state and operational thresholds
- network layer: AP/STA, DNS, NAPT, per-client ACL
- session layer: terms, quota, session token, Supabase session status
- PWA/backend layer: installation identity, dashboard state, CO2 estimate

When changing behavior, decide which layer owns it before editing code.
