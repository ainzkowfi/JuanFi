#pragma once

int calculateAddTime() {
  int totalTime = 0;
  currentValidity = 0;
  currentDataLimit = 0;
  int remainingCoin = totalCoin;
  int highestPrice = 0;
  int guard = 0;
  while (remainingCoin > 0 && guard++ < 500) {
    int candidatePrice = 0;
    int candidateIndex = -1;
    for (int i = 0; i < ratesCount; i++) {
      if (rates[i].price > 0 && rates[i].price <= remainingCoin) {
        if (candidatePrice < rates[i].price) {
          candidatePrice = rates[i].price;
          candidateIndex = i;
        }
      }
    }
    if (candidateIndex != -1) {
      if ((!isNewVoucher) && VOUCHER_VALIDITY_OPTION == 1) {
        currentValidity += rates[candidateIndex].minutes;
      } else {
        currentValidity += rates[candidateIndex].validity;
      }
      if (highestPrice < rates[candidateIndex].price) {
        highestPrice = rates[candidateIndex].price;
        if (rates[candidateIndex].profileName.length() > 0) {
          currentRateProfile = sanitizeCommandToken(rates[candidateIndex].profileName, 24, "");
        }
      }
      currentDataLimit += rates[candidateIndex].dataLimit;
      totalTime += rates[candidateIndex].minutes;
      remainingCoin -= rates[candidateIndex].price;
    } else {
      break;
    }
  }
  if (totalTime < 0) totalTime = 0;
  return totalTime * 60;
}

void updateStatistic() {
  eeWriteIntNoCommit(LIFETIME_COIN_COUNT_ADDRESS, eeGetInt(LIFETIME_COIN_COUNT_ADDRESS) + totalCoin);
  eeWriteIntNoCommit(COIN_COUNT_ADDRESS, eeGetInt(COIN_COUNT_ADDRESS) + totalCoin);
  eeWriteIntNoCommit(CUSTOMER_COUNT_ADDRESS, eeGetInt(CUSTOMER_COUNT_ADDRESS) + 1);
  EEPROM.commit();
}

void handleHealth() { setupCORSPolicy(); server.send(200, "text/plain", "ok"); }

bool checkIfSystemIsAvailable() {
  if (!ensureMikrotikConnection()) {
    digitalWrite(SYSTEM_READY_LED, evaluateTriggerOutput(TURN_OFF));
    const char *keys[] = {"status", "errorCode"};
    const char *values[] = {"false", "coin.slot.notavailable"};
    setupCORSPolicy();
    server.send(200, "application/json", toJson(keys, values, 2));
    return false;
  }
  digitalWrite(SYSTEM_READY_LED, evaluateTriggerOutput(TURN_ON));
  return true;
}

bool hasInternetConnect() {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  WiFiClient wc;
  http.setTimeout(5000);
  if (!http.begin(wc, INTERNET_CHECK_URL)) return false;
  http.addHeader("User-Agent", "curl/7.55.1");
  int httpCode = http.GET();
  bool ok = (httpCode > 0 && httpCode < 500);
  http.end();
  return ok;
}

String generateVoucher() {
  const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  const int alphaLen = (int)(sizeof(alphabet) - 1);
  char buf[32];
  size_t p = 0;
  const char *prefix = VOUCHER_PREFIX.c_str();
  int codeLen = VOUCHER_CODE_LENGTH;
  if (codeLen < 1) codeLen = 5;
  if (codeLen > 10) codeLen = 10;
  size_t reserve = (size_t)codeLen + 1;
  if (reserve > sizeof(buf)) reserve = sizeof(buf);
  while (*prefix && p < sizeof(buf) - reserve) buf[p++] = *prefix++;
  for (int i = 0; i < codeLen && p < sizeof(buf) - 1; i++) {
    uint32_t r = ESP.random();
    buf[p++] = alphabet[r % alphaLen];
  }
  buf[p] = '\0';
  return String(buf);
}

bool isValidVoucherCode(const String &v) {
  return isSafeToken(v) && v.length() >= 3 && v.length() <= 24;
}

