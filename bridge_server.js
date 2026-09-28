const http = require('http');
const fs = require('fs');
const path = require('path');
const os = require('os');
const { execSync } = require('child_process');

const PORT = 5000;
const CACHE_FILE = path.join(__dirname, 'last_quota.json');

// Claude Code: โฟลเดอร์ config (รองรับ CLAUDE_CONFIG_DIR เหมือนตัว CLI)
const CLAUDE_DIR = process.env.CLAUDE_CONFIG_DIR || path.join(os.homedir(), '.claude');
const CLAUDE_CREDS_FILE = path.join(CLAUDE_DIR, '.credentials.json');
const CLAUDE_PROJECTS_DIR = path.join(CLAUDE_DIR, 'projects');
const CLAUDE_USAGE_INTERVAL = 120000; // ดึง % โควต้าจาก Anthropic ทุก 2 นาที (ทดลอง: 60 วิเจอ 429 หนัก, 180 วิสะอาด — ถ้า 120 วิโดนบ่อย จอจะขึ้น "RATE LIMIT" เตือนเอง)
// โฟลเดอร์ใน projects ที่ไม่ใช่ session ของ Claude Code (เช่น scratch workspace ของ Claude Desktop) ไม่นำมานับ Token
const CLAUDE_EXCLUDE_PROJECT_DIRS = [/scratch-workspaces/i];

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
  // Claude Code (โควต้าของแพลน Claude.ai ที่ Claude Code ใช้)
  ccWeekly: 100,
  cc5Hr: 100,
  ccWeeklyResetTime: null,
  cc5HrResetTime: null,
  ccLastUpdated: null,
  lastUpdated: null
};

