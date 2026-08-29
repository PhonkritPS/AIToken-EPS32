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
void drawProgressRing(int cx, int cy, int r, int thickness, int percent, uint16_t color);
void drawStatRowFull(int x, int y, const char* label, const char* subtext, int percent);
void updateStatRowValues(int x, int y, const String& subtext, String& lastSubtext, int percent, int& lastPercent);
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
void drawProgressRing(int cx, int cy, int r, int thickness, int percent, uint16_t color) {
  // ล้างพื้นที่วงกลม
  tft.fillCircle(cx, cy, r + 1, TFT_BLACK);

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

// =========================================================================
// วาดหน้าจอทั้งหมด (ใช้ครั้งแรกเท่านั้น)
// =========================================================================
void drawDashboardFull(int geminiWeekly, int gemini5Hr, int claudeWeekly, int claude5Hr, 
                       String geminiWeeklySub, String gemini5HrSub, 
                       String claudeWeeklySub, String claude5HrSub) {
  tft.fillScreen(TFT_BLACK); 

  // วาดไอคอน Wi-Fi มุมขวาบนสุด
  bool isConnected = (WiFi.status() == WL_CONNECTED);
  drawWiFiIcon(294, 2, isConnected);
  lastWiFiConnected = isConnected;

  // --- Gemini Models ---
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(TL_DATUM); 
  tft.setTextSize(2);
  tft.drawString("Gemini Models", 10, 8);

  drawStatRowFull(10, 32, "Weekly Limit Remaining", geminiWeeklySub.c_str(), geminiWeekly);
  drawStatRowFull(10, 68, "Five Hour Limit Remaining", gemini5HrSub.c_str(), gemini5Hr);

  // เส้นคั่นกลาง
  tft.drawFastHLine(10, 108, 300, TFT_DARKGREY);

  // --- Claude and GPT models ---
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(TL_DATUM); 
  tft.setTextSize(2);
  tft.drawString("Claude and GPT models", 10, 120);

  drawStatRowFull(10, 144, "Weekly Limit Remaining", claudeWeeklySub.c_str(), claudeWeekly);
  drawStatRowFull(10, 180, "Five Hour Limit Remaining", claude5HrSub.c_str(), claude5Hr);

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
  // อัปเดตไอคอน WiFi เฉพาะเมื่อสถานะเปลี่ยน
  bool isConnected = (WiFi.status() == WL_CONNECTED);
  if (isConnected != lastWiFiConnected) {
    drawWiFiIcon(294, 2, isConnected);
    lastWiFiConnected = isConnected;
  }

  // อัปเดตแต่ละ row (จะวาดตัวเลขและวงกลมเฉพาะเมื่อ % เปลี่ยน และวาดเวลาเฉพาะเมื่อ subtext เปลี่ยน)
  updateStatRowValues(10, 32,  geminiWeeklySub, lastGeminiWeeklySub, geminiWeekly, lastGeminiWeekly);
  updateStatRowValues(10, 68,  gemini5HrSub,    lastGemini5HrSub,    gemini5Hr,    lastGemini5Hr);
  updateStatRowValues(10, 144, claudeWeeklySub, lastClaudeWeeklySub, claudeWeekly, lastClaudeWeekly);
  updateStatRowValues(10, 180, claude5HrSub,    lastClaude5HrSub,    claude5Hr,    lastClaude5Hr);
}

// =========================================================================
// วาด Row เต็ม (รวมกรอบสี่เหลี่ยมมุมโค้ง + ข้อความ + % + วงกลม)
// =========================================================================
void drawStatRowFull(int x, int y, const char* label, const char* subtext, int percent) {
  int bx = x;
  int by = y - 4;
  int bw = 300;
  int bh = 34;

  // 1. วาดกรอบสี่เหลี่ยมมุมโค้ง (Card Frame)
  uint16_t borderColor = 0x4B0D; // สีเทาฟ้าสว่าง (Slate Grey)
  tft.drawRoundRect(bx, by, bw, bh, 5, borderColor);

  // 2. ข้อความหัวข้อ (Title)
  tft.setTextSize(1);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(label, bx + 10, by + 5);

  // 3. ข้อความ subtext (เวลานับถอยหลัง)
  tft.fillRect(bx + 10, by + 18, 175, 11, TFT_BLACK);
  if (String(subtext).length() > 0) {
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString(subtext, bx + 10, by + 18);
  }

  // 4. ตัวเลข % โควต้า
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(TR_DATUM); 
  tft.drawNumber(percent, bx + 245, by + 7);
  tft.drawString("%", bx + 263, by + 7);

  // 5. กำหนดสีของ Progress Ring ตามปริมาณที่เหลือ
  uint16_t statusColor = TFT_GREEN;
  if (percent <= 20) {
    statusColor = TFT_RED;
  } else if (percent <= 50) {
    statusColor = TFT_ORANGE;
  }

  drawProgressRing(bx + 283, by + 17, 10, 3, percent, statusColor);
}

// =========================================================================
// อัปเดตเฉพาะค่าที่เปลี่ยน ภายในกรอบเดิม (ไม่วาดกรอบใหม่ ไม่วาด label ซ้ำ)
// =========================================================================
void updateStatRowValues(int x, int y, const String& subtext, String& lastSubtext, int percent, int& lastPercent) {
  int bx = x;
  int by = y - 4;

  // 1. อัปเดต Subtext เฉพาะเมื่อข้อความมีการเปลี่ยนแปลง (เช่น นาทีเวลานับถอยหลังลดลง)
  if (subtext != lastSubtext) {
    tft.setTextSize(1);
    tft.setTextDatum(TL_DATUM);
    tft.fillRect(bx + 10, by + 18, 175, 11, TFT_BLACK);
    if (subtext.length() > 0) {
      tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
      tft.drawString(subtext.c_str(), bx + 10, by + 18);
    }
    lastSubtext = subtext;
  }

  // 2. อัปเดตตัวเลข % และกราฟวงกลมเฉพาะเมื่อค่า % มีการเปลี่ยนแปลง
  if (percent != lastPercent) {
    // ล้างโซนตัวเลข % ภายในกรอบแล้ววาดใหม่
    tft.fillRect(bx + 186, by + 5, 80, 24, TFT_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(TR_DATUM);
    tft.drawNumber(percent, bx + 245, by + 7);
    tft.drawString("%", bx + 263, by + 7);

    // กำหนดสีของ Progress Ring ตามปริมาณที่เหลือ
    uint16_t statusColor = TFT_GREEN;
    if (percent <= 20) {
      statusColor = TFT_RED;
    } else if (percent <= 50) {
      statusColor = TFT_ORANGE;
    }

    // วาดวงกลม progress ring ใหม่
    drawProgressRing(bx + 283, by + 17, 10, 3, percent, statusColor);
    lastPercent = percent;
  }
}