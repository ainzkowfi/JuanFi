/*
 * JuanFi firmware (ESP8266)
 * Security/robustness fixes: 2,3,6,7,8
 * Removed: ESP32, LCD, e-load, charging, ESP-side coinslot banning
 */

#define CURRENT_VERSION "2.4"

#include <ESP8266TelnetClient.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266mDNS.h>
#include <DNSServer.h>
#include <Arduino.h>
#include <EEPROM.h>
#include <Updater.h>
#include <flash_hal.h>
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
String user = "pisonet";
String pwd = "abc123";
String ssid = "";
String password = "";
String adminAuth = "";
String vendorName = "";

IPAddress local_IP(192, 168, 10, 15);
IPAddress gateway(192, 168, 10, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(192, 168, 10, 1);
IPAddress apIP(172, 217, 28, 1);

WiFiClient client2;
WiFiClient client;
ESP8266telnetClient tc(client);
ESP8266WebServer server(80);
const byte DNS_PORT = 53;
DNSServer dnsServer;

const unsigned long WIFI_CONNECT_TIMEOUT = 180000;
const int WIFI_CONNECT_DELAY = 500;
const String INTERNET_CHECK_URL = "http://ifconfig.me";

bool networkConnected = false;

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

String toJson(char *keys[], char *values[], int nField) {
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
  String rows[100];
  int totalRows = split(rows, 100, data, '|');
  int n = (totalRows > 100) ? 100 : totalRows;
  for (int i = 0; i < n; i++) {
    if (rows[i].length() == 0) continue;
    String column[6];
    for (int c = 0; c < 6; c++) column[c] = "";
    int cols = split(column, 6, rows[i], '#');
    PromoRates r;
    r.rateName = (cols > 0) ? column[0] : "";
    r.price = (cols > 1) ? column[1].toInt() : 0;
    r.minutes = (cols > 2) ? column[2].toInt() : 0;
    r.validity = (cols > 3) ? column[3].toInt() : 0;
    r.dataLimit = (cols > 4) ? column[4].toInt() : 0;
    r.profileName = (cols > 5) ? column[5] : "";
    if (r.price < 0) r.price = 0;
    if (r.minutes < 0) r.minutes = 0;
    if (r.validity < 0) r.validity = 0;
    if (r.dataLimit < 0) r.dataLimit = 0;
    rates[ratesCount++] = r;
    if (ratesCount >= 100) break;
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

  if (count >= 30) {
    // Legacy 30-field: 0 vendor,1 ssid,2 wifiPW,3 mikrotikIp,4 user,5 pw,6 wait,7 adminU,8 adminP,9 abuse,10 ban,11 coinPin,12 setPin,13 ready,14 insert,15 lcd,16 btn,17 check,18 prefix,19 marquee,20 setup,21 loginOpt,22 profile,23 validity,24 ledTrig,25 ipMode,26 local,27 gw,28 sub,29 dns
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
    // Previous 27-field: included abuse/ban at 9,10
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
  } else {
    // New 25-field: abuse/ban removed
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

bool isAuthorized() {
  String auth = server.header("Authorization");
  String expectedAuth = "Basic " + adminAuth;
  return auth == expectedAuth;
}

void handleNotAuthorize() {
  server.sendHeader("WWW-Authenticate", "Basic realm=\"Secure\"");
  server.send(401, "text/html", "Authentication failed");
}

void handleNotFound() {
  if (server.method() == HTTP_OPTIONS) {
    setupCORSPolicy();
    server.send(204);
    return;
  }
  server.send(404, "text/plain", "");
}

void handleHealth() { setupCORSPolicy(); server.send(200, "text/plain", "ok"); }

void handleLogout() {
  server.sendHeader("WWW-Authenticate", "Basic realm=\"Secure\"");
  server.send(401, "text/html", "Authentication failed");
}

bool sendCommand(const String &script) {
  if (!ensureMikrotikConnection()) return false;
  int scriptLength = script.length() + 1;
  char command[scriptLength];
  script.toCharArray(command, scriptLength);
  tc.sendCommand(command);
  return true;
}

void loginMirotik() {
  if (WiFi.status() != WL_CONNECTED) {
    mikrotikConnectionSuccess = false;
    return;
  }
  if (client.connected()) {
    client.stop();
    delay(50);
  }
  tc.setPromptChar('>');
  mikrotikConnectionSuccess = tc.login(mikrotikRouterIp, user.c_str(), pwd.c_str());
  if (!mikrotikConnectionSuccess) {
    Serial.println("MikroTik login failed");
    client.stop();
  } else {
    Serial.println("MikroTik login ok");
  }
}

bool ensureMikrotikConnection(bool force = false) {
  if (!networkConnected || WiFi.status() != WL_CONNECTED) {
    mikrotikConnectionSuccess = false;
    return false;
  }
  if (mikrotikConnectionSuccess && client.connected()) {
    return true;
  }
  unsigned long now = millis();
  if (!force && (now - lastMikrotikReconnectAttempt) < MIKROTIK_RECONNECT_INTERVAL) {
    mikrotikConnectionSuccess = client.connected() && mikrotikConnectionSuccess;
    return mikrotikConnectionSuccess;
  }
  lastMikrotikReconnectAttempt = now;
  loginMirotik();
  return mikrotikConnectionSuccess && client.connected();
}

bool checkIfSystemIsAvailable() {
  if (!ensureMikrotikConnection()) {
    digitalWrite(SYSTEM_READY_LED, evaluateTriggerOutput(TURN_OFF));
    char *keys[] = {"status", "errorCode"};
    char *values[] = {"false", "coin.slot.notavailable"};
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
    char *keys[] = {"status", "errorCode"};
    char *values[] = {"false", "coinslot.busy"};
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

void registerNewVoucher(const String &voucher) {
  String v = sanitizeCommandToken(voucher, 24, "");
  if (v.length() == 0) return;
  String addCoinScript = "/ip hotspot user add name=";
  addCoinScript += v;
  addCoinScript += " limit-uptime=0 comment=0";
  if (VOUCHER_LOGIN_OPTION == 1) {
    addCoinScript += " password=";
    addCoinScript += v;
  }
  String prof = sanitizeCommandToken(VOUCHER_PROFILE, 24, "");
  if (prof.length() > 0 && prof != "default") {
    addCoinScript += " profile=";
    addCoinScript += prof;
  }
  sendCommand(addCoinScript);
}

void addTimeToVoucher(const String &voucher, int secondsToAdd) {
  String v = sanitizeCommandToken(voucher, 24, "");
  if (v.length() == 0) return;
  String safeVendor = sanitizeRouterText(vendorName, 24);
  int mins = secondsToAdd / 60;
  if (mins < 0) mins = 0;

  String script = ":global lpt; :global nlu; :set lpt [/ip hotspot user get ";
  script += v;
  script += " limit-uptime]; ";
  sendCommand(script);

  script = ":set nlu [($lpt+";
  script += String(mins);
  script += "m)]; /ip hotspot user set limit-uptime=$nlu comment=\"";
  script += String(currentValidity);
  script += "m,";
  script += String(totalCoin);
  script += isNewVoucher ? ",0," : ",1,";
  script += safeVendor;
  script += "\" ";
  String rateProf = sanitizeCommandToken(currentRateProfile, 24, "");
  if (rateProf.length() > 0) {
    script += "profile=";
    script += rateProf;
    script += " ";
  }
  script += v;
  script += ";";
  sendCommand(script);

  if (currentDataLimit > 0) {
    String dl = String(currentDataLimit);
    script = ":global tdtl; :global dtl [/ip hotspot user get ";
    script += v;
    script += " limit-bytes-total];";
    sendCommand(script);
    script = ":if ($dtl>0) do={ :set tdtl [($dtl+";
    script += dl;
    script += "*1048576)] } else { :set tdtl [(";
    script += dl;
    script += "*1048576)] }; /ip hotspot user set limit-bytes-total=$tdtl ";
    script += v;
    sendCommand(script);
  }
}

void handleAdminPage() { if (!isAuthorized()) { handleNotAuthorize(); return; } handleFileRead("/admin/system-config.html"); }
void handleAdminGeneratedVoucherPage() { if (!isAuthorized()) { handleNotAuthorize(); return; } handleFileRead("/admin/voucher-generate.html"); }
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
  if (data.length() > 2048) { server.send(413, "text/plain", "too large"); return; }
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
  String data = String(upTime) + "|" + String(lifeTimeCoinCount) + "|" + String(coinCountVal) + "|" +
                String(customerCount) + "|" + String(hasInternetConnection ? "1" : "0") + "|" +
                String(mikrotikConnectionSuccess ? "1" : "0") + "|" + currentMacAddress + "|" +
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
  char *keys[] = {"status"};
  char *values[] = {"true"};
  setupCORSPolicy();
  server.send(200, "application/json", toJson(keys, values, 1));
}

void checkCoin() {
  if (!checkIfSystemIsAvailable()) return;
  String voucher = server.arg("voucher");
  if (!validateVoucher(voucher)) return;

  if (coinExpired) {
    char *keys[] = {"status", "errorCode"};
    char *values[] = {"false", "coins.wait.expired"};
    setupCORSPolicy();
    server.send(200, "application/json", toJson(keys, values, 2));
    return;
  }

  if (!acceptCoin) {
    totalCoin += processCoin;
    timeToAdd = calculateAddTime();
    char *keys[] = {"status", "newCoin", "timeAdded", "totalCoin", "validity", "data"};
    char coinStr[16]; itoa(processCoin, coinStr, 10);
    char timeToAddStr[16]; itoa(timeToAdd, timeToAddStr, 10);
    char totalCoinStr[16]; itoa(totalCoin, totalCoinStr, 10);
    char validityStr[16]; itoa(currentValidity, validityStr, 10);
    char currentDataLimitStr[16]; itoa(currentDataLimit, currentDataLimitStr, 10);
    char *values[] = {"true", coinStr, timeToAddStr, totalCoinStr, validityStr, currentDataLimitStr};
    activateCoinSlot();
    setupCORSPolicy();
    server.send(200, "application/json", toJson(keys, values, 6));
  } else {
    char *keys[] = {"status", "errorCode", "remainTime", "timeAdded", "totalCoin", "waitTime", "validity", "data"};
    char remainTimeStr[20]; long remain = (long)targetMilis - (long)millis(); itoa(remain, remainTimeStr, 10);
    char timeToAddStr[16]; itoa(timeToAdd, timeToAddStr, 10);
    char totalCoinStr[16]; itoa(totalCoin, totalCoinStr, 10);
    char waitTimeStr[16]; itoa(MAX_WAIT_COIN_SEC, waitTimeStr, 10);
    char validityStr[16]; itoa(currentValidity, validityStr, 10);
    char currentDataLimitStr[16]; itoa(currentDataLimit, currentDataLimitStr, 10);
    char *values[] = {"false", "coin.not.inserted", remainTimeStr, timeToAddStr, totalCoinStr, waitTimeStr, validityStr, currentDataLimitStr};
    setupCORSPolicy();
    server.send(200, "application/json", toJson(keys, values, 8));
  }
}

void useVoucher() {
  if (!checkIfSystemIsAvailable()) return;
  String voucher = server.arg("voucher");
  if (!validateVoucher(voucher)) return;
  if (timeToAdd > 0 && !ensureMikrotikConnection(true)) {
    char *keys[] = {"status", "errorCode"};
    char *values[] = {"false", "coin.slot.notavailable"};
    setupCORSPolicy();
    server.send(200, "application/json", toJson(keys, values, 2));
    return;
  }
  disableCoinSlot();
  if (timeToAdd > 0) {
    registerNewVoucher(currentActiveVoucher);
    updateStatistic();
    addTimeToVoucher(currentActiveVoucher, timeToAdd);
  }
  char *keys[] = {"status", "totalCoin", "timeAdded", "validity"};
  char totalCoinStr[16]; itoa(totalCoin, totalCoinStr, 10);
  char timeToAddStr[16]; itoa(timeToAdd, timeToAddStr, 10);
  char validityStr[16]; itoa(currentValidity, validityStr, 10);
  char *values[] = {"true", totalCoinStr, timeToAddStr, validityStr};
  resetGlobalVariables();
  setupCORSPolicy();
  acceptCoin = false;
  server.send(200, "application/json", toJson(keys, values, 4));
}

void topUp() {
  bool hasInternetConnection = true;
  if (CHECK_INTERNET_CONNECTION == 1) hasInternetConnection = hasInternetConnect();
  if (!hasInternetConnection) {
    char *keys[] = {"status", "errorCode"};
    char *values[] = {"false", "no.internet.detected"};
    setupCORSPolicy();
    server.send(200, "application/json", toJson(keys, values, 2));
    return;
  }
  if (!checkIfSystemIsAvailable()) return;

  String voucher = server.arg("voucher");
  if (voucher.length() > 0 && !isValidVoucherCode(voucher)) {
    char *keys[] = {"status", "errorCode"};
    char *values[] = {"false", "invalid.voucher"};
    setupCORSPolicy();
    server.send(200, "application/json", toJson(keys, values, 2));
    return;
  }

  if (currentActiveVoucher.length() > 0 && voucher.length() > 0 && voucher != currentActiveVoucher) {
    char *keys[] = {"status", "errorCode"};
    char *values[] = {"false", "coinslot.busy"};
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
  char *keys[] = {"status", "voucher"};
  char *values[] = {"true", voucherChar};
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
  if (!ensureMikrotikConnection(true)) { server.send(503, "text/plain", "MikroTik unavailable"); return; }
  int amount = clampInt(server.arg("amt").toInt(), 0, 100000);
  int qty = clampInt(server.arg("qty").toInt(), 1, 50);
  int addToSales = server.arg("sales").toInt() ? 1 : 0;
  String prefix = sanitizeCommandToken(server.arg("pfx"), 4, VOUCHER_PREFIX);
  String voucherGenerated = "";
  for (int i = 0; i < qty; i++) {
    String voucher = prefix;
    const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    for (int j = 0; j < 4; j++) {
      voucher += alphabet[ESP.random() % (sizeof(alphabet) - 1)];
    }
    totalCoin = amount;
    timeToAdd = calculateAddTime();
    registerNewVoucher(voucher);
    if (addToSales == 1) updateStatistic();
    addTimeToVoucher(voucher, timeToAdd);
    if (i > 0) voucherGenerated += "#";
    voucherGenerated += voucher;
  }
  String safeVendor = sanitizeRouterText(vendorName, 32);
  String returnData = safeVendor + "|" + String(amount) + "|" + String(timeToAdd) + "|" + voucherGenerated;
  server.send(200, "text/plain", returnData);
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
    server.on("/testInsertCoin", testInsertCoin);
    server.onNotFound(handleNotFound);
  } else {
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP("JuanFi Setup");
    dnsServer.start(DNS_PORT, "*", apIP);
    currentIpAddress = apIP.toString();
    currentMacAddress = WiFi.softAPmacAddress();
    server.onNotFound([]() {
      server.sendHeader("Location", String("/admin"), true);
      server.send(302, "text/plain", "");
    });
  }

  server.on("/admin/api/dashboard", handleAdminDashboard);
  server.on("/admin/js/jquery.min.js", handleJquerySript);
  server.on("/admin/api/resetStatistic", handleAdminResetStats);
  server.on("/admin/api/saveSystemConfig", handleAdminSaveSystemConfig);
  server.on("/admin/api/getSystemConfig", handleAdminGetSystemConfig);
  server.on("/admin/api/getRates", handleAdminGetRates);
  server.on("/admin/api/saveRates", handleAdminSaveRates);
  server.on("/admin/api/logout", handleLogout);
  server.on("/admin/api/generateVouchers", handleGenerateVouchers);
  server.on("/admin", handleAdminPage);
  server.on("/admin/viewGeneratedVouchers", handleAdminGeneratedVoucherPage);
  server.on("/admin/updateMainBin", HTTP_POST, handleFileUploadRequest, handleFileUploadStream);

  populateRates();
  server.begin();

  if (mikrotikConnectionSuccess) {
    digitalWrite(SYSTEM_READY_LED, evaluateTriggerOutput(TURN_ON));
  }
}

void loop() {
  if (networkConnected) {
    unsigned long currentMilis = millis();

    if (mikrotikConnectionSuccess && !client.connected()) {
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
          processCoin = coin;
          coin = 0;
          coinsChange = 0;
          acceptCoin = false;
          totalCoin += processCoin;
          timeToAdd = calculateAddTime();
          activateCoinSlot();
        }
      } else {
        disableCoinSlot();
        acceptCoin = false;
        coinExpired = true;
        timeToAdd = calculateAddTime();
        if (timeToAdd > 0 && currentActiveVoucher.length() > 0) {
          if (ensureMikrotikConnection(true)) {
            if (isNewVoucher) registerNewVoucher(currentActiveVoucher);
            updateStatistic();
            addTimeToVoucher(currentActiveVoucher, timeToAdd);
            resetGlobalVariables();
          } else {
            // Keep session state so portal/user can retry when router link recovers.
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