bool validateVoucher(const String &voucher) {
  if (!isValidVoucherCode(voucher) || voucher != currentActiveVoucher) {
    const char *keys[] = {"status", "errorCode"};
    const char *values[] = {"false", "coinslot.busy"};
    setupCORSPolicy();
    server.send(200, "application/json", toJson(keys, values, 2));
    return false;
  }
  return true;
}

void resetGlobalVariables() {
  currentActiveVoucher = "";
  timeToAdd = 0;
  totalCoin = 0;
  currentDataLimit = 0;
  currentRateProfile = "";
  isNewVoucher = false;
  pulsesThisSession = 0;
  droppedPulsesThisSession = 0;
}

void handleCancelTopUp() {
  if (!checkIfSystemIsAvailable()) return;
  String voucher = server.arg("voucher");
  if (!validateVoucher(voucher)) return;
  targetMilis = millis();
  const char *keys[] = {"status"};
  const char *values[] = {"true"};
  setupCORSPolicy();
  server.send(200, "application/json", toJson(keys, values, 1));
}

void checkCoin() {
  if (!checkIfSystemIsAvailable()) return;
  String voucher = server.arg("voucher");
  if (!validateVoucher(voucher)) return;

  if (coinExpired) {
    const char *keys[] = {"status", "errorCode"};
    const char *values[] = {"false", "coins.wait.expired"};
    setupCORSPolicy();
    server.send(200, "application/json", toJson(keys, values, 2));
    return;
  }

  if (!acceptCoin) {
    if (coinSlotArming) {
      int droppedSnapshot = droppedPulsesThisSession;
      const char *keys[] = {"status", "errorCode", "remainTime", "timeAdded", "totalCoin", "waitTime", "validity", "data", "droppedPulses"};
      long armRemain = (long)(coinSlotArmAtMs - millis());
      if (armRemain < 0) armRemain = 0;
      long remain = armRemain + (long)MAX_WAIT_COIN_SEC;
      char remainTimeStr[20]; itoa(remain, remainTimeStr, 10);
      char timeToAddStr[16]; itoa(timeToAdd, timeToAddStr, 10);
      char totalCoinStr[16]; itoa(totalCoin, totalCoinStr, 10);
      char waitTimeStr[16]; itoa(MAX_WAIT_COIN_SEC, waitTimeStr, 10);
      char validityStr[16]; itoa(currentValidity, validityStr, 10);
      char currentDataLimitStr[16]; itoa(currentDataLimit, currentDataLimitStr, 10);
      char droppedStr[16]; itoa(droppedSnapshot, droppedStr, 10);
      const char *values[] = {"false", "coin.not.inserted", remainTimeStr, timeToAddStr, totalCoinStr, waitTimeStr, validityStr, currentDataLimitStr, droppedStr};
      setupCORSPolicy();
      server.send(200, "application/json", toJson(keys, values, 9));
    } else {
      int consumedCoin = processCoin;
      processCoin = 0;
      totalCoin += consumedCoin;
      timeToAdd = calculateAddTime();
      int droppedSnapshot = droppedPulsesThisSession;
      const char *keys[] = {"status", "newCoin", "timeAdded", "totalCoin", "validity", "data", "droppedPulses"};
      char coinStr[16]; itoa(consumedCoin, coinStr, 10);
      char timeToAddStr[16]; itoa(timeToAdd, timeToAddStr, 10);
      char totalCoinStr[16]; itoa(totalCoin, totalCoinStr, 10);
      char validityStr[16]; itoa(currentValidity, validityStr, 10);
      char currentDataLimitStr[16]; itoa(currentDataLimit, currentDataLimitStr, 10);
      char droppedStr[16]; itoa(droppedSnapshot, droppedStr, 10);
      const char *values[] = {"true", coinStr, timeToAddStr, totalCoinStr, validityStr, currentDataLimitStr, droppedStr};
      activateCoinSlot();
      setupCORSPolicy();
      server.send(200, "application/json", toJson(keys, values, 7));
    }
  } else {
    int droppedSnapshot = droppedPulsesThisSession;
    const char *keys[] = {"status", "errorCode", "remainTime", "timeAdded", "totalCoin", "waitTime", "validity", "data", "droppedPulses"};
    char remainTimeStr[20]; long remain = (long)(targetMilis - millis()); itoa(remain, remainTimeStr, 10);
    char timeToAddStr[16]; itoa(timeToAdd, timeToAddStr, 10);
    char totalCoinStr[16]; itoa(totalCoin, totalCoinStr, 10);
    char waitTimeStr[16]; itoa(MAX_WAIT_COIN_SEC, waitTimeStr, 10);
    char validityStr[16]; itoa(currentValidity, validityStr, 10);
    char currentDataLimitStr[16]; itoa(currentDataLimit, currentDataLimitStr, 10);
    char droppedStr[16]; itoa(droppedSnapshot, droppedStr, 10);
    const char *values[] = {"false", "coin.not.inserted", remainTimeStr, timeToAddStr, totalCoinStr, waitTimeStr, validityStr, currentDataLimitStr, droppedStr};
    setupCORSPolicy();
    server.send(200, "application/json", toJson(keys, values, 9));
  }
}

