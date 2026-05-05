# Project Context

Last reviewed: 2026-05-05

Review basis:
- Current workspace contents, not just the last commit
- Top-level build/config files
- All authored source under `main/` and `components/`
- Current SPIFFS assets under `spiffs_image/`
- Recent commits adding battery ADC + state machine (`0fc6d6f`), PZEM-004T AC monitoring (`8083542`), and event-driven port sensor sync (`671d06e`)

Current workspace note:
- The worktree has staged hardware-bring-up work in progress; an untracked SQL migration `supabase/003_station_state_battery_telemetry.sql` is on disk
- This document reflects the currently checked out codebase as of 2026-05-05

## Thesis/Product Framing

The intended system-level identity of this project is:
- Thesis title: `Development and Implementation of Solar-Powered Smart Charging Station with Integrated Connectivity`
- User-facing station/SSID brand: `SOLAR CONNECT`
- Product concept: `The Smart Solar Hub`

At the thesis level, the full system spans four layers:
- `Power layer`
  Solar panel, LiFePO4 battery, charging hardware, inverter, and energy distribution
- `Embedded control layer`
  ESP32 firmware for AP+STA networking, captive portal, session timing, and remote service handoff
- `Access/security layer`
  Captive portal acceptance flow and planned RFID-assisted physical activation
- `User application layer`
  A Vercel-hosted PWA/dashboard for transparency, session visibility, and station information

Important repo boundary:
- This repository currently implements the ESP32 networking/router/captive-portal firmware
- The Vercel PWA is referenced from this firmware, but its source code is not present here
- The solar charging electronics, port current sensing, RFID activation logic, and eco-metric calculations described in the thesis framing are partially implemented or planned for implementation in this repository

Last reviewed and updated: 2026-05-05
- Full codebase analysis completed across all main/ and components/ directories
- Current implementation status reflects the battery ADC, state machine, RFID-battery coordination, USB INA219 sync (now enabled in .env), and PZEM-004T AC reader that landed in recent commits
- Outstanding gaps and threshold-calibration mismatches documented below

## Project Summary

This repository contains ESP32 firmware for the connectivity and control subsystem of the Smart Solar Hub thesis project.

The current firmware combines:
- A SoftAP that is exposed to clients in Normal battery state (branded as `SOLAR CONNECT`); the AP is auto-disabled by the battery state machine when battery voltage drops
- An optional STA uplink to another Wi-Fi network (for MCU telemetry to Supabase). STA stays up across all non-shutdown battery states
- IPv4 NAPT and port mapping for upstream internet access
- A captive portal with session-gated internet access and terms acceptance
- RFID-based physical activation: MFRC522 reader live, 6 power-control GPIOs driven by an authorized-card check, with a battery override that forces ports off in Critical state
- USB port current sensing via four INA219 sensors on I2C (`PORT_SENSORS_ENABLED=1` and `PORT_SENSORS_SUPABASE_SYNC_ENABLED=1` are both set in the local `.env`); event-driven Supabase upserts on status change plus a 30 s heartbeat
- AC outlet monitoring via PZEM-004T v1 over UART2 (reads V/I/P/Wh every 5 s). Currently logs only — no Supabase upsert and no `port_state.outlet` mapping yet
- Battery voltage tracking: ADC1 channel 4 on GPIO 32 with a 5.5454× resistor divider, calibrated via line/curve fitting, sampled and pushed to Supabase every 5 s
- Battery-level-based operational thresholds: voltage-driven state machine (`Normal / Warning / Critical / Wake Up / Charging On`) with 30 s debounce, hysteresis, user-AP toggling, and RFID port override
- A serial CLI for configuration and diagnostics
- SPIFFS-hosted image assets used by the portal UI
- Supabase integration for session sync, USB port telemetry, and battery telemetry (`battery_percent`, `battery_voltage_v`, `battery_raw_mv`, `battery_state`)

Important product-level framing:
- The current root web experience is a captive portal at `/`
- The configuration UI still exists at `/config`
- A station test surface lives at `/ports` (manual port toggles + battery slider) and `/port-occupied` (live USB-A INA219 readings)
- The repository started from the ESP-IDF console example and an ESP32 NAT router example, then added a custom captive portal flow, diagnostics, RFID activation with battery coordination, USB INA219 telemetry, PZEM AC reader, battery ADC + state machine, Supabase sync, Vercel handoff, and Smart Solar Hub branding
- In the broader thesis architecture, this repo implements the embedded control layer that sits between the power/charging hardware and the cloud dashboard

## Thesis System Requirements

The Smart Solar Hub is designed to enforce operational thresholds based on battery state to maximize solar energy efficiency while maintaining safe charging operations. The system has six key operational modes based on battery percentage and charging/recovery direction.

### Operational Threshold Specification

The system must enforce the following battery-level-based mode transitions:

