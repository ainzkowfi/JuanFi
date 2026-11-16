#pragma once

// ---------- RouterOS API client (minimal, blocking) ----------

struct ApiResult {
  bool ok;
  bool fatal;
  bool gotDone;
  int records;
  String trapMessage;
  String ret;              // !done =ret
  String reId;             // first !re =.id
  String reName;
  String reLimitUptime;
  String reLimitBytesTotal;
  String reUptime;
  String reBytesIn;
  String reBytesOut;
  String reComment;
  String reProfile;
};

ApiResult apiLast;

void apiResetResult() {
  apiLast.ok = false;
  apiLast.fatal = false;
  apiLast.gotDone = false;
  apiLast.records = 0;
  apiLast.trapMessage = "";
  apiLast.ret = "";
  apiLast.reId = "";
  apiLast.reName = "";
  apiLast.reLimitUptime = "";
  apiLast.reLimitBytesTotal = "";
  apiLast.reUptime = "";
  apiLast.reBytesIn = "";
  apiLast.reBytesOut = "";
  apiLast.reComment = "";
  apiLast.reProfile = "";
}

bool parseU64(const String &in, unsigned long long &out) {
  out = 0;
  bool any = false;
  for (size_t i = 0; i < in.length(); i++) {
    char c = in.charAt(i);
    if (c < '0' || c > '9') continue;
    out = out * 10ULL + (unsigned long long)(c - '0');
    any = true;
  }
  return any;
}

String u64ToString(unsigned long long v) {
  if (v == 0) return String("0");
  char buf[24];
  int i = 23;
  buf[i] = '\0';
  while (v > 0 && i > 0) {
    buf[--i] = (char)('0' + (int)(v % 10ULL));
    v /= 10ULL;
  }
  return String(&buf[i]);
}

bool apiWriteLen(uint32_t len) {
  uint8_t b[5];
  int n = 0;
  if (len < 0x80) {
    b[n++] = (uint8_t)len;
  } else if (len < 0x4000) {
    len |= 0x8000;
    b[n++] = (uint8_t)(len >> 8);
    b[n++] = (uint8_t)(len & 0xFF);
  } else if (len < 0x200000) {
    len |= 0xC00000;
    b[n++] = (uint8_t)(len >> 16);
    b[n++] = (uint8_t)((len >> 8) & 0xFF);
    b[n++] = (uint8_t)(len & 0xFF);
  } else if (len < 0x10000000) {
    len |= 0xE0000000;
    b[n++] = (uint8_t)(len >> 24);
    b[n++] = (uint8_t)((len >> 16) & 0xFF);
    b[n++] = (uint8_t)((len >> 8) & 0xFF);
    b[n++] = (uint8_t)(len & 0xFF);
  } else {
    b[n++] = 0xF0;
    b[n++] = (uint8_t)(len >> 24);
    b[n++] = (uint8_t)((len >> 16) & 0xFF);
    b[n++] = (uint8_t)((len >> 8) & 0xFF);
    b[n++] = (uint8_t)(len & 0xFF);
  }
  return apiClient.write(b, n) == (size_t)n;
}

bool apiWriteWord(const char *w) {
  uint32_t len = strlen(w);
  if (!apiWriteLen(len)) return false;
  if (len == 0) return true;
  return apiClient.write((const uint8_t *)w, len) == (size_t)len;
}

bool apiWriteWord(const String &w) {
  uint32_t len = w.length();
  if (!apiWriteLen(len)) return false;
  if (len == 0) return true;
  return apiClient.write((const uint8_t *)w.c_str(), len) == (size_t)len;
}

bool apiReadExact(uint8_t *buf, size_t len, unsigned long timeoutMs) {
  size_t got = 0;
  unsigned long start = millis();
  while (got < len) {
    if ((millis() - start) > timeoutMs) return false;
    int b = apiClient.read();
    if (b < 0) {
      if (!apiClient.connected() && !apiClient.available()) return false;
      delay(1);
      yield();
      continue;
    }
    buf[got++] = (uint8_t)b;
  }
  return true;
}

bool apiReadLen(uint32_t &len, unsigned long timeoutMs) {
  uint8_t c;
  if (!apiReadExact(&c, 1, timeoutMs)) return false;
  if ((c & 0x80) == 0x00) {
    len = c;
  } else if ((c & 0xC0) == 0x80) {
    uint8_t b1;
    if (!apiReadExact(&b1, 1, timeoutMs)) return false;
    len = ((uint32_t)(c & 0x3F) << 8) | b1;
  } else if ((c & 0xE0) == 0xC0) {
    uint8_t b[2];
    if (!apiReadExact(b, 2, timeoutMs)) return false;
    len = ((uint32_t)(c & 0x1F) << 16) | ((uint32_t)b[0] << 8) | b[1];
  } else if ((c & 0xF0) == 0xE0) {
    uint8_t b[3];
    if (!apiReadExact(b, 3, timeoutMs)) return false;
    len = ((uint32_t)(c & 0x0F) << 24) | ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2];
  } else if (c == 0xF0) {
    uint8_t b[4];
    if (!apiReadExact(b, 4, timeoutMs)) return false;
    len = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
  } else {
    return false; // reserved control byte
  }
  return true;
}