// ข้อมูล Token ของ Claude Code ที่นับจากไฟล์ log (ไม่เก็บลงแคช คำนวณใหม่ทุกรอบ)
let ccTokens = {
  window: { input: 0, output: 0, cacheCreate: 0, cacheRead: 0 },
  today: { input: 0, output: 0, cacheCreate: 0, cacheRead: 0 }
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
  if (!resetTime) return { text: "", short: "", isExpired: true };
  const now = new Date();
  const reset = new Date(resetTime);
  const diffMs = reset - now;

  if (diffMs <= 0) {
    return { text: "", short: "", isExpired: true }; // หมดเวลารอแล้ว = โควต้ารีเฟรชกลับเป็น 100%
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

  // แบบสั้นสำหรับการ์ดครึ่งจอ เช่น "4d 23h", "2h 15m", "34m"
  const short = days > 0 ? `${days}d ${hours}h` : (hours > 0 ? `${hours}h ${mins}m` : `${mins}m`);

  return { text, short, isExpired: false };
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

// =========================================================================
// Claude Code: ดึง % โควต้า 5 ชั่วโมง / รายสัปดาห์ จาก Anthropic (endpoint เดียวกับคำสั่ง /usage)
// ใช้ access token ที่ Claude Code เก็บไว้ในเครื่อง (Claude Code จะ refresh token เองเมื่อหมดอายุ)
// =========================================================================
let lastCcUsageFetch = 0;
let ccRetryUntil = 0;

async function fetchClaudeCodeUsage() {
  const now = Date.now();
  if (now < ccRetryUntil) return;
  if (now - lastCcUsageFetch < CLAUDE_USAGE_INTERVAL) return;
  lastCcUsageFetch = now;

  try {
    const creds = JSON.parse(fs.readFileSync(CLAUDE_CREDS_FILE, 'utf8')).claudeAiOauth;
    if (!creds || !creds.accessToken) return;

    const res = await fetch('https://api.anthropic.com/api/oauth/usage', {
      headers: {
        'Authorization': `Bearer ${creds.accessToken}`,
        'anthropic-beta': 'oauth-2025-04-20'
      },
      signal: AbortSignal.timeout(5000)
    });

    if (!res.ok) {
      if (res.status === 429) {
        const retrySec = parseInt(res.headers.get('retry-after') || '1800', 10);
        ccRetryUntil = Date.now() + (retrySec * 1000);
        const retryTime = new Date(ccRetryUntil).toLocaleTimeString();
        process.stdout.write(`\n[${new Date().toLocaleTimeString()}] Claude Code API rate-limited (429). Pausing requests until ${retryTime} (~${Math.ceil(retrySec / 60)} min).\n`);
      }
      return;
    }

    const data = await res.json();
    if (data.five_hour) {
      quotaStore.cc5Hr = Math.max(0, Math.round(100 - (data.five_hour.utilization ?? 0)));
      quotaStore.cc5HrResetTime = data.five_hour.resets_at || null;
    }
    if (data.seven_day) {
      quotaStore.ccWeekly = Math.max(0, Math.round(100 - (data.seven_day.utilization ?? 0)));
      quotaStore.ccWeeklyResetTime = data.seven_day.resets_at || null;
    }
    quotaStore.ccLastUpdated = new Date().toLocaleTimeString('th-TH');

    try {
      fs.writeFileSync(CACHE_FILE, JSON.stringify(quotaStore, null, 2));
    } catch (e) {}
  } catch (e) {}
}

// =========================================================================
// Claude Code: นับ Token จากไฟล์ log (~/.claude/projects/**/*.jsonl)
// แคชผลแยกตามไฟล์ อ่านใหม่เฉพาะไฟล์ที่มีการเปลี่ยนแปลง
// =========================================================================
const jsonlCache = new Map(); // filePath -> { mtimeMs, size, entries: [{ key, ts, input, output, cacheCreate, cacheRead }] }

function listJsonlFiles(dir, sinceMs, out = []) {
  let items;
  try {
    items = fs.readdirSync(dir, { withFileTypes: true });
  } catch (e) {
    return out;
  }
  for (const item of items) {
    const full = path.join(dir, item.name);
    if (item.isDirectory()) {
      if (CLAUDE_EXCLUDE_PROJECT_DIRS.some((re) => re.test(item.name))) continue;
      listJsonlFiles(full, sinceMs, out);
    } else if (item.name.endsWith('.jsonl')) {
      try {
        const st = fs.statSync(full);
        if (st.mtimeMs >= sinceMs) out.push({ file: full, st });
      } catch (e) {}
    }
  }
  return out;
}

function parseJsonlFile(file) {
  const entries = [];
  let lastContext = 0;
  let text;
  try {
    text = fs.readFileSync(file, 'utf8');
  } catch (e) {
    return { entries, lastContext: 0 };
  }
  for (const line of text.split('\n')) {
    if (!line.includes('"usage"')) continue;
    try {
      const obj = JSON.parse(line);
      const msg = obj.message;
      if (!msg || !msg.usage || !obj.timestamp) continue;
      const u = msg.usage;
      const inTok = u.input_tokens || 0;
      const outTok = u.output_tokens || 0;
      const cCreate = u.cache_creation_input_tokens || 0;
      const cRead = u.cache_read_input_tokens || 0;
      entries.push({
        key: `${msg.id || ''}:${obj.requestId || ''}`,
        ts: Date.parse(obj.timestamp),
        input: inTok,
        output: outTok,
        cacheCreate: cCreate,
        cacheRead: cRead
      });
      lastContext = inTok + cCreate + cRead;
    } catch (e) {}
  }
  return { entries, lastContext };
}

function scanClaudeCodeTokens() {
  const now = Date.now();
  const startOfToday = new Date();
  startOfToday.setHours(0, 0, 0, 0);

  // ช่วงเวลา 5 ชั่วโมงปัจจุบัน = เวลา reset ลบ 5 ชั่วโมง (ถ้าไม่รู้ ใช้ 5 ชั่วโมงย้อนหลัง)
  const resetMs = quotaStore.cc5HrResetTime ? Date.parse(quotaStore.cc5HrResetTime) : NaN;
  const windowStart = (resetMs > now) ? resetMs - 5 * 3600 * 1000 : now - 5 * 3600 * 1000;
  const sinceMs = Math.min(startOfToday.getTime(), windowStart);

  const files = listJsonlFiles(CLAUDE_PROJECTS_DIR, sinceMs);
  const empty = () => ({ input: 0, output: 0, cacheCreate: 0, cacheRead: 0 });
  const win = empty();
  const today = empty();
  const seen = new Set(); // กันนับซ้ำ (ข้อความเดียวกันอาจถูกเขียนหลายบรรทัด/หลายไฟล์)
  let activeContext = 0;
  let latestMtime = 0;

  for (const { file, st } of files) {
    let cached = jsonlCache.get(file);
    if (!cached || cached.mtimeMs !== st.mtimeMs || cached.size !== st.size) {
      const parsed = parseJsonlFile(file);
      cached = { mtimeMs: st.mtimeMs, size: st.size, entries: parsed.entries, lastContext: parsed.lastContext };
      jsonlCache.set(file, cached);
    }
    if (st.mtimeMs > latestMtime && cached.lastContext > 0) {
      latestMtime = st.mtimeMs;
      activeContext = cached.lastContext;
    }
    for (const e of cached.entries) {
      if (e.ts < sinceMs || seen.has(e.key)) continue;
      seen.add(e.key);
      const add = (t) => {
        t.input += e.input;
        t.output += e.output;
        t.cacheCreate += e.cacheCreate;
        t.cacheRead += e.cacheRead;
      };
      if (e.ts >= windowStart) add(win);
      if (e.ts >= startOfToday.getTime()) add(today);
    }
  }

  ccTokens = { window: win, today, activeContext };
}

// จัดรูปแบบตัวเลข Token ให้สั้น เช่น 1234 -> 1.2k, 1234567 -> 1.23M
function formatTokens(n) {
  if (!n || n <= 0) return "0";
  if (n < 1000) return `${n}`;
  if (n < 1e6) return `${(n / 1e3).toFixed(n < 1e4 ? 1 : 0)}k`;
  if (n < 1e9) return `${(n / 1e6).toFixed(n < 1e7 ? 2 : 1)}M`;
  return `${(n / 1e9).toFixed(n < 1e10 ? 2 : 1)}B`;
}

// New tokens: input (รวม cache creation) + output (ไม่นำ cacheRead ประวัติเดิมที่วนซ้ำมาบวกทับถม)
function tokenTotal(t) {
  return t.input + t.cacheCreate + t.output;
}

// ส่งรายละเอียด token แยกเป็นฟิลด์ (prefix เช่น "ccWindow" -> ccWindowTokens, ccWindowIn, ...)
function tokenFields(prefix, t) {
  return {
    [`${prefix}Tokens`]: formatTokens(tokenTotal(t)),
    [`${prefix}In`]: formatTokens(t.input + t.cacheCreate),
    [`${prefix}Out`]: formatTokens(t.output),
    [`${prefix}Cache`]: formatTokens(t.cacheRead)
  };
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

  const ccW = calculateCountdown(quotaStore.ccWeeklyResetTime);
  const cc5 = calculateCountdown(quotaStore.cc5HrResetTime);

  // สำหรับ Claude Code: ใช้ % โควต้าจริงจาก Anthropic API
  const ccWeeklyVal = quotaStore.ccWeekly;
  const cc5HrVal = quotaStore.cc5Hr;

  // สถานะโดน 429 Rate Limit: true ระหว่างที่รอครบ Retry-After แล้วหายไปเองทันทีที่ครบเวลา
  // (คำนวณสดทุกครั้งที่ตอบ request ไม่ผูกกับ field เดิมใน quotaStore เพื่อให้จอไหนก็ใช้ได้)
  const ccRateLimitedNow = Date.now() < ccRetryUntil;
  let ccRateLimitReset = "";
  if (ccRateLimitedNow) {
    const mins = Math.max(1, Math.ceil((ccRetryUntil - Date.now()) / 60000));
    ccRateLimitReset = mins >= 60 ? `${Math.floor(mins / 60)}h ${mins % 60}m` : `${mins}m`;
  }

  return {
    connected: quotaStore.connected || quotaStore.lastUpdated !== null,
    ideRunning: quotaStore.ideRunning,
    geminiWeekly: geminiWeeklyVal,
    gemini5Hr: gemini5HrVal,
    geminiWeeklySubtext: geminiWeeklyVal < 100 ? gWeekly.text : "",
    gemini5HrSubtext: gemini5HrVal < 100 ? g5Hr.text : "",
    geminiWeeklyReset: geminiWeeklyVal < 100 ? gWeekly.short : "",
    gemini5HrReset: gemini5HrVal < 100 ? g5Hr.short : "",
    claudeWeekly: claudeWeeklyVal,
    claude5Hr: claude5HrVal,
    claudeWeeklySubtext: claudeWeeklyVal < 100 ? cWeekly.text : "",
    claude5HrSubtext: claude5HrVal < 100 ? c5Hr.text : "",
    claudeWeeklyReset: claudeWeeklyVal < 100 ? cWeekly.short : "",
    claude5HrReset: claude5HrVal < 100 ? c5Hr.short : "",
    ccWeekly: ccWeeklyVal,
    cc5Hr: cc5HrVal,
    ccWeeklySubtext: ccWeeklyVal < 100 ? ccW.text : "",
    cc5HrSubtext: cc5HrVal < 100 ? cc5.text : "",
    ccWeeklyReset: ccWeeklyVal < 100 ? ccW.short : "",
    cc5HrReset: cc5HrVal < 100 ? cc5.short : "",
    ccActiveContext: formatTokens(ccTokens.activeContext || 0),
    ...tokenFields('ccWindow', ccTokens.window),
    ...tokenFields('ccToday', ccTokens.today),
    ccLastUpdated: quotaStore.ccLastUpdated,
    // สถานะแจ้งเตือน 429: จอไหนอยากโชว์เตือนก็เช็ค ccRateLimited ได้เลย ไม่ต้องรู้เรื่อง Retry-After เอง
    ccRateLimited: ccRateLimitedNow,
    ccRateLimitReset: ccRateLimitReset,
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

  await fetchClaudeCodeUsage();
  try {
    scanClaudeCodeTokens();
  } catch (e) {}

  const current = buildDynamicResponse();
  const time = new Date().toLocaleTimeString();
  const ctxStr = current.ccActiveContext && current.ccActiveContext !== '0' ? ` (Ctx ${current.ccActiveContext})` : '';
  const ccInfo = `CC ${current.cc5Hr}%/${current.ccWeekly}% | Tok ${current.ccWindowTokens}${ctxStr}`;
  if (current.ideRunning) {
    writeStatusLine(`[${time}] ON  | Gemini ${current.geminiWeekly}%(${current.gemini5Hr}%) | Claude ${current.claudeWeekly}%(${current.claude5Hr}%) | ${ccInfo}`);
  } else {
    const sub = current.gemini5HrSubtext ? `5Hr ${current.gemini5HrSubtext}` : `Wk ${current.geminiWeeklySubtext}`;
    writeStatusLine(`[${time}] OFF | Reset ${sub} | ${ccInfo}`);
  }
}

// ปิดการตัดบรรทัดอัตโนมัติของ terminal (DECAWM) ข้อความที่ยาวเกินจะถูกตัดที่ขอบจอ ไม่ขึ้นบรรทัดใหม่
process.stdout.write('\x1b[?7l');
process.on('exit', () => process.stdout.write('\x1b[?7h\n'));
process.on('SIGINT', () => process.exit(0));

// เขียนสถานะทับบรรทัดเดิม (ตัดให้พอดีความกว้าง terminal เพื่อไม่ให้ขึ้นบรรทัดใหม่)
function writeStatusLine(text) {
  // \r = กลับต้นบรรทัด, \x1b[2K = ล้างทั้งบรรทัด
  process.stdout.write(`\r\x1b[2K${text}`);
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
  const nets = os.networkInterfaces();
  const ips = [];
  for (const name of Object.keys(nets)) {
    for (const net of nets[name]) {
      if (net.family === 'IPv4' && !net.internal) {
        ips.push(`${net.address} (${name})`);
      }
    }
  }

  console.log(`=======================================================`);
  console.log(`🚀 Antigravity AI Quota Bridge Server is RUNNING!`);
  console.log(`📡 Local API endpoint: http://localhost:${PORT}/api/quota`);
  ips.forEach(ip => {
    console.log(`🌐 For ESP32 on Wi-Fi: http://${ip.split(' ')[0]}:${PORT}/api/quota  [${ip.split(' ')[1] || ''}]`);
  });
  console.log(`⏱️  Auto Real-time Countdown: Enabled even when IDE is closed!`);
  console.log(`=======================================================`);
});
