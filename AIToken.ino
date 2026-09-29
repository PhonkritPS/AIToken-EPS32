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

// กำหนดรอบการดึงข้อมูล (ทุกๆ 10 วินาที)
const unsigned long refreshInterval = 10000;
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
const int PANELS = 4;
const int ROW_H = 25;                 // ความสูงแต่ละแถวข้อมูลทั่วไป
const int HEADER_H[PANELS] = { 24, 13, 13, 13 }; // แผง 0 สูงกว่าเพราะมีไอคอน Wi-Fi
const int PANEL_Y[PANELS]  = { 1, 81, 137, 183 };
const int PANEL_H[PANELS]  = { 77, 53, 43, 54 };

const uint16_t PANEL_BG[PANELS]        = { 0x08C5, 0x28A1, 0x0903, 0x0182 }; // กรมท่า / น้ำตาลอมส้ม / เขียวอมฟ้าเข้ม / เขียวเข้ม Cyber
const uint16_t PANEL_BORDER[PANELS]    = { 0x4B0D, 0x6A06, 0x3CB1, 0x2D86 };
const uint16_t PANEL_TAG_COLOR[PANELS] = { 0x443E, 0xDBAA, 0x56F7, 0x7FE0 }; // ฟ้า / ส้ม / มินต์ / เขียวมะนาว NVIDIA
const char* PANEL_TAG[PANELS] = { "ANTIGRAVITY", "CLAUDE CODE", "CODEX", "SPARK LOCAL" };

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
String panelLastUpdated[PANELS] = { "", "", "", "" };
String lastPanelLastUpdated[PANELS] = { "", "", "", "" };
bool panelRateLimited[2] = { false, false };       // [0]=Claude Code(แผง1), [1]=Codex(แผง2)
String panelRateLimitReset[2] = { "", "" };
bool lastPanelRateLimited[2] = { false, false };
String lastPanelRateLimitReset[2] = { "", "" };

// ข้อมูล Spark Local AI (Ollama model + % CPU + RAM)
bool sparkConnected = false;
String sparkModel = "Local AI";
String sparkStatus = "Ready";
int sparkCpu = 0;
int sparkRam = 0;
String sparkRamUsed = "0G";
String sparkRamRatio = "0G/0G";
int lastSparkCpu = -1;
int lastSparkRam = -1;
String lastSparkStatus = "";
String lastSparkModel = "";
String lastSparkRamRatio = "";
bool lastSparkConnected = false;

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
void drawSparkRamSlot(int clearX, int clearW, int rowTop, int ringCx, int pctX, int textX,
                      int percent, const String &ratio, uint16_t bg);
void drawSparkCpuSlot(int clearX, int clearW, int rowTop, int ringCx, int pctX, int textX,
                      int percent, uint16_t bg);
void drawSparkRowFull();
void updateSparkRowValues();
uint16_t cpuColor(int percent);

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

      sparkConnected = doc["sparkConnected"] | false;
      sparkModel = doc["sparkModel"] | "Local AI";
      sparkStatus = doc["sparkStatus"] | "Offline";
      sparkCpu = doc["sparkCpu"] | 0;
      sparkRam = doc["sparkRam"] | 0;
      sparkRamUsed = doc["sparkRamUsed"] | "0G";
      sparkRamRatio = doc["sparkRamRatio"] | "0G/0G";

      panelLastUpdated[0] = doc["lastUpdated"] | "";
      panelLastUpdated[1] = doc["ccLastUpdated"] | "";
      panelLastUpdated[2] = doc["codexLastUpdated"] | "";
      panelLastUpdated[3] = doc["sparkLastUpdated"] | "";

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

// สีตาม % การใช้งาน CPU (ค่ายิ่งต่ำยิ่งดี: <=60% เขียว, <=85% ส้ม, >85% แดง)
uint16_t cpuColor(int percent) {
  if (percent >= 85) return TFT_RED;
  if (percent >= 60) return TFT_ORANGE;
  return TFT_GREEN;
}

// ตำแหน่ง Y บนสุดของแถวที่ i (คำนวณจากแผงและลำดับแถวในแผงนั้น)
int rowTopY(int group, int indexInGroup) {
  if (group == 0) {
    return PANEL_Y[0] + HEADER_H[0] + indexInGroup * 26;
  } else if (group == 1) {
    return (indexInGroup == 0) ? (PANEL_Y[1] + HEADER_H[1]) : (PANEL_Y[1] + HEADER_H[1] + 24);
  } else {
    return PANEL_Y[group] + HEADER_H[group] + indexInGroup * ROW_H;
  }
}

