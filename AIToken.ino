#include <TFT_eSPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h> // ติดตั้งผ่าน Arduino Library Manager (ArduinoJson by Benoit Blanchon)

TFT_eSPI tft = TFT_eSPI(); 

// Wi-Fi Config
const char* ssid = "H101-2.4GHz";
const char* password = "0817115775";

// ตั้งค่า IP ของเครื่องคอมพิวเตอร์ที่รัน bridge_server.js
const char* apiUrl = "http://192.168.10.33:5000/api/quota";

// กำหนดรอบการดึงข้อมูล (ทุกๆ 15 วินาที)
const unsigned long refreshInterval = 15000;
unsigned long lastFetchTime = 0;

// แฟลกสำหรับแยกว่าเป็นการวาดครั้งแรก (full draw) หรืออัปเดตบางส่วน (partial update)
bool isFirstDraw = true;

// ตัวแปรเก็บค่าเดิมเพื่อเปรียบเทียบ (ถ้าเป็นค่าเดิมจะไม่รีเฟรชตัวเลขและวงกลม)
int lastGeminiWeekly = -1;
int lastGemini5Hr = -1;
int lastClaudeWeekly = -1;
int lastClaude5Hr = -1;
String lastGeminiWeeklySub = "";
String lastGemini5HrSub = "";
String lastClaudeWeeklySub = "";
String lastClaude5HrSub = "";
bool lastWiFiConnected = false;

// ประกาศฟังก์ชันล่วงหน้า
void drawWiFiIcon(int x, int y, bool connected);
void drawProgressRing(int cx, int cy, int r, int thickness, int percent, uint16_t color, uint16_t bgFillColor = TFT_BLACK);
void drawGroupCard(int bx, int by, int bw, int bh, 
                   const char* label1, const char* subtext1, int percent1,
                   const char* label2, const char* subtext2, int percent2);
void updateSingleItem(int bx, int subtextY, int numY, int ringY, 
                      const String& subtext, String& lastSubtext, 
                      int percent, int& lastPercent);
void drawDashboardFull(int geminiWeekly, int gemini5Hr, int claudeWeekly, int claude5Hr,
                       String geminiWeeklySub, String gemini5HrSub,
                       String claudeWeeklySub, String claude5HrSub);
