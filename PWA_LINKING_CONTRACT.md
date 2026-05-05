# PWA Linking Contract

Last updated: 2026-05-06

This file documents the contract between this ESP32 firmware repo, Supabase, and the external PWA repo.

The PWA source is not in this repository. The ESP32 firmware is responsible for generating session/device identifiers and publishing raw station inputs. The PWA is responsible for persistent browser identity, dashboard rendering, and estimated CO2 savings.

## Identity Model

### `installation_id`

Created by the PWA.

Properties:
- long-lived browser/PWA installation identity
- generated on first app load
- stored in persistent browser storage
- lost if browser storage is cleared
- scoped to one browser profile/app install

Recommended implementation:
- generate with `crypto.randomUUID()`
- store in `localStorage`

### `device_hash`

Created by the ESP32 firmware.

Properties:
- intended to identify the client device from the ESP32/backend side
- derived from client MAC when available
- weaker if firmware has to fall back to an IP-derived hash
- used by Supabase to resolve future sessions for the same linked install

### `session_token`

Created by the ESP32 firmware when access is granted.

Properties:
- unique per access session
- passed once to the PWA for linking
- not a permanent user identity
- should be removed from the visible URL after successful claim

## Expected Routes

Firmware currently links users to:

```text
https://solarconnect.live/?session_token=<token>
```

Generic PWA route:

```text
https://solarconnect.live
```

Local ESP32 recovery route:

```text
http://192.168.4.1/
```

## First-Time Linking Flow

1. User connects to `SOLAR CONNECT`.
2. ESP32 captive portal opens or user manually visits `http://192.168.4.1/`.
3. User accepts terms.
4. ESP32 creates a `session_token` and `device_hash`.
5. ESP32 shows the PWA link with `session_token`.
6. PWA reads or creates `installation_id`.
7. PWA calls `claim_session_link(session_token, installation_id)`.
8. Supabase links the browser installation to the device/session identity.
9. PWA redirects to the clean root route without the token.

Recommended clean redirect:

```text
https://solarconnect.live
```

## Returning User Flow

On a normal visit to `https://solarconnect.live`:

1. PWA reads `installation_id`.
2. PWA calls `resolve_installation_session(installation_id)`.
3. Supabase resolves the latest relevant session through the linked `device_hash`.
4. PWA renders the current status/countdown if found.
5. If no session is found, PWA shows an unresolved state.

The PWA should not require a tokenized URL every day for the same browser/device.

## Unresolved State

If the PWA cannot resolve a linked session, show instructions instead of guessing.

Recommended wording:

```text
Connect to SOLAR CONNECT to see your status.
```

Recommended action:

```text
Open Solar Connect Portal
```

Target:

```text
http://192.168.4.1/
```

Helper:

```text
If the page does not open, connect to SOLAR CONNECT first.
```

## Browser Limitations

These are normal browser/PWA constraints:

- the PWA cannot read the user's MAC address
- the PWA cannot reliably read Wi-Fi SSID/BSSID
- the PWA cannot access a stable hardware identifier
- an HTTPS PWA cannot reliably probe `http://192.168.4.1/` in code
- browser storage clear means `installation_id` is lost
- browser/device changes require linking again
- operating systems may not reopen the captive portal popup on every reconnect

Therefore:
- the local portal link is a manual recovery path
- captive portal popup behavior should be treated as a convenience, not a guaranteed entry point

## Firmware Session Contract

The ESP32 publishes session rows through Supabase REST.

Important fields:
- `session_token`
- `device_hash`
- `remaining_seconds`
- `status`
- `ap_connected`
- `last_heartbeat`
- `session_start`
- `session_end`

Typical statuses:
- `active`
- `expired`
- `disconnected`

Current firmware behavior:
- daily quota is 3600 seconds per device/MAC
- active sessions are RAM-only
- quota records are NVS-backed
- `DEV_RESET_QUOTA_ON_BOOT=1` clears quota on every ESP32 boot for dev testing

PWA implication:
- if the ESP32 reboots while the dev flag is on, Supabase may show a fresh quota/session path after the device comes back
- production/demo firmware should use `DEV_RESET_QUOTA_ON_BOOT=0` if daily quota must survive power loss

## Station Telemetry Contract

These tables are populated by the ESP32 firmware and can be read by the PWA dashboard.

### `station_state`

One row per `station_id`, default:

```text
solar-hub-01
```

Battery fields published by the battery task:
- `battery_percent`
- `battery_voltage_v`
- `battery_raw_mv`
- `battery_state`
- `updated_at`

