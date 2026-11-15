/*
 * JuanFi firmware (ESP8266)
 * RouterOS API transport (plain TCP 8728 by default), replacing telnet.
 * Config schema (current): 26 fields, with legacy 25/27/30 compatibility.
 */

#define CURRENT_VERSION "v1"

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
volatile int pulsesThisSession = 0;
volatile int droppedPulsesThisSession = 0;
const int MAX_PULSE_PER_SESSION = 20;
String currentActiveVoucher = "";
int timeToAdd = 0;
bool coinSlotActive = false;
bool acceptCoin = false;
bool coinSlotArming = false;
unsigned long coinSlotArmAtMs = 0;
unsigned long targetMilis = 0;
bool coinExpired = false;
bool mikrotikConnectionSuccess = false;
unsigned long lastMikrotikReconnectAttempt = 0;
const unsigned long MIKROTIK_RECONNECT_INTERVAL = 15000;
String currentMacAddress = "";
String currentIpAddress = "";
const String HARDWARE_TYPE = "ESP8266";
int AUTO_RESTART_MINUTES = 0;
unsigned long autoRestartAtMs = 0;
unsigned long lastAdminPanelSeenMs = 0;
const unsigned long ADMIN_PANEL_ACTIVE_WINDOW_MS = 120000;

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
    if (pulsesThisSession < MAX_PULSE_PER_SESSION) {
      pulsesThisSession++;
      coin = coin + 1;
      coinsChange = 1;
    } else {
      droppedPulsesThisSession++;
    }
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

int adminLoginFails = 0;
unsigned long adminLockoutUntilMs = 0;
const int ADMIN_MAX_FAILS = 5;
const unsigned long ADMIN_LOCKOUT_MS = 30000UL;

IPAddress local_IP(10, 0, 0, 254);
IPAddress gateway(10, 0, 0, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(1, 1, 1, 1);
IPAddress apIP(172, 217, 28, 1);

WiFiClient apiClient;
ESP8266WebServer server(80);
const byte DNS_PORT = 53;
DNSServer dnsServer;

const unsigned long WIFI_CONNECT_TIMEOUT = 360000; // 6 minutes
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

// Append a JSON-escaped string (including surrounding quotes) into buf.
// Caller stops on overflow; the buffer is always NUL-terminated when cap > 0.
static void appendJsonString(char *buf, size_t cap, size_t &pos, const char *s) {
  if (cap == 0) return;
  auto put = [&](char c) {
    if (pos + 1 < cap) buf[pos++] = c;
  };
  put('"');
  if (s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
      unsigned char c = *p;
      switch (c) {
        case '"':  put('\\'); put('"');  break;
        case '\\': put('\\'); put('\\'); break;
        case '\b': put('\\'); put('b');  break;
        case '\f': put('\\'); put('f');  break;
        case '\n': put('\\'); put('n');  break;
        case '\r': put('\\'); put('r');  break;
        case '\t': put('\\'); put('t');  break;
        default:
          if (c < 0x20) {
            char esc[7];
            snprintf(esc, sizeof(esc), "\\u%04x", c);
            for (char *e = esc; *e; e++) put(*e);
          } else {
            put((char)c);
          }
          break;
      }
      if (pos + 1 >= cap) break;
    }
  }
  put('"');
  buf[pos < cap ? pos : (cap - 1)] = '\0';
}

const char* toJson(const char *keys[], const char *values[], int nField) {
  static char buf[512];
  size_t pos = 0;
  if (pos + 1 < sizeof(buf)) buf[pos++] = '{';
  for (int i = 0; i < nField; i++) {
    if (i > 0 && pos + 1 < sizeof(buf)) buf[pos++] = ',';
    appendJsonString(buf, sizeof(buf), pos, keys[i] ? keys[i] : "");
    if (pos + 1 < sizeof(buf)) buf[pos++] = ':';
    appendJsonString(buf, sizeof(buf), pos, values[i] ? values[i] : "");
    if (pos + 1 >= sizeof(buf)) break;
  }
  if (pos + 1 < sizeof(buf)) buf[pos++] = '}';
  buf[pos < sizeof(buf) ? pos : (sizeof(buf) - 1)] = '\0';
  return buf;
}

void setupCORSPolicy() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Max-Age", "10000");
  server.sendHeader("Access-Control-Allow-Methods", "PUT,POST,GET,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "*");
  server.sendHeader("Access-Control-Allow-Credentials", "false");
}

// ---------- EEPROM / FS / config (mostly unchanged) ----------

