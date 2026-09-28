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
// Layout หน้าเดียว 320x240: 3 แผง (Antigravity / Claude Code / Codex)
// แต่ละแผงมีแถวแบบกระชับ 1 แถวต่อผู้ให้บริการ 1 ราย: label + วงแหวน 2 วง (ซ้าย/ขวา)
// + % + เวลารีเซ็ตแบบย่อ ในแถวเดียวกัน (ไม่แยกเป็นการ์ดครึ่งจอเหมือนก่อนหน้า
// เพื่อให้มีที่พอใส่ Codex เป็นผู้ให้บริการที่ 4 ได้โดยไม่ต้องสลับหน้า)
// =========================================================================
const int ROW_H = 26;                 // ความสูงแต่ละแถวข้อมูล
const int HEADER_H[3] = { 24, 13, 13 }; // แผง 0 สูงกว่าเพราะมีไอคอน Wi-Fi
const int PANEL_Y[3] = { 1, 84, 156 };
const int PANEL_H[3] = { 79, 68, 42 };

const uint16_t PANEL_BG[3]        = { 0x08C5, 0x28A1, 0x0903 }; // กรมท่า / น้ำตาลอมส้ม / เขียวอมฟ้าเข้ม
const uint16_t PANEL_BORDER[3]    = { 0x4B0D, 0x6A06, 0x3CB1 };
const uint16_t PANEL_TAG_COLOR[3] = { 0x443E, 0xDBAA, 0x56F7 };
const char* PANEL_TAG[3] = { "ANTIGRAVITY", "CLAUDE CODE", "CODEX" };

// ตำแหน่งคอลัมน์ในแต่ละแถว (label ซ้ายสุด, วงแหวน+ %+เวลารีเซ็ต 2 ชุดถัดไป)
const int LABEL_X = 8;
const int RING1_CX = 80,  PCT1_X = 94,  RESET1_X = 146;
const int RING2_CX = 210, PCT2_X = 224, RESET2_X = 276;
const int RING_R = 9, RING_THICK = 2;

// การ์ด % 4 แถว: Gemini / Claude & GPT / Plan Usage (Claude Code) / Codex
// ค่า -1 หมายถึง "ไม่มีข้อมูลช่องนี้" (เช่น Codex แพลน Free ยังไม่มีหน้าต่างที่สอง) ให้เว้นว่างไม่วาด
const int PCT_ROWS = 4;
const char* ROW_LABEL[PCT_ROWS]  = { "Gemini", "Claude&GPT", "Plan Usage", "Codex" };
const int   ROW_GROUP[PCT_ROWS]  = { 0, 0, 1, 2 };
const int   ROW_INDEX[PCT_ROWS]  = { 0, 1, 0, 0 };
const char* PCT_KEY1[PCT_ROWS]   = { "geminiWeekly", "claudeWeekly", "ccWeekly", "codexPrimaryPercent" };
const char* RESET_KEY1[PCT_ROWS] = { "geminiWeeklyReset", "claudeWeeklyReset", "ccWeeklyReset", "codexPrimaryReset" };
const char* PCT_KEY2[PCT_ROWS]   = { "gemini5Hr", "claude5Hr", "cc5Hr", "codexSecondaryPercent" };
const char* RESET_KEY2[PCT_ROWS] = { "gemini5HrReset", "claude5HrReset", "cc5HrReset", "codexSecondaryReset" };

int pctValue1[PCT_ROWS], pctValue2[PCT_ROWS];
String pctReset1[PCT_ROWS], pctReset2[PCT_ROWS];
int lastPctValue1[PCT_ROWS] = { -1, -1, -1, -1 };
int lastPctValue2[PCT_ROWS] = { -1, -1, -1, -1 };
String lastPctReset1[PCT_ROWS], lastPctReset2[PCT_ROWS];

// แถว Token ของ Claude Code (อยู่แผง 1 แถวที่ 2 ต่อจาก Plan Usage)
String tokenToday = "-", tokenWindow = "-";
String lastTokenToday = "", lastTokenWindow = "";