// Read one reply word straight into a caller-supplied buffer. No heap traffic per word:
// the API reply loop runs on every transaction, so allocating a String per word (and
// more per attribute split) is what fragments the ESP8266 heap over a long uptime.
bool apiReadWordTo(char *buf, size_t bufSize, unsigned long timeoutMs, uint32_t maxLen = 4096) {
  if (bufSize == 0) return false;
  buf[0] = '\0';
  uint32_t len = 0;
  if (!apiReadLen(len, timeoutMs)) return false;
  if (len == 0) return true;
  if (len > maxLen || len >= (uint32_t)bufSize) {
    // Drain the oversized word to keep the stream aligned, but fail closed: returning
    // an empty word would be misread as an end-of-sentence marker and desync parsing.
    uint8_t tmp[64];
    uint32_t remaining = len;
    while (remaining > 0) {
      uint32_t chunk = remaining > sizeof(tmp) ? sizeof(tmp) : remaining;
      if (!apiReadExact(tmp, chunk, timeoutMs)) return false;
      remaining -= chunk;
    }
    return false;
  }
  if (!apiReadExact((uint8_t *)buf, len, timeoutMs)) return false;
  buf[len] = '\0';
  return true;
}

bool apiEndSentence() { return apiWriteLen(0); }

enum class ApiReplyType { NONE, RE, DONE, TRAP, FATAL, EMPTY };

// Send a sentence (command + attribute words + end-of-sentence) and read replies
// until !done / !trap / !fatal or timeout. Captures the first !re record fields,
// the !done =ret= value and the !trap message into apiLast.
bool apiTransact(const String &cmd, const String attrs[], int attrCount, unsigned long timeoutMs = 7000) {
  apiResetResult();
  if (!apiClient.connected()) { apiLast.fatal = true; return false; }

  if (!apiWriteWord(cmd)) { apiLast.fatal = true; apiClient.stop(); return false; }
  for (int i = 0; i < attrCount; i++) {
    if (!apiWriteWord(attrs[i])) { apiLast.fatal = true; apiClient.stop(); return false; }
  }
  if (!apiEndSentence()) { apiLast.fatal = true; apiClient.stop(); return false; }

  ApiReplyType current = ApiReplyType::NONE;
  bool finished = false;
  unsigned long deadline = millis() + timeoutMs;

  while (!finished) {
    if ((long)(millis() - deadline) >= 0) { apiLast.fatal = true; apiClient.stop(); return false; }

    char word[192];
    unsigned long remain = deadline - millis();
    if (remain < 1) remain = 1;
    if (!apiReadWordTo(word, sizeof(word), remain)) { apiLast.fatal = true; apiClient.stop(); return false; }

    if (word[0] == '\0') {
      // End of a reply sentence. RouterOS sends !done after !trap for the same
      // command, so a trap must keep reading or the stale !done desyncs the next call.
      if (current == ApiReplyType::DONE) { apiLast.gotDone = true; finished = true; }
      else if (current == ApiReplyType::TRAP) { /* consume trailing !done */ }
      else if (current == ApiReplyType::FATAL) { finished = true; }
      current = ApiReplyType::NONE;
      continue;
    }

    if (!strcmp(word, "!re")) { current = ApiReplyType::RE; apiLast.records++; }
    else if (!strcmp(word, "!done")) { current = ApiReplyType::DONE; }
    else if (!strcmp(word, "!trap")) { current = ApiReplyType::TRAP; }
    else if (!strcmp(word, "!fatal")) { current = ApiReplyType::FATAL; apiLast.fatal = true; }
    else if (!strcmp(word, "!empty")) { current = ApiReplyType::EMPTY; }
    else if (word[0] == '=') {
      // Split "=key=value" in place instead of two substrings per attribute.
      char *key = word + 1;
      char *val = (char *)"";
      char *eq = strchr(key, '=');
      if (eq) { *eq = '\0'; val = eq + 1; }
      if (current == ApiReplyType::TRAP || current == ApiReplyType::FATAL) {
        if (!strcmp(key, "message") && apiLast.trapMessage.length() == 0) apiLast.trapMessage = val;
      } else if (current == ApiReplyType::DONE) {
        if (!strcmp(key, "ret")) apiLast.ret = val;
      } else if (current == ApiReplyType::RE && apiLast.records == 1) {
        if (!strcmp(key, ".id") && apiLast.reId.length() == 0) apiLast.reId = val;
        else if (!strcmp(key, "name") && apiLast.reName.length() == 0) apiLast.reName = val;
        else if (!strcmp(key, "limit-uptime") && apiLast.reLimitUptime.length() == 0) apiLast.reLimitUptime = val;
        else if (!strcmp(key, "limit-bytes-total") && apiLast.reLimitBytesTotal.length() == 0) apiLast.reLimitBytesTotal = val;
        else if (!strcmp(key, "uptime") && apiLast.reUptime.length() == 0) apiLast.reUptime = val;
        else if (!strcmp(key, "bytes-in") && apiLast.reBytesIn.length() == 0) apiLast.reBytesIn = val;
        else if (!strcmp(key, "bytes-out") && apiLast.reBytesOut.length() == 0) apiLast.reBytesOut = val;
        else if (!strcmp(key, "comment") && apiLast.reComment.length() == 0) apiLast.reComment = val;
        else if (!strcmp(key, "profile") && apiLast.reProfile.length() == 0) apiLast.reProfile = val;
      }
    }
  }

  apiLast.ok = !apiLast.fatal && apiLast.gotDone && apiLast.trapMessage.length() == 0;
  if (apiLast.fatal) apiClient.stop();
  return apiLast.ok;
}

