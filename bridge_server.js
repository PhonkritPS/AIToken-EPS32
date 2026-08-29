const http = require('http');
const fs = require('fs');
const path = require('path');
const { execSync } = require('child_process');

const PORT = 5000;
const CACHE_FILE = path.join(__dirname, 'last_quota.json');

// โครงสร้างข้อมูลหลัก พร้อมเก็บ resetTime (ISO string) เพื่อใช้นับถอยหลังจริงแม้ปิด IDE
let quotaStore = {
  connected: false,
  ideRunning: false,
  geminiWeekly: 100,
  gemini5Hr: 100,
  geminiWeeklyResetTime: null,
  gemini5HrResetTime: null,
  claudeWeekly: 100,
  claude5Hr: 100,
  claudeWeeklyResetTime: null,
  claude5HrResetTime: null,
  lastUpdated: null
};

// โหลดข้อมูลล่าสุดจากไฟล์แคชขึ้นมาทันทีที่เซิร์ฟเวอร์เริ่มทำงาน
try {
  if (fs.existsSync(CACHE_FILE)) {
    const saved = JSON.parse(fs.readFileSync(CACHE_FILE, 'utf8'));
    quotaStore = { ...quotaStore, ...saved, ideRunning: false };
  }
} catch (e) {}

// ฟังก์ชันคำนวณเวลานับถอยหลังแบบเรียลไทม์ (Live Countdown)
function calculateCountdown(resetTime) {
  if (!resetTime) return { text: "", isExpired: true };
  const now = new Date();
  const reset = new Date(resetTime);
  const diffMs = reset - now;

  if (diffMs <= 0) {
    return { text: "", isExpired: true }; // หมดเวลารอแล้ว = โควต้ารีเฟรชกลับเป็น 100%
  }

  const diffMins = Math.floor(diffMs / (60 * 1000));
  const days = Math.floor(diffMins / (24 * 60));
  const hours = Math.floor((diffMins % (24 * 60)) / 60);
  const mins = diffMins % 60;

  let text = "";
  if (days > 0) {
    text = `in ${days} day${days > 1 ? 's' : ''}, ${hours} hour${hours > 1 ? 's' : ''}.`;
  } else if (hours > 0) {
    text = `in ${hours} hour${hours > 1 ? 's' : ''}, ${mins} min${mins > 1 ? 's' : ''}.`;
  } else {
    text = `in ${mins} min${mins > 1 ? 's' : ''}.`;
  }

  return { text, isExpired: false };
}

// Helper: ยิงดึงข้อมูลจาก Language Server ของ Antigravity
function fetchFromLS(port, csrfToken) {
  return new Promise((resolve) => {
    const data = JSON.stringify({});
    const req = http.request({
      hostname: '127.0.0.1',
      port: port,
      path: '/exa.language_server_pb.LanguageServerService/RetrieveUserQuotaSummary',
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        'x-codeium-csrf-token': csrfToken,
        'Content-Length': Buffer.byteLength(data)
      },
      timeout: 2000
    }, (res) => {
      let body = '';
      res.on('data', chunk => body += chunk);
      res.on('end', () => {
        if (res.statusCode === 200) {
          try {
            resolve(JSON.parse(body));
          } catch(e) { resolve(null); }
        } else {
          resolve(null);
        }
      });
    });
    req.on('error', () => resolve(null));
    req.on('timeout', () => { req.destroy(); resolve(null); });
    req.write(data);
    req.end();
  });
}

// ค้นหา Process ของ Antigravity Language Server และ Port โดยอัตโนมัติ
async function getAntigravityQuotaLive() {
  try {
    const psCmd = `Get-CimInstance Win32_Process | Where-Object { $_.Name -like '*language_server*' } | Select-Object ProcessId, CommandLine | ConvertTo-Json`;
    const out = execSync(`powershell -NoProfile -Command "${psCmd}"`, { stdio: ['pipe', 'pipe', 'ignore'] }).toString();
    const procs = JSON.parse(out);
    const procList = Array.isArray(procs) ? procs : [procs];

    const netstatOut = execSync('netstat -ano -p tcp', { stdio: ['pipe', 'pipe', 'ignore'] }).toString();

    for (const proc of procList) {
      if (!proc || !proc.CommandLine) continue;
      const tokenMatch = proc.CommandLine.match(/--csrf_token\s+([a-f0-9-]+)/);
      if (!tokenMatch) continue;
      const csrfToken = tokenMatch[1];
      const pid = proc.ProcessId;

      const lines = netstatOut.split('\n');
      for (const line of lines) {
        if (line.includes('LISTENING') && line.trim().endsWith(pid.toString())) {
          const m = line.trim().match(/TCP\s+(?:127\.0\.0\.1|0\.0\.0\.0|\[::\]):(\d+)/);
          if (m) {
            const port = parseInt(m[1]);
            const result = await fetchFromLS(port, csrfToken);
            if (result && result.response && result.response.groups) {
              return result.response;
            }
          }
        }
      }
    }
  } catch (err) {}
  return null;
}