void eeWriteIntNoCommit(int pos, int val) {
  byte *p = (byte *)&val;
  EEPROM.write(pos, *p);
  EEPROM.write(pos + 1, *(p + 1));
  EEPROM.write(pos + 2, *(p + 2));
  EEPROM.write(pos + 3, *(p + 3));
}

void eeWriteInt(int pos, int val) {
  eeWriteIntNoCommit(pos, val);
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
  File file = SPIFFS.open(path, "w");
  if (!file) return false;
  size_t bytesWritten = file.print(content);
  file.close();
  return (bytesWritten > 0) || (content.length() == 0);
}

String readFile(const String &path) {
  if (!SPIFFS.exists(path)) return String("");
  File file = SPIFFS.open(path, "r");
  if (!file) return String("");
  String content = file.readStringUntil('\n');
  file.close();
  return content;
}

// Config fields are short (<= 64 chars by schema), so accumulate each row in a fixed
// buffer and hand it to String once instead of growing a String per character.
int split(String rows[], int maxRows, const String &data, char delimeter) {
  if (maxRows <= 0) return 0;
  int count = 0;
  char buf[128];
  size_t p = 0;
  buf[0] = '\0';
  const char *s = data.c_str();
  for (int i = 0; s[i]; i++) {
    char c = s[i];
    if (c == '\r' || c == '\n') continue;
    if (c != delimeter) {
      if (p + 1 < sizeof(buf)) { buf[p++] = c; buf[p] = '\0'; }
    } else {
      if (count < maxRows) rows[count] = buf;
      count++;
      p = 0;
      buf[0] = '\0';
    }
  }
  if (count < maxRows) {
    rows[count] = buf;
    count++;
  }
  return count;
}