| Battery % | Trend | Charging Ports | WiFi (Users) | WiFi (MCU Telemetry) | System State |
|-----------|-------|---|---|---|---|
| 100% – 26% | Any | ON | ON | ON | Full Service |
| 25% | Falling | ON | **OFF** | ON | Users WiFi Disabled |
| 15% | Falling | **OFF** | OFF | ON | Ports Disabled, MCU Telemetry Only |
| 10% | Falling | OFF | OFF | **OFF** | **Complete Shutdown** |
| 13% | Recovering | OFF | OFF | **ON** | MCU Wakes, Telemetry Resumes |
| 20% | Recovering | **ON** | OFF | ON | Charging Ports Resume |
| 30% | Recovering | ON | **ON** | ON | WiFi Users Resume (Full Service) |

**Key design intent:**
- RFID card presence enables system activation
- Battery level determines what functionality is available
- System gracefully degrades: Users → Ports → Telemetry → Shutdown
- Asymmetric recovery thresholds (13%, 20%, 30%) prevent battery cycling near critical thresholds
- MCU telemetry continues until 10% to report final state to Supabase before shutdown

## Current Implementation Status

### ✅ FULLY IMPLEMENTED

1. **RFID Logic**
   - MFRC522 over SPI3 (VSPI), pins SDA=5, SCK=18, MOSI=23, MISO=19, RST=4
   - Card authentication against 2 authorized 4-byte NUIDs (Mifare Classic)
   - GPIO control of 6 power pins (4 MOSFETs + SSR + relay) on pins 12, 14, 27, 26, 25, 13
   - 1-second card presence timeout, 50 ms poll, antenna bounce after each read
   - **Battery override**: `rfid_reader_set_ports_allowed()` lets the battery state machine force-off the ports regardless of card presence; on re-enable the next valid card poll will re-energize them

2. **WiFi AP (SoftAP)**
   - "SOLAR CONNECT" SSID (configurable, WPA2/WPA3 PSK or open if password < 8 chars)
   - Optional STA uplink for telemetry; STA stays up in every battery state below shutdown
   - DHCP server for AP clients; advertises ESP32 itself as DNS for captive portal hijack
   - Max 8 concurrent clients
   - `set_user_ap_enabled()` switches `WIFI_MODE_APSTA` ↔ `WIFI_MODE_STA` based on battery state

3. **Captive Portal & Session Management**
   - Terms and conditions gate at `/` with checkbox acceptance
   - Session token generation + stable device_hash (MAC-based)
   - 3600 seconds (1 hour) daily quota per device/MAC
   - Supabase sync: heartbeat (30 s), session state (active/expired/disconnected)
   - Per-client gating via `client_acl_*` admit/revoke + lwIP IPv4 input hook (`lwip_hooks.c`) that drops non-admitted egress and rewrites probe traffic to the portal IP
   - DHCP option 114 advertises the captive portal URL
   - PWA one-time linking via `/confirm` redirect

4. **USB Port Sensing (INA219)**
   - I2C bus on SDA=21, SCL=22, 100 kHz; external 5 V pull-ups on the breakouts
   - Four INA219s mapped: USB-C 1 → 0x44, USB-C 2 → 0x41, USB-A 1 → 0x40, USB-A 2 → 0x45
   - Current threshold 50 mA (configurable) → `available` / `in_use`
   - Event-driven sync task: pushes to Supabase on status flip, plus 30 s heartbeat per port
   - **Hardware caveat**: USB-C INA219s at 0x40 and 0x41 are reported faulty on the current PCB rev (SCL pin internally shorted to GND); the live `/port-occupied` page intentionally only renders USB-A 1 and USB-A 2
   - Live readings exposed at `/api/ports/sensors`; one-shot Supabase sync at `POST /api/ports/sensors/sync`

5. **AC Outlet Sensing (PZEM-004T v1)**
   - UART2: TX=GPIO 17 (→ PZEM RX), RX=GPIO 16 (← PZEM TX), 9600 baud
   - Legacy 7-byte Peacefair binary protocol (NOT Modbus); device address 192.168.1.1 (default)
   - Reads voltage / current / power / cumulative energy every 5 s
   - **Limitation**: data is logged only. PZEM does not yet upsert `port_state.outlet` and does not yet write any AC fields to `station_state` or a dedicated table

6. **Battery Voltage + State Machine**
   - ADC1 channel 4 on GPIO 32, 12-bit, full-range attenuation (0–3.3 V), 16-sample average
   - Voltage divider ratio 5.5454× (R1 = 100 kΩ, R2 = 22 kΩ)
   - Curve fitting / line fitting calibration depending on target
   - Five states: `Normal / Warning / Critical / Wake Up / Charging On`, with 30 s debounce (6 × 5 s)
   - Boot averaging (5 samples × 500 ms) selects initial state
   - Side effects per state: user AP enable/disable, RFID port allow/block
   - Supabase upsert every 5 s: `battery_percent`, `battery_voltage_v`, `battery_raw_mv`, `battery_state`
   - **Mismatch**: voltage thresholds and the linear `voltage → percent` mapping do not line up with the thesis spec (see Mismatches section)

7. **Coordinated Power State (RFID × Battery)**
   - `set_user_ap_enabled()` and `rfid_reader_set_ports_allowed()` are declared in `router_globals.h:67-68` and called from `apply_actions_for_state()` in `battery_sensor.c:131`
   - In Critical state the RFID-controlled power outputs are forced off; in Warning state the user AP is dropped; STA telemetry continues across all non-shutdown states
   - **Limitation**: The 10 % shutdown step is delegated to the hardware MPPT cut — there is no firmware action that flushes a final telemetry sync before power loss