bool removeVoucherUser(const String &target) {
  String a[1];
  a[0] = "=numbers=" + target;
  if (apiTransact("/ip/hotspot/user/remove", a, 1, 7000)) return true;
  Serial.print("remove voucher failed: ");
  Serial.println(apiLast.trapMessage);
  return false;
}

void useVoucher() {
  if (!checkIfSystemIsAvailable()) return;
  String voucher = server.arg("voucher");
  if (!validateVoucher(voucher)) return;
  if (timeToAdd > 0 && !ensureMikrotikConnection(true)) {
    const char *keys[] = {"status", "errorCode"};
    const char *values[] = {"false", "coin.slot.notavailable"};
    setupCORSPolicy();
    server.send(200, "application/json", toJson(keys, values, 2));
    return;
  }
  disableCoinSlot();
  if (timeToAdd > 0) {
    bool existedBefore = false;
    bool regOk = registerNewVoucher(currentActiveVoucher, &existedBefore);
    bool extOk = regOk && addTimeToVoucher(currentActiveVoucher, timeToAdd);
    if (extOk) {
      updateStatistic();
    } else {
      // Roll back a newly-created voucher so coins are not lost to a dead code.
      if (regOk && !existedBefore) {
        String rollbackTarget = sanitizeCommandToken(currentActiveVoucher, 24, "");
        if (rollbackTarget.length() > 0) {
          removeVoucherUser(rollbackTarget);
        }
      }
      // Do not commit stats/state on failure; keep voucher context out but report error
      const char *keys[] = {"status", "errorCode"};
      const char *values[] = {"false", "coin.slot.notavailable"};
      setupCORSPolicy();
      server.send(200, "application/json", toJson(keys, values, 2));
      resetGlobalVariables();
      acceptCoin = false;
      mikrotikConnectionSuccess = apiClient.connected();
      return;
    }
  }
  const char *keys[] = {"status", "totalCoin", "timeAdded", "validity"};
  char totalCoinStr[16]; itoa(totalCoin, totalCoinStr, 10);
  char timeToAddStr[16]; itoa(timeToAdd, timeToAddStr, 10);
  char validityStr[16]; itoa(currentValidity, validityStr, 10);
  const char *values[] = {"true", totalCoinStr, timeToAddStr, validityStr};
  resetGlobalVariables();
  setupCORSPolicy();
  acceptCoin = false;
  server.send(200, "application/json", toJson(keys, values, 4));
}

int findConvertAttemptSlot(uint32_t ip) {
  unsigned long now = millis();
  int freeIdx = -1;
  int oldestIdx = 0;
  for (int i = 0; i < CONVERT_ATTEMPT_SLOTS; i++) {
    ConvertAttemptSlot &s = convertAttemptSlots[i];
    if (s.ip == ip) return i;
    bool expired = (s.ip != 0) && s.lockoutUntilMs == 0 && s.lastSeenMs != 0 && (unsigned long)(now - s.lastSeenMs) > CONVERT_ATTEMPT_WINDOW_MS;
    if (expired) { s.ip = 0; s.fails = 0; s.lockoutUntilMs = 0; s.lastSeenMs = 0; }
    if (s.ip == 0 && freeIdx < 0) freeIdx = i;
    if (s.lastSeenMs <= convertAttemptSlots[oldestIdx].lastSeenMs) oldestIdx = i;
  }
  return (freeIdx >= 0) ? freeIdx : oldestIdx;
}

