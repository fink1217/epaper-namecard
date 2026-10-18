/*
  Wi-Fi 電子紙名片 — ESP32-S3 SuperMini + SPI 電子紙模組 (GND VCC SCL SDA RES DC CS BUSY)

  架構：
    排版、文字、圖片縮放、抖色(dithering)、QR Code 全部在「連上 ESP 的手機/電腦瀏覽器」裡運算，
    瀏覽器把最終的 1-bit 點陣圖送給 ESP；ESP 只負責存檔與刷新電子紙。
    最多存 MAX_PAGES 張圖，每張都有自己可再編輯的版面檔，用旋轉編碼器或 BOOT 鍵切換。

  需求：
    - Arduino IDE 2.x，開發板套件 "esp32 by Espressif Systems" 3.x
    - 程式庫：GxEPD2（會自動帶入 Adafruit GFX）
    - 開發板選 "ESP32S3 Dev Module"，設定見 README
*/
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <SPI.h>
#include <GxEPD2_BW.h>
#include <mbedtls/base64.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <driver/gpio.h>

#include "config.h"
#include "web_ui.h"

// ---------- 面板 ----------
#if defined(PANEL_213)
  #define EPD_DRIVER GxEPD2_213_BN
  #define PANEL_NAME "2.13in 122x250 SSD1680"
#elif defined(PANEL_266)
  #define EPD_DRIVER GxEPD2_266_GDEY0266T90
  #define PANEL_NAME "2.66in 152x296 SSD1680"
#elif defined(PANEL_290_V2)
  #define EPD_DRIVER GxEPD2_290_T94_V2
  #define PANEL_NAME "2.9in 128x296 SSD1680"
#elif defined(PANEL_290_OLD)
  #define EPD_DRIVER GxEPD2_290
  #define PANEL_NAME "2.9in 128x296 IL3820"
#elif defined(PANEL_213_B74)
  #define EPD_DRIVER GxEPD2_213_B74
  #define PANEL_NAME "2.13in 122x250 SSD1680 B74"
#elif defined(PANEL_213_B73)
  #define EPD_DRIVER GxEPD2_213_B73
  #define PANEL_NAME "2.13in 122x250 SSD1675B"
#elif defined(PANEL_290_BS)
  #define EPD_DRIVER GxEPD2_290_BS
  #define PANEL_NAME "2.9in 128x296 SSD1680 BS"
#elif defined(PANEL_290_T5)
  #define EPD_DRIVER GxEPD2_290_T5
  #define PANEL_NAME "2.9in 128x296 UC8151D"
#else
  #error "請在 config.h 選擇一種面板"
#endif