8. **Supabase Schema & Integration**
   - `sessions` table: legacy + new identifiers, `installation_id`, `remaining_seconds`, `status`
   - `port_state` table: `port_key`, `status`, `current_ma`, `bus_voltage_v`
   - `station_state` table: `battery_percent`, `battery_voltage_v`, `battery_raw_mv`, `battery_state`, `updated_at`
   - Three RPCs in the live DB: `claim_session_link()`, `resolve_installation_session()`, `cleanup_old_sessions()` (the SQL file ships only stub bodies; live bodies must be exported from Supabase Studio)
   - Session heartbeat task + Supabase update queue in `http_server.c`
   - Migration `003_station_state_battery_telemetry.sql` adds the battery telemetry columns; **untracked in git as of 2026-05-05**

### ⚠️ PARTIALLY IMPLEMENTED / NEEDS WORK

1. **AC Outlet Telemetry Pipeline**
   - PZEM hardware reads work; nothing is published.
   - **Needed**: map current/power to `port_state.outlet` `available`/`in_use`, and decide whether AC voltage/power/energy belong on `station_state` or a new table.

2. **Battery Threshold Calibration**
   - State machine is in place but acts on raw voltages that don't line up with the thesis spec's percentage thresholds (see Mismatch 1 below).
   - The linear curve assumes a generic 12 V profile; LiFePO4 voltage curves are flat in the middle and need a piecewise lookup or coulomb counting for accurate SoC.

3. **Graceful Shutdown at 10 %**
   - No firmware-side action; relies on MPPT cutoff.
   - **Needed**: a `Shutdown` state that issues a final telemetry flush and disables radios cleanly before voltage drops further.

4. **Eco Metrics**
   - `eco_metrics.c` is wired in but `ECO_METRICS_ENABLED=0`; the PWA fakes CO₂ savings client-side from `portsInUseCount × 3.5 g`.
   - **Needed**: enable the module, drive samples from the INA219 task and the PZEM reader, persist to Supabase.

5. **Session Persistence Across Reboot**
   - Sessions are RAM-only; daily quota records do persist in NVS, but `DEV_RESET_QUOTA_ON_BOOT=1` in `http_server.c:69` clears them on every boot.
   - **Needed for production demo**: flip `DEV_RESET_QUOTA_ON_BOOT` to `0`.

### ❌ NOT YET IMPLEMENTED

- USB-C port sensing on the production PCB (waiting on the 0x40 / 0x41 INA219 hardware fix)
- Eco-achievement / CO₂-savings calculation surfaced from real port energy
- Push of PZEM AC telemetry into Supabase
- Firmware action for the 10 % shutdown threshold

### Detailed Feature Gaps

| Feature | Status | Dependency | Thesis Impact |
|---------|--------|------------|---|
| AC outlet → Supabase | ⚠️ partial | PZEM read works; outlet upsert + schema decision pending | Outlet on dashboard reflects manual `/ports` toggle only |
| AC voltage/power/energy telemetry | ❌ | New columns/table + ESP32 push | No AC analytics on dashboard |
| Battery threshold calibration | ⚠️ partial | State machine exists; thresholds don't match spec percentages | Transitions trigger at the wrong battery levels |
| LiFePO4 SoC curve | ❌ | Piecewise lookup or coulomb counter | Reported `battery_percent` is unreliable mid-discharge |
| Graceful shutdown action | ❌ | Final-flush hook before MPPT cut | Last telemetry can be lost |
| Eco metrics | ❌ | `ECO_METRICS_ENABLED=1` + sampling task | CO₂ savings on dashboard are stubbed |
| USB-C INA219 readings | ❌ | PCB rev fixing the 0x40 / 0x41 chips | USB-C ports unreported in live readings |
| Persistent active sessions | ❌ | NVS-backed session table | Reboot drops everyone's countdown |
| Session quota persistence demo-mode | ⚠️ | Flip `DEV_RESET_QUOTA_ON_BOOT` to 0 | Reboot grants every device a fresh hour |

## Critical Mismatches: Specification vs. Implementation

### Mismatch 1: Battery thresholds use voltage, but the spec is in percent — and the mapping is off ⚠️ HIGHEST PRIORITY
**Specification:** transitions at 25 / 15 / 10 % falling and 13 / 20 / 30 % recovering.
**Implementation:** `battery_sensor.h:62-77` linearly maps `11.6 V → 0 %` and `13.6 V → 100 %`, then transitions on raw volts:

| Spec event | Code threshold | What that voltage maps to in % |
|---|---|---|
| 25 % falling → user AP off | `12.6 V` (`BATTERY_V_WARNING_FALL`) | **50 %** |
| 15 % falling → ports off | `11.8 V` (`BATTERY_V_CRITICAL_FALL`) | **10 %** |
| 10 % shutdown | `11.6 V` (delegated to MPPT) | **0 %** |
| 13 % recovering → wake | `12.4 V` (`BATTERY_V_WAKEUP_BOOT_LOW`) | **40 %** |
| 20 % recovering → ports on | `12.8 V` (`BATTERY_V_CHARGING_RISE`) | **60 %** |
| 30 % recovering → users on | `13.2 V` (`BATTERY_V_NORMAL_RISE`) | **80 %** |

