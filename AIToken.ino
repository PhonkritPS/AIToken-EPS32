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

bool isFirstDraw = true;
bool lastWiFiConnected = false;

// =========================================================================
// Layout 320x240:
// แผง 0: ANTIGRAVITY (Gemini & Claude/GPT) (กรอบบนสุด)
// แผง 1: CLAUDE CODE (Plan Limits + Tokens Day / 5H)
// แผง 2: OPENAI CODEX (Weekly Limit & Plan)
// แผง 3: SPARK LOCAL (Host Telemetry, Docker Containers, Tokens & Savings)
// =========================================================================
const int PANELS = 4;
const int HEADER_H[PANELS] = { 14, 14, 14, 14 };
const int PANEL_Y[PANELS]  = { 1, 54, 107, 144 };
const int PANEL_H[PANELS]  = { 50, 50, 34, 94 };

const uint16_t PANEL_BG[PANELS]        = { 0x08C5, 0x28A1, 0x0903, 0x0182 }; // กรมท่า (Antigravity) / น้ำตาลอมส้ม (Claude) / เขียวอมฟ้าเข้ม (Codex) / เขียวเข้ม Cyber (Spark)
const uint16_t PANEL_BORDER[PANELS]    = { 0x4B0D, 0x6A06, 0x3CB1, 0x2D86 };
const uint16_t PANEL_TAG_COLOR[PANELS] = { 0x443E, 0xDBAA, 0x56F7, 0x7FE0 }; // ฟ้า / ส้ม / มินต์ / เขียวมะนาว NVIDIA
const char* PANEL_TAG[PANELS]          = { "ANTIGRAVITY", "CLAUDE CODE", "OPENAI CODEX", "SPARK LOCAL" };

const int LABEL_X = 8;

// -------------------------------------------------------------------------
// 1. ข้อมูล Antigravity IDE (แผง 0)
// -------------------------------------------------------------------------
bool ideRunning = true;
int geminiWeekly = 100, gemini5Hr = 100;
String geminiWeeklyReset = "", gemini5HrReset = "";
int claudeWeekly = 100, claude5Hr = 100;
String claudeWeeklyReset = "", claude5HrReset = "";

// -------------------------------------------------------------------------
// 2. ข้อมูล Claude Code (แผง 1)
// -------------------------------------------------------------------------
int ccWeekly = 100, cc5Hr = 100;
String ccWeeklyReset = "", cc5HrReset = "";
String tokenToday = "-", tokenTodayCache = "0";
String tokenWindow = "-", tokenWindowCache = "0";
String ccPlanType = "";

// -------------------------------------------------------------------------
// 3. ข้อมูล OpenAI Codex (แผง 2)
// -------------------------------------------------------------------------
bool codexConnected = false;
String codexPlanType = "";
int codexPrimaryPercent = -1;
String codexPrimaryReset = "";

// -------------------------------------------------------------------------
// 4. ข้อมูล Spark Local AI (แผง 3)
// -------------------------------------------------------------------------
struct ContainerInfo {
  String name = "";
  String cpuStr = "0.00%";
  int cpuVal = 0;
  bool running = true;
};

const int MAX_CONTAINERS = 3;
ContainerInfo sparkContainers[MAX_CONTAINERS];
int sparkContainerCount = 0;

bool sparkConnected = false;
String sparkModel = "Local AI";
String sparkStatus = "Ready";
int sparkCpu = 0;
int sparkRam = 0;
String sparkRamUsed = "0G";
String sparkRamRatio = "0G/0G";
String sparkTotalTokens = "0";
String sparkTodayTokens = "0";
String sparkSpeed = "";
String sparkSavedCost = "$0";
String sparkSavedThb = "฿0";

// -------------------------------------------------------------------------
// ข้อมูล Header รวม
// -------------------------------------------------------------------------
String panelLastUpdated[PANELS] = { "", "", "", "" };
bool panelRateLimited[PANELS] = { false, false, false, false };       // [1]=Claude Code(แผง1), [2]=Codex(แผง2)
String panelRateLimitReset[PANELS] = { "", "", "", "" };
String dataSource = "local";