// =========================================================================
// วาด/อัปเดตช่อง % หนึ่งช่อง (วงแหวน + ตัวเลข % + เวลารีเซ็ตย่อ)
// clearX/clearW กำหนดเองต่อช่อง เพื่อไม่ให้การอัปเดตช่องหนึ่งไปเคลียร์ทับอีกช่อง
// =========================================================================
void drawPctSlot(int clearX, int clearW, int rowTop, int ringCx, int pctX, int resetX,
                 int value, const String &reset, uint16_t bg) {
  int slotH = (rowTop >= 90 && rowTop < 135) ? 23 : ROW_H;
  tft.fillRect(clearX, rowTop, clearW, slotH, bg);
  if (value < 0) return; // ไม่มีข้อมูลช่องนี้ (เช่น Codex ยังไม่มีหน้าต่างที่สอง) -> เว้นว่าง

  int ringCy = rowTop + (slotH / 2);
  drawProgressRing(ringCx, ringCy, RING_R, RING_THICK, value, statusColor(value), bg);

  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, bg);
  tft.setTextDatum(TL_DATUM);
  int pctY = rowTop + (slotH - 16) / 2;
  tft.drawString(String(value) + "%", pctX, pctY);

  if (reset.length() > 0) {
    tft.setTextSize(1);
    tft.setTextColor(TFT_DARKGREY, bg);
    int resetY = rowTop + (slotH - 8) / 2;
    tft.drawString(reset, resetX, resetY);
  }
}