GxEPD2_BW<EPD_DRIVER, EPD_DRIVER::HEIGHT> display(EPD_DRIVER(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

WebServer server(80);
Preferences prefs;
String apSsid, wifiSsid, wifiPass;
uint32_t lastActivity = 0;
int curPage = 0;  // 目前螢幕上顯示的頁

// 每頁兩個檔：/pN.bin = [w lo][w hi][h lo][h hi][rot] + 點陣資料；/pN.json = 版面檔
String imgPath(int p) { return "/p" + String(p) + ".bin"; }
String layoutPath(int p) { return "/p" + String(p) + ".json"; }
static const char *LAYOUT_TMP = "/layout.tmp";
static const size_t MAX_LAYOUT = 512 * 1024;

void led(uint8_t r, uint8_t g, uint8_t b) { rgbLedWrite(LED_PIN, r, g, b); }

// ---------- 設定 ----------
void loadSettings() {
  prefs.begin("card", true);
  wifiSsid = prefs.getString("ssid", "");
  wifiPass = prefs.getString("pass", "");
  curPage = prefs.getUChar("page", 0) % MAX_PAGES;
  prefs.end();
}

void saveWifi() {
  prefs.begin("card", false);
  prefs.putString("ssid", wifiSsid);
  prefs.putString("pass", wifiPass);
  prefs.end();
}

void saveCurPage() {
  prefs.begin("card", false);
  prefs.putUChar("page", curPage);
  prefs.end();
}

// 2.0 版只有單張圖，搬到第 1 頁
void migrateOldFiles() {
  if (LittleFS.exists("/image.bin") && !LittleFS.exists(imgPath(0))) LittleFS.rename("/image.bin", imgPath(0));
  if (LittleFS.exists("/layout.json") && !LittleFS.exists(layoutPath(0))) LittleFS.rename("/layout.json", layoutPath(0));
}

bool pageValid(int p) { return p >= 0 && p < MAX_PAGES; }
bool pageHasImage(int p) { return pageValid(p) && LittleFS.exists(imgPath(p)); }
bool anyImage() {
  for (int p = 0; p < MAX_PAGES; p++)
    if (pageHasImage(p)) return true;
  return false;
}

// ---------- 顯示 ----------
// bits：每列 (w+7)/8 bytes，MSB 在左，1 = 黑
void showBitmap(const uint8_t *bits, int w, int h, uint8_t rot, bool fast) {
  display.setRotation(rot);
  display.setFullWindow();
  display.fillScreen(GxEPD_WHITE);
  display.drawBitmap(0, 0, bits, w, h, GxEPD_BLACK);
  display.display(fast);  // fast = 局部刷新（快、可能殘影）；否則全刷
  display.hibernate();
}

bool showPage(int p) {
  File f = LittleFS.open(imgPath(p), "r");
  if (!f || f.size() < 6) return false;
  size_t n = f.size();
  uint8_t *buf = (uint8_t *)malloc(n);
  if (!buf) { f.close(); return false; }
  f.read(buf, n);
  f.close();
  int w = buf[0] | (buf[1] << 8), h = buf[2] | (buf[3] << 8);
  bool ok = n == 5 + (size_t)((w + 7) / 8) * h;
  if (ok) {
    showBitmap(buf + 5, w, h, buf[4] & 3, false);
    curPage = p;
    saveCurPage();
  }
  free(buf);
  return ok;
}

// 在「有圖的頁」之間前進/後退 n 張（循環）
bool stepPages(int n) {
  int list[MAX_PAGES], cnt = 0, idx = -1;
  for (int p = 0; p < MAX_PAGES; p++)
    if (pageHasImage(p)) {
      if (p == curPage) idx = cnt;
      list[cnt++] = p;
    }
  if (cnt == 0) return false;
  if (idx < 0) idx = n > 0 ? -1 : 0;  // 目前頁沒圖：往後從第一張開始、往前從最後一張開始
  int target = list[(((idx + n) % cnt) + cnt) % cnt];
  return target == curPage ? true : showPage(target);
}
bool showNextPage() { return stepPages(1); }
bool showFirstPage() {
  for (int p = 0; p < MAX_PAGES; p++)
    if (pageHasImage(p)) return p == curPage ? true : showPage(p);
  return false;
}

void renderNetInfo() {
  display.setRotation(1);
  display.setFullWindow();
  display.fillScreen(GxEPD_WHITE);
  display.setTextColor(GxEPD_BLACK);
  display.setTextSize(2);
  display.setCursor(6, 6);
  display.print("E-Paper Card");
  display.setTextSize(1);
  int y = 34;
  auto line = [&](const String &s) { display.setCursor(6, y); display.print(s); y += 14; };
  line("WiFi: " + apSsid);
  line("Pass: " AP_PASSWORD);
  line("Open: http://" + WiFi.softAPIP().toString());
  if (WiFi.status() == WL_CONNECTED) {
    line(wifiSsid + ": http://" + WiFi.localIP().toString());
    line("or http://" MDNS_NAME ".local");
  }
  display.display(false);
  display.hibernate();
}

// ---------- 網頁伺服器 ----------
String jsonEsc(const String &s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += c;
  }
  return o;
}

void ok() { server.send(200, "text/plain", "ok"); }

// 取得 ?page=，沒帶就用目前頁；超出範圍回 -1
int argPage() {
  if (!server.hasArg("page")) return curPage;
  int p = server.arg("page").toInt();
  return pageValid(p) ? p : -1;
}

void handleInfo() {
  String j = "{";
  auto str = [&](const char *k, const String &v) { j += "\""; j += k; j += "\":\""; j += jsonEsc(v); j += "\","; };
  str("panel", PANEL_NAME);
  str("ver", FW_VERSION);
  str("ap", apSsid);
  str("apip", WiFi.softAPIP().toString());
  str("ssid", wifiSsid);
  str("staip", WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String());
  j += "\"w\":" + String(EPD_DRIVER::WIDTH) + ",\"h\":" + String(EPD_DRIVER::HEIGHT);
  j += ",\"sleepMin\":" + String(SLEEP_AFTER_MIN);
  j += ",\"button\":" + String(BUTTON_PIN) + ",\"encoder\":[" + String(ENC_CLK_PIN) + "," + String(ENC_DT_PIN) + "," + String(ENC_SW_PIN) + "]";
  j += ",\"cur\":" + String(curPage) + ",\"pages\":[";
  for (int p = 0; p < MAX_PAGES; p++) {
    if (p) j += ",";
    j += pageHasImage(p) ? "1" : "0";
  }
  j += "],\"fsFree\":" + String(LittleFS.totalBytes() - LittleFS.usedBytes()) + "}";
  server.send(200, "application/json", j);
}

// POST /api/image?page=&w=&h=&rot=&fast=   body = base64(1-bit 點陣)
void handleImage() {
  int page = argPage();
  if (page < 0) { server.send(400, "text/plain", "bad page"); return; }
  int w = server.arg("w").toInt(), h = server.arg("h").toInt();
  uint8_t rot = server.arg("rot").toInt() & 3;
  bool fast = server.arg("fast") == "1";
  int ew = (rot & 1) ? EPD_DRIVER::HEIGHT : EPD_DRIVER::WIDTH;
  int eh = (rot & 1) ? EPD_DRIVER::WIDTH : EPD_DRIVER::HEIGHT;
  if (w != ew || h != eh) {
    server.send(400, "text/plain", "size mismatch: expect " + String(ew) + "x" + String(eh));
    return;
  }
  const String &b64 = server.arg("plain");
  size_t need = (size_t)((w + 7) / 8) * h, len = 0;
  uint8_t *buf = (uint8_t *)malloc(need + 8);
  if (!buf) { server.send(500, "text/plain", "no memory"); return; }
  int rc = mbedtls_base64_decode(buf + 5, need + 3, &len, (const uint8_t *)b64.c_str(), b64.length());
  if (rc != 0 || len != need) {
    free(buf);
    server.send(400, "text/plain", "bad bitmap");
    return;
  }
  buf[0] = w & 0xFF; buf[1] = w >> 8; buf[2] = h & 0xFF; buf[3] = h >> 8; buf[4] = rot;
  File f = LittleFS.open(imgPath(page), "w");
  if (f) { f.write(buf, need + 5); f.close(); }
  showBitmap(buf + 5, w, h, rot, fast);
  free(buf);
  curPage = page;
  saveCurPage();
  ok();
}

void handleLayoutGet() {
  int page = argPage();
  File f = page < 0 ? File() : LittleFS.open(layoutPath(page), "r");
  if (!f) { server.send(404, "text/plain", "no layout"); return; }
  server.streamFile(f, "application/json");
  f.close();
}

// 版面檔用 multipart 上傳，邊收邊寫入 LittleFS，不佔 RAM
File layoutUpload;
size_t layoutSize = 0;
void handleLayoutUpload() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    layoutSize = 0;
    layoutUpload = LittleFS.open(LAYOUT_TMP, "w");
  } else if (up.status == UPLOAD_FILE_WRITE) {
    layoutSize += up.currentSize;
    if (layoutUpload && layoutSize <= MAX_LAYOUT) layoutUpload.write(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END || up.status == UPLOAD_FILE_ABORTED) {
    if (layoutUpload) layoutUpload.close();
  }
}