String hexToBinary(const String &hex) {
  int outLen = (int)hex.length() / 2;
  if (outLen <= 0) return String();
  char stackBuf[64];
  char *buf = stackBuf;
  bool heapBuf = false;
  if (outLen + 1 > (int)sizeof(stackBuf)) {
    buf = (char *)malloc(outLen + 1);
    if (!buf) return String();
    heapBuf = true;
  }
  int n = 0;
  for (int i = 0; i + 1 < (int)hex.length(); i += 2) {
    char pair[3] = { hex[i], hex[i + 1], 0 };
    char *endp = nullptr;
    long v = strtol(pair, &endp, 16);
    if (endp == pair) break;
    buf[n++] = (char)(v & 0xFF);
  }
  buf[n] = '\0';
  String out;
  out.reserve(n);
  out.concat(buf, (unsigned int)n);
  if (heapBuf) free(buf);
  return out;
}

bool mikrotikLegacyLogin(const String &challengeHex) {
  String challenge = hexToBinary(challengeHex);
  if (challenge.length() == 0) return false;

  MD5Builder md5;
  md5.begin();
  uint8_t zero = 0;
  md5.add(&zero, 1);
  md5.add((const uint8_t *)pwd.c_str(), pwd.length());
  md5.add((const uint8_t *)challenge.c_str(), challenge.length());
  md5.calculate();

  String attrs[2];
  attrs[0] = "=name=" + user;
  attrs[1] = "=response=00" + md5.toString();
  return apiTransact("/login", attrs, 2, 5000);
}

bool mikrotikApiLogin() {
  // 1) RouterOS 6.43+ plain-text login.
  String attrs[2];
  attrs[0] = "=name=" + user;
  attrs[1] = "=password=" + pwd;
  bool ok = apiTransact("/login", attrs, 2, 5000);
  if (ok && apiLast.ret.length() == 0) return true;
  if (apiLast.fatal) return false;

  // 2) Legacy challenge-response (pre-6.43): bare /login answers with =ret=<challenge>.
  String challenge = apiLast.ret;
  if (challenge.length() == 0) {
    if (!apiTransact("/login", nullptr, 0, 5000)) return false;
    challenge = apiLast.ret;
  }
  if (challenge.length() > 0) return mikrotikLegacyLogin(challenge);
  return apiLast.ok;
}

bool loginMirotik() {
  if (WiFi.status() != WL_CONNECTED) {
    mikrotikConnectionSuccess = false;
    return false;
  }
  if (apiClient.connected()) {
    apiClient.stop();
    delay(50);
  }
  if (!apiClient.connect(mikrotikRouterIp, MIKROTIK_API_PORT)) {
    Serial.println("MikroTik API connect failed");
    mikrotikConnectionSuccess = false;
    return false;
  }
  bool ok = mikrotikApiLogin();
  mikrotikConnectionSuccess = ok;
  if (!ok) {
    Serial.println("MikroTik API login failed");
    apiClient.stop();
  } else {
    Serial.println("MikroTik API login ok");
  }
  return ok;
}

bool ensureMikrotikConnection(bool force = false) {
  if (!networkConnected || WiFi.status() != WL_CONNECTED) {
    mikrotikConnectionSuccess = false;
    return false;
  }
  if (mikrotikConnectionSuccess && apiClient.connected()) {
    return true;
  }
  unsigned long now = millis();
  if (!force && (now - lastMikrotikReconnectAttempt) < MIKROTIK_RECONNECT_INTERVAL) {
    mikrotikConnectionSuccess = apiClient.connected() && mikrotikConnectionSuccess;
    return mikrotikConnectionSuccess;
  }
  lastMikrotikReconnectAttempt = now;
  return loginMirotik();
}

// ---------- Hotspot summary (cached, low-stress) ----------
struct HotspotSummaryCache {
  unsigned long ms = 0;
  bool ok = false;
  int activeCount = 0;
  int totalCount = 0;
  char payload[224] = { 0 }; // "active|total|ip,ttl;ip,ttl" max 5
} hotspotSummaryCache;
const unsigned long HOTSPOT_SUMMARY_TTL_MS = 30000;

