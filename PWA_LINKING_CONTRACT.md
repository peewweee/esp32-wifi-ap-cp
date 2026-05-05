# PWA Linking Contract

This repo now uses a `one-time redirect link` to bind a browser installation to an ESP32-managed device identity.

The important rule is:
- `installation_id` is the long-lived PWA/browser identity
- `device_hash` is the long-lived ESP32/backend identity for that client device
- `session_token` identifies one access session, not the user forever

The user should only need to complete the redirect-based bind once per browser installation, as long as browser storage is preserved and the firmware can continue to resolve a stable `device_hash` for that device.

## Repo boundary

This repository contains:
- ESP32 firmware that creates `session_token`
- ESP32 firmware that creates `device_hash`
- Supabase SQL contract files used by the dashboard/backend integration
- ESP32 firmware that pushes USB port telemetry (`port_state`) and battery telemetry (`station_state`)

This repository does not contain:
- the real Vercel PWA source code
- the live dashboard implementation
- the live Supabase edge/backend code outside the SQL files stored here

This file therefore documents the expected contract that the PWA must implement.

## Identity model

### `installation_id`

Created by the PWA.

Properties:
- generated on first app load
- stored in persistent browser storage
- scoped to one browser storage context
- lost if browser storage is cleared

Recommended implementation:
- generate with `crypto.randomUUID()`
- store in `localStorage`

### `device_hash`

Created by the ESP32 firmware.

Properties:
- intended to be stable for the same client device
- used by Supabase to link later sessions back to the same browser install
- should not require the user to re-link every new day

Important limitation:
- if the firmware cannot reliably determine the client MAC for a given path and falls back to IP-derived hashing, long-term linkage becomes weaker and may require relinking more often than desired

### `session_token`

Created by the ESP32 firmware when internet access is granted for a session.

Properties:
- unique per session
- passed to the one-time PWA linking route
- should not be treated by the PWA as the long-term user identity

Important design rule:
- the PWA should resolve the latest session through `installation_id -> device_hash -> latest relevant session`
- the PWA should not require the user to keep opening tokenized URLs on every new day

## Expected routes

Routes currently expected by the firmware:
- Generic app: `https://solarconnect.live`
- One-time link route: `https://solarconnect.live/?session_token=<token>`

Local recovery route served by the ESP32:
- `http://192.168.4.1/`

## PWA responsibilities

### 1. Create and persist `installation_id`

On first app load:
1. check persistent storage for `installation_id`
2. if missing, create one with `crypto.randomUUID()`
3. store it locally

The PWA should treat this as the browser-install identity.

### 2. Generic dashboard route behavior

Route:
- `https://solarconnect.live`

Behavior:
1. read `installation_id`
2. call `resolve_installation_session(installation_id)`
3. if a session is found, show the user's countdown/status
4. if no session is found, show an unresolved state instead of guessing who the user is

The unresolved state should explain that the app cannot automatically identify a never-linked user in a normal browser.

Recommended unresolved-state UI:
- message:
  `Connect to SOLAR CONNECT to see your status.`
- action:
  `Open Solar Connect Portal`
- target:
  `http://192.168.4.1/`
- helper text:
  `If the page does not open, connect to SOLAR CONNECT first.`

### 3. One-time linking route behavior

Route:
- `https://solarconnect.live/?session_token=<token>`

Behavior:
1. read `session_token` from the URL
2. read or create `installation_id`
3. call `claim_session_link(session_token, installation_id)`
4. after success, remove the token from the visible URL by redirecting to the clean app root route

Recommended redirect target after success:
- `https://solarconnect.live`

Reason:
- avoids leaving `session_token` in browser history, screenshots, or accidental shares

### 4. Countdown/status behavior

When a session resolves successfully, the PWA should display at minimum:
- `remaining_seconds`
- `status`
- enough UI context to tell the user whether time is active, disconnected, expired, or unavailable

Recommended behavior:
- tick the displayed countdown locally between refreshes
- refresh from Supabase periodically
- refresh when the app becomes visible again after backgrounding

## Recovery flow for missed first-time linking

This is the recommended fallback when the user connected to `SOLAR CONNECT` but did not click the PWA redirect link the first time.

### PWA side

If the generic dashboard cannot resolve a linked session:
- show the unresolved-state message
- show the manual link to `http://192.168.4.1/`

### ESP32 side

When the user opens `http://192.168.4.1/` while connected to `SOLAR CONNECT`, the ESP32 should serve the local portal page.

That local page should make it easy to:
- confirm access if needed
- see that the user is on `SOLAR CONNECT`
- open the current unique PWA redirect link if linking is still needed

Practical outcome:
- users who missed the redirect link once still have a manual recovery path
- this recovery path does not depend on the operating system showing the captive-portal popup again

## Browser and privacy limitations

These limitations are fundamental to normal browser/PWA behavior and should be treated as design constraints.