void drawPctRowFull(int i) {
  int g = ROW_GROUP[i];
  int rowTop = rowTopY(g, ROW_INDEX[i]);
  uint16_t bg = PANEL_BG[g];
  int slotH = (g == 1) ? 23 : ROW_H;

  tft.setTextSize(1);
  tft.setTextColor(TFT_LIGHTGREY, bg);
  tft.setTextDatum(TL_DATUM);
  int labelY = rowTop + (slotH - 8) / 2;
  tft.drawString(ROW_LABEL[i], LABEL_X, labelY);

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
// แถว Token ของ Claude Code (Day / 5 Hour) — กล่องตัวอักษรขนาดกะทัดรัด ตัวเลขสีขาวขนาดเท่ากับ Day
// =========================================================================
void drawTokenValue(int boxX, int rowTop, const char* tag, const String &val, uint16_t panelBg) {
  int boxW = 102;
  int boxH = 15;
  int boxY = rowTop;
  uint16_t boxBg = 0x18C3;      // พื้นกล่องสีเข้มกว่าพื้นแผง Claude เล็กน้อย
  uint16_t boxBorder = 0x41C5;  // ขอบกล่องสีส้มอมน้ำตาลจางๆ

  tft.fillRoundRect(boxX, boxY, boxW, boxH, 3, boxBg);
  tft.drawRoundRect(boxX, boxY, boxW, boxH, 3, boxBorder);

  tft.setTextSize(1);
  tft.setTextDatum(TL_DATUM);

  // Tag (Day / 5H) สีส้มอมเทา
  tft.setTextColor(0xDBAA, boxBg);
  tft.drawString(tag, boxX + 6, boxY + 4);

  // ตัวเลข Token ขนาด 1 เท่ากับคำว่า Day แต่เป็นสีขาวเห็นชัดเจน
  tft.setTextColor(TFT_WHITE, boxBg);
  int valX = boxX + (strlen(tag) * 6) + 12;
  tft.drawString(val, valX, boxY + 4);
}

void drawTokenRowFull() {
  int rowTop = rowTopY(1, 1);
  uint16_t bg = PANEL_BG[1];

  tft.fillRect(LABEL_X, rowTop, 60, 15, bg);
  tft.setTextSize(1);
  tft.setTextColor(TFT_LIGHTGREY, bg);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("Tokens", LABEL_X, rowTop + 4);

  drawTokenValue(74, rowTop, "Day", tokenToday, bg);
  drawTokenValue(204, rowTop, "5H", tokenWindow, bg);

  lastTokenToday = tokenToday;
  lastTokenWindow = tokenWindow;
}

void updateTokenRowValues() {
  int rowTop = rowTopY(1, 1);
  uint16_t bg = PANEL_BG[1];

  if (tokenToday != lastTokenToday) {
    drawTokenValue(74, rowTop, "Day", tokenToday, bg);
    lastTokenToday = tokenToday;
  }
  if (tokenWindow != lastTokenWindow) {
    drawTokenValue(204, rowTop, "5H", tokenWindow, bg);
    lastTokenWindow = tokenWindow;
  }
}

// =========================================================================
// =========================================================================
// หัวแผง: ชื่อกลุ่ม + เวลาอัปเดตล่าสุด (ซ้าย) + คำเตือน Rate Limit (ขวา)
// แผง 0 (Antigravity) ไม่มี Rate Limit เพราะดึงจาก Language Server ในเครื่อง ไม่ใช่ internet API
// =========================================================================
void drawPanelHeader(int g) {
  int y = PANEL_Y[g];
  uint16_t bg = PANEL_BG[g];
  int textY = y + (HEADER_H[g] - 8) / 2;

  // เคลียร์โซนด้านหน้า (เว้น y+1 และความสูง -2 เพื่อไม่ให้ลบเส้นขอบบนของแผง)
  tft.fillRect(LABEL_X, y + 1, 140, HEADER_H[g] - 2, bg);
  tft.setTextSize(1);
  tft.setTextDatum(TL_DATUM);

  // 1. ชื่อ AI อยู่ด้านหน้า
  tft.setTextColor(PANEL_TAG_COLOR[g], bg);
  tft.drawString(PANEL_TAG[g], LABEL_X, textY);

  // 2. ตามด้วยเวลา Last Update
  if (panelLastUpdated[g].length() > 0) {
    int timeX = LABEL_X + tft.textWidth(PANEL_TAG[g]) + 6;
    tft.setTextColor(TFT_DARKGREY, bg);
    tft.drawString(panelLastUpdated[g], timeX, textY);
  }

  // 3. ป้ายเตือน Rate Limit ทางขวาสุด (ถ้ามี) หรือสถานะ + IP ของ Spark
  tft.fillRect(135, y + 1, 178, HEADER_H[g] - 2, bg);
  int rlIdx = g - 1; // แผง1(Claude Code)->0, แผง2(Codex)->1
  if (rlIdx >= 0 && rlIdx < 2 && panelRateLimited[rlIdx]) {
    String msg = "RATE LIMIT " + panelRateLimitReset[rlIdx];
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(TFT_RED, bg);
    tft.drawString(msg, 286, textY);
  } else if (g == 3) {
    // แสดงสถานะ Spark [Active] / [Ready] / [Offline] บน Header
    tft.setTextDatum(TL_DATUM);
    uint16_t statCol = (sparkStatus == "Active") ? TFT_GREEN : (sparkConnected ? TFT_CYAN : TFT_RED);
    tft.setTextColor(statCol, bg);
    tft.drawString("[" + sparkStatus + "]", 136, textY);

    // IP ทางขวาสุด
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(TFT_DARKGREY, bg);
    tft.drawString("10.104.1.23", 310, textY);
  }

  lastPanelLastUpdated[g] = panelLastUpdated[g];
  if (rlIdx >= 0 && rlIdx < 2) {
    lastPanelRateLimited[rlIdx] = panelRateLimited[rlIdx];
    lastPanelRateLimitReset[rlIdx] = panelRateLimitReset[rlIdx];
  }
  if (g == 3) {
    lastSparkStatus = sparkStatus;
    lastSparkConnected = sparkConnected;
  }
}

// =========================================================================
// แถว Spark Local AI (โมเดลชื่อเต็ม + % RAM + % CPU) อยู่แผง 3
// =========================================================================
// =========================================================================
// แถว Spark Local AI (โมเดลชื่อเต็ม + % RAM 77G/128G + % CPU) อยู่แผง 3
// =========================================================================
void drawSparkRamSlot(int clearX, int clearW, int rowTop, int ringCx, int pctX, int textX,
                      int percent, const String &ratio, uint16_t bg) {
  int slotH = 24;
  tft.fillRect(clearX, rowTop, clearW, slotH, bg);
  if (!sparkConnected) return;

  int ringCy = rowTop + (slotH / 2);
  drawProgressRing(ringCx, ringCy, RING_R, RING_THICK, percent, cpuColor(percent), bg);

  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, bg);
  tft.setTextDatum(TL_DATUM);
  int pctY = rowTop + (slotH - 16) / 2;
  tft.drawString(String(percent) + "%", pctX, pctY);

  tft.setTextSize(1);
  tft.setTextColor(0x7FE0, bg); // สีเขียวมะนาว/มินต์ สำหรับคำว่า RAM
  tft.drawString("RAM", textX, rowTop + 3);

  tft.setTextColor(TFT_WHITE, bg); // สีขาวตัวเลข เช่น 77G/128G
  tft.drawString(ratio, textX, rowTop + 13);
}

void drawSparkCpuSlot(int clearX, int clearW, int rowTop, int ringCx, int pctX, int textX,
                      int percent, uint16_t bg) {
  int slotH = 24;
  tft.fillRect(clearX, rowTop, clearW, slotH, bg);
  if (!sparkConnected) return;

  int ringCy = rowTop + (slotH / 2);
  drawProgressRing(ringCx, ringCy, RING_R, RING_THICK, percent, cpuColor(percent), bg);

  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, bg);
  tft.setTextDatum(TL_DATUM);
  int pctY = rowTop + (slotH - 16) / 2;
  tft.drawString(String(percent) + "%", pctX, pctY);

  tft.setTextSize(1);
  tft.setTextColor(TFT_DARKGREY, bg);
  int resetY = rowTop + (slotH - 8) / 2;
  tft.drawString("CPU", textX, resetY);
}