bool fetchHotspotActiveList(int &activeCount, char *listOut, size_t listOutSize, int maxItems) {
  activeCount = 0;
  if (listOut && listOutSize > 0) listOut[0] = '\0';
  apiResetResult();
  if (!apiClient.connected()) { apiLast.fatal = true; return false; }

  if (!apiWriteWord("/ip/hotspot/active/print")) { apiLast.fatal = true; apiClient.stop(); return false; }
  if (!apiWriteWord("=.proplist=address,session-time-left")) { apiLast.fatal = true; apiClient.stop(); return false; }
  if (!apiEndSentence()) { apiLast.fatal = true; apiClient.stop(); return false; }

  ApiReplyType current = ApiReplyType::NONE;
  bool finished = false;
  unsigned long deadline = millis() + 7000;
  char curAddr[24];
  char curTtl[24];
  curAddr[0] = '\0';
  curTtl[0] = '\0';
  size_t listLen = 0;

  while (!finished) {
    if ((long)(millis() - deadline) >= 0) { apiLast.fatal = true; apiClient.stop(); return false; }

    char word[192];
    unsigned long remain = deadline - millis();
    if (remain < 1) remain = 1;
    if (!apiReadWordTo(word, sizeof(word), remain)) { apiLast.fatal = true; apiClient.stop(); return false; }

    if (word[0] == '\0') {
      if (current == ApiReplyType::RE) {
        apiLast.records++;
        if (apiLast.records <= maxItems && listOut && listLen + 1 < listOutSize) {
          if (listLen > 0) listOut[listLen++] = ';';
          int n = snprintf(listOut + listLen, listOutSize - listLen, "%s,%s",
                           curAddr[0] ? curAddr : "-", curTtl[0] ? curTtl : "-");
          if (n > 0) listLen += (size_t)n;
          if (listLen >= listOutSize) listLen = listOutSize - 1;
          listOut[listLen] = '\0';
        }
        curAddr[0] = '\0';
        curTtl[0] = '\0';
      } else if (current == ApiReplyType::DONE) {
        apiLast.gotDone = true; finished = true;
      } else if (current == ApiReplyType::TRAP) {
        /* consume trailing !done */
      } else if (current == ApiReplyType::FATAL) {
        finished = true;
      }
      current = ApiReplyType::NONE;
      continue;
    }

    if (!strcmp(word, "!re")) { current = ApiReplyType::RE; curAddr[0] = '\0'; curTtl[0] = '\0'; }
    else if (!strcmp(word, "!done")) { current = ApiReplyType::DONE; }
    else if (!strcmp(word, "!trap")) { current = ApiReplyType::TRAP; }
    else if (!strcmp(word, "!fatal")) { current = ApiReplyType::FATAL; apiLast.fatal = true; }
    else if (!strcmp(word, "!empty")) { current = ApiReplyType::EMPTY; }
    else if (word[0] == '=') {
      char *key = word + 1;
      char *val = (char *)"";
      char *eq = strchr(key, '=');
      if (eq) { *eq = '\0'; val = eq + 1; }
      if (current == ApiReplyType::TRAP || current == ApiReplyType::FATAL) {
        if (!strcmp(key, "message") && apiLast.trapMessage.length() == 0) apiLast.trapMessage = val;
      } else if (current == ApiReplyType::RE) {
        if (!strcmp(key, "address")) snprintf(curAddr, sizeof(curAddr), "%s", val);
        else if (!strcmp(key, "session-time-left")) snprintf(curTtl, sizeof(curTtl), "%s", val);
      }
    }
  }

  activeCount = apiLast.records;
  apiLast.ok = !apiLast.fatal && apiLast.gotDone && apiLast.trapMessage.length() == 0;
  if (apiLast.fatal) apiClient.stop();
  return apiLast.ok;
}

bool fetchHotspotTotalUsers(int &totalCount) {
  totalCount = 0;
  String attrs[1];
  attrs[0] = "=count-only=";
  if (!apiTransact("/ip/hotspot/user/print", attrs, 1, 7000)) return false;
  totalCount = apiLast.ret.toInt();
  if (totalCount < 0) totalCount = 0;
  return true;
}

void handleAdminHotspotSummary() {
  if (!isAuthorized()) { handleNotAuthorize(); return; }
  unsigned long now = millis();
  if (hotspotSummaryCache.ms != 0 && (now - hotspotSummaryCache.ms) < HOTSPOT_SUMMARY_TTL_MS) {
    server.send(200, "text/plain", hotspotSummaryCache.payload);
    return;
  }

  char resp[224];
  snprintf(resp, sizeof(resp), "0|0|");
  if (ensureMikrotikConnection()) {
    int activeCount = 0;
    int totalCount = 0;
    char list[192];
    list[0] = '\0';
    bool aok = fetchHotspotActiveList(activeCount, list, sizeof(list), 5);
    bool tok = fetchHotspotTotalUsers(totalCount);
    if (aok || tok) {
      if (!aok) activeCount = hotspotSummaryCache.ok ? hotspotSummaryCache.activeCount : 0;
      if (!tok) totalCount = hotspotSummaryCache.ok ? hotspotSummaryCache.totalCount : 0;
      const char *listPart = list;
      if (!aok) {
        // Reuse the cached list segment (after the second '|') rather than a substring.
        const char *cached = hotspotSummaryCache.payload;
        const char *first = strchr(cached, '|');
        const char *second = first ? strchr(first + 1, '|') : nullptr;
        listPart = second ? second + 1 : "";
      }
      snprintf(resp, sizeof(resp), "%d|%d|%s", activeCount, totalCount, listPart);
      hotspotSummaryCache.ms = millis();
      hotspotSummaryCache.ok = true;
      hotspotSummaryCache.activeCount = activeCount;
      hotspotSummaryCache.totalCount = totalCount;
      snprintf(hotspotSummaryCache.payload, sizeof(hotspotSummaryCache.payload), "%s", resp);
    } else if (hotspotSummaryCache.ok) {
      snprintf(resp, sizeof(resp), "%s", hotspotSummaryCache.payload); // serve stale on failure
    }
  } else if (hotspotSummaryCache.ok) {
    snprintf(resp, sizeof(resp), "%s", hotspotSummaryCache.payload);
  }

  server.send(200, "text/plain", resp);
}