- A normal PWA cannot read the device MAC address.
- A normal PWA cannot reliably read the current Wi-Fi SSID or BSSID across browsers.
- A normal PWA cannot obtain a stable hardware identifier for the user.
- A normal HTTPS PWA cannot reliably probe `http://192.168.4.1/` in code due to mixed-content and local-network restrictions.
- If browser storage is cleared, `installation_id` is lost.
- If the user changes browser, they are effectively a new install.
- If the user changes device, they are effectively a new install.

Important implication:
- the PWA cannot reliably auto-detect whether the user is currently connected to `SOLAR CONNECT`
- the `Open Solar Connect Portal` action should be presented as a manual recovery link, not as a guaranteed connectivity detector

## Captive portal popup limitation

The firmware can provide captive-portal interception and a local portal at `192.168.4.1`, but it cannot guarantee that iOS, Android, Windows, or other operating systems will automatically show the captive-portal assistant every time the user reconnects.

Reason:
- operating systems cache network state
- captive detection depends on OS-specific heuristics and probe behavior

Therefore:
- the auto-popup is a convenience only
- the manual `http://192.168.4.1/` recovery path is required

## Expected user experience

### First-time user opens the generic QR before ever linking

Expected behavior:
- the PWA cannot know who they are yet
- show instructions only
- offer the `http://192.168.4.1/` recovery link

### First-time user connects to `SOLAR CONNECT` and clicks the ESP32 redirect link

Expected behavior:
- the PWA binds `installation_id` to the active session's `device_hash`
- future sessions for the same device should resolve automatically without needing the token again

### User connected before but forgot to click the redirect link

Expected behavior:
- later opening the generic PWA should show the unresolved state
- user taps `http://192.168.4.1/`
- if connected to `SOLAR CONNECT`, the ESP32 local portal gives them the current redirect link/instructions

### Returning user later on the same browser/device

Expected behavior:
- opening the generic QR or dashboard route should resolve the latest relevant session automatically
- no manual pairing code should be required
- no token in the URL should be required again

### Returning user after storage clear, browser change, or device change

Expected behavior:
- the PWA no longer has the old `installation_id`
- the user must complete the one-time redirect-based link again

## Implementation notes for the PWA

Recommended dashboard states:
- linked and active
- linked but disconnected/stale
- linked but expired
- not linked yet
- cannot resolve because user is not on the local Wi-Fi and has never linked before

Recommended wording for the unresolved state:
- `Connect to SOLAR CONNECT to see your status.`
- `Open Solar Connect Portal`
- `If the page does not open, connect to SOLAR CONNECT first.`

Recommended wording for first-time instructions:
- explain that the user must connect to `SOLAR CONNECT`
- explain that they must open the local portal once and tap the app link
- explain that after the first successful link, later visits to the generic dashboard should work automatically on the same browser/device

## Station telemetry the PWA can read

These tables are populated by the ESP32 firmware and are intended for direct read by the PWA dashboard. They are not part of the linking contract per se but are the other half of the data model the dashboard depends on.

### `station_state`

Single row per `station_id` (default `solar-hub-01`). Battery telemetry pushed every 5 s by the ESP32 battery sensor task:
- `battery_percent` — 0–100 (currently a linear estimate from `battery_voltage_v` over an 11.6–13.6 V window)
- `battery_voltage_v` — battery terminal voltage in volts
- `battery_raw_mv` — calibrated ADC reading at GPIO 32 in millivolts (diagnostic)
- `battery_state` — one of `normal | warning | critical | wake_up | charging_on`
- `updated_at`

Notes for PWA implementers:
- The PWA currently only reads `battery_percent` and `updated_at`. Surfacing `battery_state` is recommended so the user understands why their session was disabled (e.g., "Wi-Fi paused — low battery").
- Treat `battery_percent` as approximate. A piecewise LiFePO4 SoC curve is planned; until then, the value at any specific voltage is a best estimate, not a fuel-gauge reading.
- The transition voltages in firmware do not yet line up with the spec's "25 % / 15 % / 10 %" thresholds — the dashboard may show ~50 % when `battery_state` flips to `warning`. This is a known firmware-side mismatch.

### `port_state`

One row per `(station_id, port_key)`. Currently populated by:
- USB ports (`usb_a_1`, `usb_a_2`, `usb_c_1`, `usb_c_2`) — INA219 sensors, event-driven Supabase sync on status flip plus 30 s heartbeat
- Outlet (`outlet`) — **only the manual `/ports` toggle**; the PZEM-004T AC reader runs but its data is not yet upserted into `port_state` or any other table

Hardware caveat:
- The USB-C INA219 boards on the current PCB rev (addresses 0x40 and 0x41) are physically faulty. The firmware skips them on the live `/port-occupied` page and the per-row sync. USB-C entries on the dashboard reflect only the manual `/ports` toggle until the PCB is fixed.

Future telemetry the PWA should be ready for (not yet present):
- AC voltage, current, power, cumulative energy from the PZEM-004T (final schema TBD — likely `station_state` extension or a new table)
- Real eco-metric values (`today_energy_wh`, `today_co2_saved_g`) once `eco_metrics.c` is enabled — until then the PWA is expected to compute its own client-side estimate