// ประกาศฟังก์ชันล่วงหน้า
void drawWiFiIcon(int x, int y, bool connected, uint16_t bg);
void drawMiniBar(int x, int y, int w, int h, int percent, uint16_t borderCol, uint16_t fillCol, uint16_t bg);
void drawDashboardFull();
void updateDashboardValues();
void fetchAndDisplayQuota();
void drawPanelHeader(int g);
void drawAntigravityBody();
void drawClaudeCodeBody();
void drawCodexBody();
void drawSparkRowFull();
uint16_t quotaColor(int percent);
uint16_t cpuColor(int percent);

// =========================================================================
// Setup
// =========================================================================
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

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected! IP: " + WiFi.localIP().toString());
    fetchAndDisplayQuota(); // โหลดและวาดข้อมูลจริงทันที
  } else {
    drawDashboardFull();
  }
}

void loop() {
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
      drawWiFiIcon(290, 1, false, PANEL_BG[0]);
      lastWiFiConnected = false;
    }
    return;
  }

  HTTPClient http;
  http.begin(apiUrl);
  http.setTimeout(4000);

  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    Serial.println("Received payload: " + payload);

    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, payload);

    if (!error) {
      auto formatSubtext = [](String s) -> String {
        s.trim();
        if (s.equalsIgnoreCase("null")) return "";
        s.replace("Resets in ", "");
        s.replace(" days", "d");
        s.replace(" day", "d");
        s.replace(" hours", "h");
        s.replace(" hour", "h");
        s.replace(" minutes", "m");
        s.replace(" minute", "m");
        return s;
      };

      // 1. Antigravity IDE (แผง 0)
      ideRunning = doc.containsKey("ideRunning") ? doc["ideRunning"].as<bool>() : true;
      geminiWeekly = doc["geminiWeekly"] | 100;
      gemini5Hr = doc["gemini5Hr"] | 100;
      geminiWeeklyReset = formatSubtext(doc["geminiWeeklyReset"] | doc["geminiWeeklySubtext"] | "");
      gemini5HrReset = formatSubtext(doc["gemini5HrReset"] | doc["gemini5HrSubtext"] | "");
      claudeWeekly = doc["claudeWeekly"] | 100;
      claude5Hr = doc["claude5Hr"] | 100;
      claudeWeeklyReset = formatSubtext(doc["claudeWeeklyReset"] | doc["claudeWeeklySubtext"] | "");
      claude5HrReset = formatSubtext(doc["claude5HrReset"] | doc["claude5HrSubtext"] | "");
      panelLastUpdated[0] = doc["lastUpdated"] | "";

      // 2. Claude Code (แผง 1)
      ccWeekly = doc["ccWeekly"] | 100;
      cc5Hr = doc["cc5Hr"] | 100;
      ccWeeklyReset = formatSubtext(doc["ccWeeklyReset"] | doc["ccWeeklySubtext"] | "");
      cc5HrReset = formatSubtext(doc["cc5HrReset"] | doc["cc5HrSubtext"] | "");
      tokenToday = doc["ccTodayTokens"] | "-";
      tokenTodayCache = doc["ccTodayCache"] | "0";
      tokenWindow = doc["ccWindowTokens"] | "-";
      tokenWindowCache = doc["ccWindowCache"] | "0";
      ccPlanType = doc["ccPlanType"] | "";
      panelRateLimited[1] = doc["ccRateLimited"] | false;
      panelRateLimitReset[1] = doc["ccRateLimitReset"] | "";
      panelLastUpdated[1] = doc["ccLastUpdated"] | "";

      // 3. OpenAI Codex (แผง 2)
      codexConnected = doc["codexConnected"] | false;
      codexPlanType = doc["codexPlanType"] | "";
      codexPrimaryPercent = doc["codexPrimaryPercent"] | -1;
      codexPrimaryReset = formatSubtext(doc["codexPrimaryReset"] | "");
      panelRateLimited[2] = doc["codexRateLimited"] | false;
      panelRateLimitReset[2] = doc["codexRateLimitReset"] | "";
      panelLastUpdated[2] = doc["codexLastUpdated"] | "";

      // 4. Spark Local AI (แผง 3)
      sparkConnected = doc["sparkConnected"] | false;
      sparkModel = doc["sparkModel"] | "Local AI";
      if (sparkModel.startsWith("ollama-")) {
        sparkModel = sparkModel.substring(7);
      }
      sparkStatus = doc["sparkStatus"] | "Offline";
      sparkCpu = doc["sparkCpu"] | 0;
      sparkRam = doc["sparkRam"] | 0;
      sparkRamUsed = doc["sparkRamUsed"] | "0G";
      sparkRamRatio = doc["sparkRamRatio"] | "0G/0G";
      sparkTotalTokens = doc["sparkTotalTokens"] | "0";
      sparkTodayTokens = doc["sparkTodayTokens"] | "0";
      sparkSpeed = doc["sparkSpeed"] | "";
      sparkSavedCost = doc["sparkSavedCost"] | "$0";
      sparkSavedThb = doc["sparkSavedThb"] | "฿0";
      panelLastUpdated[3] = doc["sparkLastUpdated"] | "";

      auto parseCpuInfo = [](JsonVariant v, ContainerInfo &ci) {
        if (v.isNull()) {
          ci.cpuStr = "0.00%";
          ci.cpuVal = 0;
          return;
        }
        if (v.is<const char*>() || v.is<String>()) {
          String s = v.as<String>();
          s.trim();
          if (s.length() > 0 && !s.endsWith("%")) s += "%";
          ci.cpuStr = s;
          String numOnly = s;
          numOnly.replace("%", "");
          ci.cpuVal = constrain((int)round(numOnly.toFloat()), 0, 100);
        } else if (v.is<float>()) {
          float f = v.as<float>();
          char buf[16];
          snprintf(buf, sizeof(buf), "%.2f%%", f);
          ci.cpuStr = String(buf);
          ci.cpuVal = constrain((int)round(f), 0, 100);
        } else if (v.is<int>()) {
          int i = v.as<int>();
          ci.cpuStr = String(i) + ".00%";
          ci.cpuVal = constrain(i, 0, 100);
        }
      };

      sparkContainerCount = 0;
      if (doc["sparkContainers"].is<JsonArray>()) {
        JsonArray arr = doc["sparkContainers"].as<JsonArray>();
        for (JsonObject c : arr) {
          if (sparkContainerCount >= MAX_CONTAINERS) break;
          String cName = c["name"] | c["container"] | "";
          if (cName.length() > 0) {
            ContainerInfo ci;
            ci.name = cName;
            parseCpuInfo(c["cpu"], ci);
            ci.running = c.containsKey("running") ? c["running"].as<bool>() : true;
            if (!ci.running) {
              ci.cpuStr = "OFF";
              ci.cpuVal = 0;
            }
            sparkContainers[sparkContainerCount++] = ci;
          }
        }
      }

      if (sparkContainerCount == 0) {
        if (doc.containsKey("sparkC1Name") || doc.containsKey("sparkC1Cpu")) {
          ContainerInfo ci1;
          ci1.name = doc["sparkC1Name"] | "comfyui-spark";
          parseCpuInfo(doc["sparkC1Cpu"], ci1);
          ci1.running = true;
          sparkContainers[sparkContainerCount++] = ci1;
        }
        if (doc.containsKey("sparkC2Name") || doc.containsKey("sparkC2Cpu")) {
          ContainerInfo ci2;
          String defC2 = "ollama-" + sparkModel;
          ci2.name = doc["sparkC2Name"] | defC2;
          parseCpuInfo(doc["sparkC2Cpu"], ci2);
          ci2.running = true;
          sparkContainers[sparkContainerCount++] = ci2;
        }
      }

      dataSource = doc["dataSource"] | "local";

      if (isFirstDraw) {
        drawDashboardFull();
        isFirstDraw = false;
      } else {
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
// ฟังก์ชันวาด Mini Bar สำหรับแถบ % (แทนวงแหวนโดนัททั้งหมด เหมือน AIToken-eInk)
// =========================================================================
void drawMiniBar(int x, int y, int w, int h, int percent, uint16_t borderCol, uint16_t fillCol, uint16_t bg) {
  percent = constrain(percent, 0, 100);
  tft.drawRect(x, y, w, h, borderCol);
  int fillW = (percent * (w - 2)) / 100;
  if (fillW > 0) {
    tft.fillRect(x + 1, y + 1, fillW, h - 2, fillCol);
  }
  if (fillW < w - 2) {
    tft.fillRect(x + 1 + fillW, y + 1, (w - 2) - fillW, h - 2, bg);
  }
}

// สีตามปริมาณโควต้าคงเหลือ (ยิ่งเยอะยิ่งดี)
uint16_t quotaColor(int percent) {
  if (percent <= 20) return TFT_RED;
  if (percent <= 50) return TFT_ORANGE;
  return TFT_GREEN;
}

// สีตาม % การใช้งาน CPU/RAM (ยิ่งน้อยยิ่งดี)
uint16_t cpuColor(int percent) {
  if (percent >= 85) return TFT_RED;
  if (percent >= 60) return TFT_ORANGE;
  return TFT_GREEN;
}

// =========================================================================
// หัวแผง: ชื่อบริการ + Plan Type + เวลาอัปเดต + Rate Limit / Wi-Fi
// =========================================================================
void drawPanelHeader(int g) {
  int y = PANEL_Y[g];
  uint16_t bg = PANEL_BG[g];
  int textY = y + (HEADER_H[g] - 8) / 2;

  int clearW = (g == 0) ? 275 : 304;
  tft.fillRect(LABEL_X, y + 1, clearW, HEADER_H[g] - 2, bg);
  tft.setTextSize(1);
  tft.setTextDatum(TL_DATUM);

  // 1. ชื่อบริการ
  tft.setTextColor(PANEL_TAG_COLOR[g], bg);
  tft.drawString(PANEL_TAG[g], LABEL_X, textY);
  int curX = LABEL_X + tft.textWidth(PANEL_TAG[g]);

  // 1.1 ป้าย Package / Plan Type / สถานะ [CLOSED]
  if (g == 0 && !ideRunning) {
    tft.setTextColor(TFT_RED, bg);
    tft.drawString("[CLOSED]", curX + 6, textY);
    curX += tft.textWidth("[CLOSED]") + 6;
  } else if (g == 1 && ccPlanType.length() > 0) {
    String plan = ccPlanType;
    plan.toUpperCase();
    String badge = "[" + plan + "]";
    tft.setTextColor(0xFFE0, bg); // สีทองสำหรับ Claude
    tft.drawString(badge, curX + 4, textY);
    curX += tft.textWidth(badge) + 4;
  } else if (g == 2 && codexPlanType.length() > 0) {
    String plan = codexPlanType;
    plan.toUpperCase();
    String badge = "[" + plan + "]";
    uint16_t badgeCol = (plan == "PRO" || plan == "PLUS" || plan == "PROLITE") ? TFT_GREEN : 0x56F7;
    tft.setTextColor(badgeCol, bg);
    tft.drawString(badge, curX + 4, textY);
    curX += tft.textWidth(badge) + 4;
  }

  // 2. เวลา Last Update
  if (panelLastUpdated[g].length() > 0) {
    tft.setTextColor(TFT_DARKGREY, bg);
    tft.drawString(panelLastUpdated[g], curX + 5, textY);
  }

  // 3. ป้ายเตือน Rate Limit หรือสถานะ Spark
  if (g == 1 && panelRateLimited[1]) {
    String msg = "RATE LIMIT " + panelRateLimitReset[1];
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(TFT_RED, bg);
    tft.drawString(msg, 310, textY);
  } else if (g == 2 && panelRateLimited[2]) {
    String msg = "RATE LIMIT " + panelRateLimitReset[2];
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(TFT_RED, bg);
    tft.drawString(msg, 310, textY);
  } else if (g == 3) {
    String statChar = "-R";
    uint16_t statCol = TFT_CYAN;
    if (sparkStatus == "Active") {
      statChar = "-A";
      statCol = TFT_GREEN;
    } else if (!sparkConnected) {
      statChar = "-X";
      statCol = TFT_RED;
    }
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(statCol, bg);
    tft.drawString(statChar, 132, textY);

    if (sparkConnected && sparkTotalTokens.length() > 0 && sparkTotalTokens != "0") {
      tft.setTextColor(0x7FE0, bg);
      tft.drawString("All:", 152, textY);
      tft.setTextColor(TFT_WHITE, bg);
      tft.drawString(sparkTotalTokens, 178, textY);
    }

    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(TFT_DARKGREY, bg);
    tft.drawString("10.104.1.23", 310, textY);
  }

  // แผง 0 (อยู่บนสุด) แสดง dataSource (-M / -L) หน้าไอคอน Wi-Fi
  if (g == 0) {
    tft.setTextDatum(TR_DATUM);
    if (dataSource == "mirror") {
      tft.setTextColor(TFT_CYAN, bg);
      tft.drawString("-M", 286, textY);
    } else {
      tft.setTextColor(TFT_DARKGREY, bg);
      tft.drawString("-L", 286, textY);
    }
  }

  // เส้นแบ่งใต้ Header
  tft.drawFastHLine(2, y + HEADER_H[g], 316, PANEL_BORDER[g]);
}

// =========================================================================
// แผง 0: ANTIGRAVITY (Gemini & Claude/GPT Mini Bars)
// =========================================================================
void drawAntigravityBody() {
  int bodyTop = PANEL_Y[0] + HEADER_H[0] + 1;
  int bodyH = PANEL_H[0] - HEADER_H[0] - 2;
  uint16_t bg = PANEL_BG[0];
  uint16_t border = PANEL_BORDER[0];

  tft.fillRect(2, bodyTop, 316, bodyH, bg);

  if (!ideRunning) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(1);
    tft.setTextColor(0xFD20, bg); // ส้มอมเหลืองเตือน
    tft.drawString("IDE Not Running / Closed", 160, bodyTop + 14);
    tft.setTextColor(0xAD55, bg); // เทาสว่าง
    tft.drawString("Open Antigravity IDE to view live quota", 160, bodyTop + 28);
    return;
  }

  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);

  // Row 1: Gemini (y = bodyTop + 3)
  int r1Y = bodyTop + 3;
  tft.setTextColor(TFT_WHITE, bg);
  tft.drawString("Gemini", LABEL_X, r1Y);

  drawMiniBar(50, r1Y, 40, 8, geminiWeekly, border, quotaColor(geminiWeekly), bg);
  tft.setTextColor(TFT_WHITE, bg);
  tft.drawString(String(geminiWeekly) + "%", 94, r1Y);
  tft.setTextColor(TFT_DARKGREY, bg);
  tft.drawString(geminiWeeklyReset, 122, r1Y);

  tft.setTextColor(TFT_WHITE, bg);
  tft.drawString("5H", 168, r1Y);
  drawMiniBar(188, r1Y, 40, 8, gemini5Hr, border, quotaColor(gemini5Hr), bg);
  tft.setTextColor(TFT_WHITE, bg);
  tft.drawString(String(gemini5Hr) + "%", 232, r1Y);
  tft.setTextColor(TFT_DARKGREY, bg);
  tft.drawString(gemini5HrReset, 260, r1Y);

  // Row 2: Claude & GPT (y = bodyTop + 19)
  int r2Y = bodyTop + 19;
  tft.setTextColor(TFT_WHITE, bg);
  tft.drawString("Claude", LABEL_X, r2Y);

  drawMiniBar(50, r2Y, 40, 8, claudeWeekly, border, quotaColor(claudeWeekly), bg);
  tft.setTextColor(TFT_WHITE, bg);
  tft.drawString(String(claudeWeekly) + "%", 94, r2Y);
  tft.setTextColor(TFT_DARKGREY, bg);
  tft.drawString(claudeWeeklyReset, 122, r2Y);

  tft.setTextColor(TFT_WHITE, bg);
  tft.drawString("5H", 168, r2Y);
  drawMiniBar(188, r2Y, 40, 8, claude5Hr, border, quotaColor(claude5Hr), bg);
  tft.setTextColor(TFT_WHITE, bg);
  tft.drawString(String(claude5Hr) + "%", 232, r2Y);
  tft.setTextColor(TFT_DARKGREY, bg);
  tft.drawString(claude5HrReset, 260, r2Y);
}

