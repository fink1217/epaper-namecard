/*
  電子紙接線探針 — 不用任何程式庫，用 GPIO 慢速模擬 SPI，逐段確認問題在哪：
    1. BUSY 有沒有被驅動          → 模組有沒有吃到電、BUSY 線有沒有接上
    2. 下指令後 BUSY 有沒有反應    → SCL/SDA/CS/DC/RES 通不通、控制器是哪一家
    3. 直接下指令全刷一次（一半黑、一半白）→ 面板與排線本身正不正常

  用法：設定同主韌體（ESP32S3 Dev Module、USB CDC On Boot = Enabled），
  燒錄後開序列埠監控 115200，每 20 秒重跑一輪，看「結論」那幾行。
  這支只檢查接線與控制器，不判斷面板尺寸；接線正常後再用 panel_test 找型號。
*/
#include <Arduino.h>

// 腳位與主韌體相同
#define EPD_SCK   13
#define EPD_MOSI  12
#define EPD_RST   11
#define EPD_DC    10
#define EPD_CS     9
#define EPD_BUSY   8

// SSD16xx：寫滿 SSD1680 整塊 RAM（176x296），不管面板實際多寬都不會留下沒寫到的亂碼
#define SSD_W 176
#define SSD_H 296
#define SSD_BYTES (SSD_W / 8 * SSD_H)
// UC81xx：解析度要明確設定，用最常見的 2.9" 128x296
#define UC_W 128
#define UC_H 296
#define UC_BYTES (UC_W / 8 * UC_H)

// ---------- 慢速軟體 SPI（模式 0、MSB 先送），排除硬體 SPI 設定與杜邦線品質的干擾 ----------
void epdWrite(bool isData, uint8_t b) {
  digitalWrite(EPD_DC, isData);
  digitalWrite(EPD_CS, LOW);
  for (int i = 7; i >= 0; i--) {
    digitalWrite(EPD_MOSI, (b >> i) & 1);
    delayMicroseconds(1);
    digitalWrite(EPD_SCK, HIGH);
    delayMicroseconds(1);
    digitalWrite(EPD_SCK, LOW);
  }
  digitalWrite(EPD_CS, HIGH);
}
void epdCmd(uint8_t c) { epdWrite(false, c); }
void epdData(uint8_t d) { epdWrite(true, d); }

// 前一半的列黑（0x00）、後一半白（0xFF）
void epdHalfBlack(int bytes) {
  for (int i = 0; i < bytes; i++) epdData(i < bytes / 2 ? 0x00 : 0xFF);
}

void hwReset() {
  digitalWrite(EPD_RST, LOW);
  delay(10);
  digitalWrite(EPD_RST, HIGH);
}

// ---------- 觀察 BUSY ----------
// BUSY 會跟著內部上拉/下拉跑 = 沒有人在驅動這條線
bool busyFloating() {
  pinMode(EPD_BUSY, INPUT_PULLUP);
  delay(5);
  int up = digitalRead(EPD_BUSY);
  pinMode(EPD_BUSY, INPUT_PULLDOWN);
  delay(5);
  int down = digitalRead(EPD_BUSY);
  pinMode(EPD_BUSY, INPUT);
  return up == 1 && down == 0;
}

// 盯著 BUSY ms 毫秒，印出前 8 次變化的時間點；回傳變化次數
int watchBusy(const char *what, uint32_t ms) {
  uint32_t at[8];
  int n = 0, start = digitalRead(EPD_BUSY), last = start;
  uint32_t t0 = micros();
  while (micros() - t0 < ms * 1000) {
    int v = digitalRead(EPD_BUSY);
    if (v == last) continue;
    if (n < 8) at[n] = micros() - t0;
    n++;
    last = v;
  }
  Serial.printf("  %s：BUSY 起始 = %d", what, start);
  for (int i = 0; i < n && i < 8; i++) Serial.printf("，%lu us 變 %d", at[i], (start + i + 1) & 1);
  Serial.printf("（共變化 %d 次）\n", n);
  return n;
}

// 等 BUSY 回到閒置電位，回傳花了幾毫秒；逾時回傳 -1
long waitIdle(int idle, uint32_t timeoutMs) {
  delay(1);
  uint32_t t0 = millis();
  while (digitalRead(EPD_BUSY) != idle)
    if (millis() - t0 > timeoutMs) return -1;
  return millis() - t0;
}

