# ESP32 AI Token Monitor (Antigravity Quota Dashboard)

A real-time hardware status monitor for AI Token quotas (Gemini Models & Claude/GPT Models) built with ESP32 and TFT LCD display.

---

## ✨ Features
- **Real-time Quota Tracking**: Monitors Weekly Limit & 5-Hour Limit for Gemini and Claude/GPT models.
- **Smooth Circular Progress Ring**: Clean high-resolution percentage arc rendering with color indicators (Green / Orange / Red).
- **Smart Change Detection (Zero Flicker)**: Refreshes only values that change without clearing the entire screen.
- **Countdown Reset Timers**: Displays accurate reset countdowns (e.g. in 4 hours, 15 min.).
- **Wi-Fi Status Indicator**: Real-time connection icon with auto-reconnect.
- **Single-Screen Layout**: Gemini, Claude & GPT (Antigravity) and Claude Code on one screen, each as a pair of half-width cards (Weekly | 5 Hour). Antigravity (navy panel) and Claude Code (warm brown panel) are grouped on separate background panels.
- **Claude Code Usage**: Plan limits (weekly / 5-hour remaining) plus token usage for today and the current 5-hour window (in / out / cache).
  - Limits come from the same endpoint used by Claude Code's `/usage`, using the local login in `~/.claude/.credentials.json` (polled every 60s).
  - Token counts are summed from Claude Code's local logs in `~/.claude/projects/**/*.jsonl`.

---

## 📁 Repository Structure
`	ext
ESP32-AITokenMonitor/
├── AIToken.ino           # Main Arduino / ESP32 source code
├── bridge_server.js      # Node.js bridge server to query Antigravity LS
├── start_bridge.bat      # One-click starter script for Windows
├── last_quota.json       # Cached quota data
├── User_Setup.h          # TFT_eSPI pinout and display driver config
├── User_Setup_Select.h   # TFT_eSPI setup selector
└── README.md             # Documentation
`

---

## 🚀 Getting Started

### 1. Requirements
- **Hardware**: ESP32 Development Board + TFT SPI Display (ILI9341 / ST7789 or compatible).
- **Software**: 
  - Arduino IDE (ESP32 Board Support installed)
  - Node.js (v16+)

### 2. Arduino Libraries
Install the following libraries via **Arduino Library Manager**:
1. TFT_eSPI by Bodmer
2. ArduinoJson by Benoit Blanchon (v6 or v7)

### 3. Display Configuration (User_Setup.h)
Copy User_Setup.h from this repository to your Arduino libraries folder:
Documents/Arduino/libraries/TFT_eSPI/User_Setup.h

### 4. Configure AIToken.ino
Open AIToken.ino in Arduino IDE and set your Wi-Fi credentials and PC IP:
`cpp
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
const char* apiUrl = "http://<YOUR_PC_IP>:5000/api/quota";
`

### 5. Run the Bridge Server
On your computer where Antigravity IDE is running:
- Double click start_bridge.bat or run:
`ash
node bridge_server.js
`
The server will start listening on port 5000 and provide the /api/quota endpoint for ESP32.
