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
- **RFID Access Control** — Valid cards activate charging ports and WiFi user access
- **Captive Portal** — Terms acceptance gate for users joining the WiFi network
- **Session Management** — 1-hour daily quota per device with one-time PWA linking
- **Port Telemetry** — USB port current sensing (INA219, 4 ports)
- **Supabase Integration** — Real-time sync of sessions, port state, battery level
- **PWA Linking** — One-time redirect URL binds browser to station identity
- **Eco Metrics Framework** — Energy (Wh) and CO2 savings (g) calculation ready
- **Serial CLI** — Configuration and diagnostics via UART console

#### 🚧 Planned / In Progress
- **Battery ADC Reading** — Measure LiFePO4 state of charge from voltage divider
- **AC Sensor Reading** — Current measurement for AC outlet
- **Battery Operational Thresholds** — Graceful system degradation:
  - **25% falling:** WiFi (Users) disabled, charging continues
  - **15% falling:** Charging ports OFF, WiFi (MCU telemetry only)
  - **10% falling:** Complete system shutdown to protect battery
  - **13%-30% recovering:** Asymmetric recovery to avoid battery cycling
- **System State Machine** — Global coordination of RFID + battery level + WiFi mode
- **Power State Coordination** — Override RFID when battery critical
- **Graceful Shutdown** — Final telemetry sync before power loss

---

## Quick Start

### Prerequisites
- ESP32 development board (tested: ESP32D0WDQ6, ESP32-C3, ESP32-S3)
- ESP-IDF v5.1.2+ or PlatformIO
- MFRC522 RFID module (wired, currently disabled)
- INA219 current sensors (optional, disabled by default)
- LiFePO4 battery with voltage divider (planned)

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
1. Start as WiFi access point `SOLAR CONNECT` (open network)
2. Initialize RFID polling (disabled if `RFID_ENABLED=0`)
3. Expose captive portal at `http://192.168.4.1/`
4. Start Supabase heartbeat (if API key configured)
5. Open serial console at 115200 baud

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

### Battery Voltage (Planned)
- Voltage divider on GPIO (ADC input) → LiFePO4 battery
- BMS or fuel gauge IC (if available)

---

## Configuration

### Build-Time (CMake)
```cmake
PROJECT_SUPABASE_BASE_URL=https://your-project.supabase.co
PROJECT_SUPABASE_API_KEY=your-api-key
PROJECT_ADMIN_USERNAME=admin
PROJECT_ADMIN_PASSWORD=your-password
PORT_SENSORS_ENABLED=0  # Set to 1 to enable INA219 reading
PORT_SENSORS_SUPABASE_SYNC_ENABLED=0  # Set to 1 for automatic sync
```

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
- `battery_percent` — 0-100 (from ADC, currently manual test UI)
- `updated_at` — Last update timestamp

---

## Operational Thresholds (In Development)

The system will enforce these battery-level-based modes to maximize solar efficiency and protect hardware:

| Battery % | Trend | Charging | WiFi (Users) | WiFi (MCU) | State |
|-----------|-------|---|---|---|---|
| 100-26% | Any | ON | ON | ON | Full Service |
| 25% | ↓ | ON | **OFF** | ON | Users WiFi Disabled |
| 15% | ↓ | **OFF** | OFF | ON | **MCU Telemetry Only** |
| 10% | ↓ | OFF | OFF | **OFF** | **SHUTDOWN** |
| 13% | ↑ | OFF | OFF | **ON** | Waking Up |
| 20% | ↑ | **ON** | OFF | ON | Charging Resumes |
| 30% | ↑ | ON | **ON** | ON | Full Service |

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
- ✅ RFID card authentication (working)
- ✅ Port sensor code prepared (INA219)
- 🚧 Battery threshold state machine (design phase)
- 🚧 Power state coordination (design phase)

### Known Limitations
- RFID power control is independent of battery level (will be coordinated)
- WiFi AP always active when RFID card present (need mode switching)
- Battery reading only via manual test UI (need ADC integration)
- AC outlet has no current sensor yet (waiting for hardware)
- No graceful shutdown at battery critical threshold (planned)

### Next Steps
1. Implement battery ADC reading from LiFePO4 voltage divider
2. Build battery-aware state machine with hysteresis
3. Integrate WiFi mode switching (AP vs. STA-only) based on state
4. Refactor RFID module to respect battery thresholds
5. Add system state reporting to Supabase and PWA dashboard
6. Validate full threshold cycle with simulated battery curves

---

## Documentation

- [PROJECT_CONTEXT.md](PROJECT_CONTEXT.md) — Complete system architecture and implementation status
- [SYSTEM_ARCHITECTURE.md](SYSTEM_ARCHITECTURE.md) — Detailed subsystem design
- [PWA_LINKING_CONTRACT.md](PWA_LINKING_CONTRACT.md) — Browser-device identity binding spec
- [supabase/001_schema.sql](supabase/001_schema.sql) — Database schema and RPC definitions

---

## Repository

- **Base:** ESP-IDF + PlatformIO
- **Main:** `main/esp32_nat_router.c`, `http_server.c`, `rfid_reader.c`, `port_sensors.c`, `supabase_client.c`
- **Components:** Router CLI, NVS CLI, System CLI, LWIP hooks
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

**Last Updated:** May 1, 2026  
**Status:** In Active Development — Battery Thresholds & State Machine Phase

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
