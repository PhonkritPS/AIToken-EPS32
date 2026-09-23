#include <TFT_eSPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h> // ติดตั้งผ่าน Arduino Library Manager (ArduinoJson by Benoit Blanchon)

TFT_eSPI tft = TFT_eSPI();

// Wi-Fi Config
const char* ssid = "H101-2.4GHz";
const char* password = "0817115775";

// ตั้งค่า IP ของเครื่องคอมพิวเตอร์ที่รัน bridge_server.js
const char* apiUrl = "http://192.168.10.99:5000/api/quota";

// กำหนดรอบการดึงข้อมูล (ทุกๆ 15 วินาที)
const unsigned long refreshInterval = 15000;
unsigned long lastFetchTime = 0;

// แฟลกสำหรับแยกว่าเป็นการวาดครั้งแรก (full draw) หรืออัปเดตบางส่วน (partial update)
bool isFirstDraw = true;
bool lastWiFiConnected = false;

// =========================================================================
// Layout หน้าเดียว 320x240: 3 ส่วน (Gemini / Claude & GPT / Claude Code)
// แต่ละส่วนมีการ์ดครึ่งจอ 2 ใบ ซ้าย = Weekly, ขวา = 5 Hour
// ใต้ส่วน Claude Code มีการ์ด Token อีก 2 ใบ ซ้าย = วันนี้, ขวา = 5 ชม. ปัจจุบัน
// =========================================================================
const int CARD_W = 154;
const int CARD_H = 42;
const int COL_X[2] = { 4, 162 };
const int SECTION_Y[3] = { 5, 66, 127 };   // ตำแหน่งหัวข้อของแต่ละส่วน (การ์ดอยู่ต่ำลงมา 12px)
const int TOKEN_Y = 185;

// พื้นหลังแยกกลุ่ม: 0 = Antigravity (Gemini + Claude & GPT), 1 = Claude Code
const uint16_t PANEL_BG[2] = { 0x08C5, 0x28A1 };      // กรมท่าเข้ม / น้ำตาลอมส้มเข้ม
const uint16_t PANEL_BORDER[2] = { 0x4B0D, 0x6A06 };  // กรอบการ์ด: เทาฟ้า / ส้มอิฐ
const char* PANEL_TAG[2] = { "ANTIGRAVITY", "CLAUDE CODE" };
const int PANEL_Y[2] = { 1, 123 };
const int PANEL_H[2] = { 121, 116 };
int sectionGroup(int s) { return s < 2 ? 0 : 1; }
const uint16_t SECTION_COLOR[3] = { 0x443E, 0xA45F, 0xDBAA }; // ฟ้า Gemini / ม่วง / ส้ม Claude
const char* SECTION_TITLE[3] = { "Gemini Models", "Claude & GPT", "Plan Usage" };

// การ์ด % โควต้า 6 ใบ: [ส่วน * 2 + คอลัมน์]
const int PCT_CARDS = 6;
const char* PCT_KEY[PCT_CARDS] = { "geminiWeekly", "gemini5Hr", "claudeWeekly", "claude5Hr", "ccWeekly", "cc5Hr" };
int pctValue[PCT_CARDS];
String pctReset[PCT_CARDS];
int lastPctValue[PCT_CARDS] = { -1, -1, -1, -1, -1, -1 };
String lastPctReset[PCT_CARDS];

// การ์ด Token 2 ใบ
const int TOKEN_CARDS = 2;
const char* TOKEN_KEY[TOKEN_CARDS] = { "ccToday", "ccWindow" };
const char* TOKEN_LABEL[TOKEN_CARDS] = { "Day Tokens", "5H Tokens" };
String tokenTotal[TOKEN_CARDS], tokenIn[TOKEN_CARDS], tokenOut[TOKEN_CARDS], tokenCache[TOKEN_CARDS];
String lastTokenTotal[TOKEN_CARDS], lastTokenDetail[TOKEN_CARDS];

// ประกาศฟังก์ชันล่วงหน้า
void drawWiFiIcon(int x, int y, bool connected, uint16_t bg);
void drawProgressRing(int cx, int cy, int r, int thickness, int percent, uint16_t color, uint16_t bg);
void drawDashboardFull();
void updateDashboardValues();
void fetchAndDisplayQuota();

