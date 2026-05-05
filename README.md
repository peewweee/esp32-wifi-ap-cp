# Smart Solar Hub — ESP32 Embedded Control Firmware

## Thesis Project

**Title:** Development and Implementation of Solar-Powered Smart Charging Station with Integrated Connectivity

**Station Brand:** SOLAR CONNECT

**Project Concept:** The Smart Solar Hub — an off-grid solar charging station that combines renewable energy harvesting, intelligent power management, RFID-based access control, and cloud-connected transparency through a Progressive Web App dashboard.

This repository contains the **ESP32 embedded control firmware** that implements:
- WiFi connectivity and captive portal access control
- RFID-based physical activation of charging hardware
- Real-time port current sensing and energy tracking
- Battery-level-based operational thresholds for safe energy management
- Cloud synchronization with Supabase for remote visibility
- PWA dashboard integration for user access and station transparency

---

## System Overview

### Architecture Layers

```
┌─────────────────────────────────────────────────┐
│  Cloud / PWA Dashboard                          │
│  (Vercel-hosted @ https://solarconnect.live)   │
└─────────────────────────────────────────────────┘
                      ↑
                 Supabase REST API
                      ↓
┌─────────────────────────────────────────────────┐
│  ESP32 Embedded Control (THIS REPO)             │
│  • WiFi AP + Captive Portal                     │
│  • Session & Access Control                     │
│  • RFID Card Authentication                     │
│  • Port Telemetry (USB-A, USB-C, AC)           │
│  • Battery Management & Thresholds              │
│  • Energy Tracking & Eco Metrics                │
└─────────────────────────────────────────────────┘
                      ↓
┌─────────────────────────────────────────────────┐
│  Power & Charging Hardware                      │
│  • Solar Panel (200W)                           │
│  • LiFePO4 Battery (12V 100Ah)                 │
│  • Charging Ports (2x USB-A, 2x USB-C, 1x AC) │
│  • Current Sensors (INA219)                     │
│  • Power Switching (MOSFET, SSR, Relay)        │
│  • RFID Reader (MFRC522)                        │
└─────────────────────────────────────────────────┘
```

### Key Features

#### ✅ Implemented
- **RFID Access Control** — MFRC522 over SPI3, two authorized 4-byte NUIDs, controls 6 power GPIOs (4 MOSFETs + SSR + relay), with a 1 s presence timeout
- **Captive Portal** — Terms acceptance gate, lwIP IPv4 input hook + per-client ACL for strict per-client egress, DHCP option 114 portal advertisement
- **Session Management** — 1-hour daily quota per device, MAC-based `device_hash`, Supabase heartbeat every 30 s
- **USB Port Telemetry** — Four INA219s on I2C, event-driven Supabase upserts on status change plus 30 s heartbeat (`.env` enables `PORT_SENSORS_ENABLED=1` and `PORT_SENSORS_SUPABASE_SYNC_ENABLED=1`)
- **AC Outlet Reader** — PZEM-004T v1 over UART2 reading V/I/P/Wh every 5 s (currently logs only — Supabase pipeline pending)
- **Battery ADC + State Machine** — GPIO 32 ADC1 channel 4 with calibration; voltage-driven state machine (`Normal / Warning / Critical / Wake Up / Charging On`) with 30 s debounce and hysteresis; auto-toggles user AP and RFID-controlled ports
- **Battery Telemetry** — Supabase upsert every 5 s of `battery_percent`, `battery_voltage_v`, `battery_raw_mv`, `battery_state`
- **Coordinated Power State** — Battery state machine drives `set_user_ap_enabled()` and `rfid_reader_set_ports_allowed()`; ports cannot energize in Critical state regardless of card presence
- **Supabase Integration** — Real-time sync of sessions, USB port state, and battery telemetry
- **PWA Linking** — One-time redirect URL binds browser to station identity
- **Serial CLI** — Configuration and diagnostics via UART console