// Parse RouterOS time strings to seconds: "1w2d3h4m5s", "03:04:05", "1d03:04:05", "0s".
// Works on a trimmed copy in a fixed buffer: this runs on every voucher extend/convert,
// and the old String-based version allocated several temporaries per call.
long parseRosTimeToSeconds(const String &in) {
  char s[40];
  const char *src = in.c_str();
  size_t start = 0;
  size_t end = in.length();
  while (start < end && (src[start] == ' ' || src[start] == '\t' || src[start] == '\r' || src[start] == '\n')) start++;
  while (end > start && (src[end - 1] == ' ' || src[end - 1] == '\t' || src[end - 1] == '\r' || src[end - 1] == '\n')) end--;
  size_t n = end - start;
  if (n >= sizeof(s)) n = sizeof(s) - 1;
  if (n > 0) memcpy(s, src + start, n);
  s[n] = '\0';
  if (n == 0) return 0;

  if (strchr(s, ':') != nullptr) {
    long total = 0;
    char *dayPos = strchr(s, 'd');
    char *rest = s;
    if (dayPos && dayPos > s) {
      bool digits = true;
      for (char *p = s; p < dayPos; p++) if (*p < '0' || *p > '9') { digits = false; break; }
      if (digits) {
        *dayPos = '\0';
        total += atol(s) * 86400L;
        rest = dayPos + 1;
      }
    }
    const char *parts[3];
    int pc = 0;
    char *cursor = rest;
    for (char *p = rest; *p && pc < 3; p++) {
      if (*p == ':') { *p = '\0'; parts[pc++] = cursor; cursor = p + 1; }
    }
    if (pc < 3) parts[pc++] = cursor;
    if (pc == 3) total += (long)atol(parts[0]) * 3600L + (long)atol(parts[1]) * 60L + atol(parts[2]);
    else if (pc == 2) total += (long)atol(parts[0]) * 60L + atol(parts[1]);
    else if (pc == 1) total += atol(parts[0]);
    return total < 0 ? 0 : total;
  }

  long total = 0;
  long val = 0;
  bool haveDigits = false;
  bool sawUnit = false;
  for (const char *p = s; *p; p++) {
    char c = *p;
    if (c >= '0' && c <= '9') {
      val = val * 10 + (c - '0');
      haveDigits = true;
      continue;
    }
    if (c == 'w') total += val * 604800L;
    else if (c == 'd') total += val * 86400L;
    else if (c == 'h') total += val * 3600L;
    else if (c == 'm') total += val * 60L;
    else if (c == 's') total += val;
    val = 0;
    haveDigits = false;
    sawUnit = true;
  }
  if (haveDigits && !sawUnit) total += val;
  return total < 0 ? 0 : total;
}

String formatRosTimeSeconds(long seconds) {
  if (seconds < 0) seconds = 0;
  // RouterOS accepts compact "1w2d3h4m5s" time values.
  long w = seconds / 604800L; seconds %= 604800L;
  long d = seconds / 86400L; seconds %= 86400L;
  long h = seconds / 3600L; seconds %= 3600L;
  long m = seconds / 60L; long sec = seconds % 60L;
  char buf[32];
  size_t p = 0;
  if (w > 0) p += (size_t)snprintf(buf + p, sizeof(buf) - p, "%ldw", w);
  if (d > 0 && p < sizeof(buf)) p += (size_t)snprintf(buf + p, sizeof(buf) - p, "%ldd", d);
  if (h > 0 && p < sizeof(buf)) p += (size_t)snprintf(buf + p, sizeof(buf) - p, "%ldh", h);
  if (m > 0 && p < sizeof(buf)) p += (size_t)snprintf(buf + p, sizeof(buf) - p, "%ldm", m);
  if ((sec > 0 || p == 0) && p < sizeof(buf)) p += (size_t)snprintf(buf + p, sizeof(buf) - p, "%lds", sec);
  if (p >= sizeof(buf)) p = sizeof(buf) - 1;
  buf[p] = '\0';
  return String(buf);
}

// ---------- Admin: hotspot active sessions & user management (RAM-only, no ESP writes) ----------

static void apiJsonEscapeAppend(String &out, const char *in) {
  if (!in) return;
  for (const char *p = in; *p; ++p) {
    unsigned char c = (unsigned char)*p;
    if (c == '\\' || c == '"') { out += '\\'; out += (char)c; }
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else if (c < 0x20) out += ' ';
    else out += (char)c;
  }
}

static void apiSendJsonErr(int code, const char *msg) {
  String out = "{\"ok\":false,\"error\":\"";
  apiJsonEscapeAppend(out, msg ? msg : "error");
  out += "\"}";
  server.send(code, "application/json", out);
}