void handleLayoutDone() {
  int page = argPage();
  if (page < 0 || layoutSize == 0 || layoutSize > MAX_LAYOUT || !LittleFS.exists(LAYOUT_TMP)) {
    LittleFS.remove(LAYOUT_TMP);
    server.send(400, "text/plain", layoutSize > MAX_LAYOUT ? "layout too large (max 512KB)" : "upload failed");
    return;
  }
  LittleFS.remove(layoutPath(page));
  LittleFS.rename(LAYOUT_TMP, layoutPath(page));
  ok();
}

void handleShow() {
  int page = argPage();
  if (page < 0 || !pageHasImage(page)) { server.send(404, "text/plain", "no image on this page"); return; }
  showPage(page) ? ok() : server.send(500, "text/plain", "read failed");
}

void handleDelete() {
  int page = argPage();
  if (page < 0) { server.send(400, "text/plain", "bad page"); return; }
  LittleFS.remove(imgPath(page));
  LittleFS.remove(layoutPath(page));
  if (page == curPage) {
    if (!showNextPage()) renderNetInfo();
  }
  ok();
}

void connectSta() {
  if (wifiSsid.isEmpty()) return;
  WiFi.disconnect();
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
}

void handleWifi() {
  wifiSsid = server.arg("ssid");
  wifiSsid.trim();
  if (server.arg("pass").length()) wifiPass = server.arg("pass");
  if (wifiSsid.isEmpty()) wifiPass = "";
  saveWifi();
  ok();
  delay(100);
  if (wifiSsid.isEmpty()) WiFi.disconnect(); else connectSta();
}

// 所有路由都經過這裡，順便更新「最後操作時間」
void route(const char *path, HTTPMethod m, std::function<void()> fn) {
  server.on(path, m, [fn] { lastActivity = millis(); fn(); });
}