bool isConvertVoucherRateLimited() {
  uint32_t ip = (uint32_t)server.client().remoteIP();
  if (ip == 0) return false;
  unsigned long now = millis();
  int idx = findConvertAttemptSlot(ip);
  ConvertAttemptSlot &s = convertAttemptSlots[idx];
  if (s.ip != ip) return false;
  if (s.lockoutUntilMs != 0 && (long)(s.lockoutUntilMs - now) > 0) return true;
  if (s.lockoutUntilMs != 0 && (long)(s.lockoutUntilMs - now) <= 0) {
    s.lockoutUntilMs = 0;
    s.fails = 0;
  }
  if (s.lastSeenMs != 0 && (unsigned long)(now - s.lastSeenMs) > CONVERT_ATTEMPT_WINDOW_MS) {
    s.fails = 0;
  }
  return false;
}

void recordConvertVoucherFailure() {
  uint32_t ip = (uint32_t)server.client().remoteIP();
  if (ip == 0) return;
  unsigned long now = millis();
  int idx = findConvertAttemptSlot(ip);
  ConvertAttemptSlot &s = convertAttemptSlots[idx];
  if (s.ip != ip) { s.ip = ip; s.fails = 0; s.lockoutUntilMs = 0; s.lastSeenMs = 0; }
  if (s.lockoutUntilMs != 0 && (long)(s.lockoutUntilMs - now) <= 0) {
    s.lockoutUntilMs = 0;
    s.fails = 0;
  }
  if (s.lastSeenMs != 0 && (unsigned long)(now - s.lastSeenMs) > CONVERT_ATTEMPT_WINDOW_MS) {
    s.fails = 0;
  }
  s.lastSeenMs = now;
  if (s.fails < 255) s.fails++;
  if (s.fails >= CONVERT_MAX_FAILS) {
    s.fails = 0;
    s.lockoutUntilMs = now + CONVERT_LOCKOUT_MS;
  }
}

void clearConvertVoucherFailures() {
  uint32_t ip = (uint32_t)server.client().remoteIP();
  if (ip == 0) return;
  int idx = findConvertAttemptSlot(ip);
  ConvertAttemptSlot &s = convertAttemptSlots[idx];
  if (s.ip == ip) { s.fails = 0; s.lockoutUntilMs = 0; s.lastSeenMs = millis(); }
}

void replyConvertRateLimited() {
  const char *keys[] = {"status", "errorCode"};
  const char *values[] = {"false", "convertVoucher.rateLimited"};
  setupCORSPolicy();
  server.send(429, "application/json", toJson(keys, values, 2));
}

void replyConvertRefused(bool countFailure = true) {
  if (countFailure) recordConvertVoucherFailure();
  const char *keys[] = {"status", "errorCode"};
  const char *values[] = {"false", "convertVoucher.refused"};
  setupCORSPolicy();
  server.send(200, "application/json", toJson(keys, values, 2));
}

