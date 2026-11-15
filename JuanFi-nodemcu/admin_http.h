#pragma once

#include <bearssl/bearssl.h>

String generateSessionToken() {
  String t = "";
  const char *hex = "0123456789abcdef";
  for (int i = 0; i < 32; i++) {
    t += hex[ESP.random() & 0x0F];
  }
  return t;
}

bool hasValidAdminSession() {
  if (adminSessionToken.length() == 0) return false;
  String cookie = server.header("Cookie");
  if (cookie.length() == 0) return false;
  String needle = "jf_admin=" + adminSessionToken;
  int p = cookie.indexOf(needle);
  if (p < 0) return false;
  int end = p + needle.length();
  return (end == (int)cookie.length() || cookie.charAt(end) == ';');
}

bool isAuthorized() {
  if (hasValidAdminSession()) return true;
  String auth = server.header("Authorization");
  if (auth.length() == 0) return false;
  String expectedAuth = "Basic " + adminAuth;
  return auth == expectedAuth;
}

void handleNotAuthorize() {
  // No WWW-Authenticate challenge: that breaks phone captive mini-browsers
  // when setup mode redirects them here. The login page handles auth.
  server.send(401, "text/plain", "Authentication failed");
}

void handleAdminLoginPage() {
  handleFileRead("/admin/login.html");
}

void handleAdminLogin() {
  unsigned long now = millis();
  if (adminLockoutUntilMs != 0 && (long)(adminLockoutUntilMs - now) > 0) {
    unsigned long remainMs = adminLockoutUntilMs - now;
    unsigned long remainSec = (remainMs + 999UL) / 1000UL;
    char msg[64];
    snprintf(msg, sizeof(msg), "Too many failed attempts. Try again in %lu s.", remainSec);
    server.send(429, "text/plain", msg);
    return;
  }
  if (adminLockoutUntilMs != 0 && (long)(adminLockoutUntilMs - now) <= 0) {
    adminLockoutUntilMs = 0;
    adminLoginFails = 0;
  }

  String u = server.arg("username");
  String p = server.arg("password");
  if (u == ADMIN_USER && p == ADMIN_PW) {
    adminLoginFails = 0;
    adminLockoutUntilMs = 0;
    if (adminSessionToken.length() == 0) adminSessionToken = generateSessionToken();
    server.sendHeader("Set-Cookie", "jf_admin=" + adminSessionToken + "; Path=/; HttpOnly; SameSite=Lax");
    server.send(200, "text/plain", "ok");
    return;
  }

  adminLoginFails++;
  if (adminLoginFails >= ADMIN_MAX_FAILS) {
    adminLoginFails = 0;
    adminLockoutUntilMs = millis() + ADMIN_LOCKOUT_MS;
    char msg[64];
    snprintf(msg, sizeof(msg), "Too many failed attempts. Try again in %lu s.", (unsigned long)(ADMIN_LOCKOUT_MS / 1000UL));
    server.send(429, "text/plain", msg);
    return;
  }
  server.send(401, "text/plain", "Invalid username or password");
}

void handleNotFound() {
  if (server.method() == HTTP_OPTIONS) {
    setupCORSPolicy();
    server.send(204);
    return;
  }
  server.send(404, "text/plain", "");
}

void handleLogout() {
  adminSessionToken = "";
  server.sendHeader("Set-Cookie", "jf_admin=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax");
  server.send(200, "text/plain", "ok");
}

void handleAdminPage() { if (!isAuthorized()) { server.sendHeader("Location", "/admin/login", true); server.send(302, "text/plain", ""); return; } handleFileRead("/admin/system-config.html"); }
void handleAdminGeneratedVoucherPage() { if (!isAuthorized()) { server.sendHeader("Location", "/admin/login", true); server.send(302, "text/plain", ""); return; } handleFileRead("/admin/voucher-generate.html"); }
void handleJquerySript() { handleFileRead("/admin/js/jquery.min.js"); }
void handleUserGetRates() { setupCORSPolicy(); handleFileRead("/admin/config/rates.data"); }
void handleAdminGetRates() { if (!isAuthorized()) { handleNotAuthorize(); return; } handleFileRead("/admin/config/rates.data"); }

void handleAdminSaveRates() {
  if (!isAuthorized()) { handleNotAuthorize(); return; }
  // Bind the request argument by reference: the body itself is one unavoidable
  // allocation inside server.arg(), so do not copy up to 4 KB of it again.
  const String &data = server.arg("data");
  if (data.length() > 4096) { server.send(413, "text/plain", "too large"); return; }
  if (!handleFileWrite("/admin/config/rates.data", data)) { server.send(500, "text/plain", "write failed"); return; }
  populateRates();
  server.send(200, "text/plain", "ok");
}