// =========================================================================
// แผง 1: CLAUDE CODE (Plan Limits + Tokens Day / 5H)
// =========================================================================
void drawClaudeCodeBody() {
  int y0 = PANEL_Y[1] + HEADER_H[1] + 1;
  int bodyH = PANEL_H[1] - HEADER_H[1] - 2;
  uint16_t bg = PANEL_BG[1];
  uint16_t border = PANEL_BORDER[1];

  tft.fillRect(2, y0, 316, bodyH, bg);
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);

  // Row 1: Plan Limits (y = y0 + 3)
  int r1Y = y0 + 3;
  tft.setTextColor(TFT_WHITE, bg);
  tft.drawString("Limit", LABEL_X, r1Y);

  drawMiniBar(48, r1Y, 40, 8, ccWeekly, border, quotaColor(ccWeekly), bg);
  tft.setTextColor(TFT_WHITE, bg);
  tft.drawString(String(ccWeekly) + "%", 92, r1Y);
  tft.setTextColor(TFT_DARKGREY, bg);
  tft.drawString(ccWeeklyReset, 120, r1Y);

  tft.setTextColor(TFT_WHITE, bg);
  tft.drawString("5H", 168, r1Y);
  drawMiniBar(188, r1Y, 40, 8, cc5Hr, border, quotaColor(cc5Hr), bg);
  tft.setTextColor(TFT_WHITE, bg);
  tft.drawString(String(cc5Hr) + "%", 232, r1Y);
  tft.setTextColor(TFT_DARKGREY, bg);
  tft.drawString(cc5HrReset, 260, r1Y);

  // Row 2: Tokens (y = y0 + 19)
  int r2Y = y0 + 19;
  tft.setTextColor(0xDBAA, bg);
  tft.drawString("Tokens", LABEL_X, r2Y);

  tft.setTextColor(0xAD55, bg);
  tft.drawString("Day:", 48, r2Y);
  tft.setTextColor(TFT_WHITE, bg);
  tft.drawString(tokenToday, 74, r2Y);
  tft.setTextColor(TFT_DARKGREY, bg);
  tft.drawString("(c:" + tokenTodayCache + ")", 108, r2Y);

  tft.setTextColor(0xAD55, bg);
  tft.drawString("5H:", 168, r2Y);
  tft.setTextColor(TFT_WHITE, bg);
  tft.drawString(tokenWindow, 188, r2Y);
  tft.setTextColor(TFT_DARKGREY, bg);
  tft.drawString("(c:" + tokenWindowCache + ")", 222, r2Y);
}