#### 🚧 Planned / In Progress
- **PZEM → Supabase pipeline** — Map AC current/power to `port_state.outlet`, decide schema for AC voltage/power/energy, push on the same 5 s cadence
- **Battery threshold calibration** — Voltage thresholds in `battery_sensor.h` do not match the thesis spec percentages (see PROJECT_CONTEXT.md → Mismatches)
- **LiFePO4 SoC curve** — Replace the linear `voltage → percent` mapping with a piecewise lookup or coulomb counter
- **Firmware-side graceful shutdown** — The 10 % shutdown step is currently delegated to the hardware MPPT cut; a final Supabase flush before power loss is missing
- **Eco Metrics** — `eco_metrics.c` is wired but `ECO_METRICS_ENABLED=0`; needs to be enabled and fed from the INA219 + PZEM tasks
- **USB-C INA219 fix on PCB** — The 0x40 / 0x41 boards are reported faulty on the current rev; live readings only show USB-A 1 / 2
- **Persistent active sessions across reboot** — Currently RAM-only; daily quota is NVS-backed but `DEV_RESET_QUOTA_ON_BOOT=1` clears it on every boot

---

## Quick Start

### Prerequisites
- ESP32 development board (tested: ESP32D0WDQ6, ESP32-C3, ESP32-S3)
- ESP-IDF v5.1.2+ or PlatformIO
- MFRC522 RFID module (wired and live)
- INA219 current sensors on USB-A 1/2 (USB-C 1/2 boards faulty on the current PCB rev)
- PZEM-004T v1 AC monitor on UART2 (live; logs only at the moment)
- 12 V LiFePO4 battery with a resistor divider on GPIO 32 (live)

### Building

#### Using PlatformIO
```bash
pio run -e esp32dev -t upload -t monitor
```

#### Using ESP-IDF
```bash
idf.py set-target esp32
idf.py build
idf.py flash monitor
```

### First Boot

After flashing, the ESP32 will:
1. Start as WiFi access point `SOLAR CONNECT` (open network) — disabled automatically when battery state drops to Warning or below
2. Initialize port sensors (INA219 I2C bus on SDA=21 / SCL=22)
3. Initialize RFID polling
4. Initialize the PZEM-004T reader on UART2
5. Initialize the battery ADC + start the state-machine task
6. Expose captive portal at `http://192.168.4.1/`
7. Start Supabase heartbeat (if API key configured)
8. Open serial console at 115200 baud

Connect to `SOLAR CONNECT` WiFi, open browser, and you should see the terms & conditions page.

---

## Hardware Integration

### Wiring (RFID Module - MFRC522)
| MFRC522 | ESP32 |
|---------|-------|
| SDA/SS  | GPIO 5 |
| SCK     | GPIO 18 |
| MOSI    | GPIO 23 |
| MISO    | GPIO 19 |
| RST     | GPIO 4 |
| VCC     | 3.3V |
| GND     | GND |

### Power Control GPIOs
- **GPIO 12** — USB-C Port 1 MOSFET
- **GPIO 14** — USB-C Port 2 MOSFET
- **GPIO 27** — USB-A Port 1 MOSFET
- **GPIO 26** — USB-A Port 2 MOSFET
- **GPIO 25** — AC Outlet SSR
- **GPIO 13** — Relay (future use)

### Port Current Sensors (INA219)
I2C bus (GPIO 21 SDA, GPIO 32 SCL) at 100 kHz

| Port | I2C Address |
|------|-------------|
| USB-C 1 | 0x40 |
| USB-C 2 | 0x41 |
| USB-A 1 | 0x44 |
| USB-A 2 | 0x45 |

### Battery Voltage (Live)
- ADC1 channel 4 on GPIO 32, 12-bit, 0–3.3 V attenuation, 16-sample averaged
- Resistor divider: R1 = 100 kΩ (top), R2 = 22 kΩ (bottom) → ratio 5.5454× → ~2.16 V at GPIO 32 for a 12 V battery
- Calibration via curve fitting / line fitting depending on the chip variant
- 5 s sample cadence with 30 s state-change debounce

### AC Outlet (Live read, no telemetry yet)
- PZEM-004T v1 over UART2 — TX=GPIO 17, RX=GPIO 16, 9600 baud
- Reads AC voltage, current, power, and cumulative energy every 5 s
- Pipeline to Supabase / `port_state.outlet` is **not yet wired**

---

## Configuration