**Impact:** the dashboard shows ~60 % when the firmware decides to re-enable charging ports, etc. Either recalibrate the voltage thresholds against measured battery behavior under load, or move the state machine to act on percent and use voltage only as the SoC estimator.

### Mismatch 2: SoC curve assumes a generic 12 V profile, hardware is LiFePO4
**Specification / hardware:** 12 V 100 Ah LiFePO4 battery.
**Implementation:** `battery_sensor.h:13-29` describes the curve as a "12 V lead-acid curve"; `battery_percent_from_voltage()` does linear interpolation between 11.6 V and 13.6 V.
**Impact:** LiFePO4 voltage is very flat (~13.0–13.4 V) over the middle of its discharge, so a linear curve will misreport SoC during normal operation. A piecewise lookup table or a coulomb counter (INA219 + PZEM integrated against time) is more accurate.

### Mismatch 3: AC outlet telemetry not pushed
**Specification:** "Send AC port availability/in-use status to Supabase."
**Implementation:** `pzem_reader.c` reads V/I/P/Wh every 5 s but only `ESP_LOGI`s them. There is no `port_state.outlet` upsert based on PZEM current and no AC field in `station_state`.
**Impact:** Outlet on the PWA reflects only the manual `/ports` toggle; AC voltage / power / energy never reach the dashboard.

### Mismatch 4: Graceful shutdown step is delegated to hardware
**Specification:** "10 % falling: Complete system shutdown. Final telemetry sync before power loss."
**Implementation:** `battery_sensor.c:197-208` comments out shutdown handling on the firmware side and notes "Falling further triggers hardware MPPT shutdown — ESP loses power and we never see it."
**Impact:** The last battery state and last session state may not reach Supabase; the dashboard can show stale data after a shutdown.

### Mismatch 5: USB-C port sensing is silent on the current PCB
**Specification:** All four USB ports report current and bus voltage.
**Implementation:** USB-C INA219s at 0x40 and 0x41 are flagged as faulty in `admin_ports.c:245-248`; the live readings page only renders USB-A 1 and USB-A 2.
**Impact:** USB-C status on the dashboard depends entirely on the manual `/ports` toggle until the PCB is fixed.

### Mismatch 6: Eco metrics are stubbed
**Specification:** "Eco-achievement / CO₂ savings calculation."
**Implementation:** `eco_metrics.c` exists but `ECO_METRICS_ENABLED=0`; the dashboard fakes CO₂ savings with `portsInUseCount × 3.5 g`.
**Impact:** The thesis's eco-metric story is not based on real energy data.

### Mismatch 7: Active session persistence and dev-mode quota reset
**Specification:** Sessions and daily quotas survive reboot.
**Implementation:** Sessions are RAM-only; quota is NVS-backed but `DEV_RESET_QUOTA_ON_BOOT=1` in `http_server.c:69` wipes the quota table on every boot.
**Impact:** A reboot during the demo restarts every connected device's countdown and gives all of them a fresh hour.

### Mismatch 8: Hard-coded admin credentials
**Specification:** Admin surface should be protected.
**Implementation:** `admin / admin123` in `http_server.c:46-52`, overridable through CMake but defaulted in source.
**Impact:** Anyone on `SOLAR CONNECT` can reach `/config` with the default credentials.

## System Architecture: Current

```
Battery ADC (GPIO 32) ──────► battery_sensor_task
                                  │  voltage → percent (linear, 11.6–13.6 V)
                                  │  evaluate_transitions() with 30 s debounce + hysteresis
                                  ▼
                          [State Machine: NORMAL / WARNING / CRITICAL / WAKE_UP / CHARGING_ON]
                                  │
                ┌─────────────────┼─────────────────┐
                ▼                 ▼                 ▼
   set_user_ap_enabled()   rfid_reader_set_   supabase_post_upsert(station_state)
   (APSTA ↔ STA-only)      ports_allowed()    (battery_percent, voltage_v, raw_mv, state)
                                  │
                                  ▼
                          [RFID poll @ 50 ms]
                          MFRC522 → authorized UID?
                                  │
                                  ▼
                          GPIO 12/14/27/26/25/13 (4 MOSFETs + SSR + relay)

USB INA219s (0x44 / 0x41 / 0x40 / 0x45)  ──► port_sensors_sync_task
                                              event-driven on status flip + 30 s heartbeat
                                              ──► supabase_post_upsert(port_state)

PZEM-004T (UART2 GPIO 16/17)             ──► pzem_task @ 5 s  (LOG ONLY — no upsert yet)

SoftAP "SOLAR CONNECT"
   │
   ▼
DHCP / DNS hijack ──► captive portal /  → /confirm
   │                              │
   │                              ▼
   ▼                       client_acl_admit() + lwip ip4_input hook
HTTP session table (RAM)         │
   │                              ▼
   ▼                          NAPT egress allowed; quota counted in NVS
supabase_create_session / heartbeat / disconnect (sessions table)
```

