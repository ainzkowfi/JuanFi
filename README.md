# JuanFi (ESP8266 + MikroTik RouterOS v6)

This repository contains a JuanFi-based coinslot vendo firmware for **ESP8266 (NodeMCU)** and the accompanying **MikroTik RouterOS v6** hotspot portal files. It integrates a physical coinslot with a RouterOS hotspot so customers can insert coins, receive or extend voucher time, and manage sessions through a captive portal.

> Based on upstream [JuanFi](https://github.com/ivanalayan15/JuanFi). See [`LICENSE`](LICENSE) and [`THIRD-PARTY-NOTICES`](THIRD-PARTY-NOTICES) for licensing and attribution details.

## What’s in this repo

- **[`JuanFi-nodemcu/`](JuanFi-nodemcu/)** — ESP8266 Arduino firmware (NodeMCU v2 target). Handles coinslot input, voucher/session operations against the MikroTik RouterOS API, the device-hosted admin interface, and related HTTP endpoints used by the portal.
- **[`hotspot/`](hotspot/)** — RouterOS hotspot/captive-portal files (`login.html`, `status.html`, `logout.html`, `portal.html`, assets, `settings.json`, etc.) to upload to the router’s hotspot directory.
- **[`juanfi-setup.rsc`](juanfi-setup.rsc)** — RouterOS helper/setup script for the hotspot environment.
- **[`OnLogin.txt`](OnLogin.txt) / [`OnLogout.txt`](OnLogout.txt)** — MikroTik hotspot user-profile login/logout scripts used by the voucher/session flow.

## Key capabilities

- Coinslot-driven voucher purchase/confirmation with configurable promo rates.
- Voucher login/extend flows, including merging/converting remaining time from another code onto an active session (when supported/enabled by the build/portal).
- Pause/resume session handling via the portal (where enabled).
- Multi-vendo-aware portal configuration (`hotspot/settings.json`).
- Device-hosted admin UI for system/rates configuration, dashboard/status, and voucher generation.
- RouterOS v6 integration over the plain RouterOS API (default TCP **8728**), with voucher/session operations performed on the router.

## Compatibility

- **MCU:** ESP8266 / NodeMCU (built for `esp8266:esp8266:nodemcuv2`, ESP8266 Arduino core 3.x).
- **Router:** MikroTik **RouterOS v6** with the API service enabled and reachable from the vendo.
- Captive portal behavior depends on RouterOS hotspot templates/variables and client captive browsers; validate changes on target devices.

## Building the firmware

Requires [Arduino CLI](https://arduino.github.io/arduino-cli/) with the ESP8266 core installed.

```bash
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 JuanFi-nodemcu
```

The firmware and web UI assets are served from the device (SPIFFS/filesystem image as required by your flash layout). Use the partition/flash layout appropriate for your 4 MB NodeMCU module.

## Flashing & deployment

- **Firmware/SPIFFS:** Flash the compiled firmware (and filesystem image, if used) to the ESP8266. Refer to `flashing.txt` and any prebuilt `.bin` assets provided with a release for exact offsets and steps.
- **Hotspot portal:** Upload the contents of [`hotspot/`](hotspot/) to the router’s hotspot files path (commonly `hotspot/` or `flash/hotspot/` depending on the device). Upload the set together so the aligned cache-busted assets stay consistent.
- **RouterOS:** Enable/configure the RouterOS API service and credentials to match the device configuration, and apply the hotspot scripts/setup (e.g. [`juanfi-setup.rsc`](juanfi-setup.rsc), [`OnLogin.txt`](OnLogin.txt), [`OnLogout.txt`](OnLogout.txt)) appropriate to your deployment.

## Notes

- This is RouterOS **v6**–oriented firmware/portal behavior. Do not assume RouterOS v7 API/provisioning compatibility without testing.
- Captive portals run in constrained browser environments; test end-to-end on the actual router and client devices (Android/iOS captive browsers and a regular browser) after changes.
- `hotspot/settings.json` contains deployment-specific branding/vendo addressing in this checkout; adjust it for your environment before deploying.

## License

Licensed under the **Apache License 2.0**. See [`LICENSE`](LICENSE) and [`THIRD-PARTY-NOTICES`](THIRD-PARTY-NOTICES) for full terms and upstream/third-party attribution.