### Build-Time (CMake / `.env`)
The top-level `CMakeLists.txt` reads a local `.env` (key=value lines) and falls back to environment variables. Recognized keys:
```
SUPABASE_BASE_URL=https://your-project.supabase.co
SUPABASE_API_KEY=your-api-key
ADMIN_USERNAME=admin
ADMIN_PASSWORD=your-password
PORT_SENSORS_ENABLED=1                # 1 enables INA219 reads on the current bench
PORT_SENSORS_SUPABASE_SYNC_ENABLED=1  # 1 enables event-driven port_state upserts
PORT_IN_USE_THRESHOLD_MA=50.0         # mA threshold for available -> in_use
```

The current bench `.env` enables both sensor flags. CMake defaults still ship at `0/0` so a fresh checkout without an `.env` skips hardware reads.

### Runtime (Serial Console)
```bash
# Configure STA uplink (for MCU telemetry)
set_sta SSID password

# Configure AP settings
set_ap "SOLAR CONNECT" ""

# View config
show

# Reboot
restart
```

### NVS Namespace: `esp32_nat`
- `ssid` — STA SSID
- `passwd` — STA password
- `ap_ssid` — AP SSID (default: "SOLAR CONNECT")
- `ap_passwd` — AP password
- `ap_ip` — AP IP address (default: 192.168.4.1)
- `lock` — Disable web services if set to "1"

---

## Web Interface

### Captive Portal (`/`)
- Terms & conditions acceptance gate
- Redirects to `/confirm` after acceptance
- Returns connected status to OS probes

### Session Status (`/api/status`)
- JSON response with remaining seconds and session state
- Used by PWA to display countdown

### Admin Test UI (`/ports`)
- Manual toggle of port status (test/demo only)
- Slider for battery percentage simulation
- Sends to Supabase for dashboard preview

### Configuration (`/config`)
- AP/STA WiFi settings (HTTP Basic auth: admin/admin123)
- Reboot button
- **Note:** Web config is secondary; CLI is more reliable

---

## Supabase Schema

Three main tables for thesis integration:

### `sessions`
- `session_token` — Unique per login session
- `device_hash` — Stable client identity
- `installation_id` — PWA browser identity (set by one-time link)
- `remaining_seconds` — Time left in quota
- `status` — active | expired | disconnected

### `port_state`
- `station_id` — "solar-hub-01"
- `port_key` — usb_a_1 | usb_a_2 | usb_c_1 | usb_c_2 | outlet
- `status` — available | in_use | fault | offline
- `current_ma` — Current draw (mA)
- `bus_voltage_v` — Bus voltage (V)

### `station_state`
- `station_id` — "solar-hub-01"
- `battery_percent` — 0-100 (live from ADC; the `/ports` test UI can also override it manually)
- `battery_voltage_v` — battery terminal voltage in volts (live)
- `battery_raw_mv` — raw post-calibration ADC reading at GPIO 32 in mV (live)
- `battery_state` — `normal | warning | critical | wake_up | charging_on` (live)
- `updated_at` — Last update timestamp

> Migration: `supabase/003_station_state_battery_telemetry.sql` adds the three battery columns.
> The PWA currently only reads `battery_percent` and `updated_at`; the other fields are pushed but unused.

---

## Operational Thresholds

Specification (thesis):

| Battery % | Trend | Charging | WiFi (Users) | WiFi (MCU) | State |
|-----------|-------|---|---|---|---|
| 100-26% | Any | ON | ON | ON | Full Service |
| 25% | ↓ | ON | **OFF** | ON | Users WiFi Disabled |
| 15% | ↓ | **OFF** | OFF | ON | **MCU Telemetry Only** |
| 10% | ↓ | OFF | OFF | **OFF** | **SHUTDOWN** (delegated to MPPT) |
| 13% | ↑ | OFF | OFF | **ON** | Waking Up |
| 20% | ↑ | **ON** | OFF | ON | Charging Resumes |
| 30% | ↑ | ON | **ON** | ON | Full Service |

Implementation (voltage thresholds in `battery_sensor.h`):