## Implementation Roadmap

Phases 1–3 from the original plan are largely complete in code. Remaining work, in priority order:

**Phase 4: Calibrate the battery state machine**
- Recalibrate `BATTERY_V_*` against measured battery behavior under representative load, OR move the state machine to act on percent and use voltage only as the SoC estimator
- Replace the linear `voltage → percent` curve with a LiFePO4-shaped piecewise lookup
- Implement a `Shutdown` state with a final Supabase flush before MPPT cuts power

**Phase 5: AC outlet telemetry**
- Map PZEM current/power to `port_state.outlet` `available`/`in_use`
- Decide schema location for AC voltage / power / cumulative energy and push it on the same 5 s cadence
- Update the PWA to render AC fields

**Phase 6: Eco metrics**
- Flip `ECO_METRICS_ENABLED=1` and feed it from the INA219 sync task and the PZEM reader
- Persist `today_energy_wh` and `today_co2_saved_g` to Supabase
- Replace the PWA's stub estimate with the real value

**Phase 7: Hardening for thesis demo**
- Set `DEV_RESET_QUOTA_ON_BOOT=0` and verify quota persistence across reboot
- Replace hard-coded admin password (CMake or NVS-backed)
- Persist active sessions in NVS so a reboot doesn't drop everyone's countdown
- Fix the USB-C INA219 PCB issue (0x40 / 0x41) so live USB-C readings work

## Repository Layout

Main authored areas:
- `main/esp32_nat_router.c`
  App entrypoint, NVS and SPIFFS init, Wi-Fi/AP+STA setup, event handlers, NAT/portmap helpers, LED thread, console loop, web/DNS startup, and the launchers for `port_sensors`, `rfid_reader`, `pzem_reader`, and `battery_sensor`
- `main/http_server.c`
  Captive portal HTTP server, admin config page, per-client session tracking, captive-probe handling, SPIFFS image serving, dashboard handoff, Supabase session sync
- `main/dns_server.c`
  Custom UDP DNS server used for captive portal DNS hijacking and upstream DNS forwarding (with a per-day RAM cache)
- `main/lwip_hooks.c`
  IPv4 input hook that drops non-admitted egress and rewrites probe traffic to the captive portal IP; DHCP option 114 emitter
- `main/client_acl.c`
  Per-client admit/revoke list shared by HTTP, DNS, and the lwIP hook
- `main/net_diag.c`
  Runtime network diagnostics, NAPT state logging, and active connectivity probes
- `main/supabase_client.c`
  REST integration for remote session creation, heartbeat updates, disconnect status, and the generic `supabase_post_upsert()` used by port/battery telemetry
- `main/admin_ports.c`
  HTML test surfaces (`/ports`, `/port-occupied`) and JSON APIs for manual port toggling, battery-percent simulation, INA219 live readings, I2C scan, and one-shot port sync
- `main/port_sensors.c`
  INA219 driver, I2C bus management, status classification, event-driven Supabase sync task
- `main/rfid_reader.c`
  MFRC522 driver, authorized UID list, GPIO power-pin control, battery override hook
- `main/pzem_reader.c`
  PZEM-004T v1 binary protocol implementation over UART2; periodic read task (logging only at present)
- `main/battery_sensor.c`
  ADC sampling, calibration, voltage-driven state machine, Supabase upsert task
- `main/eco_metrics.c`
  Stubbed energy / CO₂ accumulator (`ECO_METRICS_ENABLED=0` for now)
- `main/pages.h`
  Embedded HTML for the legacy config/admin page and an unused lock page
- `components/cmd_router/cmd_router.c`
  Router-specific CLI commands and config helpers; declares the `set_user_ap_enabled()` and `rfid_reader_set_ports_allowed()` battery-action hooks
- `components/cmd_nvs/cmd_nvs.c`
  Generic NVS CLI commands
- `components/cmd_system/cmd_system.c`
  Generic system and diagnostics CLI commands

Other notable files:
- `README.md`
  User-facing documentation, but it does not fully match the current firmware behavior
- `platformio.ini`
  PlatformIO configuration using ESP-IDF
- `sdkconfig.defaults`
  Project defaults for NAT, partition table, task stats, and console settings
- `sdkconfig`
  Current checked-in active config for this workspace build
- `partitions_example.csv`
  Partition layout with NVS, factory app, and SPIFFS storage
- `test.html`
  Standalone mock/prototype of the current "connected" page visual design
- `spiffs_image/`
  Image assets flashed into the SPIFFS `storage` partition

External-but-coupled systems not stored in this repo:
- The Vercel PWA reached through `https://solarconnect.live`
- The Supabase backend configured through `.env` or CMake cache variables
- The physical solar/charging/RFID subsystem described in the thesis narrative

Generated or vendor-managed areas:
- `build/`
  Generated ESP-IDF build output
- `.pio/`
  PlatformIO output
- `managed_components/`
  External managed components, not part of the authored firmware logic

## Boot And Runtime Flow