void setup() {
  Serial.begin(115200);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  delay(500);

  String mac = WiFi.macAddress();

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(2);
  tft.drawString("Connecting to Wi-Fi...", tft.width() / 2, (tft.height() / 2) - 20);

  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextSize(2);
  tft.drawString("MAC: " + mac, tft.width() / 2, (tft.height() / 2) + 15);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  // ดึงข้อมูลครั้งแรกทันทีที่ต่อเน็ตสำเร็จ
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected! IP: " + WiFi.localIP().toString());
    fetchAndDisplayQuota();
  } else {
    for (int i = 0; i < PCT_CARDS; i++) pctValue[i] = 0;
    drawDashboardFull();
  }
}

void loop() {
  // ดึงข้อมูลอัตโนมัติตามรอบเวลาที่กำหนด
  if (millis() - lastFetchTime >= refreshInterval) {
    lastFetchTime = millis();
    fetchAndDisplayQuota();
  }
}

// =========================================================================
// ฟังก์ชันยิง HTTP GET ไปดึงข้อมูล Quota จาก Bridge Server
// =========================================================================
void fetchAndDisplayQuota() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected. Retrying connection...");
    WiFi.reconnect();
    if (lastWiFiConnected) {
      drawWiFiIcon(294, 3, false, PANEL_BG[0]);
      lastWiFiConnected = false;
    }
    return;
  }

  HTTPClient http;
  http.begin(apiUrl);
  http.setTimeout(4000); // 4 วินาที timeout

  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    Serial.println("Received payload: " + payload);

    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, payload);

    if (!error) {
      for (int i = 0; i < PCT_CARDS; i++) {
        pctValue[i] = doc[PCT_KEY[i]] | 100;
        pctReset[i] = doc[String(PCT_KEY[i]) + "Reset"] | "";
      }
      for (int i = 0; i < TOKEN_CARDS; i++) {
        String key = TOKEN_KEY[i];
        tokenTotal[i] = doc[key + "Tokens"] | "-";
        tokenIn[i] = doc[key + "In"] | "-";
        tokenOut[i] = doc[key + "Out"] | "-";
        tokenCache[i] = doc[key + "Cache"] | "-";
      }

      if (isFirstDraw) {
        // ครั้งแรก: วาดหน้าจอทั้งหมด รวมถึงหัวข้อและกรอบการ์ด
        drawDashboardFull();
        isFirstDraw = false;
      } else {
        // ครั้งต่อไป: อัปเดตเฉพาะตัวเลข, เวลา reset, วงกลม และไอคอน WiFi (เฉพาะส่วนที่เปลี่ยน)
        updateDashboardValues();
      }
    } else {
      Serial.print("JSON Parse error: ");
      Serial.println(error.c_str());
    }
  } else {
    Serial.printf("HTTP GET failed, error: %s (code: %d)\n", http.errorToString(httpCode).c_str(), httpCode);
  }

  http.end();
}

// =========================================================================
// ฟังก์ชันวาดไอคอน Wi-Fi ขนาดเล็ก (ประมาณ 20x14 px อยู่บนแถวหัวข้อ)
// =========================================================================
void drawWiFiIcon(int x, int y, bool connected, uint16_t bg) {
  uint16_t iconColor = connected ? TFT_LIGHTGREY : TFT_DARKGREY;
  int cx = x + 10;
  int cy = y + 12;

  tft.fillRect(x, y, 21, 14, bg);

  tft.drawCircle(cx, cy, 9, iconColor);
  tft.drawCircle(cx, cy, 8, iconColor);
  tft.drawCircle(cx, cy, 5, iconColor);
  tft.drawCircle(cx, cy, 4, iconColor);

  // ตัดให้เหลือเฉพาะส่วนโค้งด้านบน 90 องศา
  tft.fillTriangle(cx, cy, cx - 10, cy, cx - 10, cy - 10, bg);
  tft.fillTriangle(cx, cy, cx + 10, cy, cx + 10, cy - 10, bg);
  tft.fillRect(cx - 10, cy + 1, 21, 9, bg);

  tft.fillCircle(cx, cy - 1, 1, iconColor);

  if (!connected) {
    tft.drawLine(x + 3, y + 1, x + 17, y + 13, TFT_RED);
    tft.drawLine(x + 17, y + 1, x + 3, y + 13, TFT_RED);
  }
}

