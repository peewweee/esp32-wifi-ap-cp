# Project Context

Last reviewed: 2026-04-07

Review basis:
- Current workspace contents, not just the last commit
- Top-level build/config files
- All authored source under `main/` and `components/`
- Current SPIFFS assets under `spiffs_image/`

Current workspace note:
- The current worktree is clean according to `git status --short`
- This document reflects the currently checked out codebase plus the thesis/product framing provided on 2026-04-07

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

Last reviewed and updated: 2026-05-01
- Full codebase analysis completed across all main/ and components/ directories
- Current implementation status documented with all mismatches identified
- System requirements vs. actual implementation gaps documented below

## Project Summary

This repository contains ESP32 firmware for the connectivity and control subsystem of the Smart Solar Hub thesis project.

The current firmware combines:
- A SoftAP that is always exposed to clients (branded as `SOLAR CONNECT`)
- An optional STA uplink to another Wi-Fi network (for MCU telemetry to Supabase)
- IPv4 NAPT and port mapping for upstream internet access
- A captive portal with session-gated internet access and terms acceptance
- RFID-based physical activation (card presence detection and GPIO control)
- Port current sensing integration (INA219 sensors, disabled by default)
- Battery percentage tracking (manual test UI; hardware integration planned)
- Battery-level-based operational thresholds (NOT YET IMPLEMENTED)
- A serial CLI for configuration and diagnostics
- SPIFFS-hosted image assets used by the portal UI
- Supabase integration for session sync, port telemetry, and battery state

Important product-level framing:
- The current root web experience is a captive portal at `/`
- The configuration UI still exists at `/config`
- The repository started from the ESP-IDF console example and an ESP32 NAT router example, then added a custom captive portal flow, diagnostics, RFID control, port sensors, Supabase sync, Vercel handoff, and Smart Solar Hub branding
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
   - Card authentication against 2 valid UID list
   - GPIO control of 6 power pins (4 MOSFETs + SSR + relay) on pins 12, 14, 27, 26, 25, 13
   - 1-second card presence timeout
   - **Limitation**: Power pins are controlled directly by RFID without checking battery level

2. **WiFi AP (SoftAP)**
   - "SOLAR CONNECT" SSID (configurable)
   - Optional STA uplink for telemetry
   - DHCP server for AP clients
   - Max 8 concurrent clients

3. **Captive Portal & Session Management**
   - Terms and conditions gate at `/` with checkbox acceptance
   - Session token generation + stable device_hash (MAC-based)
   - 3600 seconds (1 hour) daily quota per device/MAC
   - Supabase sync: heartbeat (30s), session state (active/expired/disconnected)
   - PWA one-time linking via `/confirm` redirect

4. **Port State Tracking**
   - All 5 ports defined: USB-A 1/2, USB-C 1/2, Outlet
   - Manual test UI at `/ports` with toggles
   - INA219 sensor code prepared (disabled: `PORT_SENSORS_ENABLED=0`)
   - Current threshold: 50mA (configurable) for in_use detection
   - Sends to Supabase: current_ma, bus_voltage_v, status (available/in_use/fault/offline)

5. **Manual Battery Status (Test UI)**
   - Slider at `/ports` to simulate battery percentage (0-100)
   - Sends to Supabase: `station_state.battery_percent`
   - **Important**: This is manual-only; no hardware ADC reading yet

6. **Supabase Schema & Integration**
   - `sessions` table: session_token, device_hash, installation_id, remaining_seconds, status
   - `port_state` table: port_key, status, current_ma, bus_voltage_v
   - `station_state` table: battery_percent, updated_at
   - Three RPCs defined: `claim_session_link()`, `resolve_installation_session()`, `cleanup_old_sessions()`
   - Session heartbeat task + state update queue in http_server.c

### ❌ NOT YET IMPLEMENTED

1. **AC Sensor Reading**
   - No AC current measurement code
   - Outlet port exists in schema but only manually settable in test UI
   - **Required for**: Full port telemetry per thesis design

2. **Battery Percentage Hardware Reading**
   - No ADC reading from voltage divider
   - No fuel gauge IC integration (e.g., BMS serial data)
   - LiFePO4 battery state must be read from hardware
   - **Current workaround**: Manual slider in test UI only
   - **Required for**: Operational threshold automation