void handleAdminHotspotActiveList() {
  if (!isAuthorized()) { handleNotAuthorize(); return; }
  if (!ensureMikrotikConnection()) { apiSendJsonErr(503, "MikroTik not connected"); return; }
  apiResetResult();
  if (!apiClient.connected()) { apiSendJsonErr(503, "MikroTik not connected"); return; }
  if (!apiWriteWord("/ip/hotspot/active/print")) { apiLast.fatal = true; apiClient.stop(); apiSendJsonErr(500, "API write failed"); return; }
  if (!apiWriteWord("=.proplist=.id,user,address,mac-address,uptime,session-time-left")) { apiLast.fatal = true; apiClient.stop(); apiSendJsonErr(500, "API write failed"); return; }
  if (!apiEndSentence()) { apiLast.fatal = true; apiClient.stop(); apiSendJsonErr(500, "API write failed"); return; }

  String out;
  out.reserve(1024);
  out = "{\"ok\":true,\"items\":[";
  ApiReplyType current = ApiReplyType::NONE;
  bool finished = false;
  unsigned long deadline = millis() + 8000;
  char curId[24] = {0}, curUser[40] = {0}, curAddr[24] = {0}, curMac[24] = {0}, curUp[24] = {0}, curLeft[24] = {0};
  int emitted = 0;
  const int MAX_ITEMS = 20;

  while (!finished) {
    if ((long)(millis() - deadline) >= 0) { apiLast.fatal = true; apiClient.stop(); apiSendJsonErr(504, "MikroTik timeout"); return; }
    char word[192];
    unsigned long remain = deadline - millis(); if (remain < 1) remain = 1;
    if (!apiReadWordTo(word, sizeof(word), remain)) { apiLast.fatal = true; apiClient.stop(); apiSendJsonErr(500, "API read failed"); return; }
    if (word[0] == '\0') {
      if (current == ApiReplyType::RE) {
        if (emitted < MAX_ITEMS) {
          if (emitted > 0) out += ',';
          out += "{\"id\":\""; apiJsonEscapeAppend(out, curId);
          out += "\",\"user\":\""; apiJsonEscapeAppend(out, curUser);
          out += "\",\"address\":\""; apiJsonEscapeAppend(out, curAddr);
          out += "\",\"mac\":\""; apiJsonEscapeAppend(out, curMac);
          out += "\",\"uptime\":\""; apiJsonEscapeAppend(out, curUp);
          out += "\",\"timeLeft\":\""; apiJsonEscapeAppend(out, curLeft);
          out += "\"}";
          emitted++;
        }
        curId[0]=curUser[0]=curAddr[0]=curMac[0]=curUp[0]=curLeft[0]='\0';
      } else if (current == ApiReplyType::DONE) { apiLast.gotDone = true; finished = true; }
      else if (current == ApiReplyType::TRAP) { /* trailing !done */ }
      else if (current == ApiReplyType::FATAL) { finished = true; }
      current = ApiReplyType::NONE;
      continue;
    }
    if (!strcmp(word, "!re")) { current = ApiReplyType::RE; curId[0]=curUser[0]=curAddr[0]=curMac[0]=curUp[0]=curLeft[0]='\0'; }
    else if (!strcmp(word, "!done")) { current = ApiReplyType::DONE; }
    else if (!strcmp(word, "!trap")) { current = ApiReplyType::TRAP; }
    else if (!strcmp(word, "!fatal")) { current = ApiReplyType::FATAL; apiLast.fatal = true; }
    else if (!strcmp(word, "!empty")) { current = ApiReplyType::EMPTY; }
    else if (word[0] == '=') {
      char *key = word + 1; char *val = (char*)""; char *eq = strchr(key, '=');
      if (eq) { *eq = '\0'; val = eq + 1; }
      if (current == ApiReplyType::TRAP || current == ApiReplyType::FATAL) {
        if (!strcmp(key, "message") && apiLast.trapMessage.length() == 0) apiLast.trapMessage = val;
      } else if (current == ApiReplyType::RE) {
        if (!strcmp(key, ".id")) snprintf(curId, sizeof(curId), "%s", val);
        else if (!strcmp(key, "user")) snprintf(curUser, sizeof(curUser), "%s", val);
        else if (!strcmp(key, "address")) snprintf(curAddr, sizeof(curAddr), "%s", val);
        else if (!strcmp(key, "mac-address")) snprintf(curMac, sizeof(curMac), "%s", val);
        else if (!strcmp(key, "uptime")) snprintf(curUp, sizeof(curUp), "%s", val);
        else if (!strcmp(key, "session-time-left")) snprintf(curLeft, sizeof(curLeft), "%s", val);
      }
    }
  }
  if (apiLast.fatal) { apiClient.stop(); apiSendJsonErr(500, apiLast.trapMessage.length() ? apiLast.trapMessage.c_str() : "API error"); return; }
  if (apiLast.trapMessage.length()) { apiSendJsonErr(500, apiLast.trapMessage.c_str()); return; }
  out += "],\"count\":";
  out += String(emitted);
  out += "}";
  server.send(200, "application/json", out);
}