// หัวแผง: เวลาอัปเดตล่าสุด (ทุกแผง) + สถานะ Rate Limit (เฉพาะแผง 1,2 เพราะดึงจาก internet API)
String panelLastUpdated[3] = { "", "", "" };
String lastPanelLastUpdated[3] = { "", "", "" };
bool panelRateLimited[2] = { false, false };       // [0]=Claude Code(แผง1), [1]=Codex(แผง2)
String panelRateLimitReset[2] = { "", "" };
bool lastPanelRateLimited[2] = { false, false };
String lastPanelRateLimitReset[2] = { "", "" };

// ประกาศฟังก์ชันล่วงหน้า
void drawWiFiIcon(int x, int y, bool connected, uint16_t bg);
void drawProgressRing(int cx, int cy, int r, int thickness, int percent, uint16_t color, uint16_t bg);
void drawDashboardFull();
void updateDashboardValues();
void fetchAndDisplayQuota();
void drawPanelHeader(int g);
void drawPctRowFull(int i);
void updatePctRowValues(int i);
void drawTokenRowFull();
void updateTokenRowValues();

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
    for (int i = 0; i < PCT_ROWS; i++) { pctValue1[i] = -1; pctValue2[i] = -1; }
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
      drawWiFiIcon(290, 4, false, PANEL_BG[0]);
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
      for (int i = 0; i < PCT_ROWS; i++) {
        pctValue1[i] = doc[PCT_KEY1[i]] | -1;
        pctReset1[i] = doc[RESET_KEY1[i]] | "";
        pctValue2[i] = doc[PCT_KEY2[i]] | -1;
        pctReset2[i] = doc[RESET_KEY2[i]] | "";
      }
      tokenToday = doc["ccTodayTokens"] | "-";
      tokenWindow = doc["ccWindowTokens"] | "-";

      panelLastUpdated[0] = doc["lastUpdated"] | "";
      panelLastUpdated[1] = doc["ccLastUpdated"] | "";
      panelLastUpdated[2] = doc["codexLastUpdated"] | "";

      panelRateLimited[0] = doc["ccRateLimited"] | false;
      panelRateLimitReset[0] = doc["ccRateLimitReset"] | "";
      panelRateLimited[1] = doc["codexRateLimited"] | false;
      panelRateLimitReset[1] = doc["codexRateLimitReset"] | "";

      if (isFirstDraw) {
        // ครั้งแรก: วาดหน้าจอทั้งหมด รวมถึงพื้นแผงและกรอบ
        drawDashboardFull();
        isFirstDraw = false;
      } else {
        // ครั้งต่อไป: อัปเดตเฉพาะค่าที่เปลี่ยน
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
// ฟังก์ชันวาดไอคอน Wi-Fi ขนาดเล็ก
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
  tft.fillCircle(cx, cy, r + 1, bg);

  // 1. วาดวงแหวนพื้นหลังสีเทาเข้ม (Track แสดงพื้นที่ 100%)
  uint16_t trackColor = 0x3186;
  for (float angle = 0; angle < 360.0f; angle += 1.0f) {
    float rad = angle * 0.0174532925f;
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
    for (float angle = 0; angle <= endAngle; angle += 1.0f) {
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

// สีตามปริมาณที่เหลือ
uint16_t statusColor(int percent) {
  if (percent <= 20) return TFT_RED;
  if (percent <= 50) return TFT_ORANGE;
  return TFT_GREEN;
}

// ตำแหน่ง Y บนสุดของแถวที่ i (คำนวณจากแผงและลำดับแถวในแผงนั้น)
int rowTopY(int group, int indexInGroup) {
  return PANEL_Y[group] + HEADER_H[group] + indexInGroup * ROW_H;
}

// =========================================================================
// วาด/อัปเดตช่อง % หนึ่งช่อง (วงแหวน + ตัวเลข % + เวลารีเซ็ตย่อ)
// clearX/clearW กำหนดเองต่อช่อง เพื่อไม่ให้การอัปเดตช่องหนึ่งไปเคลียร์ทับอีกช่อง
// =========================================================================
void drawPctSlot(int clearX, int clearW, int rowTop, int ringCx, int pctX, int resetX,
                 int value, const String &reset, uint16_t bg) {
  tft.fillRect(clearX, rowTop, clearW, ROW_H, bg);
  if (value < 0) return; // ไม่มีข้อมูลช่องนี้ (เช่น Codex ยังไม่มีหน้าต่างที่สอง) -> เว้นว่าง

  drawProgressRing(ringCx, rowTop + ROW_H / 2, RING_R, RING_THICK, value, statusColor(value), bg);

  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, bg);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(String(value) + "%", pctX, rowTop + 5);

  if (reset.length() > 0) {
    tft.setTextSize(1);
    tft.setTextColor(TFT_DARKGREY, bg);
    tft.drawString(reset, resetX, rowTop + 9);
  }
}

void drawPctRowFull(int i) {
  int g = ROW_GROUP[i];
  int rowTop = rowTopY(g, ROW_INDEX[i]);
  uint16_t bg = PANEL_BG[g];

  tft.setTextSize(1);
  tft.setTextColor(TFT_LIGHTGREY, bg);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(ROW_LABEL[i], LABEL_X, rowTop + 9);

  drawPctSlot(69, 128, rowTop, RING1_CX, PCT1_X, RESET1_X, pctValue1[i], pctReset1[i], bg);
  drawPctSlot(199, 117, rowTop, RING2_CX, PCT2_X, RESET2_X, pctValue2[i], pctReset2[i], bg);

  lastPctValue1[i] = pctValue1[i];
  lastPctReset1[i] = pctReset1[i];
  lastPctValue2[i] = pctValue2[i];
  lastPctReset2[i] = pctReset2[i];
}

void updatePctRowValues(int i) {
  int g = ROW_GROUP[i];
  int rowTop = rowTopY(g, ROW_INDEX[i]);
  uint16_t bg = PANEL_BG[g];

  if (pctValue1[i] != lastPctValue1[i] || pctReset1[i] != lastPctReset1[i]) {
    drawPctSlot(69, 128, rowTop, RING1_CX, PCT1_X, RESET1_X, pctValue1[i], pctReset1[i], bg);
    lastPctValue1[i] = pctValue1[i];
    lastPctReset1[i] = pctReset1[i];
  }
  if (pctValue2[i] != lastPctValue2[i] || pctReset2[i] != lastPctReset2[i]) {
    drawPctSlot(199, 117, rowTop, RING2_CX, PCT2_X, RESET2_X, pctValue2[i], pctReset2[i], bg);
    lastPctValue2[i] = pctValue2[i];
    lastPctReset2[i] = pctReset2[i];
  }
}

// =========================================================================
// แถว Token ของ Claude Code (Day / 5 Hour) — ใช้กริดคอลัมน์เดียวกับแถว %
// =========================================================================
void drawTokenValue(int clearX, int clearW, int rowTop, int tagX, int valX,
                    const char* tag, const String &val, uint16_t bg) {
  tft.fillRect(clearX, rowTop, clearW, ROW_H, bg);

  tft.setTextSize(1);
  tft.setTextColor(TFT_DARKGREY, bg);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(tag, tagX, rowTop + 9);

  tft.setTextSize(2);
  tft.setTextColor(TFT_CYAN, bg);
  tft.drawString(val, valX, rowTop + 5);
}

void drawTokenRowFull() {
  int rowTop = rowTopY(1, 1);
  uint16_t bg = PANEL_BG[1];

  tft.setTextSize(1);
  tft.setTextColor(TFT_LIGHTGREY, bg);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("Tokens", LABEL_X, rowTop + 9);

  drawTokenValue(69, 128, rowTop, RING1_CX, PCT1_X, "Day", tokenToday, bg);
  drawTokenValue(199, 117, rowTop, RING2_CX, PCT2_X, "5H", tokenWindow, bg);

  lastTokenToday = tokenToday;
  lastTokenWindow = tokenWindow;
}

void updateTokenRowValues() {
  int rowTop = rowTopY(1, 1);
  uint16_t bg = PANEL_BG[1];

  if (tokenToday != lastTokenToday) {
    drawTokenValue(69, 128, rowTop, RING1_CX, PCT1_X, "Day", tokenToday, bg);
    lastTokenToday = tokenToday;
  }
  if (tokenWindow != lastTokenWindow) {
    drawTokenValue(199, 117, rowTop, RING2_CX, PCT2_X, "5H", tokenWindow, bg);
    lastTokenWindow = tokenWindow;
  }
}

// =========================================================================
// หัวแผง: เวลาอัปเดตล่าสุด (ซ้าย) + ป้ายชื่อกลุ่ม หรือคำเตือน Rate Limit (ขวา)
// แผง 0 (Antigravity) ไม่มี Rate Limit เพราะดึงจาก Language Server ในเครื่อง ไม่ใช่ internet API
// =========================================================================
void drawPanelHeader(int g) {
  int y = PANEL_Y[g];
  uint16_t bg = PANEL_BG[g];
  int textY = y + (HEADER_H[g] - 8) / 2;

  // เวลาอัปเดตล่าสุด
  tft.fillRect(LABEL_X, y, 100, HEADER_H[g] - 1, bg);
  tft.setTextSize(1);
  tft.setTextColor(TFT_DARKGREY, bg);
  tft.setTextDatum(TL_DATUM);
  if (panelLastUpdated[g].length() > 0) {
    tft.drawString(panelLastUpdated[g], LABEL_X, textY);
  }

  // ป้ายชื่อกลุ่ม / คำเตือน Rate Limit (เว้นที่ทางขวาสุดไว้ให้ไอคอน Wi-Fi ของแผง 0)
  tft.fillRect(150, y, 136, HEADER_H[g] - 1, bg);
  tft.setTextDatum(TR_DATUM);
  int rlIdx = g - 1; // แผง1(Claude Code)->0, แผง2(Codex)->1, แผง0 ไม่มี
  if (rlIdx >= 0 && panelRateLimited[rlIdx]) {
    String msg = "RATE LIMIT " + panelRateLimitReset[rlIdx];
    tft.setTextColor(TFT_RED, bg);
    tft.drawString(msg, 286, textY);
  } else {
    tft.setTextColor(PANEL_TAG_COLOR[g], bg);
    tft.drawString(PANEL_TAG[g], 286, textY);
  }

  lastPanelLastUpdated[g] = panelLastUpdated[g];
  if (rlIdx >= 0) {
    lastPanelRateLimited[rlIdx] = panelRateLimited[rlIdx];
    lastPanelRateLimitReset[rlIdx] = panelRateLimitReset[rlIdx];
  }
}

// =========================================================================
// วาดหน้าจอทั้งหมด (ใช้ครั้งแรกเท่านั้น)
// =========================================================================
void drawDashboardFull() {
  tft.fillScreen(TFT_BLACK);

  // พื้นหลังแผงทั้ง 3
  for (int g = 0; g < 3; g++) {
    tft.fillRoundRect(1, PANEL_Y[g], 318, PANEL_H[g], 6, PANEL_BG[g]);
  }

  bool isConnected = (WiFi.status() == WL_CONNECTED);
  drawWiFiIcon(290, 4, isConnected, PANEL_BG[0]);
  lastWiFiConnected = isConnected;

  for (int g = 0; g < 3; g++) drawPanelHeader(g);
  for (int i = 0; i < PCT_ROWS; i++) drawPctRowFull(i);
  drawTokenRowFull();
}

// =========================================================================
// อัปเดตเฉพาะส่วนที่เปลี่ยนแปลง ไม่ fillScreen ทั้งหน้า
// =========================================================================
void updateDashboardValues() {
  bool isConnected = (WiFi.status() == WL_CONNECTED);
  if (isConnected != lastWiFiConnected) {
    drawWiFiIcon(290, 4, isConnected, PANEL_BG[0]);
    lastWiFiConnected = isConnected;
  }

  for (int g = 0; g < 3; g++) {
    int rlIdx = g - 1;
    bool rlChanged = (rlIdx >= 0) &&
                     (panelRateLimited[rlIdx] != lastPanelRateLimited[rlIdx] ||
                      panelRateLimitReset[rlIdx] != lastPanelRateLimitReset[rlIdx]);
    if (panelLastUpdated[g] != lastPanelLastUpdated[g] || rlChanged) {
      drawPanelHeader(g);
    }
  }

  for (int i = 0; i < PCT_ROWS; i++) updatePctRowValues(i);
  updateTokenRowValues();
}