Battery notes:
- `battery_percent` is currently a linear estimate from voltage
- current code maps 11.6 V to 0% and 13.9 V to 100%
- this is not yet calibrated to the thesis percentage threshold table
- `battery_state` is more useful for explaining why a service is disabled

AC fields published by the PZEM task:
- `ac_voltage_v`
- `ac_current_a`
- `ac_power_w`
- `ac_energy_wh`
- `ac_energy_wh_today`

AC notes:
- current firmware uses PZEM-004T v3.0 Modbus RTU
- AC fields are already upserted by firmware
- `ac_energy_wh_today` is a raw input for estimated CO2 savings

### `port_state`

One row per `(station_id, port_key)`.

Expected port keys:
- `usb_a_1`
- `usb_a_2`
- `usb_c_1`
- `usb_c_2`
- `outlet`

USB port rows are populated by:
- INA219 status sync task
- manual `/ports` test UI

Current USB sensor upsert includes:
- `station_id`
- `port_key`
- `status`
- `daily_in_use_seconds`

Current USB sensor upsert does not include:
- `current_ma`
- `bus_voltage_v`

Those columns exist in the schema and the ESP32 can read them locally, but the current event-driven Supabase payload does not send them.

Outlet row is populated by:
- PZEM power threshold logic
- manual `/ports` test UI

Current outlet rule:
- `in_use` when `ac_power_w > 1.0`
- `available` otherwise

Hardware caveat:
- USB-C INA219 hardware is faulty on the current PCB revision
- USB-C dashboard values may be missing/stale until the hardware is fixed

## Estimated CO2 Contract

Eco metrics are estimated CO2 savings.

The firmware does not need to calculate final CO2 savings. The PWA calculates it from raw telemetry.

Firmware raw inputs:
- USB-A `daily_in_use_seconds`
- USB-C `daily_in_use_seconds`
- `ac_energy_wh_today`

PWA formula:

```text
(10 * USB-A hours + 15 * USB-C hours + acEnergyWhToday) * 0.70 = grams CO2 saved
```

Recommended interpretation:
- `USB-A hours` = sum of USB-A port `daily_in_use_seconds` divided by 3600
- `USB-C hours` = sum of USB-C port `daily_in_use_seconds` divided by 3600
- `acEnergyWhToday` = latest `station_state.ac_energy_wh_today`
- result is displayed as estimated grams CO2 saved

`main/eco_metrics.c` is disabled/stubbed and is not part of the current PWA calculation path.

## Battery/Service State Display Guidance

The PWA should be prepared to explain service availability using firmware state:

| Firmware/input state | Recommended user-facing meaning |
|---|---|
| `battery_state = normal` | full service available if RFID/session conditions allow |
| `battery_state = warning` | user Wi-Fi may be disabled when threshold enforcement is enabled |
| `battery_state = critical` | charging ports blocked when threshold enforcement is enabled |
| `battery_state = wake_up` | station is recovering, telemetry only |
| `battery_state = charging_on` | charging ports can resume, user Wi-Fi still recovering |

Current caveat:
- local `.env` sets `BATTERY_SENSOR_ENFORCE_THRESHOLDS=0`, so state labels may publish without the firmware actually disabling AP/ports in dev builds

## RFID/User Wi-Fi Caveat

Expected thesis behavior:
- RFID gates charging ports and user Wi-Fi

Current firmware behavior:
- RFID gates charging power GPIOs only
- user Wi-Fi is not currently linked to RFID presence

PWA implication:
- do not assume a valid RFID card is required for a Wi-Fi session unless firmware is updated to enforce that rule

## Supabase RPC Expectations

The PWA/backend should provide or preserve:
- `claim_session_link(session_token, installation_id)`
- `resolve_installation_session(installation_id)`
- `cleanup_old_sessions()` if used operationally

The SQL files in this repo document the intended tables and RPC signatures, but the live Supabase project remains the operational source for deployed RPC bodies and policies.

## Recommended PWA States

The dashboard should handle:
- linked and active
- linked but disconnected
- linked but expired
- not linked yet
- station telemetry stale/offline
- battery low/service unavailable
- no current Supabase row
- dev reset/reboot behavior during testing

## Manual Recovery

When a user missed the first linking click:

1. PWA shows unresolved state.
2. User connects to `SOLAR CONNECT`.
3. User opens `http://192.168.4.1/`.
4. ESP32 local portal provides the current session/link path.
5. PWA claims the session token and stores/uses `installation_id`.

This path avoids relying on the OS captive portal popup reopening automatically.