| Spec event | Code threshold | What that voltage maps to in % |
|---|---|---|
| 25 % falling → user AP off | `12.6 V` | 50 % |
| 15 % falling → ports off | `11.8 V` | 10 % |
| 10 % shutdown | `11.6 V` (MPPT) | 0 % |
| 13 % recovering → wake | `12.4 V` | 40 % |
| 20 % recovering → ports on | `12.8 V` | 60 % |
| 30 % recovering → users on | `13.2 V` | 80 % |

> The voltage thresholds and the linear `voltage → percent` curve don't yet line up with the spec percentages. See `PROJECT_CONTEXT.md` → "Critical Mismatches" → Mismatch 1 for the calibration plan.

---

## Performance

Performance tested on ESP32D0WDQ6 with iperf3:

| Optimization | CPU Freq | Throughput | Power |
|---|---|---|---|
| `-0g` | 240 MHz | 16.0 Mbps | 1.6 W |
| `-0s` | 240 MHz | 10.0 Mbps | 1.8 W |
| `-0g` | 160 MHz | 15.2 Mbps | 1.4 W |
| `-0s` | 160 MHz | 14.1 Mbps | 1.5 W |

---

## Project Status

### Current Focus
- ✅ WiFi AP + Captive Portal (stable)
- ✅ Session management + PWA linking (stable)
- ✅ RFID card authentication with battery override (working)
- ✅ USB INA219 port sensors live for USB-A 1/2 (USB-C boards faulty on PCB)
- ✅ PZEM-004T AC reader live (read-only — no Supabase yet)
- ✅ Battery ADC + state machine driving user AP and RFID port allow/block
- 🚧 Battery threshold calibration vs. thesis-spec percentages
- 🚧 PZEM → Supabase pipeline
- 🚧 Eco metrics fed from real INA219 + PZEM data
- 🚧 Firmware-side graceful shutdown at 10 %

### Known Limitations
- Voltage-driven thresholds don't yet line up with spec percentages — see `PROJECT_CONTEXT.md`
- LiFePO4 SoC curve is approximated by a linear 11.6–13.6 V mapping; real LFP voltage is flat in the middle
- AC outlet status on the dashboard reflects only the manual `/ports` toggle until the PZEM upsert is wired
- USB-C INA219 boards (0x40, 0x41) are faulty on the current PCB rev
- No graceful firmware shutdown at battery critical threshold; relies on hardware MPPT cut
- Active sessions are RAM-only and do not survive a reboot
- `DEV_RESET_QUOTA_ON_BOOT=1` clears the daily-quota table on every boot (development convenience)
- Admin credentials default to `admin / admin123` unless overridden in `.env`

### Next Steps
1. Calibrate battery thresholds against measured battery behavior, OR move the state machine to act on percent and use voltage only as the SoC estimator
2. Replace the linear `voltage → percent` curve with a LiFePO4-shaped piecewise lookup
3. Wire PZEM readings into `port_state.outlet` (and decide where AC voltage / power / energy live)
4. Add a firmware-side `Shutdown` action that flushes Supabase before MPPT cuts power
5. Enable `ECO_METRICS_ENABLED=1` and feed the accumulator from the INA219 + PZEM tasks
6. Flip `DEV_RESET_QUOTA_ON_BOOT` to `0` and replace hard-coded admin creds for the demo

---

## Documentation

- [PROJECT_CONTEXT.md](PROJECT_CONTEXT.md) — Complete system architecture and implementation status
- [SYSTEM_ARCHITECTURE.md](SYSTEM_ARCHITECTURE.md) — Detailed subsystem design
- [PWA_LINKING_CONTRACT.md](PWA_LINKING_CONTRACT.md) — Browser-device identity binding spec
- [supabase/001_schema.sql](supabase/001_schema.sql) — Database schema and RPC definitions

---

## Repository

- **Base:** ESP-IDF + PlatformIO
- **Main:** `main/esp32_nat_router.c`, `http_server.c`, `rfid_reader.c`, `port_sensors.c`, `pzem_reader.c`, `battery_sensor.c`, `admin_ports.c`, `client_acl.c`, `lwip_hooks.c`, `eco_metrics.c`, `supabase_client.c`
- **Components:** Router CLI, NVS CLI, System CLI
- **Partition:** NVS (24K) + Factory App (1200K) + SPIFFS (2M)
- **Build Targets:** ESP32, ESP32-C3, ESP32-S3

