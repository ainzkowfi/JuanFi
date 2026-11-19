# JuanFi (ESP8266 + MikroTik RouterOS v6)

Coinslot vendo firmware for **ESP8266 (NodeMCU)** plus the **MikroTik RouterOS v6** hotspot portal it talks to. A customer inserts coins, the ESP registers a hotspot user over the RouterOS API, and the portal logs them in with that code. Sessions, extensions, expiry and the unused-code list all live on the router.

> Based on upstream [JuanFi](https://github.com/ivanalayan15/JuanFi). See [`LICENSE`](LICENSE) and [`THIRD-PARTY-NOTICES`](THIRD-PARTY-NOTICES).

## Contents

| Path | What it is |
| --- | --- |
| `JuanFi-nodemcu/` | ESP8266 firmware. Coin input, API calls to the router, admin UI, portal endpoints (`/topUp`, `/checkCoin`, `/useVoucher`, `/getRates`, `/convertVoucher`, `/cancelTopUp`). |
| `hotspot/` | Portal files for the router's hotspot directory. Upload `settings.json` with them. |
| `juanfi-setup.rsc` | RouterOS setup script. Creates the data dir, site-id, NTP, API service/user group, firewall exceptions, daily re-run. |
| `OnLogin.txt` / `OnLogout.txt` | Hotspot **user profile** scripts. Paste into `default` -> Login / Logout tabs. |

```
phone browser ──► router hotspot portal (hotspot/) ──► ESP http://<vendo-ip>/{topUp,checkCoin,useVoucher}
                                   │                            │
                                   │                            └── RouterOS API :8728 ──► hotspot user create/extend
                                   └── data/site-id.txt, data/<mac>.txt ◄── on-login script writes session files
```

## Prerequisites

- MikroTik **RouterOS v6** (built and tested on 6.49.x, hAP lite).
- ESP8266 NodeMCU, 4 MB flash. Build target `esp8266:esp8266:nodemcuv2:eesz=4M2M`, ESP8266 core 3.x.
- A hotspot on the router with the ESP on the same L2 segment as the hotspot bridge.

---

# 1. RouterOS

## 1.1 Hotspot

The portal assumes a normal RouterOS hotspot. This deployment runs one hotspot per VLAN; the setup script only cares about the profile name at the top of the file (`hsprof1` by default).

```rsc
/ip hotspot profile set [find name="hsprof1"] login-by=http-chap,http-pap
/ip hotspot user profile set [find name="default"] idle-timeout=none keepalive-timeout=30s status-autorefresh=1m
```

- `http-pap` must be enabled. The portal posts the code as a plain form; chap-only breaks it.
- `idle-timeout=none` stops the router dropping a paying customer who is not generating traffic.
- Do not put the login script on the **server** profile. On Login / On Logout are properties of the **user** profile, and every voucher the ESP creates has no `profile=` so they all land on `default`.

## 1.2 Upload the portal files

Copy the contents of `hotspot/` into the router's hotspot directory (`hotspot/` on hAP lite, `flash/hotspot/` on devices that use flash storage). Upload the whole set at once; `core.js` and `core.css` carry matched version tags.

Required `hotspot/data/` directory: the setup script creates it with a dummy file because RouterOS cannot create empty directories.

## 1.3 Run the setup script

Open `juanfi-setup.rsc`, adjust the two settings at the top (`PROF`, NTP servers if needed), paste into a WinBox terminal, run once. It is idempotent and it installs itself as a daily scheduler.

What it does:

1. Detects the hotspot directory by looking for `portal.html`.
2. Creates `data/` and `data/site-id.txt` (router serial number). The portal scopes customer storage by this ID; a missing site-id means shared/incorrect voucher history per browser.
3. Sets NTP + `Asia/Manila`. **Clock must be right**: voucher expiry, `next-run`, and the on-login scheduler all read it.
4. Sets `login-by=http-chap,http-pap` and the `default` profile timeouts.
5. Adds a forward filter rule that lets hotspot traffic to ports 80/443 bypass fasttrack, and moves it to the top. Without this, some boards fasttrack the portal and logins stall.
6. Enables the API service on **8728**, creates user group `juanfi-api` with `api,read,write`, and adds an input accept for 8728 from `10.0.0.0/16`.

## 1.4 API user for the coin box

```rsc
/user add name=juanfi group=juanfi-api password=<strong-password>
```

Enter exactly these credentials in the ESP admin page. Policy `api,read,write` is enough for voucher create/extend/list. Do not hand it `full`.

If the vendo lives on a different subnet than `10.0.0.0/16`, widen or replace the source of the `juanfi api from hotspots` firewall rule, otherwise the ESP cannot connect.

## 1.5 On Login / On Logout

**Location:** WinBox -> IP -> Hotspot -> User Profiles -> `default` -> Login tab (and Logout tab). Not Server Profiles.

Paste the whole file, including the `#` header lines. Both files end with a closing brace; braces must balance.

`OnLogin.txt` does four things:

1. Reads and then **clears** the voucher comment. A non-empty comment is what the firmware reports as an *unused* code, so clearing is what takes a used code off that list. Clearing happens before any parsing that could throw.
2. Parses the comment `Xm,coins,new,configName`. The first field is pending validity minutes, the second is the coin amount, the third marks a new voucher (`0`) or an extension (`1`), the fourth is the vendo name.
3. On a new voucher, creates a scheduler named after the code that removes the user, the active session, the cookie, itself and the session file when the time is up.
4. Writes `data/<MAC-without-colons>.txt` as `<code>#<valid-until>`. The portal reads that file on load to resume a session without asking for the code again.

`OnLogout.txt` re-anchors the scheduler when a session ends by `session timeout`, so the remaining time is not lost to an already-computed next run.

**Identifiers matter:** RouterOS rejects variable names that start with `_`. If you copy a script from anywhere and logins stop clearing comments, check `/system script` compile errors and the log for a silent abort.

## 1.6 Verify the router side

```rsc
/ip hotspot user print detail where comment!=""
/file print where name~"hotspot/data"
/log print where topics~"script"
```

- Log in with a test code. `On-Login(...)` should appear in the log and the comment should go empty.
- `data/<mac>.txt` appears for that device.
- `/log print` must be free of `hotspot user ... changed` errors from the script.

---

# 2. Firmware

## 2.1 Flash

From a release (built by `.github/workflows/build-firmware.yml`):

| File | Offset | Contains |
| --- | --- | --- |
| `JuanFi-FlashFile1.bin` | `0x000000` | sketch |
| `JuanFi-FlashFile2.bin` | `0x200000` | SPIFFS (admin UI, config files) |

Flash both on a blank board. Later updates: only the file that changed. Firmware-only changes (coin logic, API, rates parsing) need `FlashFile1`; admin page or shipped config changes need `FlashFile2`. The admin page can also upload either image (`Bin 1 - sketch` / `Bin 2 - filesystem`).

## 2.2 First boot

With no valid configuration the ESP starts as an access point:

- SSID `JuanFi Setup`, password `juanfi1234`
- Box at `172.217.28.1`
- Any URL opens the admin page (captive behaviour)

Connect, log in, and fill the configuration while still on the AP. Saving applies the network settings and reboots; the box then joins the configured Wi-Fi as a station.

## 2.3 Vendo and network

| Field | Meaning |
| --- | --- |
| Vendo name | Shown in the admin header, in the voucher comment and on printed sheets. |
| Wi-Fi SSID / password | Network the ESP joins. Must be the hotspot LAN (bridge or VLAN), not a separate office AP. |
| Local IP / Subnet / Gateway / DNS | Static address of the box. Saving always stores static mode. Keep the gateway = router hotspot IP for that VLAN. |
| Router IP address | MikroTik address the ESP calls. |
| API port | 8728 unless changed in `/ip service`. |
| API username / password | Member of `juanfi-api`. |
| Admin username / password | Login for `/admin`. Set it before the box leaves the bench. |

The default network values in this repo are `10.0.0.254 / 255.255.255.0 / 10.0.0.1 / 1.1.1.1`, which suit a flat `10.0.0.0/24` hotspot. For VLAN setups each vendo keeps `10.0.<vlan>.254` and points at `10.0.<vlan>.1`.

## 2.4 Vendo behaviour and hardware

| Field | Meaning |
| --- | --- |
| Insert-wait time | How long a coin session stays armed after `/topUp` before it gives up (`5`-`600` s). |
| Auto restart ESP | Reboot interval in minutes, `0` = off. Applied by a timer, not by cron. |
| Voucher prefix | Prefix on generated codes, up to 5 chars, sanitized to safe token characters. |
| Voucher code length | Random part length, default 5, max 10. Stored in `/admin/config/vcode.data`; a missing file keeps the compiled default. |
| Hotspot profile | Profile applied to new voucher users. Empty or `default` leaves RouterOS on `default`. |
| Coinslot pin / set pin | GPIO for the coin pulse and the coin-slot enable line. |
| System ready LED / insert coin LED | Status outputs. |
| LED/relay trigger level | Active high or active low for all three outputs. |

Pin values are GPIO numbers, not NodeMCU labels: `16`=D0, `0`=D3, `2`=D4, `14`=D5, `12`=D6, `13`=D7, `15`=D8, `3`=RX. Pins cannot repeat; the page rejects duplicates.

**Saving restarts the device.** It reboots about a second after the response is sent, by design, even if a customer is mid-session. A coin pulse in that window is counted and lost and the portal reports `coin.slot.notavailable` for a couple of seconds.

## 2.5 Admin panel

`http://<vendo-ip>/admin`

- Dashboard: coins, sessions, router reachability.
- Rates: the list customers buy from.
- Generate vouchers: batch mint without coins, optional append to sheet and add-to-sales.
- Unused voucher codes: hotspot users that still carry a comment, i.e. codes that never logged in. Add time or delete from here.
- Add hotspot user: manual code with days/hours/minutes.
- Firmware update: Bin 1 / Bin 2 upload.
- Settings export/import: `system.data` and `rates.data` as plain files.

## 2.6 `system.data`

`/admin/config/system.data` is a pipe-delimited line, 26 fields:

```
vendorName|ssid|password|routerIp|apiUser|apiPassword|waitSec|adminUser|adminPassword|
coinPin|coinSetPin|readyLedPin|insertLedPin|0|voucherPrefix|setupDone|0|voucherProfile|0|
ledTrigger|ipMode|localIp|gateway|subnet|dns|apiPort
```

Legacy 25/27/30-field files are still parsed. `ipMode` is `1` in files written by this UI. Back the file up before upgrading; restore with the import button, not by hand.

---

# 3. Rates

`/admin/config/rates.data`, one row per rate, `|` between rows:

```
name#price#minutes#validity#dataLimit#profile
```

| Column | Meaning |
| --- | --- |
| `name` | Label on the button, e.g. `P5`. |
| `price` | Coins required. |
| `minutes` | Time granted. |
| `validity` | Pending validity written into the voucher comment and consumed by `OnLogin`. `0` = no expiry logic. |
| `dataLimit` | MB cap, written to `limit-bytes-total`. `0` = unlimited. Empty means `0`. |
| `profile` | Optional hotspot profile override for that rate. |

Example from this deployment:

```
P1#1#12#0##|P5#5#90#0##|P10#10#210#0##|P20#20#480#0##
```

`validity=0` and `dataLimit=0` everywhere means customers only run out of time. If you set `validity`, the comment's first field stops being `0m` and `OnLogin` applies expiry on the next login.

`/getRates` serves this file unauthenticated, so the portal and anything else on the hotspot LAN can read it. Keep it free of anything private.

---

# 4. Portal `settings.json`

Uploaded with `hotspot/`. Every key is optional; a missing file is a supported state.

| Key | Type | Meaning |
| --- | --- | --- |
| `isMultiVendo` | bool | Enables the address/interface lookup. |
| `multiVendoOption` | `0`/`1`/`2` | `0` manual picker, `1` match by hotspot address, `2` match by interface name. Fallback is the picker. |
| `multiVendoAddresses` | array | `{vendoName, vendoIp, hotspotAddress, interfaceName, brandHeaderHtml}` per vendo. |
| `vendorIpAddress` | string | Forces the ESP IP the portal calls. Empty = resolve from the multi-vendo entry. |
| `brandHeaderHtml` | string | Header markup. |
| `footerSocialUrl` / `footerSocialLabel` / `footerSocialIcon` | string | Footer link. Only `http(s)` is rendered. |
| `currency` | string | Symbol used in prices, e.g. `\u20b1`. |
| `showMemberSection` | bool | Show code field and login box. |
| `showTrialLogin` / `trialNoExtend` | bool | Trial button, and whether trial users can be topped up. |
| `macAsVoucherCode` | bool | Send the client MAC as the code. Codes become 12 hex chars instead of 5 random ones. |
| `showConvertVoucher` | bool | Show the merge-remaining-time flow (needs `/convertVoucher`, present here). |
| `showPauseTime` | bool | Show pause/resume. |
| `enableCoinslotBan` / `coinslotBanCount` / `coinslotBanMinutes` | bool / int / int | Client-side lockout after repeated failed inserts. |
| `portalDebug` | bool | On-screen `dbgLog` output. Leave off in production. |

`macAsVoucherCode` only exists in the portal; the ESP never sees the flag. With it on, hotspot users are named like `8ADF42D263F4` and the admin generator is not used for walk-in sales.

---

# 5. Multi-vendo / VLAN

One router, several hotspots, one vendo each. Each vendo is on its VLAN with its own static IP, and `settings.json` maps hotspot IP or interface name to vendo IP:

```json
{ "vendoName": "NETBRO", "vendoIp": "10.0.10.254", "hotspotAddress": "10.0.10.1",
  "interfaceName": "vlan10-pisowifi", "brandHeaderHtml": "VLAN<em>10</em>" }
```

- Option `1` matches the hotspot address the client came through; option `2` matches the interface name. No match falls back to the manual picker so the customer is never stuck.
- Every vendo needs the API input rule covering its subnet and its own API credentials.
- The on-login script writes session files per router, so a customer roaming VLANs keeps the file on the router they logged into.

---

# 6. Voucher lifecycle

1. Coins land, `/useVoucher` runs `registerNewVoucher` (create if new) then `addTimeToVoucher` (set `limit-uptime`, write the comment `Xm,coins,new,configName`).
2. Portal shows the code. The code is a hotspot user from that moment but has no session.
3. First login: `OnLogin` reads the comment, clears it, creates the expiry scheduler, writes `data/<mac>.txt`.
4. Top-up on a live session: the comment is rewritten but no login happens, so the code can show as unused until the customer reconnects. That is inherent to using the comment as the marker.
5. Scheduler fires: user, session, cookie, scheduler and file are removed.

Minted codes are checked against the router before being handed out; a collision redraws up to 5 times and then refuses the sale instead of extending a stranger's voucher. Manually typed codes are not checked, because typing an existing code is how an extension is requested.

---

# 7. Troubleshooting

| Symptom | Cause / fix |
| --- | --- |
| Portal loads, nothing happens on connect | `login-by` missing `http-pap`. |
| `coin.slot.notavailable` | Router unreachable, API user wrong, or policy missing `api,read,write`. Check `/log print` for API logins. |
| Coins counted, no user created | API user has no `write`, or the router rejected the name/limit. Check the serial log at 115200 for `registerNewVoucher trap`. |
| "Unused voucher codes" lists codes that are in use | On Login is not running. Check `/log print where topics~"script"` for `On-Login(...)`. A script with a syntax error aborts silently. |
| Session expires too early or never | Clock/NTP, or the scheduler named after the code was deleted by hand. |
| Voucher code length always 5 | Pre-v1.1 wrote `vouchercodelen.data`, a 33-char path SPIFFS cannot hold. Current builds use `vcode.data`. |
| Box stays on DHCP after saving static IP | Pre-v1.1 passed gateway/DNS in the wrong slots. Current builds call `WiFi.config(ip, gateway, subnet, dns)`. |
| `/data/<mac>.txt` 404 for a new customer | Normal. The file appears on first login. |

---

# 8. Build and release

The GitHub Actions workflow is `workflow_dispatch` only and produces both images plus release notes with offsets and sizes.

```
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2:eesz=4M2M JuanFi-nodemcu
```

Local core differences matter: CI pins ESP8266 core 3.1.2. A local 3.0.2 build is close but not byte-identical, and IRAM usage is checked in CI (fails at 95%).

Version string lives in `JuanFi-nodemcu.ino` (`CURRENT_VERSION`), shown in the admin header and About tab.

## Licence

Apache License 2.0. See [`LICENSE`](LICENSE) and [`THIRD-PARTY-NOTICES`](THIRD-PARTY-NOTICES).