// ประมวลผลข้อมูลสดจาก IDE
function processLiveData(raw) {
  if (!raw || !raw.groups) return;

  quotaStore.connected = true;
  quotaStore.ideRunning = true;
  quotaStore.lastUpdated = new Date().toLocaleTimeString('th-TH');

  for (const group of raw.groups) {
    const name = (group.displayName || "").toLowerCase();
    const isGemini = name.includes('gemini');
    const isClaude = name.includes('claude') || name.includes('gpt') || name.includes('3p');

    if (group.buckets) {
      for (const bucket of group.buckets) {
        const pct = Math.round((bucket.remainingFraction ?? 1) * 100);
        const isWeekly = bucket.window === 'weekly' || (bucket.bucketId && bucket.bucketId.includes('weekly')) || (bucket.displayName && bucket.displayName.toLowerCase().includes('weekly'));

        if (isGemini) {
          if (isWeekly) {
            quotaStore.geminiWeekly = pct;
            quotaStore.geminiWeeklyResetTime = bucket.resetTime || null;
          } else {
            quotaStore.gemini5Hr = pct;
            quotaStore.gemini5HrResetTime = bucket.resetTime || null;
          }
        } else if (isClaude) {
          if (isWeekly) {
            quotaStore.claudeWeekly = pct;
            quotaStore.claudeWeeklyResetTime = bucket.resetTime || null;
          } else {
            quotaStore.claude5Hr = pct;
            quotaStore.claude5HrResetTime = bucket.resetTime || null;
          }
        }
      }
    }
  }

  // บันทึกลง Disk เสมอ
  try {
    fs.writeFileSync(CACHE_FILE, JSON.stringify(quotaStore, null, 2));
  } catch (e) {}
}

// สร้าง Response ที่คำนวณเวลานับถอยหลัง ณ เสี้ยววินาทีปัจจุบัน (Real-Time Dynamic Response)
function buildDynamicResponse() {
  const gWeekly = calculateCountdown(quotaStore.geminiWeeklyResetTime);
  const g5Hr = calculateCountdown(quotaStore.gemini5HrResetTime);
  const cWeekly = calculateCountdown(quotaStore.claudeWeeklyResetTime);
  const c5Hr = calculateCountdown(quotaStore.claude5HrResetTime);

  // ถ้าหมดเวลา Reset แล้ว ปรับ % กลับเป็น 100% อัตโนมัติทันที
  const geminiWeeklyVal = (gWeekly.isExpired && quotaStore.geminiWeekly < 100) ? 100 : quotaStore.geminiWeekly;
  const gemini5HrVal = (g5Hr.isExpired && quotaStore.gemini5Hr < 100) ? 100 : quotaStore.gemini5Hr;
  const claudeWeeklyVal = (cWeekly.isExpired && quotaStore.claudeWeekly < 100) ? 100 : quotaStore.claudeWeekly;
  const claude5HrVal = (c5Hr.isExpired && quotaStore.claude5Hr < 100) ? 100 : quotaStore.claude5Hr;

  return {
    connected: quotaStore.connected || quotaStore.lastUpdated !== null,
    ideRunning: quotaStore.ideRunning,
    geminiWeekly: geminiWeeklyVal,
    gemini5Hr: gemini5HrVal,
    geminiWeeklySubtext: geminiWeeklyVal < 100 ? gWeekly.text : "",
    gemini5HrSubtext: gemini5HrVal < 100 ? g5Hr.text : "",
    claudeWeekly: claudeWeeklyVal,
    claude5Hr: claude5HrVal,
    claudeWeeklySubtext: claudeWeeklyVal < 100 ? cWeekly.text : "",
    claude5HrSubtext: claude5HrVal < 100 ? c5Hr.text : "",
    lastUpdated: quotaStore.lastUpdated
  };
}

// Background Poller ตรวจสอบสถานะทุกๆ 10 วินาที
async function pollLoop() {
  const raw = await getAntigravityQuotaLive();
  if (raw) {
    processLiveData(raw);
  } else {
    quotaStore.ideRunning = false;
  }

  const current = buildDynamicResponse();
  if (current.ideRunning) {
    process.stdout.write(`\r[${new Date().toLocaleTimeString()}] ✅ Antigravity IDE Online | Gemini: ${current.geminiWeekly}% (${current.gemini5Hr}%) | Claude: ${current.claudeWeekly}% (${current.claude5Hr}%)   `);
  } else {
    const sub = current.gemini5HrSubtext ? `(5Hr resets ${current.gemini5HrSubtext})` : `(Weekly resets ${current.geminiWeeklySubtext})`;
    process.stdout.write(`\r[${new Date().toLocaleTimeString()}] ⏸️  Antigravity IDE Closed | Countdown Active ${sub}                     `);
  }
}
setInterval(pollLoop, 10000);
pollLoop();

// Start HTTP Server
const server = http.createServer((req, res) => {
  res.setHeader('Access-Control-Allow-Origin', '*');
  res.setHeader('Content-Type', 'application/json');

  if (req.url === '/api/quota' || req.url === '/') {
    const responseData = buildDynamicResponse();
    res.writeHead(200);
    res.end(JSON.stringify(responseData, null, 2));
  } else {
    res.writeHead(404);
    res.end(JSON.stringify({ error: 'Not found' }));
  }
});

server.listen(PORT, '0.0.0.0', () => {
  console.log(`=======================================================`);
  console.log(`🚀 Antigravity AI Quota Bridge Server is RUNNING!`);
  console.log(`📡 Local API endpoint: http://localhost:${PORT}/api/quota`);
  console.log(`🌐 For ESP32 on Wi-Fi: http://192.168.10.33:${PORT}/api/quota`);
  console.log(`⏱️  Auto Real-time Countdown: Enabled even when IDE is closed!`);
  console.log(`=======================================================`);
});
