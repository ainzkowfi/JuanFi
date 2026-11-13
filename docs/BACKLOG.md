# Backlog — held ideas

Ideas reviewed and **held**: nothing in this file is implemented. It exists so the
findings from the [ZxheiFi](https://github.com/ainzkowfi/ZxheiFi) review are not lost
and so each one can be picked up later without re-reading that repo.

- Baseline: JuanFi firmware `52ab2e6` (ESP8266 only, RouterOS API transport).
- ZxheiFi is MIT (© 2026 ZxheiFi), credits `ivanalayan15/JuanFi`. **Any code copied
  from it must carry that notice** — see Licensing at the bottom.
- Most items are firmware- or portal-only and therefore **RouterOS-version independent**.
  The two that touch the router are flagged with their v6/v7 differences.

## Priority table

| # | Item | Touches | RouterOS | Size | Why |
|---|------|---------|----------|------|-----|
| 1 | Telegram sale notifier | firmware | none | S–M | Owner sees every sale/abandon on a phone |
| 2 | Voucher rollback on failed provisioning | firmware | none | S | Stops burning codes on a failed sale |
| 3 | Admin auth: salted hash + lockout | firmware | none | S | Today: plaintext password, unlimited tries |
| 4 | Router preflight + setup-script fixes | `juanfi-setup.rsc`, README | **v6/v7 differ** | S | Silent setup failures cost an evening |
| 5 | Mock API + contract tests | new `tools/` | none | M | Only way to test the portal without hardware |
| 6 | Voucher print sheet with QR | portal | none | M | Printable tickets instead of raw text codes |
| 7 | Pending-credit persistence | firmware | none | M | Power cut must not swallow paid coins |
| 8 | Coin-acceptor wiring + FAQ docs | docs | none | S | Support load |
| 9 | Portal sound cues | portal | none | S | Their `sounds/` pattern, muted by default |
| 10 | Setup Companion (desktop) | new app | **v6/v7 differ** | XL | Flash + configure + upload in one wizard |

---

## 1. Telegram sale notifier

**Goal (requested):** when a customer finishes inserting coins, send one message with
the **total amount they inserted** and what it bought.

**Hook points in our firmware**

- `useVoucher()` (line ~1266) — success path right after `updateStatistic()`: the
  "Done inserting coins / confirm" action in the portal (`#saveVoucherButton` →
  `POST /useVoucher`). This is the sale to report.
- `handleCancelTopUp()` (line ~1226) — the customer walked away after inserting coins;
  worth a "abandoned, coins not applied" message so the owner can reconcile the cash box.
- `convertVoucher()` — a converted balance move; optional, same shape as a sale.

**Message contents** (all already in RAM at those points): inserted total `totalCoin`
(pesos, integer), `timeToAdd` / `currentValidity` minutes, `currentDataLimit`,
`currentRateProfile`, voucher code, `currentMacAddress`, `currentIpAddress`, shop label
from system config. Per-customer total is naturally per-insert-session: `resetGlobalVariables()`
zeroes `totalCoin`, so a top-up of an existing voucher reports only the new money.

**Design constraints**

- Never send inline from a handler: a TLS handshake is 1–3 s and would stall the
  `/useVoucher` response the customer is waiting on.
- Bounded ring queue (their constant is `TELEGRAM_QUEUE_SIZE`, 4-ish) flushed **one
  message per `loop()` iteration**, newest-drops-oldest on overflow, `loop()` skips while
  a send is in flight.
- Multi-line message, capped length; escape per Bot API; no retry storm — one retry, then drop.
- Credentials (bot token, chat id) in system config, never in the portal JS.

**Cost / risk on our chip**

- Needs `BearSSL::WiFiClientSecure` (+`setInsecure()`, as theirs does — no cert pinning;
  acceptable for a token-authenticated notifier, but state it in the README).
- Our last clean build: IROM 360,592 B, RAM 44%, **IRAM 93%**. BearSSL's buffers are heap,
  so RAM is the pressure point, and the RSA/BearSSL code lands in flash — probably fine,
  but measure a build before committing to it.
- **Prerequisite on the router:** the EPS must reach the internet. In a hotspot setup it is
  itself a client and sits behind the walled garden (we only grant clients access *to* it via
  `/ip hotspot walled-garden ip … dst-address-list=JuanfiVendo`). Needs
  `/ip hotspot ip-binding add type=bypassed` for the board, or a LAN port outside the hotspot.
  Document this, or the notifier silently never fires. Works the same on v6 and v7.

## 2. Voucher rollback on failed provisioning

Our `useVoucher()` calls `registerNewVoucher()` then `addTimeToVoucher()`; if the second
fails we report `coin.slot.notavailable` and leave a **zero-validity hotspot user holding
that code** — the customer's coins are lost and the code is spent. `handleGenerateVouchers()`
has the same shape with `continue`.

Their `firmware/coin_slot.h:259-263` deprovisions (`…->deprovision(code, "hotspot")`) when the
provisioning step fails. Equivalent for us: on `extOk == false`, remove the user we just
registered (only if `isNewVoucher`, never the customer's pre-existing voucher), then report
the error. ~10 lines. RouterOS-version independent.

## 3. Admin auth: salted hash + login lockout

Today `ADMIN_USER`/`ADMIN_PW` sit in EEPROM as plaintext (line ~380-494), `adminAuth` is a
static base64 Basic string, and `isAuthorized()` (line ~505) compares one header — unlimited
retries, so the admin page on the hotspot LAN can be brute-forced offline-free.

Their `firmware/admin_api.h` stores salted MD5 with `salt=""` for legacy rows, so old
passwords keep working, and `gui_handler.h` returns **429** after `LOGIN_MAX_FAILURES`
with a `LOGIN_LOCKOUT_MS` window.

For us: keep Basic auth (the portal's admin pages already send it), but store
`salt:md5(salt+pw)` in EEPROM, migrate the existing plaintext value on first boot, add a
failure counter + lockout, and keep `MD5Builder` (already included) for the digest. Also
rate-limit `handleNotAuthorize()` responses with a small delay. Independent of RouterOS.

## 4. Router preflight + setup-script fixes

Theirs learns these the hard way and documents them; we hit the same class of failure silently
because `juanfi-setup.rsc` wraps everything in `on-error`.

- **`/system device-mode`** — on a fresh RouterOS **7** router, hotspot and/or scheduler can be
  forbidden until
  `/system device-mode update hotspot=yes scheduler=yes` + power cycle. **v7 only** — the menu
  does not exist on v6, so the check must be guarded (`:if ([:len [/system device-mode print]] > 0)`)
  or skipped on 6.x. We add three schedulers and never check this.
- **`/ip hotspot reset-html`** — the hotspot `html-directory` may not exist yet (and its path
  differs: `hotspot` on hAP lite, `flash/hotspot` on hEX). Read it from
  `/ip hotspot profile` (we already do, line ~16) and verify the folder before assuming;
  document `reset-html` as the recovery step. Exists on both v6 and v7.
- **Bug in our script, line ~47:** the "hotspot before fasttrack" rule uses
  `place-before="top"`, which is not a valid value — the `add` fails and `on-error` swallows it,
  so the rule is never added/ordered. Same bug already fixed for the API rule via
  `add` + `/ip firewall filter move … destination=0`. Fix: `add` then `move` to position.
- Add the failure table (symptom → cause → command) from their
  `mikrotik/import_guide.md` and `docs/08-faq-troubleshoot.md` to our README/FAQ.

## 5. Mock API + contract tests

Their `tools/mock_server.py` impersonates the firmware's JSON API (and serves the GUI on the
same origin), and `tools/regression_test.py` starts it and asserts the contract: auth, response
codes, required fields, error codes. Their suite runs in seconds, needs no hardware, and exits
non-zero on failure — I ran it here: **103 passed, 0 failed**.

We have no automated checks at all. Adapt the pattern to our API: `/checkCoin` (both states),
`/useVoucher` success + `coin.slot.notavailable` + replay, `/topUp` (`invalid.voucher`,
`coinslot.busy`), `/convertVoucher` + its refusals, admin endpoints 401/200. This also gives the
portal a target for browser testing and gives CI something beyond "it compiles".

## 6. Voucher print sheet with QR

Our `/admin/voucher-generate.html` emits text codes. Theirs renders printable tickets with QR
codes (`mikrotik/gui/print.html`, vendored `qrcode.min.js` from davidshimjs/qrcodejs, MIT) and
an offline batch generator (`vouchers/generator.py`, needs `qrcode` + `pillow`, emits CSV ledger
+ tickets). For us: a print view in the portal admin, QR encoding the voucher code, plus an
optional CSV export the shop can keep. Portal-side only, so RouterOS-version independent.
Their login page also scans QR with the browser's native `BarcodeDetector` — nice-to-have, but
it needs HTTPS or localhost in Chrome and is not available in the in-app browsers many phones
use for captive portals, so treat it as optional.

## 7. Pending-credit persistence

Our pending coin total (`totalCoin`) and the active voucher live in RAM. A brownout between
"coins accepted" and "confirmed" (or during a RouterOS outage, where we deliberately keep
state so the customer can retry) loses the customer's money. Theirs persists sessions to SPIFFS
(`firmware/session.h`).

For us: write a tiny record (`voucher|totalCoin|targetMilis|mac`) to SPIFFS after each
`/checkCoin` acceptance and clear it on success/expiry, then offer "you have ₱X unclaimed"
on the login page after reboot. Deliberately **not** ArduinoJson — hand-rolled parse, to keep
the flash/IRAM budget we already track.

## 8. Coin-acceptor wiring + FAQ docs

Three-phase pulse input, common-GND pitfalls, 12 V supply, which pin (ours: `COIN_SELECTOR_PIN`,
RISING interrupt, `coin` counter), optical vs mechanical acceptors, and the "coins counted but
value wrong" symptom. Theirs is `docs/11-coin-acceptor-wiring.md`; the table style of their
`docs/08-faq-troubleshoot.md` is the reusable part.

## 9. Portal sound cues

They ship `sounds/*.mp3` with a global mute default. Ours has `hotspot/assets/sounds/` but no
consistent use. Low priority; must respect the browser autoplay rule (first tap unlocks audio).

## 10. Setup Companion (desktop)

Their `desktop-app/` is ~5k lines Python/CustomTkinter: esptool flash with MAC read-back,
MikroTik configure with a "is this the right router" check and per-version command plans,
GUI upload over FTP (enabling/restoring the service, verifying sizes), bilingual guide. It is
their biggest usability win and a project in its own right — held as a direction, not a port.
It is also the only item where **v6 vs v7 matters a lot**: their command plans are v7-shaped
(`/system device-mode`, `/interface wifi` instead of `/interface wireless`, hotspot as a separate
package on some v7 devices). A v6-safe version would need a version probe
(`/system resource print` → `version`) and two command sets; the majority of the user's routers
are v6.

---

## Considered and rejected

- **Router-served GUI** (their whole model: pages in the RouterOS hotspot folder, JS calling the
  board's API cross-origin). Opposite of ours and it adds four failure modes — static DNS entry,
  walled-garden accept, CORS, reserved DHCP lease — each presenting as "the page does nothing".
  Not adopting.
- **"Remove then add" hotspot user creation** — our collision-safe redraw is better in a shared
  code space.
- **Exact-denomination coin matching** (any pulse that matches no rate is discarded into an
  `unmatchedPesos` counter) — our greedy largest-rate-first accumulation is friendlier for
  ₱1/₱5 coins. Worth taking only the visible "unmatched coins" feedback, not the rule.
- **PPPoE, subscriptions, multi-admin roles, QoS, content filter** — their product scope.
- **ArduinoJson + `std::vector<String>` throughout** — their firmware is 554 KB vs our 360 KB;
  we do not have that headroom (IRAM 93%).

## Licensing

ZxheiFi is MIT and ships `THIRD-PARTY-NOTICES.md`. This repo currently has **no LICENSE file**,
and upstream JuanFi's licensing is unclear. Before porting any code from the fork: add a LICENSE
for our own work and a `THIRD-PARTY-NOTICES.md` carrying the ZxheiFi MIT notice
("Copyright (c) 2026 ZxheiFi"), and attribute Ivan Julius Alayan's original project as they do.