// =========================================================================
// แผง 2: OPENAI CODEX (Weekly Limit Mini Bar)
// =========================================================================
void drawCodexBody() {
  int y0 = PANEL_Y[2] + HEADER_H[2] + 1;
  int bodyH = PANEL_H[2] - HEADER_H[2] - 2;
  uint16_t bg = PANEL_BG[2];
  uint16_t border = PANEL_BORDER[2];

  tft.fillRect(2, y0, 316, bodyH, bg);
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);

  int rY = y0 + 5;
  tft.setTextColor(TFT_WHITE, bg);
  tft.drawString("Weekly", LABEL_X, rY);

  if (!codexConnected) {
    tft.setTextColor(TFT_RED, bg);
    tft.drawString("Not Connected", 54, rY);
    return;
  }

  if (codexPrimaryPercent < 0) {
    tft.setTextColor(0x56F7, bg);
    tft.drawString("Syncing usage...", 54, rY);
    return;
  }

  String pctStr = String(codexPrimaryPercent) + "%";
  String rstStr = codexPrimaryReset;

  tft.setTextDatum(TR_DATUM);
  int rightX = 310;
  if (rstStr.length() > 0) {
    tft.setTextColor(TFT_DARKGREY, bg);
    tft.drawString(rstStr, rightX, rY);
    rightX -= tft.textWidth(rstStr) + 6;
  }

  tft.setTextColor(TFT_WHITE, bg);
  tft.drawString(pctStr, rightX, rY);
  rightX -= tft.textWidth(pctStr) + 6;

  int barX = 50;
  int barW = rightX - barX;
  if (barW < 30) barW = 30;

  drawMiniBar(barX, rY, barW, 8, codexPrimaryPercent, border, quotaColor(codexPrimaryPercent), bg);
  tft.setTextDatum(TL_DATUM);
}