void handleAdminSaveSystemConfig() {
  if (!isAuthorized()) { handleNotAuthorize(); return; }
  // See handleAdminSaveRates: keep a reference so the config body is not copied twice.
  const String &data = server.arg("data");
  if (data.length() > 4096) { server.send(413, "text/plain", "too large"); return; }
  if (!handleFileWrite("/admin/config/system.data", data)) { server.send(500, "text/plain", "write failed"); return; }
  server.send(200, "text/plain", "ok");
  delay(1000);
  ESP.restart();
}

void handleAdminGetSystemConfig() { if (!isAuthorized()) { handleNotAuthorize(); return; } handleFileRead("/admin/config/system.data"); }

void handleAdminResetStats() {
  if (!isAuthorized()) { handleNotAuthorize(); return; }
  String type = server.arg("type");
  if (type == "lifeTimeCount") eeWriteIntNoCommit(LIFETIME_COIN_COUNT_ADDRESS, 0);
  else if (type == "coinCount") eeWriteIntNoCommit(COIN_COUNT_ADDRESS, 0);
  else if (type == "customerCount") eeWriteIntNoCommit(CUSTOMER_COUNT_ADDRESS, 0);
  EEPROM.commit();
  server.send(200, "text/plain", "ok");
}

void handleAdminDashboard() {
  if (!isAuthorized()) { handleNotAuthorize(); return; }
  unsigned long upTime = millis();
  int lifeTimeCoinCount = eeGetInt(LIFETIME_COIN_COUNT_ADDRESS);
  int coinCountVal = eeGetInt(COIN_COUNT_ADDRESS);
  int customerCount = eeGetInt(CUSTOMER_COUNT_ADDRESS);
  bool hasInternetConnection = true;
  // Refresh mikrotik status opportunistically (lightweight) without blocking long; rely on ensure state
  bool mtOk = mikrotikConnectionSuccess && apiClient.connected();
  // The admin UI polls this every few seconds, so build the payload in one bounded
  // buffer instead of chaining a String per field (13 temporaries per poll).
  char data[224];
  snprintf(data, sizeof(data), "%lu|%d|%d|%d|%d|%d|%s|%s|%s|%s|%u|%u|%u",
           upTime, lifeTimeCoinCount, coinCountVal, customerCount,
           hasInternetConnection ? 1 : 0, mtOk ? 1 : 0,
           currentMacAddress.c_str(), currentIpAddress.c_str(), HARDWARE_TYPE.c_str(),
           CURRENT_VERSION, ESP.getFreeHeap(), ESP.getHeapFragmentation(), ESP.getMaxFreeBlockSize());
  server.send(200, "text/plain", data);
}

void testInsertCoin() {
  if (!isAuthorized()) { handleNotAuthorize(); return; }
  int c = server.arg("coin").toInt();
  if (c < 0) c = 0;
  if (coinSlotActive) { coin += c; coinsChange = 1; }
  server.send(200, "text/plain", "ok");
}

void handleAdminGetAutoRestart() {
  if (!isAuthorized()) { handleNotAuthorize(); return; }
  server.send(200, "text/plain", String(AUTO_RESTART_MINUTES));
}

void handleAdminSaveAutoRestart() {
  if (!isAuthorized()) { handleNotAuthorize(); return; }
  int mins = clampInt(server.arg("minutes").toInt(), 0, 10080);
  if (!handleFileWrite("/admin/config/autorestart.data", String(mins))) {
    server.send(500, "text/plain", "write failed");
    return;
  }
  AUTO_RESTART_MINUTES = mins;
  scheduleAutoRestart();
  server.send(200, "text/plain", "ok");
}

void handleAdminPanelPing() {
  if (!isAuthorized()) { handleNotAuthorize(); return; }
  lastAdminPanelSeenMs = millis();
  server.send(200, "text/plain", "ok");
}


void handleAdminSetOtaKey() {
  if (!isAuthorized()) { handleNotAuthorize(); return; }
  String keyHex = server.arg("key");
  keyHex.trim();
  uint8_t key[32];
  if (!hexToBytes32(keyHex, key)) {
    server.send(400, "text/plain", "Invalid OTA key: expected 64 hex characters (32 bytes)");
    return;
  }
  File f = SPIFFS.open("/admin/config/ota.key", "w");
  if (!f) { server.send(500, "text/plain", "Failed to write OTA key"); return; }
  f.print(keyHex);
  f.close();
  server.send(200, "text/plain", "ok");
}


