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

bool apiReadWord(String &out, unsigned long timeoutMs, uint32_t maxLen = 4096) {
  out = "";
  uint32_t len = 0;
  if (!apiReadLen(len, timeoutMs)) return false;
  if (len == 0) return true;
  if (len > maxLen) {
    // Drain oversized word to keep the stream aligned, but fail closed: returning an
    // empty word would be misread as an end-of-sentence marker and desync parsing.
    uint8_t tmp[64];
    uint32_t remaining = len;
    while (remaining > 0) {
      uint32_t chunk = remaining > sizeof(tmp) ? sizeof(tmp) : remaining;
      if (!apiReadExact(tmp, chunk, timeoutMs)) return false;
      remaining -= chunk;
    }
    return false;
  }
  char stackBuf[128];
  char *buf = stackBuf;
  bool heapBuf = false;
  if (len >= sizeof(stackBuf)) {
    buf = (char *)malloc(len + 1);
    if (!buf) {
      uint8_t tmp[64];
      uint32_t remaining = len;
      while (remaining > 0) {
        uint32_t chunk = remaining > sizeof(tmp) ? sizeof(tmp) : remaining;
        if (!apiReadExact(tmp, chunk, timeoutMs)) return false;
        remaining -= chunk;
      }
      return false;
    }
    heapBuf = true;
  }
  unsigned long start = millis();
  for (uint32_t i = 0; i < len; i++) {
    int b = -1;
    while (b < 0) {
      if ((millis() - start) > timeoutMs) { if (heapBuf) free(buf); return false; }
      b = apiClient.read();
      if (b < 0) {
        if (!apiClient.connected() && !apiClient.available()) { if (heapBuf) free(buf); return false; }
        delay(1);
        yield();
      }
    }
    buf[i] = (char)b;
  }
  buf[len] = '\0';
  out = String(buf);
  if (heapBuf) free(buf);
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

    String word;
    unsigned long remain = deadline - millis();
    if (remain < 1) remain = 1;
    if (!apiReadWord(word, remain)) { apiLast.fatal = true; apiClient.stop(); return false; }

    if (word.length() == 0) {
      // End of a reply sentence. RouterOS sends !done after !trap for the same
      // command, so a trap must keep reading or the stale !done desyncs the next call.
      if (current == ApiReplyType::DONE) { apiLast.gotDone = true; finished = true; }
      else if (current == ApiReplyType::TRAP) { /* consume trailing !done */ }
      else if (current == ApiReplyType::FATAL) { finished = true; }
      current = ApiReplyType::NONE;
      continue;
    }

    if (word == "!re") { current = ApiReplyType::RE; apiLast.records++; }
    else if (word == "!done") { current = ApiReplyType::DONE; }
    else if (word == "!trap") { current = ApiReplyType::TRAP; }
    else if (word == "!fatal") { current = ApiReplyType::FATAL; apiLast.fatal = true; }
    else if (word == "!empty") { current = ApiReplyType::EMPTY; }
    else if (word.charAt(0) == '=') {
      int eq = word.indexOf('=', 1);
      String key = (eq >= 0) ? word.substring(1, eq) : word.substring(1);
      String val = (eq >= 0) ? word.substring(eq + 1) : "";
      if (current == ApiReplyType::TRAP || current == ApiReplyType::FATAL) {
        if (key == "message" && apiLast.trapMessage.length() == 0) apiLast.trapMessage = val;
      } else if (current == ApiReplyType::DONE) {
        if (key == "ret") apiLast.ret = val;
      } else if (current == ApiReplyType::RE && apiLast.records == 1) {
        if (key == ".id" && apiLast.reId.length() == 0) apiLast.reId = val;
        else if (key == "name" && apiLast.reName.length() == 0) apiLast.reName = val;
        else if (key == "limit-uptime" && apiLast.reLimitUptime.length() == 0) apiLast.reLimitUptime = val;
        else if (key == "limit-bytes-total" && apiLast.reLimitBytesTotal.length() == 0) apiLast.reLimitBytesTotal = val;
        else if (key == "uptime" && apiLast.reUptime.length() == 0) apiLast.reUptime = val;
        else if (key == "bytes-in" && apiLast.reBytesIn.length() == 0) apiLast.reBytesIn = val;
        else if (key == "bytes-out" && apiLast.reBytesOut.length() == 0) apiLast.reBytesOut = val;
        else if (key == "comment" && apiLast.reComment.length() == 0) apiLast.reComment = val;
        else if (key == "profile" && apiLast.reProfile.length() == 0) apiLast.reProfile = val;
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
  for (int i = 0; i < n; i++) out += buf[i];
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
  String list; // "ip,ttl;ip,ttl" max 5
} hotspotSummaryCache;
const unsigned long HOTSPOT_SUMMARY_TTL_MS = 30000;

bool fetchHotspotActiveList(int &activeCount, String &listOut, int maxItems) {
  activeCount = 0;
  listOut = "";
  apiResetResult();
  if (!apiClient.connected()) { apiLast.fatal = true; return false; }

  if (!apiWriteWord("/ip/hotspot/active/print")) { apiLast.fatal = true; apiClient.stop(); return false; }
  if (!apiWriteWord("=.proplist=address,session-time-left")) { apiLast.fatal = true; apiClient.stop(); return false; }
  if (!apiEndSentence()) { apiLast.fatal = true; apiClient.stop(); return false; }

  ApiReplyType current = ApiReplyType::NONE;
  bool finished = false;
  unsigned long deadline = millis() + 7000;
  String curAddr = "";
  String curTtl = "";

  while (!finished) {
    if ((long)(millis() - deadline) >= 0) { apiLast.fatal = true; apiClient.stop(); return false; }

    String word;
    unsigned long remain = deadline - millis();
    if (remain < 1) remain = 1;
    if (!apiReadWord(word, remain)) { apiLast.fatal = true; apiClient.stop(); return false; }

    if (word.length() == 0) {
      if (current == ApiReplyType::RE) {
        apiLast.records++;
        if (apiLast.records <= maxItems) {
          if (curAddr.length() == 0) curAddr = "-";
          if (curTtl.length() == 0) curTtl = "-";
          if (listOut.length() > 0) listOut += ";";
          listOut += curAddr + "," + curTtl;
        }
        curAddr = "";
        curTtl = "";
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

    if (word == "!re") { current = ApiReplyType::RE; curAddr = ""; curTtl = ""; }
    else if (word == "!done") { current = ApiReplyType::DONE; }
    else if (word == "!trap") { current = ApiReplyType::TRAP; }
    else if (word == "!fatal") { current = ApiReplyType::FATAL; apiLast.fatal = true; }
    else if (word == "!empty") { current = ApiReplyType::EMPTY; }
    else if (word.charAt(0) == '=') {
      int eq = word.indexOf('=', 1);
      String key = (eq >= 0) ? word.substring(1, eq) : word.substring(1);
      String val = (eq >= 0) ? word.substring(eq + 1) : "";
      if (current == ApiReplyType::TRAP || current == ApiReplyType::FATAL) {
        if (key == "message" && apiLast.trapMessage.length() == 0) apiLast.trapMessage = val;
      } else if (current == ApiReplyType::RE) {
        if (key == "address") curAddr = val;
        else if (key == "session-time-left") curTtl = val;
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
    server.send(200, "text/plain", hotspotSummaryCache.list);
    return;
  }

  String resp = "0|0|";
  if (ensureMikrotikConnection()) {
    int activeCount = 0;
    int totalCount = 0;
    String list = "";
    bool aok = fetchHotspotActiveList(activeCount, list, 5);
    bool tok = fetchHotspotTotalUsers(totalCount);
    if (aok || tok) {
      if (!aok) activeCount = hotspotSummaryCache.ok ? hotspotSummaryCache.activeCount : 0;
      if (!tok) totalCount = hotspotSummaryCache.ok ? hotspotSummaryCache.totalCount : 0;
      resp = String(activeCount) + "|" + String(totalCount) + "|" + (aok ? list : hotspotSummaryCache.list.substring(hotspotSummaryCache.list.indexOf('|', hotspotSummaryCache.list.indexOf('|') + 1) + 1));
      hotspotSummaryCache.ms = millis();
      hotspotSummaryCache.ok = true;
      hotspotSummaryCache.activeCount = activeCount;
      hotspotSummaryCache.totalCount = totalCount;
      hotspotSummaryCache.list = resp;
    } else if (hotspotSummaryCache.ok) {
      resp = hotspotSummaryCache.list; // serve stale on failure
    }
  } else if (hotspotSummaryCache.ok) {
    resp = hotspotSummaryCache.list;
  }

  server.send(200, "text/plain", resp);
}

// Parse RouterOS time strings to seconds: "1w2d3h4m5s", "03:04:05", "1d03:04:05", "0s".
long parseRosTimeToSeconds(const String &in) {
  String s = in;
  s.trim();
  if (s.length() == 0) return 0;

  int colonCount = 0;
  for (size_t i = 0; i < s.length(); i++) if (s[i] == ':') colonCount++;

  if (colonCount >= 1) {
    long total = 0;
    String rest = s;
    int dIdx = s.indexOf('d');
    if (dIdx > 0) {
      bool digits = true;
      for (int i = 0; i < dIdx; i++) if (s[i] < '0' || s[i] > '9') { digits = false; break; }
      if (digits) {
        total += (long)s.substring(0, dIdx).toInt() * 86400L;
        rest = s.substring(dIdx + 1);
      }
    }
    String parts[3];
    int pc = 0;
    String cur = "";
    for (size_t i = 0; i < rest.length() && pc < 3; i++) {
      char c = rest[i];
      if (c == ':') { parts[pc++] = cur; cur = ""; }
      else cur += c;
    }
    if (pc < 3) parts[pc++] = cur;
    if (pc == 3) total += (long)parts[0].toInt() * 3600L + (long)parts[1].toInt() * 60L + parts[2].toInt();
    else if (pc == 2) total += (long)parts[0].toInt() * 60L + parts[1].toInt();
    else if (pc == 1) total += parts[0].toInt();
    return total < 0 ? 0 : total;
  }

  long total = 0;
  String num = "";
  bool sawUnit = false;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c >= '0' && c <= '9') { num += c; continue; }
    long v = num.toInt();
    if (c == 'w') total += v * 604800L;
    else if (c == 'd') total += v * 86400L;
    else if (c == 'h') total += v * 3600L;
    else if (c == 'm') total += v * 60L;
    else if (c == 's') total += v;
    num = "";
    sawUnit = true;
  }
  if (num.length() > 0 && !sawUnit) total += num.toInt();
  return total < 0 ? 0 : total;
}

String formatRosTimeSeconds(long seconds) {
  if (seconds < 0) seconds = 0;
  // RouterOS accepts compact "1w2d3h4m5s" time values.
  long w = seconds / 604800L; seconds %= 604800L;
  long d = seconds / 86400L; seconds %= 86400L;
  long h = seconds / 3600L; seconds %= 3600L;
  long m = seconds / 60L; long sec = seconds % 60L;
  String out = "";
  if (w > 0) { out += String(w); out += "w"; }
  if (d > 0) { out += String(d); out += "d"; }
  if (h > 0) { out += String(h); out += "h"; }
  if (m > 0) { out += String(m); out += "m"; }
  if (sec > 0 || out.length() == 0) { out += String(sec); out += "s"; }
  return out;
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

