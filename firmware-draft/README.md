# Firmware draft archive

This folder keeps the previous **telnet-based** JuanFi firmware so the older build stays
reproducible. It is an unchanged snapshot and is not the firmware to flash.

| Folder | Transport | Config version | Status |
| --- | --- | --- | --- |
| `JuanFi-nodemcu-telnet-v2.4/` (here) | telnet CLI, port 23, `ESP8266TelnetClient` | 25 fields (no API port) | archived, superseded |
| `../JuanFi-nodemcu/` | RouterOS API, port 8728 (login v6.43+ with legacy fallback) | 26 fields (`mikrotikApiPort` last) | current |

## Build notes for the archived snapshot

- Open `JuanFi-nodemcu-telnet-v2.4/JuanFi-nodemcu.ino` in the Arduino IDE and install
  **ESP8266TelnetClient** (Library Manager) in addition to the ESP8266 core, as the
  original firmware required.
- Upload the `data/` folder with the LittleFS/SPIFFS uploader as usual.
- `data/admin/config/system.data` here still has 25 fields. Do not copy it over the
  current firmware's file: the current build expects a 26th trailing field
  (RouterOS API port, `8728`).
- The current firmware's admin page rewrites `system.data` with the API port appended,
  so an older config file is upgraded automatically by the firmware's 25-field
  compatibility path if you flash it onto an existing device.