bool hasUploadError = false;
String otaUploadErrorMsg = "";
bool otaSigRequired = false;
bool otaSigVerified = false;
br_hmac_context otaHmacCtx;
bool otaHmacActive = false;
uint8_t otaExpectedHmac[32];

int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

bool hexToBytes32(const String &hex, uint8_t out[32]) {
  if ((int)hex.length() != 64) return false;
  for (int i = 0; i < 32; i++) {
    int hi = hexNibble(hex.charAt(i * 2));
    int lo = hexNibble(hex.charAt(i * 2 + 1));
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

bool constantTimeEq32(const uint8_t a[32], const uint8_t b[32]) {
  uint8_t diff = 0;
  for (int i = 0; i < 32; i++) diff |= (uint8_t)(a[i] ^ b[i]);
  return diff == 0;
}

bool loadOtaKeyBytes(uint8_t keyOut[32]) {
  File f = SPIFFS.open("/admin/config/ota.key", "r");
  if (!f) return false;
  String hex = f.readString();
  f.close();
  hex.trim();
  return hexToBytes32(hex, keyOut);
}

bool initOtaSignatureVerification() {
  otaSigRequired = false;
  otaSigVerified = false;
  otaHmacActive = false;
  uint8_t key[32];
  if (!loadOtaKeyBytes(key)) {
    return true; // no key configured: handled by policy in caller
  }
  String sigHex = server.arg("otaSig");
  sigHex.trim();
  if (!hexToBytes32(sigHex, otaExpectedHmac)) {
    otaUploadErrorMsg = "OTA signature required (otaSig)";
    return false;
  }
  br_hmac_key_context kc;
  br_hmac_key_init(&kc, &br_sha256_vtable, key, sizeof(key));
  br_hmac_init(&otaHmacCtx, &kc, 0);
  otaHmacActive = true;
  otaSigRequired = true;
  return true;
}

void handleFileUploadRequest() {
  if (Update.hasError() || hasUploadError) {
    String msg = otaUploadErrorMsg.length() ? otaUploadErrorMsg : String("Upload failed");
    server.send(400, "text/plain", msg);
  } else {
    server.client().setNoDelay(true);
    server.send_P(200, PSTR("text/html"), "Upload done");
    delay(100);
    server.client().stop();
    ESP.restart();
  }
}

void handleFileUploadStream() {
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    if (!isAuthorized()) { handleNotAuthorize(); hasUploadError = true; return; }
    hasUploadError = false;
    otaUploadErrorMsg = "";
    otaSigRequired = false;
    otaSigVerified = false;
    otaHmacActive = false;

    uint8_t probeKey[32];
    bool hasKey = loadOtaKeyBytes(probeKey);
    if (!hasKey) {
      hasUploadError = true;
      otaUploadErrorMsg = "OTA signing key not configured (/admin/config/ota.key). Set an OTA key before uploading.";
      return;
    }
    if (!initOtaSignatureVerification()) {
      hasUploadError = true;
      if (!otaUploadErrorMsg.length()) otaUploadErrorMsg = "Invalid OTA signature";
      return;
    }

    if (upload.name == "filesystem") {
      backupSystemConfig();
      close_all_fs();
      size_t fsSize = ((size_t)&_FS_end - (size_t)&_FS_start);
      if (!Update.begin(fsSize, U_FS)) { hasUploadError = true; otaUploadErrorMsg = "Update begin failed (filesystem)"; }
    } else {
      uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
      if (!Update.begin(maxSketchSpace, U_FLASH)) { hasUploadError = true; otaUploadErrorMsg = "Update begin failed (sketch)"; }
    }
  } else if (upload.status == UPLOAD_FILE_WRITE && !hasUploadError) {
    if (otaHmacActive) {
      br_hmac_update(&otaHmacCtx, upload.buf, upload.currentSize);
    }
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      hasUploadError = true;
      otaUploadErrorMsg = "Update write failed";
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!hasUploadError) {
      if (otaHmacActive) {
        uint8_t got[32];
        br_hmac_out(&otaHmacCtx, got);
        otaHmacActive = false;
        if (!constantTimeEq32(got, otaExpectedHmac)) {
          hasUploadError = true;
          otaUploadErrorMsg = "OTA signature verification failed";
          Update.end();
          return;
        }
        otaSigVerified = true;
      }
      if (!Update.end(true)) { hasUploadError = true; otaUploadErrorMsg = "Update finalize failed"; }
    } else {
      Update.end();
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.end();
    hasUploadError = true;
    if (!otaUploadErrorMsg.length()) otaUploadErrorMsg = "Upload aborted";
  }
  delay(0);
}