---

## License

This project is part of a thesis submission. See repository for license details.

---

## References

- [ESP-IDF NAT Router Example](https://github.com/jonask1337/esp-idf-nat-example)
- [Supabase Documentation](https://supabase.com/docs)
- [MFRC522 RFID Reader](https://github.com/miguelbalboa/rfid)
- [INA219 Current Sensor](https://adafruit.github.io/Adafruit_INA219/)

---

**Last Updated:** May 5, 2026  
**Status:** In Active Development — Battery Threshold Calibration & PZEM Telemetry Pipeline

You can change the console output to USB_SERIAL_JTAG:

**Menuconfig:**
`Component config` -> `ESP System Settings` -> `Channel for console output` -> `USB Serial/JTAG Controller`

**Changing sdkconfig directly**
```
CONFIG_ESP_CONSOLE_UART_DEFAULT=n
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
```

[Board comparison list](https://docs.espressif.com/projects/esp-idf/en/v5.0.4/esp32/hw-reference/chip-series-comparison.html)

## Flashing the prebuild Binaries

Get and install [esptool](https://github.com/espressif/esptool):

```
cd ~
python3 -m pip install pyserial
git clone https://github.com/espressif/esptool
cd esptool
python3 setup.py install
```

Go to esp32_nat_router project directory and build for any kind of esp32 target.

For esp32:

```bash
esptool.py --chip esp32 \
--before default_reset --after hard_reset write_flash \
-z --flash_mode dio --flash_freq 40m --flash_size detect \
0x1000 build/esp32/bootloader.bin \
0x8000 build/esp32/partitions.bin \
0x10000 build/esp32/firmware.bin
```

For esp32c3:

```bash
esptool.py --chip esp32c3 \
--before default_reset --after hard_reset write_flash \
-z --flash_size detect \
0x0 build/esp32c3/bootloader.bin \
0x8000 build/esp32c3/partitions.bin \
0x10000 build/esp32c3/firmware.bin
```

As an alternative you might use [Espressif's Flash Download Tools](https://www.espressif.com/en/products/hardware/esp32/resources) with the parameters given in the figure below (thanks to mahesh2000), update the filenames accordingly:

![image](https://raw.githubusercontent.com/martin-ger/esp32_nat_router/master/FlasherUI.jpg)

Note that the prebuilt binaries do not include WPA2 Enterprise support.

## Building the Binaries (Method 1 - ESPIDF)
The following are the steps required to compile this project:

1. Download and setup the ESP-IDF.

2. In the project directory run `make menuconfig` (or `idf.py menuconfig` for cmake).
    1. *Component config -> LWIP > [x] Enable copy between Layer2 and Layer3 packets.
    2. *Component config -> LWIP > [x] Enable IP forwarding.
    3. *Component config -> LWIP > [x] Enable NAT (new/experimental).
3. Build the project and flash it to the ESP32.

A detailed instruction on how to build, configure and flash a ESP-IDF project can also be found the official ESP-IDF guide. 

## Building the Binaries (Method 2 - Platformio)
The following are the steps required to compile this project:

1. Download Visual Studio Code, and the Platform IO extension.
2. In Platformio, install the ESP-IDF framework.
3. Build the project and flash it to the ESP32.

### DNS
As soon as the ESP32 STA has learned a DNS IP from its upstream DNS server on first connect, it passes that to newly connected clients.
Before that by default the DNS-Server which is offerd to clients connecting to the ESP32 AP is set to 8.8.8.8.
Replace the value of the *MY_DNS_IP_ADDR* with your desired DNS-Server IP address (in hex) if you want to use a different one.

## Troubleshooting

### Line Endings

The line endings in the Console Example are configured to match particular serial monitors. Therefore, if the following log output appears, consider using a different serial monitor (e.g. Putty for Windows or GtkTerm on Linux) or modify the example's UART configuration.

```
This is an example of ESP-IDF console component.
Type 'help' to get the list of commands.
Use UP/DOWN arrows to navigate through command history.
Press TAB when typing command name to auto-complete.
Your terminal application does not support escape sequences.
Line editing and history features are disabled.
On Windows, try using Putty instead.
esp32>
```
