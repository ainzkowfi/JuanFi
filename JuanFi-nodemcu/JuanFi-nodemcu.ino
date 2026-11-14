/*
 * JuanFi firmware (ESP8266)
 * RouterOS API transport (plain TCP 8728 by default), replacing telnet.
 * Config schema (current): 26 fields, with legacy 25/27/30 compatibility.
 */

#define CURRENT_VERSION "2.5-api"

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266mDNS.h>
#include <DNSServer.h>
#include <Arduino.h>
#include <EEPROM.h>
#include <Updater.h>
#include <flash_hal.h>
#include <MD5Builder.h>
#include "FS.h"
#include <base64.h>

const int TURN_OFF = 0;
const int TURN_ON = 1;

volatile int coin = 0;
volatile int processCoin = 0;
volatile int totalCoin = 0;
bool isNewVoucher = false;
volatile int coinsChange = 0;
String currentActiveVoucher = "";
int timeToAdd = 0;
bool coinSlotActive = false;
bool acceptCoin = false;
unsigned long targetMilis = 0;
bool coinExpired = false;
bool mikrotikConnectionSuccess = false;
unsigned long lastMikrotikReconnectAttempt = 0;
const unsigned long MIKROTIK_RECONNECT_INTERVAL = 15000;
String currentMacAddress = "";
String currentIpAddress = "";
const String HARDWARE_TYPE = "ESP8266";

typedef struct {
  String rateName;
  int price;
  int minutes;
  int validity;
  int dataLimit;
  String profileName;
} PromoRates;

PromoRates rates[100];
int ratesCount = 0;
int currentValidity = 0;
int currentDataLimit = 0;
String currentRateProfile = "";
String ADMIN_USER = "";
String ADMIN_PW = "";

const int LIFETIME_COIN_COUNT_ADDRESS = 0;
const int COIN_COUNT_ADDRESS = 5;
const int CUSTOMER_COUNT_ADDRESS = 10;
const int BACKUP_CONFIG_LENGTH_INDEX = 20;
const int EEPROM_SIZE = 512;

void ICACHE_RAM_ATTR coinInserted() {
  if (coinSlotActive) {
    coin = coin + 1;
    coinsChange = 1;
  }
}

int COIN_SELECTOR_PIN = 0;
int COIN_SET_PIN = 0;
int INSERT_COIN_LED = 0;
int SYSTEM_READY_LED = 0;
int CHECK_INTERNET_CONNECTION = 0;
int LED_TRIGGER_TYPE = 1;
int IP_ADDRESS_MODE = 0;
int VOUCHER_LOGIN_OPTION = 0;
int VOUCHER_VALIDITY_OPTION = 0;
String VOUCHER_PROFILE = "default";
String VOUCHER_PREFIX = "P";

int MAX_WAIT_COIN_SEC = 30000;
int SETUP_FINISH = 0;

IPAddress mikrotikRouterIp(10, 0, 0, 1);
int MIKROTIK_API_PORT = 8728;
String user = "pisonet";
String pwd = "abc123";
String ssid = "";
String password = "";
String adminAuth = "";
String adminSessionToken = "";
String vendorName = "";

IPAddress local_IP(192, 168, 10, 15);
IPAddress gateway(192, 168, 10, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(192, 168, 10, 1);
IPAddress apIP(172, 217, 28, 1);

WiFiClient apiClient;
ESP8266WebServer server(80);
const byte DNS_PORT = 53;
DNSServer dnsServer;

const unsigned long WIFI_CONNECT_TIMEOUT = 180000;
const int WIFI_CONNECT_DELAY = 500;
const String INTERNET_CHECK_URL = "http://ifconfig.me";

bool networkConnected = false;

// ---------- Utilities ----------

String safeField(const String &in, int maxLen) {
  if (maxLen < 0) maxLen = 0;
  String out = in;
  if ((int)out.length() > maxLen) out = out.substring(0, maxLen);
  return out;
}

bool isSafeToken(const String &s) {
  if (s.length() == 0 || s.length() > 32) return false;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    if (!ok) return false;
  }
  return true;
}

String sanitizeCommandToken(const String &in, int maxLen, const String &fallback) {
  String out;
  for (size_t i = 0; i < in.length() && (int)out.length() < maxLen; i++) {
    char c = in.charAt(i);
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    if (ok) out += c;
  }
  if (out.length() == 0) return fallback;
  return out;
}

String sanitizeRouterText(const String &in, int maxLen) {
  String out;
  for (size_t i = 0; i < in.length() && (int)out.length() < maxLen; i++) {
    char c = in.charAt(i);
    if (c == '"' || c == '\\' || c == ';' || c == '\r' || c == '\n' || c == '|') continue;
    if (c >= 32 && c <= 126) out += c;
  }
  return out;
}