void setupServer() {
  route("/", HTTP_GET, [] {
    server.sendHeader("Content-Encoding", "gzip");
    server.send_P(200, "text/html; charset=utf-8", (const char *)INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
  });
  route("/api/info", HTTP_GET, handleInfo);
  route("/api/image", HTTP_POST, handleImage);
  route("/api/layout", HTTP_GET, handleLayoutGet);
  server.on("/api/layout", HTTP_POST, [] { lastActivity = millis(); handleLayoutDone(); }, [] {
    lastActivity = millis();
    handleLayoutUpload();
  });
  route("/api/show", HTTP_POST, handleShow);
  route("/api/next", HTTP_POST, [] { showNextPage() ? ok() : server.send(404, "text/plain", "no image"); });
  route("/api/delete", HTTP_POST, handleDelete);
  route("/api/wifi", HTTP_POST, handleWifi);
  route("/api/netinfo", HTTP_POST, [] { renderNetInfo(); ok(); });
  route("/api/redraw", HTTP_POST, [] { showPage(curPage) ? ok() : server.send(404, "text/plain", "no image"); });
  route("/api/clear", HTTP_POST, [] {
    display.setFullWindow();
    display.fillScreen(GxEPD_WHITE);
    display.display(false);
    display.hibernate();
    ok();
  });
  route("/api/reboot", HTTP_POST, [] {
    ok();
    delay(300);
    ESP.restart();
  });
  server.onNotFound([] {  // 熱點連線時，手機開任何網址都導回首頁
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/");
    server.send(302, "text/plain", "");
  });
  server.begin();
}

// ---------- 按鈕 / 旋轉編碼器 ----------
struct Button {
  int pin;
  bool down = false, waitRelease = false;
  uint32_t downAt = 0;

  // 回傳 0 = 沒事、1 = 短按、2 = 長按（按住 2 秒時立即觸發）。放開時才判斷短按，避免長按途中誤觸。
  int poll() {
    bool pressed = digitalRead(pin) == LOW;
    if (waitRelease) {
      if (!pressed) waitRelease = false;
      return 0;
    }
    if (pressed && !down) {
      down = true;
      downAt = millis();
    } else if (pressed && down && millis() - downAt > 2000) {
      down = false;
      waitRelease = true;
      return 2;
    } else if (!pressed && down) {
      down = false;
      uint32_t held = millis() - downAt;
      if (held > 40 && held < 1000) return 1;  // 40ms 去彈跳
    }
    return 0;
  }
};
Button bootBtn{BUTTON_PIN}, encBtn{ENC_SW_PIN};

// 編碼器用中斷計數（格雷碼查表），主迴圈停止轉動 250ms 後才一次換圖，連轉幾格就跳幾張
volatile int32_t encRaw = 0;
volatile uint8_t encState = 0;
void IRAM_ATTR encISR() {
  static const int8_t T[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
  encState = ((encState << 2) | (gpio_get_level((gpio_num_t)ENC_CLK_PIN) << 1) | gpio_get_level((gpio_num_t)ENC_DT_PIN)) & 0x0F;
  encRaw = encRaw + T[encState];
}

void pollEncoder() {
  static int32_t used = 0, lastRaw = 0;
  static uint32_t lastMove = 0;
  int32_t raw = encRaw;
  if (raw != lastRaw) {
    lastRaw = raw;
    lastMove = millis();
    lastActivity = millis();
  }
  int32_t det = (raw - used) / ENC_STEPS_PER_DETENT;
  if (det != 0 && millis() - lastMove > 250) {
    used += det * ENC_STEPS_PER_DETENT;
    led(0, 8, 0);
    stepPages(ENC_REVERSE ? -det : det);
    led(0, 0, 8);
  }
}

void showNetInfoFromButton() {
  led(8, 0, 8);
  renderNetInfo();
  led(0, 0, 8);
}

void pollInputs() {
  pollEncoder();
  int b = encBtn.poll();
  if (b) {
    lastActivity = millis();
    if (b == 1) { led(0, 8, 0); showFirstPage(); led(0, 0, 8); }
    else showNetInfoFromButton();
  }
  b = bootBtn.poll();
  if (b) {
    lastActivity = millis();
    if (b == 1) { led(0, 8, 0); showNextPage(); led(0, 0, 8); }
    else showNetInfoFromButton();
  }
}

void setupInputs(bool fromSleep) {
  if (fromSleep) {  // 睡眠時這些腳切成 RTC 腳，改回一般 GPIO
    rtc_gpio_deinit((gpio_num_t)BUTTON_PIN);
    rtc_gpio_deinit((gpio_num_t)ENC_SW_PIN);
#if ENC_GND_PIN >= 0
    rtc_gpio_hold_dis((gpio_num_t)ENC_GND_PIN);
    rtc_gpio_deinit((gpio_num_t)ENC_GND_PIN);
#endif
  }
#if ENC_VCC_PIN >= 0
  pinMode(ENC_VCC_PIN, OUTPUT);
  digitalWrite(ENC_VCC_PIN, HIGH);
#endif
#if ENC_GND_PIN >= 0
  pinMode(ENC_GND_PIN, OUTPUT);
  digitalWrite(ENC_GND_PIN, LOW);
#endif
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(ENC_SW_PIN, INPUT_PULLUP);
  pinMode(ENC_CLK_PIN, INPUT_PULLUP);
  pinMode(ENC_DT_PIN, INPUT_PULLUP);
  delay(2);
  encState = (digitalRead(ENC_CLK_PIN) << 1) | digitalRead(ENC_DT_PIN);
  attachInterrupt(ENC_CLK_PIN, encISR, CHANGE);
  attachInterrupt(ENC_DT_PIN, encISR, CHANGE);
  // 開機/喚醒時按鈕還按著：等放開後才開始偵測
  bootBtn.waitRelease = digitalRead(BUTTON_PIN) == LOW;
  encBtn.waitRelease = digitalRead(ENC_SW_PIN) == LOW;
}

// ---------- 省電 ----------
void goToSleep() {
  Serial.println("idle, deep sleep (press BOOT or the knob to wake)");
  led(0, 0, 0);
  display.hibernate();
  WiFi.mode(WIFI_OFF);
#if ENC_GND_PIN >= 0
  // 睡眠中編碼器的 GND 腳要維持 LOW，按下旋鈕才能喚醒
  rtc_gpio_init((gpio_num_t)ENC_GND_PIN);
  rtc_gpio_set_direction((gpio_num_t)ENC_GND_PIN, RTC_GPIO_MODE_OUTPUT_ONLY);
  rtc_gpio_set_level((gpio_num_t)ENC_GND_PIN, 0);
  rtc_gpio_hold_en((gpio_num_t)ENC_GND_PIN);
#endif
  // BOOT 鍵或旋鈕按下（任一變 LOW）都能喚醒；RTC 周邊保持供電，內部上拉才有效
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  esp_sleep_enable_ext1_wakeup((1ULL << BUTTON_PIN) | (1ULL << ENC_SW_PIN), ESP_EXT1_WAKEUP_ANY_LOW);
  for (int pin : {BUTTON_PIN, ENC_SW_PIN}) {
    rtc_gpio_pullup_en((gpio_num_t)pin);
    rtc_gpio_pulldown_dis((gpio_num_t)pin);
  }
  esp_deep_sleep_start();
}

// ---------- 主程式 ----------
void setup() {
  Serial.begin(115200);
  led(0, 0, 8);
  bool fromSleep = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1;
  setupInputs(fromSleep);
  loadSettings();
  if (!LittleFS.begin(true)) Serial.println("LittleFS mount failed");
  migrateOldFiles();

  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  display.init(115200, !fromSleep, 10, false);

  // 按按鈕喚醒 = 想看下一張，先換圖再開 Wi-Fi，反應比較快
  if (fromSleep) showNextPage();

  char id[5];
  snprintf(id, sizeof id, "%04X", (uint16_t)(ESP.getEfuseMac() >> 32));
  apSsid = String(AP_SSID_PREFIX) + id;

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apSsid.c_str(), AP_PASSWORD);
  connectSta();
  MDNS.begin(MDNS_NAME);
  MDNS.addService("http", "tcp", 80);
  setupServer();
  Serial.printf("AP: %s  http://%s\n", apSsid.c_str(), WiFi.softAPIP().toString().c_str());

  // 電子紙斷電仍保留畫面，所以有存過圖就不重刷；第一次使用才印出連線資訊
  if (!anyImage()) {
    if (wifiSsid.length())
      for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) delay(250);
    renderNetInfo();
  }
  lastActivity = millis();
}

void loop() {
  server.handleClient();
  pollInputs();

  static uint32_t lastTry = 0;  // 斷線時每 30 秒重試
  if (wifiSsid.length() && WiFi.status() != WL_CONNECTED && millis() - lastTry > 30000) {
    lastTry = millis();
    WiFi.reconnect();
  }

  // 有手機連著熱點就不睡
  if (WiFi.softAPgetStationNum() > 0) lastActivity = millis();
  if (SLEEP_AFTER_MIN > 0 && millis() - lastActivity > SLEEP_AFTER_MIN * 60000UL) goToSleep();
  delay(2);
}