void handleAdminHotspotUsersList() {
  if (!isAuthorized()) { handleNotAuthorize(); return; }
  String q = server.hasArg("q") ? server.arg("q") : "";
  q.trim();
  if (!ensureMikrotikConnection()) { apiSendJsonErr(503, "MikroTik not connected"); return; }
  apiResetResult();
  if (!apiClient.connected()) { apiSendJsonErr(503, "MikroTik not connected"); return; }
  // fetch limited users; optional exact name query supported by MikroTik
  String cmdAttrs[3];
  int an = 0;
  if (!apiWriteWord("/ip/hotspot/user/print")) { apiLast.fatal = true; apiClient.stop(); apiSendJsonErr(500, "API write failed"); return; }
  if (!apiWriteWord("=.proplist=.id,name,profile,limit-uptime,uptime,disabled,comment")) { apiLast.fatal = true; apiClient.stop(); apiSendJsonErr(500, "API write failed"); return; }
  if (q.length() > 0) {
    String safe = sanitizeCommandToken(q, 32, "");
    if (safe.length() > 0) {
      if (!apiWriteWord("?name=" + safe)) { apiLast.fatal = true; apiClient.stop(); apiSendJsonErr(500, "API write failed"); return; }
    }
  }
  if (!apiEndSentence()) { apiLast.fatal = true; apiClient.stop(); apiSendJsonErr(500, "API write failed"); return; }

  String out;
  out.reserve(1400);
  out = "{\"ok\":true,\"items\":[";
  ApiReplyType current = ApiReplyType::NONE;
  bool finished = false;
  unsigned long deadline = millis() + 9000;
  char curId[24]={0}, curName[40]={0}, curProf[24]={0}, curLim[24]={0}, curUp[24]={0}, curDis[8]={0}, curCom[48]={0};
  int emitted = 0;
  const int MAX_ITEMS = 20;

  while (!finished) {
    if ((long)(millis() - deadline) >= 0) { apiLast.fatal = true; apiClient.stop(); apiSendJsonErr(504, "MikroTik timeout"); return; }
    char word[192];
    unsigned long remain = deadline - millis(); if (remain < 1) remain = 1;
    if (!apiReadWordTo(word, sizeof(word), remain)) { apiLast.fatal = true; apiClient.stop(); apiSendJsonErr(500, "API read failed"); return; }
    if (word[0] == '\0') {
      if (current == ApiReplyType::RE) {
        if (emitted < MAX_ITEMS) {
          if (emitted > 0) out += ',';
          out += "{\"id\":\""; apiJsonEscapeAppend(out, curId);
          out += "\",\"name\":\""; apiJsonEscapeAppend(out, curName);
          out += "\",\"profile\":\""; apiJsonEscapeAppend(out, curProf);
          out += "\",\"limitUptime\":\""; apiJsonEscapeAppend(out, curLim);
          out += "\",\"uptime\":\""; apiJsonEscapeAppend(out, curUp);
          out += "\",\"disabled\":\""; apiJsonEscapeAppend(out, curDis);
          out += "\",\"comment\":\""; apiJsonEscapeAppend(out, curCom);
          out += "\"}";
          emitted++;
        }
        curId[0]=curName[0]=curProf[0]=curLim[0]=curUp[0]=curDis[0]=curCom[0]='\0';
      } else if (current == ApiReplyType::DONE) { apiLast.gotDone = true; finished = true; }
      else if (current == ApiReplyType::TRAP) { }
      else if (current == ApiReplyType::FATAL) { finished = true; }
      current = ApiReplyType::NONE;
      continue;
    }
    if (!strcmp(word, "!re")) { current = ApiReplyType::RE; curId[0]=curName[0]=curProf[0]=curLim[0]=curUp[0]=curDis[0]=curCom[0]='\0'; }
    else if (!strcmp(word, "!done")) { current = ApiReplyType::DONE; }
    else if (!strcmp(word, "!trap")) { current = ApiReplyType::TRAP; }
    else if (!strcmp(word, "!fatal")) { current = ApiReplyType::FATAL; apiLast.fatal = true; }
    else if (!strcmp(word, "!empty")) { current = ApiReplyType::EMPTY; }
    else if (word[0] == '=') {
      char *key = word + 1; char *val = (char*)""; char *eq = strchr(key, '=');
      if (eq) { *eq = '\0'; val = eq + 1; }
      if (current == ApiReplyType::TRAP || current == ApiReplyType::FATAL) {
        if (!strcmp(key, "message") && apiLast.trapMessage.length() == 0) apiLast.trapMessage = val;
      } else if (current == ApiReplyType::RE) {
        if (!strcmp(key, ".id")) snprintf(curId, sizeof(curId), "%s", val);
        else if (!strcmp(key, "name")) snprintf(curName, sizeof(curName), "%s", val);
        else if (!strcmp(key, "profile")) snprintf(curProf, sizeof(curProf), "%s", val);
        else if (!strcmp(key, "limit-uptime")) snprintf(curLim, sizeof(curLim), "%s", val);
        else if (!strcmp(key, "uptime")) snprintf(curUp, sizeof(curUp), "%s", val);
        else if (!strcmp(key, "disabled")) snprintf(curDis, sizeof(curDis), "%s", val);
        else if (!strcmp(key, "comment")) snprintf(curCom, sizeof(curCom), "%s", val);
      }
    }
  }
  if (apiLast.fatal) { apiClient.stop(); apiSendJsonErr(500, apiLast.trapMessage.length() ? apiLast.trapMessage.c_str() : "API error"); return; }
  if (apiLast.trapMessage.length()) { apiSendJsonErr(500, apiLast.trapMessage.c_str()); return; }
  out += "],\"count\":";
  out += String(emitted);
  out += "}";
  server.send(200, "application/json", out);
}

void handleAdminHotspotUserAction() {
  if (!isAuthorized()) { handleNotAuthorize(); return; }
  if (server.method() != HTTP_POST) { apiSendJsonErr(405, "POST required"); return; }
  String action = server.hasArg("action") ? server.arg("action") : "";
  String id = server.hasArg("id") ? server.arg("id") : "";
  String name = server.hasArg("name") ? server.arg("name") : "";
  action.trim(); id.trim(); name.trim();
  if (!ensureMikrotikConnection()) { apiSendJsonErr(503, "MikroTik not connected"); return; }

  bool ok = false;
  String err = "Invalid action";
  if (action == "disconnect") {
    // remove active session by .id
    String sid = sanitizeRouterText(id, 24);
    if (sid.length() == 0) { apiSendJsonErr(400, "Missing session id"); return; }
    String attrs[1]; attrs[0] = "=.id=" + sid;
    ok = apiTransact("/ip/hotspot/active/remove", attrs, 1, 7000);
    err = apiLast.trapMessage.length() ? apiLast.trapMessage : String("Disconnect failed");
  } else if (action == "addTime") {
    String sname = sanitizeCommandToken(name, 32, "");
    int addMinutes = server.hasArg("minutes") ? server.arg("minutes").toInt() : 0;
    if (sname.length() == 0) { apiSendJsonErr(400, "Missing user name"); return; }
    if (addMinutes <= 0) { apiSendJsonErr(400, "Minutes must be greater than 0"); return; }
    // read current user limits
    String q[2]; q[0] = "?name=" + sname; q[1] = "=.proplist=.id,limit-uptime";
    if (!apiTransact("/ip/hotspot/user/print", q, 2, 7000) || apiLast.records == 0) {
      apiSendJsonErr(404, apiLast.trapMessage.length() ? apiLast.trapMessage.c_str() : "User not found");
      return;
    }
    String uid = apiLast.reId.length() > 0 ? apiLast.reId : sname;
    long curUptime = parseRosTimeToSeconds(apiLast.reLimitUptime);
    long newUptime = curUptime + (long)addMinutes * 60L;
    if (newUptime < 0) newUptime = 0;
    String sattrs[2]; sattrs[0] = "=.id=" + uid; sattrs[1] = "=limit-uptime=" + formatRosTimeSeconds(newUptime);
    ok = apiTransact("/ip/hotspot/user/set", sattrs, 2, 7000);
    err = apiLast.trapMessage.length() ? apiLast.trapMessage : String("Failed to add time");
    if (ok) {
      // Kick active session(s) so updated limit-uptime applies on re-login/cookie re-auth.
      for (int i = 0; i < 5; i++) {
        String aq[2]; aq[0] = "?user=" + sname; aq[1] = "=.proplist=.id";
        if (!apiTransact("/ip/hotspot/active/print", aq, 2, 5000) || apiLast.records == 0 || apiLast.reId.length() == 0) break;
        String aid[1]; aid[0] = "=.id=" + apiLast.reId;
        apiTransact("/ip/hotspot/active/remove", aid, 1, 5000);
      }
    }
  } else if (action == "remove" || action == "disable" || action == "enable") {
    String sname = sanitizeCommandToken(name, 32, "");
    if (sname.length() == 0) { apiSendJsonErr(400, "Missing user name"); return; }
    // resolve user .id by name
    String q[2]; q[0] = "?name=" + sname; q[1] = "=.proplist=.id";
    if (!apiTransact("/ip/hotspot/user/print", q, 2, 7000) || apiLast.records == 0 || apiLast.reId.length() == 0) {
      apiSendJsonErr(404, apiLast.trapMessage.length() ? apiLast.trapMessage.c_str() : "User not found");
      return;
    }
    String uid = apiLast.reId;
    if (action == "remove") {
      String attrs[1]; attrs[0] = "=.id=" + uid;
      ok = apiTransact("/ip/hotspot/user/remove", attrs, 1, 7000);
      err = apiLast.trapMessage.length() ? apiLast.trapMessage : String("Remove failed");
    } else {
      String attrs[2]; attrs[0] = "=.id=" + uid; attrs[1] = (action == "disable") ? "=disabled=yes" : "=disabled=no";
      ok = apiTransact("/ip/hotspot/user/set", attrs, 2, 7000);
      err = apiLast.trapMessage.length() ? apiLast.trapMessage : String("Update failed");
    }
  } else {
    apiSendJsonErr(400, "Unknown action");
    return;
  }

  if (!ok) { apiSendJsonErr(500, err.c_str()); return; }
  server.send(200, "application/json", "{\"ok\":true}");
}