// Move the unused time (and unused data) of a second voucher onto the customer's
// active voucher, then delete the merged code so its name returns to the pool the
// generator draws from. Without this, spent codes stay behind as dead hotspot users
// and the finite code space fills up with codes nobody can use.
void convertVoucher() {
  setupCORSPolicy();
  if (isConvertVoucherRateLimited()) { replyConvertRateLimited(); return; }
  if (!checkIfSystemIsAvailable()) return;

  String active = sanitizeCommandToken(server.arg("voucher"), 24, "");
  String source = sanitizeCommandToken(server.arg("convertVoucher"), 24, "");
  if (!isValidVoucherCode(active) || !isValidVoucherCode(source) || active == source) {
    replyConvertRefused();
    return;
  }

  // 1) what is left on the code being handed in?
  String q[2];
  q[0] = "?name=" + source;
  q[1] = "=.proplist=.id,uptime,limit-uptime,limit-bytes-total,bytes-in,bytes-out";
  if (!apiTransact("/ip/hotspot/user/print", q, 2, 7000) || apiLast.records == 0) {
    replyConvertRefused();
    return;
  }
  String sourceTarget = apiLast.reId.length() > 0 ? apiLast.reId : source;
  long sourceLimit = parseRosTimeToSeconds(apiLast.reLimitUptime);
  long sourceUsed = parseRosTimeToSeconds(apiLast.reUptime);
  long remainingTime = sourceLimit - sourceUsed;
  // Unlimited profiles report no limit, and a spent code has nothing left to give.
  if (sourceLimit <= 0 || remainingTime <= 0) {
    replyConvertRefused();
    return;
  }

  unsigned long long sourceBytesLimit = 0, sourceBytesIn = 0, sourceBytesOut = 0, sourceBytesUsed = 0;
  parseU64(apiLast.reLimitBytesTotal, sourceBytesLimit);
  parseU64(apiLast.reBytesIn, sourceBytesIn);
  parseU64(apiLast.reBytesOut, sourceBytesOut);
  sourceBytesUsed = sourceBytesIn + sourceBytesOut;
  unsigned long long remainingBytes = (sourceBytesLimit > sourceBytesUsed) ? (sourceBytesLimit - sourceBytesUsed) : 0;

  // 2) what does the active voucher already hold?
  q[0] = "?name=" + active;
  q[1] = "=.proplist=.id,limit-uptime,limit-bytes-total";
  if (!apiTransact("/ip/hotspot/user/print", q, 2, 7000) || apiLast.records == 0) {
    replyConvertRefused();
    return;
  }
  String activeTarget = apiLast.reId.length() > 0 ? apiLast.reId : active;
  long activeLimit = parseRosTimeToSeconds(apiLast.reLimitUptime);
  unsigned long long activeBytes = 0;
  parseU64(apiLast.reLimitBytesTotal, activeBytes);
  // An unlimited session already has all the time it can use; adding a limit here
  // would cap the customer instead of helping them.
  if (activeLimit <= 0) {
    replyConvertRefused();
    return;
  }

  long newLimit = activeLimit + remainingTime;
  if (newLimit < 0) newLimit = activeLimit;

  // 3) grant first: the customer has already paid for both codes.
  String sattrs[4];
  int sn = 0;
  sattrs[sn++] = "=numbers=" + activeTarget;
  sattrs[sn++] = "=limit-uptime=" + formatRosTimeSeconds(newLimit);
  // Only merge data when the session is already data-limited; adding a cap to an
  // unlimited voucher would silently throttle it.
  if (remainingBytes > 0 && activeBytes > 0) sattrs[sn++] = "=limit-bytes-total=" + u64ToString(activeBytes + remainingBytes);
  if (!apiTransact("/ip/hotspot/user/set", sattrs, sn, 7000)) {
    replyConvertRefused();
    return;
  }

  // 4) best effort: free the merged code so it can be sold again.
  removeVoucherUser(sourceTarget);

  char timeAddedStr[16]; ltoa(remainingTime, timeAddedStr, 10);
  char totalTimeStr[16]; ltoa(newLimit, totalTimeStr, 10);
  const char *keys[] = {"status", "timeAdded", "totalTime"};
  const char *values[] = {"true", timeAddedStr, totalTimeStr};
  clearConvertVoucherFailures();
  setupCORSPolicy();
  server.send(200, "application/json", toJson(keys, values, 3));
}

void topUp() {
  if (!checkIfSystemIsAvailable()) return;

  String voucher = server.arg("voucher");
  if (voucher.length() > 0 && !isValidVoucherCode(voucher)) {
    const char *keys[] = {"status", "errorCode"};
    const char *values[] = {"false", "invalid.voucher"};
    setupCORSPolicy();
    server.send(200, "application/json", toJson(keys, values, 2));
    return;
  }

  if (currentActiveVoucher.length() > 0 && voucher.length() > 0 && voucher != currentActiveVoucher) {
    const char *keys[] = {"status", "errorCode"};
    const char *values[] = {"false", "coinslot.busy"};
    setupCORSPolicy();
    server.send(200, "application/json", toJson(keys, values, 2));
    return;
  }

  currentValidity = 0;
  if (voucher.length() == 0) {
    voucher = generateVoucher();
    isNewVoucher = true;
  } else if (currentActiveVoucher.length() == 0) {
    isNewVoucher = true;
  }

  int voucherLength = voucher.length() + 1;
  char voucherChar[voucherLength];
  voucher.toCharArray(voucherChar, voucherLength);
  const char *keys[] = {"status", "voucher"};
  const char *values[] = {"true", voucherChar};
  if (voucher != currentActiveVoucher) {
    resetGlobalVariables();
    activateCoinSlot();
    currentActiveVoucher = voucher;
  }
  setupCORSPolicy();
  server.send(200, "application/json", toJson(keys, values, 2));
}