int clampInt(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

int evaluateTriggerOutput(int state) {
  if (LED_TRIGGER_TYPE == 1) {
    return (state == TURN_ON) ? HIGH : LOW;
  }
  return (state == TURN_ON) ? LOW : HIGH;
}

String toJson(const char *keys[], const char *values[], int nField) {
  String json = "{";
  for (int i = 0; i < nField; i++) {
    if (i > 0) json += ",";
    json += " \"";
    json += String(keys[i]);
    json += "\": \"";
    json += String(values[i]);
    json += "\" ";
  }
  json += "}";
  return json;
}

void setupCORSPolicy() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Max-Age", "10000");
  server.sendHeader("Access-Control-Allow-Methods", "PUT,POST,GET,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "*");
  server.sendHeader("Access-Control-Allow-Credentials", "false");
}

// ---------- EEPROM / FS / config (mostly unchanged) ----------

void eeWriteInt(int pos, int val) {
  byte *p = (byte *)&val;
  EEPROM.write(pos, *p);
  EEPROM.write(pos + 1, *(p + 1));
  EEPROM.write(pos + 2, *(p + 2));
  EEPROM.write(pos + 3, *(p + 3));
  EEPROM.commit();
}

int eeGetInt(int pos) {
  int val;
  byte *p = (byte *)&val;
  *p = EEPROM.read(pos);
  *(p + 1) = EEPROM.read(pos + 1);
  *(p + 2) = EEPROM.read(pos + 2);
  *(p + 3) = EEPROM.read(pos + 3);
  if (val < 0) return 0;
  return val;
}

void eeWriteString(int addr, const String &val) {
  int str_len = val.length() + 1;
  for (int i = 0; i < str_len; ++i) {
    if (addr + i >= EEPROM_SIZE) break;
    EEPROM.write(addr + i, (i < (int)val.length()) ? val.charAt(i) : '\0');
  }
  EEPROM.commit();
}

String eeReadString(int addr, int str_len) {
  String val = "";
  if (str_len < 0) return val;
  for (int i = 0; i < str_len; ++i) {
    int idx = addr + i;
    if (idx >= EEPROM_SIZE) break;
    char c = (char)EEPROM.read(idx);
    if (c == '\0') break;
    val += c;
  }
  return val;
}

String getContentType(const String &filename) {
  if (filename.endsWith(".html")) return "text/html";
  else if (filename.endsWith(".css")) return "text/css";
  else if (filename.endsWith(".js")) return "application/javascript";
  else if (filename.endsWith(".ico")) return "image/x-icon";
  else if (filename.endsWith(".gz")) return "application/x-gzip";
  return "text/plain";
}

bool handleFileRead(String path) {
  if (path.endsWith("/")) path += "index.html";
  String contentType = getContentType(path);
  String pathWithGz = path + ".gz";
  if (SPIFFS.exists(pathWithGz) || SPIFFS.exists(path)) {
    if (SPIFFS.exists(pathWithGz)) path += ".gz";
    File file = SPIFFS.open(path, "r");
    if (!file) return false;
    server.streamFile(file, contentType);
    file.close();
    return true;
  }
  return false;
}

bool handleFileWrite(const String &path, const String &content) {
  if (!SPIFFS.exists(path)) return false;
  File file = SPIFFS.open(path, "w");
  if (!file) return false;
  size_t bytesWritten = file.print(content);
  file.close();
  return bytesWritten > 0;
}

String readFile(const String &path) {
  if (!SPIFFS.exists(path)) return String("");
  File file = SPIFFS.open(path, "r");
  if (!file) return String("");
  String content = file.readStringUntil('\n');
  file.close();
  return content;
}

int split(String rows[], int maxRows, const String &data, char delimeter) {
  if (maxRows <= 0) return 0;
  int count = 0;
  String elementData = "";
  for (int i = 0; i < (int)data.length(); i++) {
    char c = data.charAt(i);
    if (c == '\r' || c == '\n') continue;
    if (c != delimeter) {
      elementData += c;
    } else {
      if (count < maxRows) rows[count] = elementData;
      count++;
      elementData = "";
    }
  }
  if (count < maxRows) {
    rows[count] = elementData;
    count++;
  }
  return count;
}

void populateRates() {
  ratesCount = 0;
  String data = readFile("/admin/config/rates.data");
  if (data.length() == 0) return;
  int start = 0;
  int len = (int)data.length();
  while (start <= len && ratesCount < 100) {
    int end = data.indexOf('|', start);
    if (end < 0) end = len;
    if (end > start) {
      // Parse "name#price#minutes#validity#dataLimit#profile" without a 100-row stack array.
      String column[6];
      for (int c = 0; c < 6; c++) column[c] = "";
      int col = 0;
      int fstart = start;
      for (int i = start; i <= end && col < 6; i++) {
        char ch = (i < end) ? data.charAt(i) : '#';
        if (ch == '#' || i == end) {
          column[col++] = data.substring(fstart, i);
          fstart = i + 1;
        }
      }
      PromoRates r;
      r.rateName = column[0];
      r.price = column[1].toInt();
      r.minutes = column[2].toInt();
      r.validity = column[3].toInt();
      r.dataLimit = column[4].toInt();
      r.profileName = column[5];
      if (r.price < 0) r.price = 0;
      if (r.minutes < 0) r.minutes = 0;
      if (r.validity < 0) r.validity = 0;
      if (r.dataLimit < 0) r.dataLimit = 0;
      rates[ratesCount++] = r;
    }
    start = end + 1;
  }
}

void applyIpField(IPAddress &ip, const String &s, const IPAddress &fallback) {
  String parts[4];
  for (int i = 0; i < 4; i++) parts[i] = "";
  int c = split(parts, 4, s, '.');
  if (c < 4) { ip = fallback; return; }
  ip[0] = (uint8_t)parts[0].toInt();
  ip[1] = (uint8_t)parts[1].toInt();
  ip[2] = (uint8_t)parts[2].toInt();
  ip[3] = (uint8_t)parts[3].toInt();
}

void populateSystemConfiguration() {
  int backupLength = eeGetInt(BACKUP_CONFIG_LENGTH_INDEX);
  if (backupLength > 0 && backupLength < (EEPROM_SIZE - 30)) {
    String backupData = eeReadString(BACKUP_CONFIG_LENGTH_INDEX + 5, backupLength);
    if (backupData.length() > 0) {
      handleFileWrite("/admin/config/system.data", backupData);
    }
    eeWriteInt(BACKUP_CONFIG_LENGTH_INDEX, 0);
    delay(200);
    ESP.restart();
    return;
  }

  String data = readFile("/admin/config/system.data");
  String rows[40];
  for (int i = 0; i < 40; i++) rows[i] = "";
  int count = split(rows, 40, data, '|');

  auto get = [&](int idx)->String { return (idx >= 0 && idx < 40) ? rows[idx] : String(""); };

  MIKROTIK_API_PORT = 8728;

  // Configs written by the short-lived 32-field schema carried six notification
  // fields after the 26-field layout. Drop them so such a file still reads as the
  // 26-field layout instead of being mistaken for the older 30-field one.
  if (count >= 32) count = 26;

  if (count >= 30) {
    // Legacy 30-field
    vendorName = safeField(get(0), 32);
    ssid = safeField(get(1), 32);
    password = safeField(get(2), 64);
    applyIpField(mikrotikRouterIp, get(3), IPAddress(10, 0, 0, 1));
    user = safeField(get(4), 32);
    pwd = safeField(get(5), 64);
    MAX_WAIT_COIN_SEC = clampInt(get(6).toInt(), 5, 600) * 1000;
    ADMIN_USER = safeField(get(7), 32);
    ADMIN_PW = safeField(get(8), 64);
    COIN_SELECTOR_PIN = get(11).toInt();
    COIN_SET_PIN = get(12).toInt();
    SYSTEM_READY_LED = get(13).toInt();
    INSERT_COIN_LED = get(14).toInt();
    CHECK_INTERNET_CONNECTION = get(17).toInt() ? 1 : 0;
    VOUCHER_PREFIX = sanitizeCommandToken(get(18), 2, "P");
    SETUP_FINISH = get(20).toInt() ? 1 : 0;
    VOUCHER_LOGIN_OPTION = get(21).toInt() ? 1 : 0;
    VOUCHER_PROFILE = sanitizeCommandToken(get(22), 24, "default");
    VOUCHER_VALIDITY_OPTION = get(23).toInt() ? 1 : 0;
    LED_TRIGGER_TYPE = get(24).toInt() ? 1 : 0;
    IP_ADDRESS_MODE = get(25).toInt() ? 1 : 0;
    if (IP_ADDRESS_MODE == 1) {
      applyIpField(local_IP, get(26), local_IP);
      applyIpField(gateway, get(27), gateway);
      applyIpField(subnet, get(28), subnet);
      applyIpField(primaryDNS, get(29), primaryDNS);
    }
  } else if (count >= 27) {
    // Previous 27-field (abuse/ban at 9,10)
    vendorName = safeField(get(0), 32);
    ssid = safeField(get(1), 32);
    password = safeField(get(2), 64);
    applyIpField(mikrotikRouterIp, get(3), IPAddress(10, 0, 0, 1));
    user = safeField(get(4), 32);
    pwd = safeField(get(5), 64);
    MAX_WAIT_COIN_SEC = clampInt(get(6).toInt(), 5, 600) * 1000;
    ADMIN_USER = safeField(get(7), 32);
    ADMIN_PW = safeField(get(8), 64);
    COIN_SELECTOR_PIN = get(11).toInt();
    COIN_SET_PIN = get(12).toInt();
    SYSTEM_READY_LED = get(13).toInt();
    INSERT_COIN_LED = get(14).toInt();
    CHECK_INTERNET_CONNECTION = get(15).toInt() ? 1 : 0;
    VOUCHER_PREFIX = sanitizeCommandToken(get(16), 2, "P");
    SETUP_FINISH = get(17).toInt() ? 1 : 0;
    VOUCHER_LOGIN_OPTION = get(18).toInt() ? 1 : 0;
    VOUCHER_PROFILE = sanitizeCommandToken(get(19), 24, "default");
    VOUCHER_VALIDITY_OPTION = get(20).toInt() ? 1 : 0;
    LED_TRIGGER_TYPE = get(21).toInt() ? 1 : 0;
    IP_ADDRESS_MODE = get(22).toInt() ? 1 : 0;
    if (IP_ADDRESS_MODE == 1) {
      applyIpField(local_IP, get(23), local_IP);
      applyIpField(gateway, get(24), gateway);
      applyIpField(subnet, get(25), subnet);
      applyIpField(primaryDNS, get(26), primaryDNS);
    }
  } else if (count >= 26) {
    // New 26-field: 25-field + mikrotikApiPort at 25
    vendorName = safeField(get(0), 32);
    ssid = safeField(get(1), 32);
    password = safeField(get(2), 64);
    applyIpField(mikrotikRouterIp, get(3), IPAddress(10, 0, 0, 1));
    user = safeField(get(4), 32);
    pwd = safeField(get(5), 64);
    MAX_WAIT_COIN_SEC = clampInt(get(6).toInt(), 5, 600) * 1000;
    ADMIN_USER = safeField(get(7), 32);
    ADMIN_PW = safeField(get(8), 64);
    COIN_SELECTOR_PIN = get(9).toInt();
    COIN_SET_PIN = get(10).toInt();
    SYSTEM_READY_LED = get(11).toInt();
    INSERT_COIN_LED = get(12).toInt();
    CHECK_INTERNET_CONNECTION = get(13).toInt() ? 1 : 0;
    VOUCHER_PREFIX = sanitizeCommandToken(get(14), 2, "P");
    SETUP_FINISH = get(15).toInt() ? 1 : 0;
    VOUCHER_LOGIN_OPTION = get(16).toInt() ? 1 : 0;
    VOUCHER_PROFILE = sanitizeCommandToken(get(17), 24, "default");
    VOUCHER_VALIDITY_OPTION = get(18).toInt() ? 1 : 0;
    LED_TRIGGER_TYPE = get(19).toInt() ? 1 : 0;
    IP_ADDRESS_MODE = get(20).toInt() ? 1 : 0;
    if (IP_ADDRESS_MODE == 1) {
      applyIpField(local_IP, get(21), local_IP);
      applyIpField(gateway, get(22), gateway);
      applyIpField(subnet, get(23), subnet);
      applyIpField(primaryDNS, get(24), primaryDNS);
    }
    MIKROTIK_API_PORT = clampInt(get(25).toInt(), 1, 65535);
    if (MIKROTIK_API_PORT == 0) MIKROTIK_API_PORT = 8728;
  } else {
    // Previous 25-field (no API port)
    vendorName = safeField(get(0), 32);
    ssid = safeField(get(1), 32);
    password = safeField(get(2), 64);
    applyIpField(mikrotikRouterIp, get(3), IPAddress(10, 0, 0, 1));
    user = safeField(get(4), 32);
    pwd = safeField(get(5), 64);
    MAX_WAIT_COIN_SEC = clampInt(get(6).toInt(), 5, 600) * 1000;
    ADMIN_USER = safeField(get(7), 32);
    ADMIN_PW = safeField(get(8), 64);
    COIN_SELECTOR_PIN = get(9).toInt();
    COIN_SET_PIN = get(10).toInt();
    SYSTEM_READY_LED = get(11).toInt();
    INSERT_COIN_LED = get(12).toInt();
    CHECK_INTERNET_CONNECTION = get(13).toInt() ? 1 : 0;
    VOUCHER_PREFIX = sanitizeCommandToken(get(14), 2, "P");
    SETUP_FINISH = get(15).toInt() ? 1 : 0;
    VOUCHER_LOGIN_OPTION = get(16).toInt() ? 1 : 0;
    VOUCHER_PROFILE = sanitizeCommandToken(get(17), 24, "default");
    VOUCHER_VALIDITY_OPTION = get(18).toInt() ? 1 : 0;
    LED_TRIGGER_TYPE = get(19).toInt() ? 1 : 0;
    IP_ADDRESS_MODE = get(20).toInt() ? 1 : 0;
    if (IP_ADDRESS_MODE == 1) {
      applyIpField(local_IP, get(21), local_IP);
      applyIpField(gateway, get(22), gateway);
      applyIpField(subnet, get(23), subnet);
      applyIpField(primaryDNS, get(24), primaryDNS);
    }
  }


  if (MIKROTIK_API_PORT < 1 || MIKROTIK_API_PORT > 65535) MIKROTIK_API_PORT = 8728;
  if (ADMIN_USER.length() == 0) ADMIN_USER = "admin";
  if (ADMIN_PW.length() == 0) ADMIN_PW = "admin";
  adminAuth = base64::encode(ADMIN_USER + ":" + ADMIN_PW);
}

void backupSystemConfig() {
  String data = readFile("/admin/config/system.data");
  int len = data.length();
  if (len <= 0 || len >= (EEPROM_SIZE - 30)) return;
  eeWriteInt(BACKUP_CONFIG_LENGTH_INDEX, len);
  eeWriteString(BACKUP_CONFIG_LENGTH_INDEX + 5, data);
}

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
  String u = server.arg("username");
  String p = server.arg("password");
  if (u == ADMIN_USER && p == ADMIN_PW) {
    if (adminSessionToken.length() == 0) adminSessionToken = generateSessionToken();
    server.sendHeader("Set-Cookie", "jf_admin=" + adminSessionToken + "; Path=/; HttpOnly; SameSite=Lax");
    server.send(200, "text/plain", "ok");
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
  out.reserve(len);
  unsigned long start = millis();
  for (uint32_t i = 0; i < len; i++) {
    int b = -1;
    while (b < 0) {
      if ((millis() - start) > timeoutMs) return false;
      b = apiClient.read();
      if (b < 0) {
        if (!apiClient.connected() && !apiClient.available()) return false;
        delay(1);
        yield();
      }
    }
    out += (char)b;
  }
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
  String out;
  out.reserve(hex.length() / 2);
  for (int i = 0; i + 1 < (int)hex.length(); i += 2) {
    char pair[3] = { hex[i], hex[i + 1], 0 };
    char *endp = nullptr;
    long v = strtol(pair, &endp, 16);
    if (endp == pair) break;
    out += (char)(v & 0xFF);
  }
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

// ---------- HTTP handlers ----------

void handleHealth() { setupCORSPolicy(); server.send(200, "text/plain", "ok"); }

void handleLogout() {
  adminSessionToken = "";
  server.sendHeader("Set-Cookie", "jf_admin=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax");
  server.send(200, "text/plain", "ok");
}

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
  String voucher = VOUCHER_PREFIX;
  for (int i = 0; i < 6; i++) {
    uint32_t r = ESP.random();
    voucher += alphabet[r % alphaLen];
  }
  return voucher;
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
}

void disableCoinSlot() {
  coinSlotActive = false;
  digitalWrite(COIN_SET_PIN, LOW);
  digitalWrite(INSERT_COIN_LED, evaluateTriggerOutput(TURN_OFF));
}

void activateCoinSlot() {
  digitalWrite(COIN_SET_PIN, HIGH);
  delay(200);
  processCoin = 0;
  acceptCoin = true;
  coinSlotActive = true;
  targetMilis = millis() + (unsigned long)MAX_WAIT_COIN_SEC;
  digitalWrite(INSERT_COIN_LED, evaluateTriggerOutput(TURN_ON));
}

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
  eeWriteInt(LIFETIME_COIN_COUNT_ADDRESS, eeGetInt(LIFETIME_COIN_COUNT_ADDRESS) + totalCoin);
  eeWriteInt(COIN_COUNT_ADDRESS, eeGetInt(COIN_COUNT_ADDRESS) + totalCoin);
  eeWriteInt(CUSTOMER_COUNT_ADDRESS, eeGetInt(CUSTOMER_COUNT_ADDRESS) + 1);
}