// =========================================================================
// ฟังก์ชันวาดวงแหวน Circular Progress Ring ตามเปอร์เซ็นต์จริง (0 - 100%)
// =========================================================================
void drawProgressRing(int cx, int cy, int r, int thickness, int percent, uint16_t color, uint16_t bg) {
  // ล้างพื้นที่วงกลม
  tft.fillCircle(cx, cy, r + 1, bg);

  // 1. วาดวงแหวนพื้นหลังสีเทาเข้ม (Track แสดงพื้นที่ 100%)
  uint16_t trackColor = 0x3186; // เทาเข้มโปร่งๆ เหมือนใน IDE
  for (float angle = 0; angle < 360.0f; angle += 0.5f) {
    float rad = angle * 0.0174532925f; // DEG_TO_RAD
    float cosA = cos(rad);
    float sinA = sin(rad);
    for (int t = 0; t < thickness; t++) {
      int px = round(cx + (r - t) * cosA);
      int py = round(cy + (r - t) * sinA);
      tft.drawPixel(px, py, trackColor);
    }
  }

  // 2. วาดเส้นความคืบหน้า (Active Arc) ตาม % จริง (เริ่มจากจุดบนสุด -90 องศา วนตามเข็มนาฬิกา)
  percent = constrain(percent, 0, 100);
  if (percent > 0) {
    float endAngle = (percent * 360.0f) / 100.0f;
    for (float angle = 0; angle <= endAngle; angle += 0.5f) {
      float rad = (angle - 90.0f) * 0.0174532925f;
      float cosA = cos(rad);
      float sinA = sin(rad);
      for (int t = 0; t < thickness; t++) {
        int px = round(cx + (r - t) * cosA);
        int py = round(cy + (r - t) * sinA);
        tft.drawPixel(px, py, color);
      }
    }
  }
}

// ตำแหน่งการ์ด % ใบที่ i
int pctCardX(int i) { return COL_X[i % 2]; }
int pctCardY(int i) { return SECTION_Y[i / 2] + 12; }
uint16_t pctCardBg(int i) { return PANEL_BG[sectionGroup(i / 2)]; }

// สีตามปริมาณที่เหลือ
uint16_t statusColor(int percent) {
  if (percent <= 20) return TFT_RED;
  if (percent <= 50) return TFT_ORANGE;
  return TFT_GREEN;
}

// =========================================================================
// การ์ด % โควต้า (ครึ่งจอ): label + ตัวเลข % + เวลา reset + วงกลม
// =========================================================================
void drawPctValue(int i) {
  int x = pctCardX(i);
  int y = pctCardY(i);

  uint16_t bg = pctCardBg(i);

  tft.fillRect(x + 6, y + 18, 56, 18, bg);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, bg);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(String(pctValue[i]) + "%", x + 8, y + 20);

  drawProgressRing(x + CARD_W - 22, y + 21, 14, 3, pctValue[i], statusColor(pctValue[i]), bg);
}

void drawPctReset(int i) {
  int x = pctCardX(i);
  int y = pctCardY(i);

  uint16_t bg = pctCardBg(i);

  tft.fillRect(x + 62, y + 26, 54, 10, bg);
  if (pctReset[i].length() > 0) {
    tft.setTextSize(1);
    tft.setTextColor(TFT_DARKGREY, bg);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(pctReset[i].c_str(), x + 64, y + 27);
  }
}

void drawPctCardFull(int i) {
  int x = pctCardX(i);
  int y = pctCardY(i);

  uint16_t bg = pctCardBg(i);

  tft.drawRoundRect(x, y, CARD_W, CARD_H, 5, PANEL_BORDER[sectionGroup(i / 2)]);

  tft.setTextSize(1);
  tft.setTextColor(TFT_LIGHTGREY, bg);
  tft.setTextDatum(TL_DATUM);
  tft.drawString((i % 2 == 0) ? "Weekly" : "5 Hour", x + 8, y + 7);

  drawPctValue(i);
  drawPctReset(i);
  lastPctValue[i] = pctValue[i];
  lastPctReset[i] = pctReset[i];
}

// =========================================================================
// การ์ด Token (ครึ่งจอ): label + จำนวนรวม + รายละเอียด in/out/cache ชิดขวา
// =========================================================================
void drawTokenTotal(int i) {
  int x = COL_X[i];
  int y = TOKEN_Y;

  uint16_t bg = PANEL_BG[1];

  tft.fillRect(x + 6, y + 18, 70, 18, bg);
  tft.setTextSize(2);
  tft.setTextColor(TFT_CYAN, bg);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(tokenTotal[i].c_str(), x + 8, y + 20);
}