3. **Battery Operational Thresholds & System State Machine** ⚠️ CRITICAL
   - **NO** state machine for battery-driven system transitions
   - **NO** WiFi (Users) disable at 25% falling
   - **NO** charging port disable at 15% falling
   - **NO** MCU-only telemetry mode at 15% falling
   - **NO** system shutdown trigger at 10% falling
   - **NO** recovery logic for 13%, 20%, 30% thresholds
   - **Impact**: System cannot degrade gracefully based on battery level
   - **Required for**: Thesis thesis compliance and energy efficiency

4. **Coordinated Power State Management**
   - **NO** global system state machine tracking:
     - card_present (RFID)
     - battery_percent (hardware input)
     - system_mode (full service / users disabled / ports disabled / MCU only / shutdown)
   - **Current behavior**: RFID controls power pins directly, independent of battery
   - **Problem**: Even at 10% battery, RFID can activate charging ports
   - **Required for**: Safe hardware operation

### Detailed Feature Gaps

| Feature | Status | Dependency | Thesis Impact |
|---------|--------|------------|---|
| AC current sensor | ❌ | Hardware ADC + sensor IC | Incomplete telemetry |
| Battery ADC reading | ❌ | Voltage divider circuit | Cannot read actual battery state |
| Battery threshold enforcement | ❌ | Battery reading + state machine | Core thesis feature missing |
| WiFi mode switching | ❌ | System state machine | Can't restrict to telemetry-only mode |
| System shutdown trigger | ❌ | Battery threshold + power control | Can't protect battery from over-discharge |
| Recovery hysteresis | ❌ | System state machine | Can't prevent battery cycling |
| Power state coordination | ❌ | System state machine + RFID refactor | Cannot enforce battery limits on ports |

## Critical Mismatches: Specification vs. Implementation

### Mismatch 1: Power Control Architecture
**Specification:** "RFID card presence activates system. Battery thresholds enforce what the system can do."
**Implementation:** RFID directly controls power pins without checking battery level.
**Impact:** At 10% battery, RFID can still turn on charging ports → battery damage risk.

### Mismatch 2: WiFi Mode Transitions
**Specification:** "25% falling: WiFi (Users) disabled. 15% falling: WiFi (MCU telemetry only)."
**Implementation:** WiFi AP is always active when RFID card is present. No mode switching.
**Impact:** Users can get WiFi at critically low battery levels. No telemetry-only survival mode.

### Mismatch 3: Graceful Shutdown
**Specification:** "10% falling: Complete system shutdown. Final telemetry sync before power loss."
**Implementation:** No shutdown logic. System continues normal operation regardless of battery.
**Impact:** System cannot prevent LiFePO4 over-discharge damage.

### Mismatch 4: AC Outlet Telemetry
**Specification:** "Send AC port availability/in-use status to Supabase."
**Implementation:** Outlet is in port list but no AC current sensor code. Only manual test UI.
**Impact:** PWA dashboard shows incomplete charging station state.

### Mismatch 5: Battery State Visibility
**Specification:** "Read and report battery percentage to dashboard."
**Implementation:** Only manual test slider. No hardware ADC integration.
**Impact:** Dashboard cannot show real battery state. Manual testing only.

### Mismatch 6: RFID-Battery Coordination
**Specification:** "RFID physically gates access. Battery level gates functionality. Combined logic."
**Implementation:** RFID and battery tracking are decoupled. No combined logic.
**Impact:** No safety override when battery critical. Firmware doesn't enforce thesis design.

## System Architecture: Current vs. Required

### Current Architecture (Incomplete)
```
RFID Card Present
    ↓
[RFID Module] → GPIO 12,14,27,26,25,13 ON (unconditional)
    ↓
Charging Ports Active
    ↓
WiFi AP Active (if card present)
    ↓
User WiFi Session (1 hour quota)
    ↓
Supabase: session tracking + manual port/battery status
```

### Required Architecture (Full Thesis Implementation)
```
RFID Card Present + Battery %
    ↓
[System State Machine]
    ├─ 100%-26%: Full Service (All ON)
    ├─ 25% Fall: Users WiFi OFF
    ├─ 15% Fall: Ports OFF, MCU Telemetry ON
    ├─ 10% Fall: SHUTDOWN
    ├─ 13% Recov: MCU Telemetry ON
    ├─ 20% Recov: Ports ON
    └─ 30% Recov: Users WiFi ON
    ↓
[Power Control]
├─ GPIO control (ports, SSR, relay)
├─ WiFi AP mode switching (full AP vs. STA-only)
├─ Session enforcement (Users vs. MCU-only)
└─ Graceful shutdown sequence
    ↓
[Supabase Telemetry]
├─ Battery %, trending direction
├─ Port current/voltage (AC + USB)
├─ System state (mode, reason)
├─ Final sync before shutdown
└─ Session state (active/paused/shutdown)
```

