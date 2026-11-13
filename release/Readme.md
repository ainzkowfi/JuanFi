# JuanFi Firmware Releases (ESP8266)

This release folder now contains ESP8266-based firmware artifacts only. ESP32 (LAN-based ESP32) release artifacts/support have been removed from this repository state.

## Available

- Wireless/ESP8266: `release/WirelessBase/ES8622/`
- LAN/ESP8266: `release/LanBased/ESP8622/`

Each folder contains flashable `.bin` files, a flasher utility, and a local README with flashing steps.

## Notes

- Prebuilt binaries are provided as-is and are not rebuilt from the current source in this commit.
- Verify firmware provenance/integrity before flashing (hashes/signatures are not included in-repo).
- Bundled Windows flasher executables/scripts should be reviewed in a trusted environment before use.