// ---------- 直接下指令全刷 ----------
// SSD16xx（SSD1680 / SSD1675 / IL3820…）：BUSY 高 = 忙
long refreshSSD() {
  hwReset();
  delay(10);
  epdCmd(0x12);  // 軟體重置
  waitIdle(LOW, 1000);
  epdCmd(0x11); epdData(0x03);  // 資料方向：X、Y 都遞增
  epdCmd(0x44); epdData(0x00); epdData(SSD_W / 8 - 1);
  epdCmd(0x45); epdData(0x00); epdData(0x00); epdData((SSD_H - 1) & 0xFF); epdData((SSD_H - 1) >> 8);
  for (int ram : {0x24, 0x26}) {  // 黑白 RAM 與第二塊 RAM 寫一樣的內容（三色面板會讓白的那半變紅）
    epdCmd(0x4E); epdData(0x00);
    epdCmd(0x4F); epdData(0x00); epdData(0x00);
    epdCmd(ram);
    epdHalfBlack(SSD_BYTES);
  }
  epdCmd(0x22); epdData(0xF7);  // 全刷，波形用面板內建（OTP）
  epdCmd(0x20);
  long t = waitIdle(LOW, 10000);
  epdCmd(0x10); epdData(0x01);  // 深度睡眠
  return t;
}

// UC81xx（UC8151 / IL0373…）：BUSY 低 = 忙
long refreshUC() {
  hwReset();
  delay(10);
  epdCmd(0x00); epdData(0x1F);  // 黑白模式，波形用 OTP
  epdCmd(0x61); epdData(UC_W); epdData(UC_H >> 8); epdData(UC_H & 0xFF);
  epdCmd(0x50); epdData(0x97);
  epdCmd(0x04);  // 開高壓電源
  waitIdle(HIGH, 1000);
  epdCmd(0x10);  // 舊畫面：全白
  for (int i = 0; i < UC_BYTES; i++) epdData(0xFF);
  epdCmd(0x13);  // 新畫面
  epdHalfBlack(UC_BYTES);
  epdCmd(0x12);  // 刷新
  long t = waitIdle(HIGH, 10000);
  epdCmd(0x02);  // 關高壓電源
  return t;
}

// ---------- 結論 ----------
void reportRefresh(long t) {
  if (t < 0) {
    Serial.println("  刷新逾時：控制器開始刷新後一直忙碌，面板高壓電路或排線可能有問題");
    return;
  }
  Serial.printf("  刷新花了 %ld ms%s\n", t, t < 300 ? "（太快，面板沒有真的刷新）" : "");
  Serial.println("  請看畫面：");
  Serial.println("    一半黑、一半白 → 面板與接線都正常，接著燒 panel_test 找出正確型號");
  Serial.println("    有一半變紅/黃  → 這是三色面板，要改用三色驅動");
  Serial.println("    完全沒變       → 控制器正常但面板沒動：檢查面板排線（FPC）有沒有插好、卡扣有沒有壓下");
}

void runProbe() {
  Serial.println("\n========== 電子紙接線探針 ==========");
  Serial.printf("  開機前 BUSY = %d\n", digitalRead(EPD_BUSY));
  hwReset();
  watchBusy("RES 重置後", 200);
  bool floating = busyFloating();
  int idle = digitalRead(EPD_BUSY);
  Serial.printf("  BUSY 浮接測試：%s，閒置電位 = %d\n", floating ? "會跟著上拉/下拉跑（沒人驅動）" : "有被驅動", idle);

  epdCmd(0x12);  // SSD16xx：軟體重置，BUSY 會短暫變高
  int nSsd = watchBusy("送 0x12（SSD16xx 軟體重置）後", 200);
  hwReset();
  delay(20);
  epdCmd(0x04);  // UC81xx：開電源，BUSY 會短暫變低
  int nUc = watchBusy("送 0x04（UC81xx 開電源）後", 1000);

  Serial.println("---------- 結論 ----------");
  if (floating) {
    Serial.println("  BUSY 沒有被驅動：模組沒吃到電（VCC→3V3、GND→GND），或 BUSY 沒接到 GP8");
  } else if (nSsd + nUc == 0) {
    Serial.println("  模組有電，但對指令完全沒反應：逐條檢查 SCL→13、SDA→12、CS→9、DC→10、RES→11");
    Serial.println("  模組上若有 BS / 3-wire / 4-wire 的開關或電阻，要設成 4-wire");
  } else if (idle == LOW) {
    Serial.println("  SSD16xx 控制器有回應，SPI 接線正常。現在直接全刷一次…");
    reportRefresh(refreshSSD());
  } else {
    Serial.println("  UC81xx 控制器有回應，SPI 接線正常。現在直接全刷一次…");
    reportRefresh(refreshUC());
  }
}

void setup() {
  Serial.begin(115200);
  for (uint32_t t = millis(); !Serial && millis() - t < 5000;) delay(10);  // 等序列埠連上
  pinMode(EPD_CS, OUTPUT);
  digitalWrite(EPD_CS, HIGH);
  pinMode(EPD_DC, OUTPUT);
  pinMode(EPD_RST, OUTPUT);
  digitalWrite(EPD_RST, HIGH);
  pinMode(EPD_SCK, OUTPUT);
  digitalWrite(EPD_SCK, LOW);
  pinMode(EPD_MOSI, OUTPUT);
  pinMode(EPD_BUSY, INPUT);
}

void loop() {
  runProbe();
  delay(20000);
}