## Implementation Roadmap

**Phase 1: Hardware Integration** (Prerequisites)
- Implement battery ADC reading from voltage divider
- Implement AC current sensor (if hardware added)
- Test hardware inputs before state machine logic

**Phase 2: State Machine Core** (Foundation)
- Create system_state_manager module
- Implement battery threshold detection with hysteresis
- Implement state transitions (falling/recovering)
- Add state validation and logging

**Phase 3: Power & WiFi Control** (Integration)
- Refactor RFID to check system state before activating power
- Implement WiFi mode switching (AP vs. STA-only)
- Implement graceful shutdown sequence
- Add Supabase reporting of system state + reason

**Phase 4: Testing & Documentation** (Finalization)
- Validate all threshold transitions with simulated battery curves
- Document state machine behavior and recovery procedures
- Update PWA dashboard to handle new system states
- Test battery protection limits with hardware

## Repository Layout

Main authored areas:
- `main/esp32_nat_router.c`
  App entrypoint, NVS and SPIFFS init, Wi-Fi/AP+STA setup, event handlers, NAT/portmap helpers, LED thread, console loop, web/DNS startup
- `main/http_server.c`
  Captive portal HTTP server, admin config page, per-client session tracking, captive-probe handling, SPIFFS image serving, dashboard handoff, Supabase session sync
- `main/dns_server.c`
  Custom UDP DNS server used for captive portal DNS hijacking and upstream DNS forwarding
- `main/net_diag.c`
  Runtime network diagnostics, NAPT state logging, and active connectivity probes
- `main/supabase_client.c`
  REST integration for remote session creation, heartbeat updates, and disconnect status
- `main/pages.h`
  Embedded HTML for the legacy config/admin page and an unused lock page
- `components/cmd_router/cmd_router.c`
  Router-specific CLI commands and config helpers
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
- The checked-in default API key is still a placeholder, so remote session sync will not work until a real key is supplied

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

## Current Mismatches And Risks

Documentation drift:
- `README.md` still describes the old web-config-at-root behavior
- The live root route is now the captive portal, not the admin config page
- The repository currently documents a connectivity-focused firmware, while the thesis framing describes a larger smart charging station platform
- Hardware features in the thesis narrative such as RFID activation, charging-port telemetry, eco-gamification, and announcement delivery are not represented in code in this repo

Security issues:
- Admin HTTP credentials are hard-coded in source as `admin` / `admin123`
- Wi-Fi credentials are submitted through HTTP GET parameters on `/config`
- Sensitive config values are printed to the serial log and `show` output

Session enforcement limitations:
- Sessions are still stored in RAM only, so active-session recovery across ESP32 reboot is incomplete
- NAT is enabled globally, so the access-control model is still not a strict per-client firewall
- Long-term linkage depends on the firmware resolving a stable `device_hash`; if MAC lookup falls back to IP-derived hashing, the PWA/backend linkage becomes weaker
- The firmware can expose the local portal at `192.168.4.1`, but it cannot guarantee that mobile and desktop operating systems will auto-open the captive portal popup on every reconnect
- The complete one-time-bind dashboard experience depends on the external PWA implementing the contract in `PWA_LINKING_CONTRACT.md`

Admin/config limitations:
- The config page displays Enterprise and static IP fields, but the handler does not persist them from web submissions
- Web STA updates clear saved Enterprise credentials

Implementation quirks to keep in mind:
- `set_ap_ip` writes `ap_ip` to NVS but does not call `nvs_commit()`, so persistence is incomplete
- `portal_authenticated` is declared globally but is not part of the active enforcement flow
- `LOCK_PAGE` is present but unused
- `wifi_init()` waits on an event bit before `esp_wifi_start()`, so that wait does not currently gate startup in a meaningful way

Thesis alignment gaps:
- No RFID module code or MFRC522 integration is present
- No charging-port availability detection or current sensing is present
- No battery, solar, or inverter telemetry is present
- No on-device CO2-savings or gamification logic is present
- No local API for structured station metrics exists beyond `/api/status`

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
