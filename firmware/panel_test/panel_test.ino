/*
  電子紙面板測試 — 找出你的面板該選哪個型號，並檢查接線

  用法：
    1. 燒錄這支程式（設定同主韌體：ESP32S3 Dev Module、USB CDC On Boot = Enabled）
    2. 打開序列埠監控視窗（115200）
    3. 程式會輪流用每種驅動畫一個大數字與型號名稱，每種間隔約 10 秒，一直循環
    4. 看哪個數字「完整、清楚、位置正確」地出現，就在主韌體 config.h 選對應的型號：
         1 -> PANEL_290_V2     2 -> PANEL_213      3 -> PANEL_266      4 -> PANEL_290_OLD
         5 -> PANEL_213_B74    6 -> PANEL_213_B73  7 -> PANEL_290_BS   8 -> PANEL_290_T5
    序列埠會印出每一步與 BUSY 腳狀態；出現 "Busy Timeout" 表示面板沒回應（多半是接線或型號問題）。
*/
#include <SPI.h>
#include <GxEPD2_BW.h>

// 腳位與主韌體相同
#define EPD_SCK   13
#define EPD_MOSI  12
#define EPD_RST   11
#define EPD_DC    10
#define EPD_CS     9
#define EPD_BUSY   8

template <typename Driver>
void testPanel(int no, const char *name) {
  // 分頁緩衝（每次只畫 1/8 畫面），省記憶體
  GxEPD2_BW<Driver, Driver::HEIGHT / 8> display(Driver(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));
  Serial.printf("\n===== %d: %s =====\n", no, name);
  Serial.printf("BUSY before init = %d\n", digitalRead(EPD_BUSY));
  display.init(115200, true, 10, false);
  display.setRotation(1);
  display.setTextColor(GxEPD_BLACK);
  uint32_t t = millis();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.drawRect(0, 0, display.width(), display.height(), GxEPD_BLACK);
    display.drawRect(3, 3, display.width() - 6, display.height() - 6, GxEPD_BLACK);
    display.setTextSize(8);
    display.setCursor(12, 14);
    display.print(no);
    display.setTextSize(2);
    display.setCursor(70, 20);
    display.print(name);
    display.setTextSize(1);
    display.setCursor(70, 50);
    display.printf("%dx%d", display.width(), display.height());
  } while (display.nextPage());
  Serial.printf("refresh took %lu ms, BUSY after = %d\n", millis() - t, digitalRead(EPD_BUSY));
  display.hibernate();
  delay(10000);
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\nE-paper panel test");
  pinMode(EPD_BUSY, INPUT);
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
}

void loop() {
  testPanel<GxEPD2_290_T94_V2>(1, "2.9 V2");
  testPanel<GxEPD2_213_BN>(2, "2.13 BN");
  testPanel<GxEPD2_266_GDEY0266T90>(3, "2.66");
  testPanel<GxEPD2_290>(4, "2.9 old");
  testPanel<GxEPD2_213_B74>(5, "2.13 B74");
  testPanel<GxEPD2_213_B73>(6, "2.13 B73");
  testPanel<GxEPD2_290_BS>(7, "2.9 BS");
  testPanel<GxEPD2_290_T5>(8, "2.9 T5");
}