bool registerNewVoucher(const String &voucher, bool *existedBefore = nullptr) {
  if (existedBefore) *existedBefore = false;
  String v = sanitizeCommandToken(voucher, 24, "");
  if (v.length() == 0) return false;
  String prof = sanitizeCommandToken(VOUCHER_PROFILE, 24, "");

  String a[5];
  int n = 0;
  a[n++] = "=name=" + v;
  a[n++] = "=limit-uptime=0s";
  a[n++] = "=comment=0";
  if (VOUCHER_LOGIN_OPTION == 1) a[n++] = "=password=" + v;
  if (prof.length() > 0 && prof != "default") a[n++] = "=profile=" + prof;

  if (apiTransact("/ip/hotspot/user/add", a, n, 7000)) return true;

  // Idempotent create: an already existing voucher is not an error here.
  String msg = apiLast.trapMessage;
  msg.toLowerCase();
  if (msg.indexOf("already") >= 0 || msg.indexOf("exist") >= 0) {
    if (existedBefore) *existedBefore = true;
    return true;
  }
  Serial.print("registerNewVoucher trap: ");
  Serial.println(apiLast.trapMessage);
  return false;
}

bool addTimeToVoucher(const String &voucher, int secondsToAdd) {
  String v = sanitizeCommandToken(voucher, 24, "");
  if (v.length() == 0) return false;
  String safeVendor = sanitizeRouterText(vendorName, 24);
  int mins = secondsToAdd / 60;
  if (mins < 0) mins = 0;

  // 1) read the current user limits
  String q[2];
  q[0] = "?name=" + v;
  q[1] = "=.proplist=.id,limit-uptime,limit-bytes-total";
  if (!apiTransact("/ip/hotspot/user/print", q, 2, 7000)) {
    Serial.print("print voucher failed: ");
    Serial.println(apiLast.trapMessage);
    return false;
  }
  if (apiLast.records == 0) {
    Serial.println("voucher not found for extend");
    return false;
  }

  String target = (apiLast.reId.length() > 0) ? apiLast.reId : v;
  long curUptime = parseRosTimeToSeconds(apiLast.reLimitUptime);
  long newUptime = curUptime + (long)mins * 60L;
  if (newUptime < 0) newUptime = 0;

  unsigned long long curBytes = 0;
  parseU64(apiLast.reLimitBytesTotal, curBytes);
  unsigned long long newBytes = curBytes + (unsigned long long)currentDataLimit * 1048576ULL;

  String comment = String(currentValidity) + "m," + String(totalCoin) + (isNewVoucher ? ",0," : ",1,") + safeVendor;
  String rateProf = sanitizeCommandToken(currentRateProfile, 24, "");

  // 2) write back the computed limits
  String sattrs[6];
  int sn = 0;
  sattrs[sn++] = "=numbers=" + target;
  sattrs[sn++] = "=limit-uptime=" + formatRosTimeSeconds(newUptime);
  sattrs[sn++] = "=comment=" + comment;
  if (rateProf.length() > 0) sattrs[sn++] = "=profile=" + rateProf;
  if (currentDataLimit > 0) sattrs[sn++] = "=limit-bytes-total=" + u64ToString(newBytes);

  if (!apiTransact("/ip/hotspot/user/set", sattrs, sn, 7000)) {
    Serial.print("set voucher failed: ");
    Serial.println(apiLast.trapMessage);
    return false;
  }
  return true;
}