void drawSparkRowFull() {
  int modelTop = PANEL_Y[3] + HEADER_H[3];
  int rowTop = modelTop + 13;
  uint16_t bg = PANEL_BG[3];

  // 1. บรรทัดชื่อโมเดล AI (กว้างเต็มจอ x=8 ถึง x=310)
  tft.fillRect(LABEL_X, modelTop + 1, 302, 12, bg);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, bg);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(sparkModel, LABEL_X, modelTop + 3);

  // 2. บรรทัด Resource Telemetry (RAM & CPU)
  tft.fillRect(LABEL_X, rowTop, 60, 24, bg);
  tft.setTextSize(1);
  tft.setTextColor(TFT_LIGHTGREY, bg);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("Usage", LABEL_X, rowTop + 8);

  // ช่อง RAM (Column 1: Ring + % + RAM 77G/128G)
  drawSparkRamSlot(69, 128, rowTop, RING1_CX, PCT1_X, 135, sparkRam, sparkRamRatio, bg);

  // ช่อง CPU (Column 2: Ring + % + CPU)
  drawSparkCpuSlot(199, 117, rowTop, RING2_CX, PCT2_X, RESET2_X, sparkCpu, bg);

  lastSparkCpu = sparkCpu;
  lastSparkRam = sparkRam;
  lastSparkRamRatio = sparkRamRatio;
  lastSparkStatus = sparkStatus;
  lastSparkModel = sparkModel;
  lastSparkConnected = sparkConnected;
}

void updateSparkRowValues() {
  int modelTop = PANEL_Y[3] + HEADER_H[3];
  int rowTop = modelTop + 13;
  uint16_t bg = PANEL_BG[3];

  if (sparkModel != lastSparkModel) {
    tft.fillRect(LABEL_X, modelTop + 1, 302, 12, bg);
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, bg);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(sparkModel, LABEL_X, modelTop + 3);
    lastSparkModel = sparkModel;
  }

  if (sparkRam != lastSparkRam || sparkRamRatio != lastSparkRamRatio) {
    drawSparkRamSlot(69, 128, rowTop, RING1_CX, PCT1_X, 135, sparkRam, sparkRamRatio, bg);
    lastSparkRam = sparkRam;
    lastSparkRamRatio = sparkRamRatio;
  }

  if (sparkCpu != lastSparkCpu) {
    drawSparkCpuSlot(199, 117, rowTop, RING2_CX, PCT2_X, RESET2_X, sparkCpu, bg);
    lastSparkCpu = sparkCpu;
  }
}

// =========================================================================
// วาดหน้าจอทั้งหมด (ใช้ครั้งแรกเท่านั้น)
// =========================================================================
void drawDashboardFull() {
  tft.fillScreen(TFT_BLACK);

  // พื้นหลังและกรอบแผงทั้ง 4
  for (int g = 0; g < PANELS; g++) {
    tft.fillRoundRect(1, PANEL_Y[g], 318, PANEL_H[g], 6, PANEL_BG[g]);
    tft.drawRoundRect(1, PANEL_Y[g], 318, PANEL_H[g], 6, PANEL_BORDER[g]);
  }

  bool isConnected = (WiFi.status() == WL_CONNECTED);
  drawWiFiIcon(290, 4, isConnected, PANEL_BG[0]);
  lastWiFiConnected = isConnected;

  for (int g = 0; g < PANELS; g++) drawPanelHeader(g);
  for (int i = 0; i < PCT_ROWS; i++) drawPctRowFull(i);
  drawTokenRowFull();
  drawSparkRowFull();
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

  for (int g = 0; g < PANELS; g++) {
    int rlIdx = g - 1;
    bool rlChanged = (rlIdx >= 0 && rlIdx < 2) &&
                     (panelRateLimited[rlIdx] != lastPanelRateLimited[rlIdx] ||
                      panelRateLimitReset[rlIdx] != lastPanelRateLimitReset[rlIdx]);
    bool sparkHeaderChanged = (g == 3 && (sparkStatus != lastSparkStatus || sparkConnected != lastSparkConnected));
    if (panelLastUpdated[g] != lastPanelLastUpdated[g] || rlChanged || sparkHeaderChanged) {
      drawPanelHeader(g);
    }
  }

  for (int i = 0; i < PCT_ROWS; i++) updatePctRowValues(i);
  updateTokenRowValues();
  updateSparkRowValues();
}