The current boot flow in `app_main()` is:
1. Initialize NVS
2. Mount SPIFFS at `/spiffs` from partition label `storage`
3. Read router configuration from NVS namespace `esp32_nat`
4. Load any saved port mapping table from NVS
5. Initialize Wi-Fi in AP mode or AP+STA mode
6. Start the LED status thread
7. If `lock == "0"`, start the DNS server and HTTP server
8. Initialize the serial console and register CLI commands
9. Enter the interactive console loop

Important runtime facts:
- The SoftAP always exists
- STA uplink is optional and depends on saved STA credentials
- NAT is intentionally not enabled during boot
- NAT is enabled later from the captive portal `/confirm` path
- The current firmware is therefore optimized around `managed Wi-Fi access`, not direct charging-port control

## Networking Model

SoftAP behavior:
- Default AP SSID is `SOLAR CONNECT`
- Default AP password is empty, so the default AP is open
- Default AP IP is `192.168.4.1`
- AP max clients is configured to 8 in the Wi-Fi config

STA behavior:
- If `ssid` is empty, the device runs AP-only mode
- If `ssid` is present, the device runs AP+STA mode
- WPA2 Enterprise is supported through `ent_username`, `ent_identity`, and `passwd`
- Static STA IP is applied when `static_ip`, `subnet_mask`, and `gateway_addr` are all present

Event-driven state:
- `ap_connect` tracks whether the STA uplink currently has an IP
- `my_ip` stores the current STA IPv4 address
- `my_ap_ip` stores the AP IPv4 address
- `connect_count` tracks the number of stations connected to the SoftAP

Port mapping:
- Port map entries are stored in an in-memory table backed by NVS blob key `portmap_tab`
- On `IP_EVENT_STA_GOT_IP`, existing port maps are removed and then re-applied against the new STA IP

## Captive Portal And Session Model

The captive portal is implemented by the combination of:
- HTTP request routing in `main/http_server.c`
- DNS interception and forwarding in `main/dns_server.c`
- DHCP advertising the ESP32 itself as DNS for AP clients

Per-client session model:
- Sessions are tracked in RAM only
- Session matching uses client IP and, when available, a stable MAC-derived identity
- Maximum tracked clients is 20
- Daily quota is currently hard-coded to `3600` seconds
- The firmware generates a per-session random `session_token`
- The firmware also generates a stable `device_hash` intended for cross-session linkage
- Supabase is expected to use `installation_id <-> device_hash` linkage so the PWA does not need a tokenized URL every new day

Captive portal HTTP flow:
- `/`
  Shows the portal landing page with terms and a checkbox gate
- `/confirm`
  Grants access for the current session, enables NAPT when uplink is ready, and shows the connected page
- `/api/status`
  Returns JSON describing whether the current client IP is authenticated and how many seconds remain

Captive probe handling:
- Known OS probe URLs such as `/generate_204`, `/gen_204`, `/connectivity-check.html`, `/hotspot-detect.html`, `/connecttest.txt`, `/ncsi.txt`, `/redirect`, `/fwlink`, and `/library/test/success.html` are registered explicitly
- Authenticated clients get a success response appropriate to the probe
- Unauthenticated clients are redirected to the portal root

Catch-all behavior:
- Unauthenticated `GET` and `HEAD` requests to unknown paths are redirected to the portal
- Authenticated unknown paths get `404`

DNS behavior:
- The AP DHCP server is configured to advertise the ESP32 itself as DNS
- Unauthenticated clients do not get true upstream DNS answers
- For unauthenticated A-record queries, the DNS server responds with the AP IP so browsers land on the portal
- For authenticated clients, the DNS server forwards queries to the upstream DNS learned from the STA interface, falling back to `8.8.8.8` if none is known

Important enforcement detail:
- Session checks are used by HTTP and DNS
- NAPT is enabled globally with `ip_napt_enable(my_ap_ip, 1)` once `/confirm` succeeds on any client
- There is no matching code path that disables NAPT when sessions expire
- As implemented, session enforcement is therefore not a true per-client firewall

## HTTP And UI Surface

Current HTTP routes:
- `/`
  Portal landing page
- `/confirm`
  Session-start and connected page
- `/api/status`
  JSON session status
- `/config`
  Admin/config page behind HTTP Basic auth
- `/cea.png`
- `/dashboard.png`
- `/dashboard-ui.png`
- `/*`
  Catch-all redirect or 404 depending on session state

Admin/config behavior:
- The admin page is generated from `CONFIG_PAGE` in `main/pages.h`
- It is protected with hard-coded HTTP Basic credentials:
  Username: `admin`
  Password: `admin123`
- The page shows AP settings, STA settings, WPA2 Enterprise fields, static IP fields, and a reboot button

What the admin handler actually processes today:
- `reset=Reboot`
- `ap_ssid` and `ap_password`
- `ssid` and `password`

What the admin handler does not fully process today:
- Enterprise fields displayed by the form are not read from the query handler
- Static IP fields displayed by the form are not read from the query handler
- The `/config` STA form submission clears stored enterprise fields because it calls `set_sta` without passing enterprise arguments through the parser