void handleGenerateVouchers() {
  if (!isAuthorized()) { handleNotAuthorize(); return; }
  if (currentActiveVoucher.length() > 0 || coinSlotActive || coinSlotArming) {
    server.send(409, "text/plain", "Coin session in progress; try again after the customer finishes.");
    return;
  }
  if (!ensureMikrotikConnection(true)) { server.send(503, "text/plain", "MikroTik unavailable"); return; }
  int amount = clampInt(server.arg("amt").toInt(), 0, 100000);
  int qty = clampInt(server.arg("qty").toInt(), 1, 50);
  int addToSales = server.arg("sales").toInt() ? 1 : 0;
  String prefix = sanitizeCommandToken(server.arg("pfx"), 5, VOUCHER_PREFIX);
  char voucherGenerated[640];
  voucherGenerated[0] = '\0';
  size_t vgLen = 0;
  int successCount = 0;
  const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  const int alphaLen = (int)(sizeof(alphabet) - 1);
  int codeLen = VOUCHER_CODE_LENGTH;
  if (codeLen < 1) codeLen = 5;
  if (codeLen > 10) codeLen = 10;
  // Codes minted here are fresh sessions, never extensions of an existing one.
  isNewVoucher = true;
  totalCoin = amount;
  timeToAdd = calculateAddTime();
  for (int i = 0; i < qty; i++) {
    // Redraw on collision: registerNewVoucher() treats "already exists" as success,
    // so reusing a code would silently extend another customer's voucher.
    char voucherBuf[32];
    voucherBuf[0] = '\0';
    for (int attempt = 0; attempt < 5 && voucherBuf[0] == '\0'; attempt++) {
      char candidate[32];
      size_t p = 0;
      const char *pfx = prefix.c_str();
      size_t reserve = (size_t)codeLen + 1;
      if (reserve > sizeof(candidate)) reserve = sizeof(candidate);
      while (*pfx && p < sizeof(candidate) - reserve) candidate[p++] = *pfx++;
      for (int j = 0; j < codeLen && p < sizeof(candidate) - 1; j++) {
        candidate[p++] = alphabet[ESP.random() % alphaLen];
      }
      candidate[p] = '\0';
      bool existed = false;
      if (!registerNewVoucher(String(candidate), &existed)) break;  // real trap: stop retrying
      if (!existed) {
        strncpy(voucherBuf, candidate, sizeof(voucherBuf) - 1);
        voucherBuf[sizeof(voucherBuf) - 1] = '\0';
      }
    }
    if (voucherBuf[0] == '\0') continue;
    bool extOk = addTimeToVoucher(String(voucherBuf), timeToAdd);
    if (!extOk) continue;
    if (addToSales == 1) updateStatistic();
    size_t vlen = strlen(voucherBuf);
    if (successCount > 0 && vgLen + 1 < sizeof(voucherGenerated)) voucherGenerated[vgLen++] = '#';
    if (vgLen + vlen < sizeof(voucherGenerated)) {
      memcpy(voucherGenerated + vgLen, voucherBuf, vlen);
      vgLen += vlen;
      voucherGenerated[vgLen] = '\0';
    }
    successCount++;
  }
  String safeVendor = sanitizeRouterText(vendorName, 32);
  char returnData[760];
  snprintf(returnData, sizeof(returnData), "%s|%d|%d|%s", safeVendor.c_str(), amount, timeToAdd, voucherGenerated);
  if (successCount == 0) {
    server.send(503, "text/plain", "MikroTik operation failed");
  } else {
    server.send(200, "text/plain", returnData);
  }
  resetGlobalVariables();
}

