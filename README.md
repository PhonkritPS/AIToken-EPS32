# ESP32 AI Token Monitor (Antigravity Quota Dashboard)

A real-time hardware status monitor for AI Token quotas (Gemini Models & Claude/GPT Models) built with ESP32 and TFT LCD display.

---

## ✨ Features
- **Real-time Quota Tracking**: Monitors Weekly Limit & 5-Hour Limit for Gemini and Claude/GPT models.
- **Smooth Circular Progress Ring**: Clean high-resolution percentage arc rendering with color indicators (Green / Orange / Red).
- **Smart Change Detection (Zero Flicker)**: Refreshes only values that change without clearing the entire screen.
- **Countdown Reset Timers**: Displays accurate reset countdowns (e.g. in 4 hours, 15 min.).
- **Wi-Fi Status Indicator**: Real-time connection icon with auto-reconnect.
- **Single-Screen, Compact-Row Layout**: Gemini, Claude & GPT, Claude Code and OpenAI Codex all on one screen — one row per provider (label + two percent rings + short reset countdown), grouped into 3 color-coded background panels (Antigravity navy, Claude Code brown, Codex teal).
- **Claude Code Usage**: Plan limits (weekly / 5-hour remaining) plus token usage for today and the current 5-hour window.
  - Limits come from the same endpoint used by Claude Code's `/usage`, using the local login in `~/.claude/.credentials.json`.
  - Token counts are summed from Claude Code's local logs in `~/.claude/projects/**/*.jsonl`.
- **OpenAI Codex Usage**: Plan limits from the same undocumented endpoint the Codex CLI itself polls internally (`chatgpt.com/backend-api/wham/usage`), using the local login in `~/.codex/auth.json`. Free-tier accounts only expose one usage window (shown in the first slot); a second window (typically a weekly limit) appears automatically once upgraded to a paid ChatGPT plan.
- **429 Rate-Limit Banner**: If either Claude Code's or Codex's usage endpoint returns 429, that panel's header swaps to a red "RATE LIMIT &lt;time left&gt;" warning and reverts on its own once the `Retry-After` window elapses.
- **Last-Updated Timestamp**: Each panel header shows the last time its data was successfully refreshed.

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

> 📋 Setting up the bridge server on a **new machine**? Follow the step-by-step checklist in [SETUP.md](SETUP.md) instead — it's written to be followed top-to-bottom without prior context.

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

### 6. Auto-start in background (optional)
Register a Windows scheduled task that runs the bridge hidden at every logon (restarts automatically if it stops; log in `bridge.log`):
```powershell
powershell -ExecutionPolicy Bypass -File install_autostart.ps1              # install
powershell -ExecutionPolicy Bypass -File install_autostart.ps1 -Uninstall   # remove
```

### 7. Running the bridge on multiple machines with the same login?
If more than one machine (e.g. office + home) runs the bridge while logged into the **same** Claude Code / Codex account, each one independently polls that account's usage API, multiplying the real request rate and risking a 429 rate limit. Mark exactly one machine as **PRIMARY** (fetches live usage) and the rest as **SECONDARY** (skips those API calls, serves cached values instead — Antigravity data is unaffected since it's local):
```powershell
bridge_primary_on.bat    # this machine: PRIMARY (fetch live usage)
bridge_primary_off.bat   # this machine: SECONDARY (skip usage API calls)
```
Takes effect within ~10s, no restart needed. See [SETUP.md](SETUP.md) for details.