UI/asset sources:
- The captive portal page and the connected page are embedded directly in C string literals in `main/http_server.c`
- The admin page is embedded in `main/pages.h`
- `LOCK_PAGE` still exists in `main/pages.h` but is not wired into the current HTTP flow
- `test.html` is a static HTML prototype of the current connected-page visual design
- Runtime image assets are served from SPIFFS:
  `cea.png`, `dashboard.png`, `dashboard-ui.png`

Branding and handoff:
- The captive portal and connected page are branded as `SOLAR CONNECT`
- The connected page links out to:
  `https://solarconnect.live/?session_token=<token>`

PWA recovery design:
- The generic dashboard route is expected to be:
  `https://solarconnect.live`
- If the generic dashboard cannot resolve a linked installation, it should instruct the user to connect to `SOLAR CONNECT`
- The recommended manual recovery action is a link to:
  `http://192.168.4.1/`
- If that local page does not open, the user is probably not connected to `SOLAR CONNECT`
- The firmware can expose the local portal again, but it cannot guarantee that the operating system will auto-open the captive portal popup on every reconnect

How this maps to the thesis user flows:
- `User Flow A: On-site network connectivity`
  This is the main implemented path in the current repo. Users join the SoftAP, are intercepted by captive DNS/HTTP, accept terms on `/`, and are sent through `/confirm` into a timed session plus one-time dashboard linking handoff.
- `User Flow B: Remote/offline dashboard access`
  This is only represented indirectly here. The firmware syncs session state to Supabase and generates a one-time linking URL, but the actual generic dashboard and automatic linked-installation experience live outside this repository.

## Persistent Configuration

Primary namespace:
- `esp32_nat`

Observed keys used by the firmware:
- `ssid`
- `passwd`
- `ent_username`
- `ent_identity`
- `static_ip`
- `subnet_mask`
- `gateway_addr`
- `mac`
- `ap_ssid`
- `ap_passwd`
- `ap_ip`
- `ap_mac`
- `portmap_tab`
- `lock`

Key semantics:
- `ssid` and `passwd`
  STA uplink credentials
- `ent_username` and `ent_identity`
  WPA2 Enterprise metadata
- `static_ip`, `subnet_mask`, `gateway_addr`
  Optional static addressing for STA
- `mac`
  Optional custom STA MAC
- `ap_ssid`, `ap_passwd`, `ap_ip`
  SoftAP settings
- `ap_mac`
  Optional custom AP MAC
- `portmap_tab`
  Saved port forwarding table blob
- `lock`
  Web/DNS startup gate, where `"0"` means web services start

Default behavior when keys are missing:
- `ssid`, `passwd`, `ent_username`, `ent_identity`, `static_ip`, `subnet_mask`, `gateway_addr` default to empty strings
- `ap_ssid` defaults to `SOLAR CONNECT`
- `ap_passwd` defaults to empty string
- `ap_ip` defaults to `192.168.4.1`
- `lock` defaults to `"0"`

External configuration:
- Supabase base URL and API key are injected through top-level `CMakeLists.txt`, optionally loaded from a local `.env`
- `.env` keys read by the build:
  - `SUPABASE_BASE_URL`, `SUPABASE_API_KEY`
  - `ADMIN_USERNAME`, `ADMIN_PASSWORD`
  - `PORT_SENSORS_ENABLED`, `PORT_SENSORS_SUPABASE_SYNC_ENABLED`, `PORT_IN_USE_THRESHOLD_MA`
- The same three port-sensor variables can also be set in the build environment and override `.env`
- The checked-in default API key is a placeholder; remote writes fail until a real key is supplied. The `.env` file in the working directory is the canonical place to set runtime sensor flags for this PCB rev (currently `PORT_SENSORS_ENABLED=1` and `PORT_SENSORS_SUPABASE_SYNC_ENABLED=1`)

## CLI Surface

Router commands from `cmd_router`:
- `set_sta`
- `set_sta_static`
- `set_sta_mac`
- `set_ap`
- `set_ap_mac`
- `set_ap_ip`
- `portmap`
- `show`

NVS commands from `cmd_nvs`:
- `nvs_set`
- `nvs_get`
- `nvs_erase`
- `nvs_namespace`
- `nvs_list`
- `nvs_erase_namespace`

System commands from `cmd_system`:
- `free`
- `heap`
- `version`
- `restart`
- `deep_sleep`
- `light_sleep`
- `tasks`

CLI behavior worth remembering:
- Config values are stored persistently in NVS
- `preprocess_string()` decodes `%xx` hex escapes and converts `+` to space
- `show` prints current Wi-Fi credentials in clear text
- `get_config_param_str()` also logs retrieved string values, including sensitive ones, to the console log

## Build And Tooling Context

Build systems present:
- ESP-IDF CMake
- Legacy ESP-IDF Makefile
- PlatformIO

Current CMake wiring:
- Top-level `CMakeLists.txt` calls `spiffs_create_partition_image(storage spiffs_image FLASH_IN_PROJECT)`
- `main/CMakeLists.txt` currently registers:
  `esp32_nat_router.c`, `http_server.c`, `dns_server.c`, `net_diag.c`, and `supabase_client.c`

