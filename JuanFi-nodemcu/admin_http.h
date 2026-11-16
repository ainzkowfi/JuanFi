#pragma once

extern bool pendingRestartAfterSave;

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
  int start = 0;
  while (start <= (int)cookie.length()) {
    int sep = cookie.indexOf(';', start);
    if (sep < 0) sep = cookie.length();
    String part = cookie.substring(start, sep);
    part.trim();
    if (part == needle) return true;
    start = sep + 1;
  }
  return false;
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
void handleUserGetRates() {
  setupCORSPolicy();
  if (!handleFileRead("/admin/config/rates.data")) {
    server.send(200, "text/plain", "P1#1#12#0##|P5#5#90#0##|P10#10#210#0##|P20#20#480#0##");
  }
}
void handleAdminGetRates() {
  if (!isAuthorized()) { handleNotAuthorize(); return; }
  if (!handleFileRead("/admin/config/rates.data")) {
    server.send(200, "text/plain", "P1#1#12#0##|P5#5#90#0##|P10#10#210#0##|P20#20#480#0##");
  }
}

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
  const String &data = server.arg("data");
  if (data.length() > 4096) { server.send(413, "text/plain", "too large"); return; }
  if (!handleFileWrite("/admin/config/system.data", data)) { server.send(500, "text/plain", "write failed"); return; }
  if (systemIsIdleForRestart()) {
    server.send(200, "text/plain", "ok");
    delay(1000);
    ESP.restart();
  } else {
    pendingRestartAfterSave = true;
    server.send(200, "text/plain", "ok-restart-pending");
  }
}

void handleAdminGetSystemConfig() {
  if (!isAuthorized()) { handleNotAuthorize(); return; }
  if (!handleFileRead("/admin/config/system.data")) {
    server.send(404, "text/plain", "system.data not found");
  }
}

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

void handleAdminGetVoucherCodeLength() {
  if (!isAuthorized()) { handleNotAuthorize(); return; }
  server.send(200, "text/plain", String(VOUCHER_CODE_LENGTH));
}

void handleAdminSaveVoucherCodeLength() {
  if (!isAuthorized()) { handleNotAuthorize(); return; }
  int len = clampInt(server.arg("length").toInt(), 1, 10);
  if (len <= 0) len = 5;
  if (!handleFileWrite("/admin/config/vouchercodelen.data", String(len))) {
    server.send(500, "text/plain", "write failed");
    return;
  }
  VOUCHER_CODE_LENGTH = len;
  server.send(200, "text/plain", "ok");
}

void handleAdminPanelPing() {
  if (!isAuthorized()) { handleNotAuthorize(); return; }
  lastAdminPanelSeenMs = millis();
  server.send(200, "text/plain", "ok");
}


bool hasUploadError = false;

void handleFileUploadRequest() {
  if (Update.hasError() || hasUploadError) {
    server.send(200, "text/html", "Upload failed");
  } else {
    server.client().setNoDelay(true);
    if (systemIsIdleForRestart()) {
      server.send_P(200, PSTR("text/html"), "Upload done");
      delay(100);
      server.client().stop();
      ESP.restart();
    } else {
      pendingRestartAfterSave = true;
      server.send_P(200, PSTR("text/html"), "Upload done. Restart pending (device busy); it will restart when idle.");
      delay(100);
      server.client().stop();
    }
  }
}

void handleFileUploadStream() {
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    if (!isAuthorized()) { handleNotAuthorize(); hasUploadError = true; return; }
    hasUploadError = false;
    if (upload.name == "filesystem") {
      backupSystemConfig();
      close_all_fs();
      size_t fsSize = ((size_t)&_FS_end - (size_t)&_FS_start);
      if (!Update.begin(fsSize, U_FS)) hasUploadError = true;
    } else {
      uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
      if (!Update.begin(maxSketchSpace, U_FLASH)) hasUploadError = true;
    }
  } else if (upload.status == UPLOAD_FILE_WRITE && !hasUploadError) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      hasUploadError = true;
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!hasUploadError) {
      if (!Update.end(true)) hasUploadError = true;
    } else {
      Update.end();
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.end();
    hasUploadError = true;
  }
  delay(0);
}