void populateRates() {
  ratesCount = 0;
  String data = readFile("/admin/config/rates.data");
  if (data.length() == 0) return;
  // Parse "name#price#minutes#validity#dataLimit#profile" straight off the file buffer:
  // the old version took a substring per column, i.e. six heap allocations per rate row.
  const char *s = data.c_str();
  int len = (int)data.length();
  int start = 0;
  while (start <= len && ratesCount < 100) {
    int end = start;
    while (end < len && s[end] != '|') end++;
    if (end > start) {
      char column[6][48];
      int col = 0;
      int fstart = start;
      int i = start;
      while (col < 6) {
        bool atEnd = (i >= end);
        if (atEnd || s[i] == '#') {
          int flen = i - fstart;
          if (flen < 0) flen = 0;
          if (flen > (int)sizeof(column[col]) - 1) flen = (int)sizeof(column[col]) - 1;
          if (flen > 0) memcpy(column[col], s + fstart, flen);
          column[col][flen] = '\0';
          col++;
          fstart = i + 1;
          if (atEnd) break;
        }
        i++;
      }
      for (; col < 6; col++) column[col][0] = '\0';
      PromoRates r;
      r.rateName = column[0];
      r.price = atoi(column[1]);
      r.minutes = atoi(column[2]);
      r.validity = atoi(column[3]);
      r.dataLimit = atoi(column[4]);
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

// Parse "a.b.c.d" without the four Strings the split()-based version allocated.
void applyIpField(IPAddress &ip, const String &s, const IPAddress &fallback) {
  uint8_t octet[4] = { 0, 0, 0, 0 };
  int parsed = 0;
  int value = 0;
  bool digits = false;
  for (const char *p = s.c_str(); ; p++) {
    char c = *p;
    if (c >= '0' && c <= '9') {
      value = value * 10 + (c - '0');
      if (value > 255) value = 255;
      digits = true;
      continue;
    }
    if (!digits) break;
    if (parsed < 4) octet[parsed] = (uint8_t)value;
    parsed++;
    value = 0;
    digits = false;
    if (c != '.' || parsed >= 4 || c == '\0') break;
  }
  if (parsed < 4) { ip = fallback; return; }
  ip[0] = octet[0];
  ip[1] = octet[1];
  ip[2] = octet[2];
  ip[3] = octet[3];
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

  // Return a reference: by-value returns copied the row (a heap allocation) on each
  // of the ~25 field reads below.
  static const String emptySetting = "";
  auto get = [&](int idx)->const String & { return (idx >= 0 && idx < 40) ? rows[idx] : emptySetting; };

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
    CHECK_INTERNET_CONNECTION = 0;
    VOUCHER_PREFIX = sanitizeCommandToken(get(18), 5, "P");
    SETUP_FINISH = get(20).toInt() ? 1 : 0;
    VOUCHER_LOGIN_OPTION = 0;
    VOUCHER_PROFILE = sanitizeCommandToken(get(22), 24, "default");
    VOUCHER_VALIDITY_OPTION = 0;
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
    CHECK_INTERNET_CONNECTION = 0;
    VOUCHER_PREFIX = sanitizeCommandToken(get(16), 5, "P");
    SETUP_FINISH = get(17).toInt() ? 1 : 0;
    VOUCHER_LOGIN_OPTION = 0;
    VOUCHER_PROFILE = sanitizeCommandToken(get(19), 24, "default");
    VOUCHER_VALIDITY_OPTION = 0;
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
    CHECK_INTERNET_CONNECTION = 0;
    VOUCHER_PREFIX = sanitizeCommandToken(get(14), 5, "P");
    SETUP_FINISH = get(15).toInt() ? 1 : 0;
    VOUCHER_LOGIN_OPTION = 0;
    VOUCHER_PROFILE = sanitizeCommandToken(get(17), 24, "default");
    VOUCHER_VALIDITY_OPTION = 0;
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
    CHECK_INTERNET_CONNECTION = 0;
    VOUCHER_PREFIX = sanitizeCommandToken(get(14), 5, "P");
    SETUP_FINISH = get(15).toInt() ? 1 : 0;
    VOUCHER_LOGIN_OPTION = 0;
    VOUCHER_PROFILE = sanitizeCommandToken(get(17), 24, "default");
    VOUCHER_VALIDITY_OPTION = 0;
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
  int maxBackupLen = EEPROM_SIZE - (BACKUP_CONFIG_LENGTH_INDEX + 5);
  if (len <= 0) return;
  if (len > maxBackupLen) {
    Serial.println("Config backup skipped: system.data too large for EEPROM backup");
    return;
  }
  eeWriteInt(BACKUP_CONFIG_LENGTH_INDEX, len);
  eeWriteString(BACKUP_CONFIG_LENGTH_INDEX + 5, data);
}

void loadAutoRestartConfig() {
  String data = readFile("/admin/config/autorestart.data");
  data.trim();
  AUTO_RESTART_MINUTES = clampInt(data.toInt(), 0, 10080);
}

void scheduleAutoRestart() {
  if (AUTO_RESTART_MINUTES > 0) {
    autoRestartAtMs = millis() + ((unsigned long)AUTO_RESTART_MINUTES * 60000UL);
  } else {
    autoRestartAtMs = 0;
  }
}

bool systemIsIdleForRestart() {
  if (coinSlotActive || acceptCoin || coinSlotArming) return false;
  if (coin != 0 || coinsChange != 0 || processCoin != 0) return false;
  if (totalCoin != 0 || timeToAdd != 0) return false;
  if (currentActiveVoucher.length() > 0) return false;
  if (lastAdminPanelSeenMs != 0 && (long)(millis() - lastAdminPanelSeenMs) < (long)ADMIN_PANEL_ACTIVE_WINDOW_MS) return false;
  return true;
}



// Forward declarations for split headers
bool isAuthorized();
void handleNotAuthorize();
bool removeVoucherUser(const String &target);

#include "mikrotik_api.h"
#include "coin_isr.h"
#include "voucher.h"
#include "admin_http.h"

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
  loadAutoRestartConfig();
  scheduleAutoRestart();

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
  server.on("/admin/api/resetStatistic", HTTP_POST, handleAdminResetStats);
  server.on("/admin/api/saveSystemConfig", HTTP_POST, handleAdminSaveSystemConfig);
  server.on("/admin/api/getSystemConfig", handleAdminGetSystemConfig);
  server.on("/admin/api/getRates", handleAdminGetRates);
  server.on("/admin/api/saveRates", HTTP_POST, handleAdminSaveRates);
  server.on("/admin/api/login", HTTP_POST, handleAdminLogin);
  server.on("/admin/login", HTTP_GET, handleAdminLoginPage);
  server.on("/admin/api/logout", HTTP_POST, handleLogout);
  server.on("/admin/api/generateVouchers", HTTP_POST, handleGenerateVouchers);
  server.on("/admin/api/getAutoRestart", handleAdminGetAutoRestart);
  server.on("/admin/api/saveAutoRestart", HTTP_POST, handleAdminSaveAutoRestart);
  server.on("/admin/api/ping", handleAdminPanelPing);
  server.on("/admin/api/hotspotSummary", handleAdminHotspotSummary);
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

    if (autoRestartAtMs > 0 && (long)(currentMilis - autoRestartAtMs) >= 0 && systemIsIdleForRestart()) {
      ESP.restart();
    }

    if (coinSlotArming && (long)(currentMilis - coinSlotArmAtMs) >= 0) {
      finishCoinSlotArm();
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