PlatformIO:
- `src_dir = main`
- `framework = espidf`
- `platform = espressif32@^6.4.0`
- The ESP-IDF package is pinned to a Tasmota-hosted ESP-IDF `v5.1.2` zip
- Environments are defined for `esp32dev`, `esp32-c3-devkitm-1`, and `esp32s3box`

Partitioning:
- `partitions_example.csv` defines:
  `nvs` 24K
  `phy_init` 4K
  `factory` app 1200K
  `storage` SPIFFS 2M

Project defaults in `sdkconfig.defaults`:
- Custom partition table enabled
- FreeRTOS task stats enabled
- `CONFIG_LWIP_IP_FORWARD=y`
- `CONFIG_LWIP_IPV4_NAPT=y`
- Console defaults set toward `USB_SERIAL_JTAG`

Current checked-in `sdkconfig` in this workspace:
- Target is currently `ESP32`
- NAT and port mapping options are enabled
- Console output is currently configured for UART at `115200`, not USB Serial/JTAG

Local IDE note:
- `.vscode/settings.json` points to `C:/Espressif/frameworks/esp-idf-v5.3.1/`

## Assets And Documentation

SPIFFS assets currently flashed into `storage`:
- `spiffs_image/cea.png`
- `spiffs_image/dashboard.png`
- `spiffs_image/dashboard-ui.png`

Documentation/media files in the repo root:
- `README.md`
- `ESP32_NAT_UI3.png`
- `FlasherUI.jpg`

What those files represent today:
- `ESP32_NAT_UI3.png` and the README still document the older root-at-192.168.4.1 config page experience
- `test.html` and `spiffs_image/` align more closely with the current connected-page branding

## Current Risks

Security issues:
- Admin HTTP credentials default to `admin` / `admin123` (overridable via CMake but defaulted in source)
- Wi-Fi credentials are submitted through HTTP form parameters on `/config` over plain HTTP (acceptable for an AP-only admin surface, but worth flagging)
- Sensitive config values are printed to the serial log and `show` output

Session enforcement limitations:
- Sessions are still stored in RAM only, so active sessions do not recover across an ESP32 reboot
- NAT is enabled globally, but per-client gating is enforced at the lwIP IP hook + ACL layer, so non-admitted clients cannot egress
- Long-term linkage depends on the firmware resolving a stable `device_hash`; if MAC lookup falls back to IP-derived hashing, the PWA/backend linkage becomes weaker
- The firmware can expose the local portal at `192.168.4.1`, but it cannot guarantee that mobile and desktop operating systems will auto-open the captive portal popup on every reconnect
- The complete one-time-bind dashboard experience depends on the external PWA implementing the contract in `PWA_LINKING_CONTRACT.md`

Admin/config limitations:
- `/config` POST handler now processes `save_ap`, `save_sta`, `save_static`, and `reboot` actions; Enterprise fields are read; static IP is read
- A web STA save without explicit Enterprise fields will still leave the saved password as the previously stored value rather than blanking it (`load_config_string_or_default()`)

Hardware caveats to keep in mind:
- USB-C INA219 boards at 0x40 and 0x41 are reported faulty on the current PCB rev; live readings only render USB-A 1 and USB-A 2
- `BATTERY_VOLTAGE_DIVIDER_RATIO` is `5.5454f` for the current 100 kΩ + 22 kΩ divider; if the divider is changed, `battery_sensor.h` must be updated
- The PZEM-004T v1 module address defaults to `192.168.1.1`; this is the legacy Peacefair format, not Modbus, and not the v3 PZEM protocol

Implementation quirks to keep in mind:
- `set_ap_ip` writes `ap_ip` to NVS but does not call `nvs_commit()`, so persistence is incomplete
- `portal_authenticated` is declared globally but the authoritative gating is the per-client ACL + lwIP hook
- `LOCK_PAGE` is present but unused
- `wifi_init()` waits on an event bit before `esp_wifi_start()`, so that wait does not currently gate startup in a meaningful way

Thesis alignment gaps (remaining):
- AC outlet telemetry: PZEM reads but does not publish (see Mismatch 3)
- Battery threshold calibration vs. spec percentages (see Mismatch 1)
- LiFePO4-shaped SoC curve (see Mismatch 2)
- Firmware-side graceful shutdown (see Mismatch 4)
- Real eco-metric calculation (see Mismatch 6)
- Persistent active sessions across reboot (see Mismatch 7)

## Useful Mental Model

The current project is best understood as five overlapping systems:
- Smart Solar Hub connectivity firmware
- Router firmware and Wi-Fi/NAT control
- Captive portal and DNS/HTTP interception
- Session sync and external dashboard handoff
- Configuration and persistence through NVS plus serial CLI

When changing behavior, check these surfaces:
- Runtime logic in `main/`
- Persistence and commands in `components/cmd_router/`
- SPIFFS assets and hard-coded HTML
- External dashboard/backend assumptions
- README and context docs, which currently lag behind implementation

Also check the system boundary:
- If a requested feature belongs to charging hardware, RFID, eco-metrics, or dashboard UI behavior, verify whether it belongs in this firmware repo, the remote PWA/backend, or a separate hardware-control layer before implementing it