// =========================================================================
// แผง 3: SPARK LOCAL (Host Telemetry, Docker Containers, Tokens & Savings)
// =========================================================================
void drawSparkRowFull() {
  int y0 = PANEL_Y[3] + HEADER_H[3] + 1;
  int bodyH = PANEL_H[3] - HEADER_H[3] - 2;
  uint16_t bg = PANEL_BG[3];
  uint16_t border = PANEL_BORDER[3];

  tft.fillRect(2, y0, 316, bodyH, bg);

  if (!sparkConnected) {
    tft.setTextDatum(TL_DATUM);
    tft.setTextSize(1);
    tft.setTextColor(TFT_RED, bg);
    tft.drawString("Status: Offline / Host Unreachable", LABEL_X, y0 + 18);
    tft.setTextColor(TFT_DARKGREY, bg);
    tft.drawString("Check Ollama or Local AI Server", LABEL_X, y0 + 36);
    return;
  }

  auto formatContainerName = [](const String &rawName) -> String {
    if (rawName.startsWith("ollama-")) {
      return rawName.substring(7);
    }
    return rawName;
  };

  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);

  // -------------------------------------------------------------
  // Row 1: Host Telemetry - RAM & CPU (y = y0 + 3)
  // -------------------------------------------------------------
  int r1Y = y0 + 3;
  tft.setTextColor(0x7FE0, bg);
  tft.drawString("Host:", LABEL_X, r1Y);

  tft.setTextColor(0xAD55, bg);
  tft.drawString("RAM:", LABEL_X + 34, r1Y);
  tft.setTextColor(TFT_WHITE, bg);
  tft.drawString(String(sparkRam) + "%", LABEL_X + 60, r1Y);
  tft.setTextColor(0x9CD3, bg);
  tft.drawString(sparkRamRatio, LABEL_X + 88, r1Y);

  drawMiniBar(LABEL_X + 144, r1Y, 36, 8, sparkRam, border, cpuColor(sparkRam), bg);

  tft.setTextColor(0xAD55, bg);
  tft.drawString("CPU:", 198, r1Y);
  tft.setTextColor(TFT_WHITE, bg);
  tft.drawString(String(sparkCpu) + "%", 226, r1Y);
  drawMiniBar(254, r1Y, 56, 8, sparkCpu, border, cpuColor(sparkCpu), bg);

  // -------------------------------------------------------------
  // Row 2: Container 1 (y = y0 + 21)
  // -------------------------------------------------------------
  int r2Y = y0 + 21;
  if (sparkContainerCount > 0) {
    String c1 = formatContainerName(sparkContainers[0].name);
    if (c1.length() > 24) c1 = c1.substring(0, 22) + "..";
    tft.setTextColor(TFT_WHITE, bg);
    tft.drawString(c1, LABEL_X, r2Y);

    if (sparkContainers[0].running) {
      drawMiniBar(196, r2Y, 40, 8, sparkContainers[0].cpuVal, border, cpuColor(sparkContainers[0].cpuVal), bg);
    }
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(sparkContainers[0].running ? TFT_WHITE : TFT_RED, bg);
    tft.drawString(sparkContainers[0].cpuStr, 310, r2Y);
    tft.setTextDatum(TL_DATUM);
  } else {
    tft.setTextColor(TFT_DARKGREY, bg);
    tft.drawString("Waiting for container stats...", LABEL_X, r2Y);
  }

  // -------------------------------------------------------------
  // Row 3: Container 2 / Model (y = y0 + 39)
  // -------------------------------------------------------------
  int r3Y = y0 + 39;
  if (sparkContainerCount > 1) {
    String c2 = formatContainerName(sparkContainers[1].name);
    if (c2.length() > 24) c2 = c2.substring(0, 22) + "..";
    tft.setTextColor(TFT_WHITE, bg);
    tft.drawString(c2, LABEL_X, r3Y);

    if (sparkContainers[1].running) {
      drawMiniBar(196, r3Y, 40, 8, sparkContainers[1].cpuVal, border, cpuColor(sparkContainers[1].cpuVal), bg);
    }
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(sparkContainers[1].running ? TFT_WHITE : TFT_RED, bg);
    tft.drawString(sparkContainers[1].cpuStr, 310, r3Y);
    tft.setTextDatum(TL_DATUM);
  } else if (sparkModel.length() > 0 && sparkModel != "Local AI") {
    String mName = formatContainerName(sparkModel);
    if (mName.length() > 28) mName = mName.substring(0, 26) + "..";
    tft.setTextColor(0xAD55, bg);
    tft.drawString("Model:", LABEL_X, r3Y);
    tft.setTextColor(TFT_WHITE, bg);
    tft.drawString(mName, LABEL_X + 42, r3Y);
  }

  // -------------------------------------------------------------
  // Row 4: Token Usage & Saved Cost (y = y0 + 58)
  // -------------------------------------------------------------
  int r4Y = y0 + 58;
  tft.setTextColor(0x7FE0, bg);
  tft.drawString("Tokens:", LABEL_X, r4Y);

  String tokStr = (sparkTodayTokens.length() > 0 && sparkTodayTokens != "0") ? sparkTodayTokens : "0";
  if (sparkSpeed.length() > 0) {
    String sp = sparkSpeed;
    sp.trim();
    if (!sp.endsWith("t/s")) sp += " t/s";
    tokStr += " (" + sp + ")";
  } else {
    tokStr += " (0 t/s)";
  }
  tft.setTextColor(TFT_WHITE, bg);
  tft.drawString(tokStr, LABEL_X + 46, r4Y);

  if (sparkSavedCost.length() > 0 && sparkSavedCost != "$0") {
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(0x56F7, bg);
    String saveStr = "Save: " + sparkSavedCost;
    if (sparkSavedThb.length() > 0 && sparkSavedThb != "฿0") {
      saveStr += " (" + sparkSavedThb + ")";
    }
    tft.drawString(saveStr, 310, r4Y);
    tft.setTextDatum(TL_DATUM);
  }
}