void handleAdminPage() { if (!isAuthorized()) { server.sendHeader("Location", "/admin/login", true); server.send(302, "text/plain", ""); return; } handleFileRead("/admin/system-config.html"); }
void handleAdminGeneratedVoucherPage() { if (!isAuthorized()) { server.sendHeader("Location", "/admin/login", true); server.send(302, "text/plain", ""); return; } handleFileRead("/admin/voucher-generate.html"); }
void handleJquerySript() { handleFileRead("/admin/js/jquery.min.js"); }
void handleUserGetRates() { setupCORSPolicy(); handleFileRead("/admin/config/rates.data"); }
void handleAdminGetRates() { if (!isAuthorized()) { handleNotAuthorize(); return; } handleFileRead("/admin/config/rates.data"); }

void handleAdminSaveRates() {
  if (!isAuthorized()) { handleNotAuthorize(); return; }
  String data = server.arg("data");
  if (data.length() > 4096) { server.send(413, "text/plain", "too large"); return; }
  if (!handleFileWrite("/admin/config/rates.data", data)) { server.send(500, "text/plain", "write failed"); return; }
  populateRates();
  server.send(200, "text/plain", "ok");
}

void handleAdminSaveSystemConfig() {
  if (!isAuthorized()) { handleNotAuthorize(); return; }
  String data = server.arg("data");
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
  if (type == "lifeTimeCount") eeWriteInt(LIFETIME_COIN_COUNT_ADDRESS, 0);
  else if (type == "coinCount") eeWriteInt(COIN_COUNT_ADDRESS, 0);
  else if (type == "customerCount") eeWriteInt(CUSTOMER_COUNT_ADDRESS, 0);
  server.send(200, "text/plain", "ok");
}