void drawTokenDetail(int i) {
  int x = COL_X[i];
  int y = TOKEN_Y;
  int rx = x + CARD_W - 8;

  uint16_t bg = PANEL_BG[1];

  tft.fillRect(x + 78, y + 4, 70, 34, bg);
  tft.setTextSize(1);
  tft.setTextColor(TFT_DARKGREY, bg);
  tft.setTextDatum(TR_DATUM);
  tft.drawString(("in " + tokenIn[i]).c_str(), rx, y + 6);
  tft.drawString(("out " + tokenOut[i]).c_str(), rx, y + 17);
  tft.drawString(("cache " + tokenCache[i]).c_str(), rx, y + 28);
}

String tokenDetailKey(int i) {
  return tokenIn[i] + "|" + tokenOut[i] + "|" + tokenCache[i];
}

void drawTokenCardFull(int i) {
  int x = COL_X[i];
  int y = TOKEN_Y;

  tft.drawRoundRect(x, y, CARD_W, CARD_H, 5, PANEL_BORDER[1]);

  tft.setTextSize(1);
  tft.setTextColor(TFT_LIGHTGREY, PANEL_BG[1]);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(TOKEN_LABEL[i], x + 8, y + 7);

  drawTokenTotal(i);
  drawTokenDetail(i);
  lastTokenTotal[i] = tokenTotal[i];
  lastTokenDetail[i] = tokenDetailKey(i);
}

// =========================================================================
// หัวข้อของแต่ละส่วน: แถบสีเล็กๆ + ชื่อ
// =========================================================================
void drawSectionHeader(int s) {
  int y = SECTION_Y[s];
  tft.fillRoundRect(COL_X[0] + 2, y, 3, 9, 1, SECTION_COLOR[s]);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, PANEL_BG[sectionGroup(s)]);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(SECTION_TITLE[s], COL_X[0] + 10, y + 1);
}

// =========================================================================
// วาดหน้าจอทั้งหมด (ใช้ครั้งแรกเท่านั้น)
// =========================================================================
void drawDashboardFull() {
  tft.fillScreen(TFT_BLACK);

  // วาดไอคอน Wi-Fi มุมขวาบนสุด
  // พื้นหลังแผงแยกกลุ่ม Antigravity / Claude Code
  for (int g = 0; g < 2; g++) {
    tft.fillRoundRect(1, PANEL_Y[g], 318, PANEL_H[g], 6, PANEL_BG[g]);
  }

  bool isConnected = (WiFi.status() == WL_CONNECTED);
  drawWiFiIcon(294, 3, isConnected, PANEL_BG[0]);
  lastWiFiConnected = isConnected;

  for (int s = 0; s < 3; s++) drawSectionHeader(s);

  // ป้ายชื่อกลุ่มมุมขวาของแถวหัวข้อแรกในแต่ละแผง
  tft.setTextSize(1);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(SECTION_COLOR[0], PANEL_BG[0]);
  tft.drawString(PANEL_TAG[0], 288, SECTION_Y[0] + 1);
  tft.setTextColor(SECTION_COLOR[2], PANEL_BG[1]);
  tft.drawString(PANEL_TAG[1], 312, SECTION_Y[2] + 1);
  for (int i = 0; i < PCT_CARDS; i++) drawPctCardFull(i);
  for (int i = 0; i < TOKEN_CARDS; i++) drawTokenCardFull(i);
}

// =========================================================================
// อัปเดตเฉพาะส่วนที่เปลี่ยนแปลง ไม่ fillScreen ทั้งหน้า
// =========================================================================
void updateDashboardValues() {
  // อัปเดตไอคอน WiFi เฉพาะเมื่อสถานะเปลี่ยน
  bool isConnected = (WiFi.status() == WL_CONNECTED);
  if (isConnected != lastWiFiConnected) {
    drawWiFiIcon(294, 3, isConnected, PANEL_BG[0]);
    lastWiFiConnected = isConnected;
  }

  for (int i = 0; i < PCT_CARDS; i++) {
    if (pctValue[i] != lastPctValue[i]) {
      drawPctValue(i);
      lastPctValue[i] = pctValue[i];
    }
    if (pctReset[i] != lastPctReset[i]) {
      drawPctReset(i);
      lastPctReset[i] = pctReset[i];
    }
  }

  for (int i = 0; i < TOKEN_CARDS; i++) {
    if (tokenTotal[i] != lastTokenTotal[i]) {
      drawTokenTotal(i);
      lastTokenTotal[i] = tokenTotal[i];
    }
    String detail = tokenDetailKey(i);
    if (detail != lastTokenDetail[i]) {
      drawTokenDetail(i);
      lastTokenDetail[i] = detail;
    }
  }
}
