# PWA Linking Contract

This repo now sends users to a one-time linking route instead of directly to a dashboard token view.

## PWA responsibilities

1. On first load, generate an `installation_id` with `crypto.randomUUID()`.
2. Save `installation_id` in persistent browser storage.
   Recommended: `localStorage`.
3. On the generic dashboard route:
   - read `installation_id`
   - call `resolve_installation_session(installation_id)`
   - if a session is found, show that user's countdown
   - if no session is found, show instructions:
     `Connect to SOLAR CONNECT and click the redirect link once.`
4. On the one-time linking route:
   - read `session_token` from the query string
   - read or create `installation_id`
   - call `claim_session_link(session_token, installation_id)`
   - after success, redirect to the normal dashboard route

## Routes expected by the firmware

- Generic app: `https://spcs-v1.vercel.app/dashboard`
- One-time link route: `https://spcs-v1.vercel.app/dashboard/link?session_token=<token>`

## Browser limitations to design around

- A normal PWA cannot read the device MAC address.
- A normal PWA cannot reliably read the current Wi-Fi SSID.
- A normal PWA cannot identify the same user after storage is cleared.
- If browser storage is cleared, the user must link again once through the ESP32 redirect flow.
- If the user changes browser or device, the user must link again once through the ESP32 redirect flow.

## Expected user experience

- First-time user opening the generic QR:
  show instructions only.
- First-time user clicking the ESP32 redirect link:
  link the browser install to the active device session.
- Returning user opening the generic QR on the same browser later:
  automatically resolve and show the correct countdown.