void updateDashboardValues(int geminiWeekly, int gemini5Hr, int claudeWeekly, int claude5Hr,
                           String geminiWeeklySub, String gemini5HrSub,
                           String claudeWeeklySub, String claude5HrSub);
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
    drawDashboardFull(0, 0, 0, 0, "No Connection", "", "", "");
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
      drawWiFiIcon(294, 2, false);
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

    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, payload);

    if (!error) {
      int geminiWeekly = doc["geminiWeekly"] | 100;
      int gemini5Hr = doc["gemini5Hr"] | 100;
      String geminiWeeklySub = doc["geminiWeeklySubtext"].as<String>();
      String gemini5HrSub = doc["gemini5HrSubtext"].as<String>();

      int claudeWeekly = doc["claudeWeekly"] | 100;
      int claude5Hr = doc["claude5Hr"] | 100;
      String claudeWeeklySub = doc["claudeWeeklySubtext"].as<String>();
      String claude5HrSub = doc["claude5HrSubtext"].as<String>();

      if (isFirstDraw) {
        // ครั้งแรก: วาดหน้าจอทั้งหมด รวมถึงหัวข้อ, เส้นคั่น
        drawDashboardFull(geminiWeekly, gemini5Hr, claudeWeekly, claude5Hr,
                          geminiWeeklySub, gemini5HrSub, claudeWeeklySub, claude5HrSub);
        isFirstDraw = false;
      } else {
        // ครั้งต่อไป: อัปเดตเฉพาะตัวเลข%, subtext, วงกลม และไอคอน WiFi (เฉพาะส่วนที่เปลี่ยน)
        updateDashboardValues(geminiWeekly, gemini5Hr, claudeWeekly, claude5Hr,
                              geminiWeeklySub, gemini5HrSub, claudeWeeklySub, claude5HrSub);
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
// ฟังก์ชันวาดไอคอน Wi-Fi
// =========================================================================
void drawWiFiIcon(int x, int y, bool connected) {
  uint16_t iconColor = connected ? TFT_LIGHTGREY : TFT_DARKGREY;
  
  tft.fillRect(x - 2, y, 28, 34, TFT_BLACK); 
  
  int cx = x + 12;
  int cy = y + 19;
  
  tft.drawCircle(cx, cy, 12, iconColor);
  tft.drawCircle(cx, cy, 11, iconColor);
  
  tft.drawCircle(cx, cy, 8, iconColor);
  tft.drawCircle(cx, cy, 7, iconColor);
  
  tft.fillTriangle(cx, cy, x - 2, cy, x - 2, cy - 14, TFT_BLACK);             
  tft.fillTriangle(cx, cy, x + 26, cy, x + 26, cy - 14, TFT_BLACK);   
  tft.fillRect(x - 2, cy, 28, 15, TFT_BLACK);
  
  tft.fillCircle(cx, cy - 2, 2, iconColor);
  tft.drawRoundRect(x, y, 24, 24, 4, TFT_DARKGREY);
  
  if (!connected) {
    tft.drawLine(x, y, x + 24, y + 24, TFT_RED);
    tft.drawLine(x + 24, y, x, y + 24, TFT_RED);
  }
}

// =========================================================================
// ฟังก์ชันวาดวงแหวน Circular Progress Ring ตามเปอร์เซ็นต์จริง (0 - 100%)
// =========================================================================
void drawProgressRing(int cx, int cy, int r, int thickness, int percent, uint16_t color, uint16_t bgFillColor) {
  // ล้างพื้นที่วงกลมเป็นสีดำ
  tft.fillCircle(cx, cy, r + 1, bgFillColor);

  // 1. วาดวงแหวนพื้นหลังสีเทาเข้ม (Track แสดงพื้นที่ 100%)
  uint16_t trackColor = 0x3186; // เทาเข้มโปร่งๆ
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

  // 2. วาดเส้นความคืบหน้า (Active Arc) ตาม % จริง
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

// =========================================================================
// วาดหน้าจอทั้งหมด (ใช้ครั้งแรกเท่านั้น)
// =========================================================================
void drawDashboardFull(int geminiWeekly, int gemini5Hr, int claudeWeekly, int claude5Hr, 
                       String geminiWeeklySub, String gemini5HrSub, 
                       String claudeWeeklySub, String claude5HrSub) {
  tft.fillScreen(TFT_BLACK); 

  // วาดไอคอน Wi-Fi มุมขวาบนสุด (ตำแหน่งเดิม y = 2)
  bool isConnected = (WiFi.status() == WL_CONNECTED);
  drawWiFiIcon(294, 2, isConnected);
  lastWiFiConnected = isConnected;

  // --- Gemini Models (เลื่อนลง 10px -> y = 16) ---
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(TL_DATUM); 
  tft.setTextSize(2);
  tft.drawString("Gemini Models", 8, 16);

  // กรอบสี่เหลี่ยมรวมสำหรับ Gemini Models (เลื่อนลง 10px -> by = 36)
  drawGroupCard(0, 36, 320, 74, 
                "Weekly Limit Remaining", geminiWeeklySub.c_str(), geminiWeekly,
                "Five Hour Limit Remaining", gemini5HrSub.c_str(), gemini5Hr);

  // --- Claude and GPT models (เลื่อนลง 10px -> y = 118) ---
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(TL_DATUM); 
  tft.setTextSize(2);
  tft.drawString("Claude and GPT models", 8, 118);

  // กรอบสี่เหลี่ยมรวมสำหรับ Claude and GPT models (เลื่อนลง 10px -> by = 138)
  drawGroupCard(0, 138, 320, 74, 
                "Weekly Limit Remaining", claudeWeeklySub.c_str(), claudeWeekly,
                "Five Hour Limit Remaining", claude5HrSub.c_str(), claude5Hr);

  // --- ข้อความ Antigravity ด้านล่างสุดตรงกลาง ---
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.setTextDatum(BC_DATUM);
  tft.setTextSize(2);
  tft.drawString("Antigravity", tft.width() / 2, 236);

  // จำค่าปัจจุบันไว้
  lastGeminiWeekly = geminiWeekly;
  lastGemini5Hr = gemini5Hr;
  lastClaudeWeekly = claudeWeekly;
  lastClaude5Hr = claude5Hr;
  lastGeminiWeeklySub = geminiWeeklySub;
  lastGemini5HrSub = gemini5HrSub;
  lastClaudeWeeklySub = claudeWeeklySub;
  lastClaude5HrSub = claude5HrSub;
}

// =========================================================================
// อัปเดตเฉพาะส่วนที่เปลี่ยนแปลง ไม่ fillScreen ทั้งหน้า
// =========================================================================
void updateDashboardValues(int geminiWeekly, int gemini5Hr, int claudeWeekly, int claude5Hr,
                           String geminiWeeklySub, String gemini5HrSub,
                           String claudeWeeklySub, String claude5HrSub) {
  // อัปเดตไอคอน WiFi เฉพาะเมื่อสถานะเปลี่ยน (y = 2 ตามเดิม)
  bool isConnected = (WiFi.status() == WL_CONNECTED);
  if (isConnected != lastWiFiConnected) {
    drawWiFiIcon(294, 2, isConnected);
    lastWiFiConnected = isConnected;
  }

  // อัปเดต Gemini Card 1 (bx = 0, by = 36)
  updateSingleItem(0, 36 + 20, 36 + 10, 36 + 20, geminiWeeklySub, lastGeminiWeeklySub, geminiWeekly, lastGeminiWeekly);
  updateSingleItem(0, 36 + 56, 36 + 46, 36 + 56, gemini5HrSub,    lastGemini5HrSub,    gemini5Hr,    lastGemini5Hr);

  // อัปเดต Claude Card 2 (bx = 0, by = 138)
  updateSingleItem(0, 138 + 20, 138 + 10, 138 + 20, claudeWeeklySub, lastClaudeWeeklySub, claudeWeekly, lastClaudeWeekly);
  updateSingleItem(0, 138 + 56, 138 + 46, 138 + 56, claude5HrSub,    lastClaude5HrSub,    claude5Hr,    lastClaude5Hr);
}

// =========================================================================
// วาด Group Card รวม 2 รายการ ชิดขอบซ้าย-ขวา (Dark Theme)
// =========================================================================
void drawGroupCard(int bx, int by, int bw, int bh, 
                   const char* label1, const char* subtext1, int percent1,
                   const char* label2, const char* subtext2, int percent2) {
  // 1. วาดกรอบสี่เหลี่ยมชิดขอบซ้าย-ขวา (Dark Theme Card Frame)
  tft.fillRoundRect(bx, by, bw, bh, 4, TFT_BLACK);
  uint16_t borderColor = 0x4B0D; // สีเทาฟ้าสว่าง (Slate Grey)
  tft.drawRoundRect(bx, by, bw, bh, 4, borderColor);

  // --- รายการที่ 1 (Weekly Limit) ---
  int y1_sub = by + 20;
  int y1_num = by + 10;
  int y1_ring = by + 20;

  // Title 1: ข้อความสีเทาอ่อน
  tft.setTextSize(1);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(label1, bx + 8, by + 7);

  // Subtext 1: ข้อความสีเทาเข้ม
  tft.fillRect(bx + 8, y1_sub, 190, 11, TFT_BLACK);
  if (String(subtext1).length() > 0) {
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString(subtext1, bx + 8, y1_sub);
  }

  // Percent 1: ตัวเลขสีขาว
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(TR_DATUM);
  tft.drawNumber(percent1, bx + 260, y1_num);
  tft.drawString("%", bx + 278, y1_num);

  uint16_t color1 = (percent1 <= 20) ? TFT_RED : (percent1 <= 50) ? TFT_ORANGE : TFT_GREEN;
  drawProgressRing(bx + 304, y1_ring, 10, 3, percent1, color1, TFT_BLACK);

  // --- เส้นคั่นกลางภายในกรอบ ---
  tft.drawFastHLine(bx, by + 37, bw, 0x2965);

  // --- รายการที่ 2 (Five Hour Limit) ---
  int y2_sub = by + 56;
  int y2_num = by + 46;
  int y2_ring = by + 56;

  // Title 2: ข้อความสีเทาอ่อน
  tft.setTextSize(1);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(label2, bx + 8, by + 43);

  // Subtext 2: ข้อความสีเทาเข้ม
  tft.fillRect(bx + 8, y2_sub, 190, 11, TFT_BLACK);
  if (String(subtext2).length() > 0) {
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString(subtext2, bx + 8, y2_sub);
  }

  // Percent 2: ตัวเลขสีขาว
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(TR_DATUM);
  tft.drawNumber(percent2, bx + 260, y2_num);
  tft.drawString("%", bx + 278, y2_num);

  uint16_t color2 = (percent2 <= 20) ? TFT_RED : (percent2 <= 50) ? TFT_ORANGE : TFT_GREEN;
  drawProgressRing(bx + 304, y2_ring, 10, 3, percent2, color2, TFT_BLACK);
}

// =========================================================================
// อัปเดตเฉพาะค่าที่เปลี่ยนในแต่ละรายการ (Dark Theme)
// =========================================================================
void updateSingleItem(int bx, int subtextY, int numY, int ringY, 
                      const String& subtext, String& lastSubtext, 
                      int percent, int& lastPercent) {
  // 1. อัปเดต Subtext เฉพาะเมื่อข้อความเปลี่ยนแปลง
  if (subtext != lastSubtext) {
    tft.setTextSize(1);
    tft.setTextDatum(TL_DATUM);
    tft.fillRect(bx + 8, subtextY, 190, 11, TFT_BLACK);
    if (subtext.length() > 0) {
      tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
      tft.drawString(subtext.c_str(), bx + 8, subtextY);
    }
    lastSubtext = subtext;
  }

  // 2. อัปเดต % และวงกลมเฉพาะเมื่อค่า % เปลี่ยนแปลง
  if (percent != lastPercent) {
    tft.fillRect(bx + 200, numY - 2, 82, 24, TFT_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(TR_DATUM);
    tft.drawNumber(percent, bx + 260, numY);
    tft.drawString("%", bx + 278, numY);

    uint16_t color = (percent <= 20) ? TFT_RED : (percent <= 50) ? TFT_ORANGE : TFT_GREEN;
    drawProgressRing(bx + 304, ringY, 10, 3, percent, color, TFT_BLACK);
    lastPercent = percent;
  }
}