// =========================================================================
// วาดหน้าจอทั้งหมด (Full Render)
// =========================================================================
void drawDashboardFull() {
  tft.fillScreen(TFT_BLACK);

  for (int g = 0; g < PANELS; g++) {
    tft.fillRoundRect(1, PANEL_Y[g], 318, PANEL_H[g], 5, PANEL_BG[g]);
    tft.drawRoundRect(1, PANEL_Y[g], 318, PANEL_H[g], 5, PANEL_BORDER[g]);
    drawPanelHeader(g);
  }

  bool isConnected = (WiFi.status() == WL_CONNECTED);
  drawWiFiIcon(290, 1, isConnected, PANEL_BG[0]);
  lastWiFiConnected = isConnected;

  drawAntigravityBody();
  drawClaudeCodeBody();
  drawCodexBody();
  drawSparkRowFull();
}

// =========================================================================
// อัปเดตเฉพาะค่าที่เปลี่ยนแปลง ไม่ fillScreen ทั้งหน้า
// =========================================================================
void updateDashboardValues() {
  bool isConnected = (WiFi.status() == WL_CONNECTED);
  if (isConnected != lastWiFiConnected) {
    drawWiFiIcon(290, 1, isConnected, PANEL_BG[0]);
    lastWiFiConnected = isConnected;
  }

  for (int g = 0; g < PANELS; g++) {
    drawPanelHeader(g);
  }

  drawAntigravityBody();
  drawClaudeCodeBody();
  drawCodexBody();
  drawSparkRowFull();
}
