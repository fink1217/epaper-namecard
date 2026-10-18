// ===== 使用者設定 =====
#pragma once

// ---- 1. 選擇電子紙面板（只留一個 #define）----
// 看面板背面排線上的料號或賣場標題來選；不確定就逐一試，畫面正常、不殘影即為正確。
//   PANEL_213      2.13" 122x250  SSD1680  (料號常見 GDEY0213B74 / "B74" / "V4")
//   PANEL_266      2.66" 152x296  SSD1680  (GDEY0266T90，預設，實測過的面板)
//   PANEL_290_V2   2.9"  128x296  SSD1680  (GDEY029T94 / "V2"，目前最常見)
//   PANEL_290_OLD  2.9"  128x296  IL3820   (舊款 GDEH029A1 / "V1")
//   PANEL_213_B74  2.13" 122x250  SSD1680  (GDEY0213B74 另一版驅動)
//   PANEL_213_B73  2.13" 122x250  SSD1675B (舊款 "V3")
//   PANEL_290_BS   2.9"  128x296  SSD1680  (DEPG0290BS)
//   PANEL_290_T5   2.9"  128x296  UC8151D  (GDEW029T5)
// 不確定是哪一種：先燒 firmware/panel_test，看哪個編號的畫面正確，再回來選。
// 長邊多一條像 QR 碼的亂碼 = 面板比選的型號寬（例如 2.66" 選成 2.9"），換寬一點的型號。
#define PANEL_266

// ---- 2. 腳位（ESP32-S3 SuperMini）----
// 全部接在 SuperMini 右排（USB 朝上），和模組腳位順序一一對應：
// 模組絲印      SuperMini
//   GND    ->   GND
//   VCC    ->   3V3
//   SCL    ->   GP13 (SPI SCK)
//   SDA    ->   GP12 (SPI MOSI)
//   RES    ->   GP11
//   DC     ->   GP10
//   CS     ->   GP9
//   BUSY   ->   GP8
#define EPD_SCK   13
#define EPD_MOSI  12
#define EPD_RST   11
#define EPD_DC    10
#define EPD_CS     9
#define EPD_BUSY   8

#define LED_PIN   48   // 板載 WS2812 RGB LED

// ---- 旋轉編碼器 KY-040：切換圖片 ----
// 接在 SuperMini 左排最下面 5 腳（GP3~GP7），順序和模組一樣，5 條線平行接：
// 模組絲印      SuperMini
//   CLK    ->   GP3
//   DT     ->   GP4
//   SW     ->   GP5
//   +      ->   GP6  (韌體輸出 HIGH 當 3.3V 電源，編碼器耗電 < 1 mA)
//   GND    ->   GP7  (韌體輸出 LOW 當地線)
// 用 GPIO 供電是為了不用和電子紙共用 SuperMini 唯一的 3V3/GND 腳。
// 如果你想把 + / GND 接到真正的 3V3 / GND，把 ENC_VCC_PIN / ENC_GND_PIN 設成 -1。
//   往右轉一格：下一張      往左轉一格：上一張（連轉幾格就跳幾張）
//   短按：回到第 1 張       長按 2 秒：在螢幕顯示 Wi-Fi 連線資訊
//   睡眠中按一下：喚醒並切到下一張
#define ENC_CLK_PIN  3
#define ENC_DT_PIN   4
#define ENC_SW_PIN   5
#define ENC_VCC_PIN  6
#define ENC_GND_PIN  7
#define ENC_STEPS_PER_DETENT 4   // 轉一格卻跳兩張就改成 2
#define ENC_REVERSE  0           // 轉向相反就改 1

// 板載 BOOT 鍵也能用：短按 = 下一張、長按 2 秒 = 顯示連線資訊、睡眠中按一下 = 喚醒並換圖
#define BUTTON_PIN 0

// 最多可存幾張圖（每張圖都有自己的可編輯版面）
#define MAX_PAGES  8

// ---- 3. Wi-Fi 熱點 ----
// 熱點名稱會自動加上晶片 ID 後 4 碼，例如 EPaper-Card-1A2B
#define AP_SSID_PREFIX "EPaper-Card-"
#define AP_PASSWORD    "12345678"   // 至少 8 碼；改成你自己的
#define MDNS_NAME      "epaper"     // http://epaper.local

// ---- 4. 省電 ----
// 沒有任何網頁/按鈕操作超過這麼多分鐘就進入深度睡眠（畫面保留），按旋鈕或 BOOT 鍵喚醒。
// 設 0 = 永不睡眠，Wi-Fi 一直開著，隨時都能用網頁控制（適合插 USB 長期使用）。
// 接電池隨身帶時建議改成 10，否則電池大概撐不到一天。
#define SLEEP_AFTER_MIN 0

#define FW_VERSION "2.2.0"