void handleAdminDashboard() {
  if (!isAuthorized()) { handleNotAuthorize(); return; }
  unsigned long upTime = millis();
  int lifeTimeCoinCount = eeGetInt(LIFETIME_COIN_COUNT_ADDRESS);
  int coinCountVal = eeGetInt(COIN_COUNT_ADDRESS);
  int customerCount = eeGetInt(CUSTOMER_COUNT_ADDRESS);
  bool hasInternetConnection = true;
  if (CHECK_INTERNET_CONNECTION == 1) hasInternetConnection = hasInternetConnect();
  // Refresh mikrotik status opportunistically (lightweight) without blocking long; rely on ensure state
  bool mtOk = mikrotikConnectionSuccess && apiClient.connected();
  String data = String(upTime) + "|" + String(lifeTimeCoinCount) + "|" + String(coinCountVal) + "|" +
                String(customerCount) + "|" + String(hasInternetConnection ? "1" : "0") + "|" +
                String(mtOk ? "1" : "0") + "|" + currentMacAddress + "|" +
                currentIpAddress + "|" + HARDWARE_TYPE + "|" + String(CURRENT_VERSION);
  server.send(200, "text/plain", data);
}

void testInsertCoin() {
  if (!isAuthorized()) { handleNotAuthorize(); return; }
  int c = server.arg("coin").toInt();
  if (c < 0) c = 0;
  if (coinSlotActive) { coin += c; coinsChange = 1; }
  server.send(200, "text/plain", "ok");
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
    totalCoin += processCoin;
    timeToAdd = calculateAddTime();
    const char *keys[] = {"status", "newCoin", "timeAdded", "totalCoin", "validity", "data"};
    char coinStr[16]; itoa(processCoin, coinStr, 10);
    char timeToAddStr[16]; itoa(timeToAdd, timeToAddStr, 10);
    char totalCoinStr[16]; itoa(totalCoin, totalCoinStr, 10);
    char validityStr[16]; itoa(currentValidity, validityStr, 10);
    char currentDataLimitStr[16]; itoa(currentDataLimit, currentDataLimitStr, 10);
    const char *values[] = {"true", coinStr, timeToAddStr, totalCoinStr, validityStr, currentDataLimitStr};
    activateCoinSlot();
    setupCORSPolicy();
    server.send(200, "application/json", toJson(keys, values, 6));
  } else {
    const char *keys[] = {"status", "errorCode", "remainTime", "timeAdded", "totalCoin", "waitTime", "validity", "data"};
    char remainTimeStr[20]; long remain = (long)targetMilis - (long)millis(); itoa(remain, remainTimeStr, 10);
    char timeToAddStr[16]; itoa(timeToAdd, timeToAddStr, 10);
    char totalCoinStr[16]; itoa(totalCoin, totalCoinStr, 10);
    char waitTimeStr[16]; itoa(MAX_WAIT_COIN_SEC, waitTimeStr, 10);
    char validityStr[16]; itoa(currentValidity, validityStr, 10);
    char currentDataLimitStr[16]; itoa(currentDataLimit, currentDataLimitStr, 10);
    const char *values[] = {"false", "coin.not.inserted", remainTimeStr, timeToAddStr, totalCoinStr, waitTimeStr, validityStr, currentDataLimitStr};
    setupCORSPolicy();
    server.send(200, "application/json", toJson(keys, values, 8));
  }
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
    bool regOk = registerNewVoucher(currentActiveVoucher);
    bool extOk = regOk && addTimeToVoucher(currentActiveVoucher, timeToAdd);
    if (extOk) {
      updateStatistic();
    } else {
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

void replyConvertRefused() {
  const char *keys[] = {"status", "errorCode"};
  const char *values[] = {"false", "convertVoucher.refused"};
  setupCORSPolicy();
  server.send(200, "application/json", toJson(keys, values, 2));
}

bool removeVoucherUser(const String &target) {
  String a[1];
  a[0] = "=numbers=" + target;
  if (apiTransact("/ip/hotspot/user/remove", a, 1, 7000)) return true;
  Serial.print("remove voucher failed: ");
  Serial.println(apiLast.trapMessage);
  return false;
}

// Move the unused time (and unused data) of a second voucher onto the customer's
// active voucher, then delete the merged code so its name returns to the pool the
// generator draws from. Without this, spent codes stay behind as dead hotspot users
// and the finite code space fills up with codes nobody can use.
void convertVoucher() {
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
  setupCORSPolicy();
  server.send(200, "application/json", toJson(keys, values, 3));
}

void topUp() {
  bool hasInternetConnection = true;
  if (CHECK_INTERNET_CONNECTION == 1) hasInternetConnection = hasInternetConnect();
  if (!hasInternetConnection) {
    const char *keys[] = {"status", "errorCode"};
    const char *values[] = {"false", "no.internet.detected"};
    setupCORSPolicy();
    server.send(200, "application/json", toJson(keys, values, 2));
    return;
  }
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
  if (currentActiveVoucher.length() > 0 || coinSlotActive) {
    server.send(409, "text/plain", "Coin session in progress; try again after the customer finishes.");
    return;
  }
  if (!ensureMikrotikConnection(true)) { server.send(503, "text/plain", "MikroTik unavailable"); return; }
  int amount = clampInt(server.arg("amt").toInt(), 0, 100000);
  int qty = clampInt(server.arg("qty").toInt(), 1, 50);
  int addToSales = server.arg("sales").toInt() ? 1 : 0;
  String prefix = sanitizeCommandToken(server.arg("pfx"), 4, VOUCHER_PREFIX);
  String voucherGenerated = "";
  int successCount = 0;
  const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  // Codes minted here are fresh sessions, never extensions of an existing one.
  isNewVoucher = true;
  totalCoin = amount;
  timeToAdd = calculateAddTime();
  for (int i = 0; i < qty; i++) {
    // A 4-character suffix shares its name space with every voucher ever printed,
    // and registerNewVoucher() treats "already exists" as success. Redraw instead of
    // silently extending some other customer's voucher.
    String voucher = "";
    for (int attempt = 0; attempt < 5 && voucher.length() == 0; attempt++) {
      String candidate = prefix;
      for (int j = 0; j < 4; j++) {
        candidate += alphabet[ESP.random() % (sizeof(alphabet) - 1)];
      }
      bool existed = false;
      if (!registerNewVoucher(candidate, &existed)) break;  // real trap: stop retrying
      if (!existed) voucher = candidate;
    }
    if (voucher.length() == 0) continue;
    bool extOk = addTimeToVoucher(voucher, timeToAdd);
    if (!extOk) continue;
    if (addToSales == 1) updateStatistic();
    if (successCount > 0) voucherGenerated += "#";
    voucherGenerated += voucher;
    successCount++;
  }
  String safeVendor = sanitizeRouterText(vendorName, 32);
  String returnData = safeVendor + "|" + String(amount) + "|" + String(timeToAdd) + "|" + voucherGenerated;
  if (successCount == 0) {
    server.send(503, "text/plain", "MikroTik operation failed");
  } else {
    server.send(200, "text/plain", returnData);
  }
  resetGlobalVariables();
}

bool hasUploadError = false;

void handleFileUploadRequest() {
  if (Update.hasError() || hasUploadError) {
    server.send(200, "text/html", "Upload failed");
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

// ---------- Setup / Loop ----------

void setup() {
  Serial.begin(115200);
  delay(100);
  EEPROM.begin(EEPROM_SIZE);
  randomSeed(micros() ^ ESP.getCycleCount() ^ (uint32_t)ESP.getChipId() ^ analogRead(A0));

  if (!SPIFFS.begin()) {
    Serial.println("SPIFFS mount failed");
  }

  populateSystemConfiguration();

  pinMode(COIN_SELECTOR_PIN, INPUT_PULLUP);
  pinMode(INSERT_COIN_LED, OUTPUT);
  pinMode(SYSTEM_READY_LED, OUTPUT);
  pinMode(COIN_SET_PIN, OUTPUT);
  digitalWrite(COIN_SET_PIN, LOW);
  digitalWrite(INSERT_COIN_LED, evaluateTriggerOutput(TURN_OFF));
  digitalWrite(SYSTEM_READY_LED, evaluateTriggerOutput(TURN_OFF));

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);

  if (IP_ADDRESS_MODE == 1) {
    WiFi.config(local_IP, primaryDNS, gateway, subnet);
  }

  if (SETUP_FINISH == 1 && ssid.length() > 0) {
    WiFi.begin(ssid.c_str(), password.c_str());
    unsigned long start = millis();
    while (millis() - start < WIFI_CONNECT_TIMEOUT) {
      networkConnected = (WiFi.status() == WL_CONNECTED);
      if (networkConnected) break;
      digitalWrite(SYSTEM_READY_LED, evaluateTriggerOutput(TURN_OFF));
      delay(WIFI_CONNECT_DELAY);
      digitalWrite(SYSTEM_READY_LED, evaluateTriggerOutput(TURN_ON));
    }
    if (networkConnected) {
      currentIpAddress = WiFi.localIP().toString();
      currentMacAddress = WiFi.macAddress();
    }
    digitalWrite(SYSTEM_READY_LED, evaluateTriggerOutput(TURN_OFF));
  } else {
    networkConnected = false;
  }

  if (networkConnected) {
    digitalWrite(INSERT_COIN_LED, evaluateTriggerOutput(TURN_OFF));
    digitalWrite(SYSTEM_READY_LED, evaluateTriggerOutput(TURN_OFF));
    attachInterrupt(digitalPinToInterrupt(COIN_SELECTOR_PIN), coinInserted, RISING);
    loginMirotik();

    if (MDNS.begin("juanfi")) { }

    server.on("/topUp", topUp);
    server.on("/checkCoin", checkCoin);
    server.on("/useVoucher", useVoucher);
    server.on("/health", handleHealth);
    server.on("/getRates", handleUserGetRates);
    server.on("/cancelTopUp", handleCancelTopUp);
    server.on("/convertVoucher", convertVoucher);
    server.on("/testInsertCoin", testInsertCoin);
    server.onNotFound(handleNotFound);
  } else {
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP("JuanFi Setup");
    dnsServer.start(DNS_PORT, "*", apIP);
    currentIpAddress = apIP.toString();
    currentMacAddress = WiFi.softAPmacAddress();
    // Serve login directly for captive portal compatibility (no redirect/auth challenge).
    server.on("/", HTTP_GET, handleAdminLoginPage);
    server.onNotFound([]() {
      if (server.method() == HTTP_OPTIONS) {
        setupCORSPolicy();
        server.send(204);
        return;
      }
      handleAdminLoginPage();
    });
  }

  server.on("/admin/api/dashboard", handleAdminDashboard);
  server.on("/admin/js/jquery.min.js", handleJquerySript);
  server.on("/admin/api/resetStatistic", handleAdminResetStats);
  server.on("/admin/api/saveSystemConfig", handleAdminSaveSystemConfig);
  server.on("/admin/api/getSystemConfig", handleAdminGetSystemConfig);
  server.on("/admin/api/getRates", handleAdminGetRates);
  server.on("/admin/api/saveRates", handleAdminSaveRates);
  server.on("/admin/api/login", HTTP_POST, handleAdminLogin);
  server.on("/admin/login", HTTP_GET, handleAdminLoginPage);
  server.on("/admin/api/logout", handleLogout);
  server.on("/admin/api/generateVouchers", handleGenerateVouchers);
  server.on("/admin", HTTP_GET, handleAdminPage);
  server.on("/admin/viewGeneratedVouchers", handleAdminGeneratedVoucherPage);
  server.on("/admin/updateMainBin", HTTP_POST, handleFileUploadRequest, handleFileUploadStream);

  populateRates();
  server.collectHeaders("Cookie");
  server.begin();

  if (mikrotikConnectionSuccess) {
    digitalWrite(SYSTEM_READY_LED, evaluateTriggerOutput(TURN_ON));
  }
}

void loop() {
  if (networkConnected) {
    unsigned long currentMilis = millis();

    if (mikrotikConnectionSuccess && !apiClient.connected()) {
      mikrotikConnectionSuccess = false;
      digitalWrite(SYSTEM_READY_LED, evaluateTriggerOutput(TURN_OFF));
    }
    if (!mikrotikConnectionSuccess) {
      if (ensureMikrotikConnection()) {
        digitalWrite(SYSTEM_READY_LED, evaluateTriggerOutput(TURN_ON));
      }
    }

    if (acceptCoin) {
      if (targetMilis > currentMilis) {
        coinExpired = false;
        if (coinsChange > 0) {
          delay(50);
          noInterrupts();
          processCoin = coin;
          coin = 0;
          coinsChange = 0;
          interrupts();
          // Stop accepting further pulses until the portal has polled /checkCoin
          // and received this coin. checkCoin() accumulates, recalculates and
          // re-arms the slot; doing it here too would double-count and would
          // never let the portal observe the "new coin" response.
          coinSlotActive = false;
          digitalWrite(COIN_SET_PIN, LOW);
          digitalWrite(INSERT_COIN_LED, evaluateTriggerOutput(TURN_OFF));
          acceptCoin = false;
        }
      } else {
        disableCoinSlot();
        acceptCoin = false;
        coinExpired = true;
        // If a coin was detected but the portal never polled /checkCoin to
        // consume it, fold it in here so it is not lost on session expiry/commit.
        if (processCoin > 0) {
          totalCoin += processCoin;
          processCoin = 0;
        }
        timeToAdd = calculateAddTime();
        if (timeToAdd > 0 && currentActiveVoucher.length() > 0) {
          if (ensureMikrotikConnection(true)) {
            if (isNewVoucher) {
              // best-effort register; extend will confirm existence/update
              registerNewVoucher(currentActiveVoucher);
            }
            bool extOk = addTimeToVoucher(currentActiveVoucher, timeToAdd);
            if (extOk) {
              updateStatistic();
              resetGlobalVariables();
            } else {
              // Keep session state so portal/user can retry when router link recovers.
              mikrotikConnectionSuccess = apiClient.connected();
              digitalWrite(SYSTEM_READY_LED, evaluateTriggerOutput(TURN_OFF));
            }
          } else {
            mikrotikConnectionSuccess = false;
            digitalWrite(SYSTEM_READY_LED, evaluateTriggerOutput(TURN_OFF));
          }
        } else {
          resetGlobalVariables();
        }
      }
    }
  } else {
    if (SETUP_FINISH == 1 && millis() >= 600000UL) {
      ESP.restart();
    }
    dnsServer.processNextRequest();
  }

  server.handleClient();
  if (networkConnected) {
    MDNS.update();
  }
}
