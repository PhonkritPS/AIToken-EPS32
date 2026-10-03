const http = require('http');
const fs = require('fs');
const path = require('path');
const os = require('os');
const { execSync, exec } = require('child_process');

const PORT = 5000;
process.on('uncaughtException', (err) => {
  console.error(`[${new Date().toLocaleTimeString()}] Uncaught Exception:`, err.message || err);
});
process.on('unhandledRejection', (reason) => {
  console.error(`[${new Date().toLocaleTimeString()}] Unhandled Rejection:`, reason);
});

const CACHE_FILE = path.join(__dirname, 'last_quota.json');

// =========================================================================
// Primary / Secondary + Peer Mirror (ป้องกันหลายเครื่องที่ login บัญชีเดียวกันยิง API
// เช็คโควต้า Claude Code / Codex พร้อมกัน จนโดน Rate Limit ง่ายขึ้น)
// อ่านไฟล์ bridge.local.json ที่ __dirname (ไม่ commit ขึ้น git เพราะต้องตั้งต่างกันในแต่ละเครื่อง):
//   { "fetchExternalUsage": false }        -> Secondary แบบ manual ข้ามการยิง API เสมอ ใช้ bridge_primary_on/off.bat
//   { "peerBridgeUrl": "http://IP:5000/api/quota" }
//       -> ก่อนยิง API จริงเอง ลองอ่านค่าจากเครื่องนี้ผ่าน VPN/LAN ก่อน
//          ถ้าอ่านได้ -> ใช้ค่า cc*/codex* จากเครื่องนั้นแทน (ไม่ยิง API เอง)
//          ถ้าอ่านไม่ได้ (ปิดเครื่อง/VPN หลุด) -> fallback มายิง API เองอัตโนมัติ
//       ตั้งเฉพาะเครื่อง "รอง" (เช่นคอมที่บ้าน) ชี้ไปคอม "หลัก" (เช่นคอมที่ทำงาน)
//       เครื่องหลักไม่ต้องตั้งอะไรเลย ยิง API ของตัวเองตามปกติเสมอ ไม่ต้องรู้จักเครื่องรอง
// อ่านไฟล์ใหม่ทุกครั้งจึงสลับ/แก้ได้โดยไม่ต้อง restart bridge (มีผลภายในรอบโพลถัดไป)
// =========================================================================
const LOCAL_CONFIG_FILE = path.join(__dirname, 'bridge.local.json');
const PEER_TIMEOUT_MS = 3000;

function readLocalConfig() {
  try {
    if (!fs.existsSync(LOCAL_CONFIG_FILE)) return {};
    return JSON.parse(fs.readFileSync(LOCAL_CONFIG_FILE, 'utf8'));
  } catch (e) {
    return {};
  }
}

function isPrimaryForExternalUsage() {
  return readLocalConfig().fetchExternalUsage !== false; // ต้องเขียน false ชัดเจนเท่านั้นถึงจะปิด
}

function getPeerBridgeUrl() {
  const url = readLocalConfig().peerBridgeUrl;
  return (typeof url === 'string' && url.trim()) ? url.trim() : null;
}

// ฟิลด์ cc*/codex* ที่มาจากการยิง API จริง (rate-limited) เท่านั้นที่ mirror จาก peer ได้
// ไม่รวม token count (ccWindowTokens ฯลฯ) เพราะนับจาก log ในเครื่องนั้นๆ เป็นข้อมูลเฉพาะเครื่อง ไม่ใช่ของบัญชีรวม
const PEER_MIRROR_FIELDS = [
  'ccWeekly', 'cc5Hr', 'ccWeeklySubtext', 'cc5HrSubtext', 'ccWeeklyReset', 'cc5HrReset',
  'ccRateLimited', 'ccRateLimitReset', 'ccLastUpdated', 'ccPlanType',
  'codexConnected', 'codexPlanType', 'codexPrimaryPercent', 'codexPrimaryReset',
  'codexSecondaryPercent', 'codexSecondaryReset', 'codexRateLimited', 'codexRateLimitReset', 'codexLastUpdated'
];

let peerMirrorData = null; // ค่าล่าสุดที่อ่านได้จาก peer สำเร็จ (null = ยังไม่เคยสำเร็จ/ปิดฟีเจอร์นี้)
let peerReachableNow = false;

async function tryMirrorFromPeer() {
  const peerUrl = getPeerBridgeUrl();
  if (!peerUrl) {
    peerReachableNow = false;
    return false;
  }

  try {
    const res = await fetch(peerUrl, { signal: AbortSignal.timeout(PEER_TIMEOUT_MS) });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const data = await res.json();

    const mirrored = {};
    for (const key of PEER_MIRROR_FIELDS) {
      if (data[key] !== undefined) mirrored[key] = data[key];
    }
    peerMirrorData = mirrored;
    peerReachableNow = true;
    return true;
  } catch (e) {
    // เครื่องหลักปิดอยู่ / VPN หลุด / เน็ตช้าเกิน timeout -> ไม่เจอ ให้ fallback ไปยิง API เอง
    peerReachableNow = false;
    return false;
  }
}

// Spark Local AI (10.104.1.23)
const SPARK_HOST = '10.104.1.23';
const SPARK_SSH_USER = 'admin';
const SPARK_USAGE_INTERVAL = 10000; // ตรวจสอบทุก 10 วินาที
let lastSparkPoll = 0;
let isSparkPolling = false;

// Claude Code: โฟลเดอร์ config (รองรับ CLAUDE_CONFIG_DIR เหมือนตัว CLI)
const CLAUDE_DIR = process.env.CLAUDE_CONFIG_DIR || path.join(os.homedir(), '.claude');
const CLAUDE_CREDS_FILE = path.join(CLAUDE_DIR, '.credentials.json');
const CLAUDE_PROJECTS_DIR = path.join(CLAUDE_DIR, 'projects');
const CLAUDE_USAGE_INTERVAL = 180000; // ดึง % โควต้าจาก Anthropic ทุก 3 นาที (180 วิ เพื่อป้องกัน 429 Rate Limit)
// โฟลเดอร์ใน projects ที่ไม่ใช่ session ของ Claude Code (เช่น scratch workspace ของ Claude Desktop) ไม่นำมานับ Token
const CLAUDE_EXCLUDE_PROJECT_DIRS = [/scratch-workspaces/i];

// OpenAI Codex CLI: โฟลเดอร์ config (รองรับ CODEX_HOME เหมือนตัว CLI)
const CODEX_DIR = process.env.CODEX_HOME || path.join(os.homedir(), '.codex');
const CODEX_AUTH_FILE = path.join(CODEX_DIR, 'auth.json');
// เท่ากับรอบที่ Codex CLI ตัวจริงโพลเอง (เจอจากการ reverse-engineer ว่า client เรียก endpoint นี้ทุก 60 วิ)
const CODEX_USAGE_INTERVAL = 120000; // ดึง % โควต้าจาก OpenAI Codex ทุก 2 นาที (120 วิ เพื่อความปลอดภัย)

// 🧪 จำลองหน้าต่างที่สอง (secondary_window) ของ Codex ไว้ดูตัวอย่างก่อนอัปเกรดแพลนจริง
// ใช้เฉพาะตอนแพลนปัจจุบันยังไม่มี secondary_window (เช่น Free) — พอไหนได้ค่าจริงจาก API
// (หลังอัปเกรดเป็น Plus/Pro) จะใช้ค่าจริงแทนทันทีโดยไม่ต้องแก้อะไร ปิดจำลองได้ด้วยการตั้งเป็น false
const CODEX_SIMULATE_SECONDARY = false;

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
  ccPlanType: null,
  ccLastUpdated: null,
  // OpenAI Codex (โควต้าตามแพลน ChatGPT ที่ผูกกับ Codex CLI)
  codexConnected: false,
  codexPlanType: null,
  codexPrimaryPercent: null,   // null = ยังไม่เคยดึงสำเร็จ (แพลน Free จะมีแค่ช่องนี้ ไม่มี secondary)
  codexPrimaryResetTime: null,
  codexSecondaryPercent: null, // null = แพลนนี้ไม่มีหน้าต่างที่สอง (เช่น Free) หรือยังไม่เคยดึงสำเร็จ
  codexSecondaryResetTime: null,
  codexLastUpdated: null,
  // Spark Local AI (Ollama on 10.104.1.23)
  sparkConnected: false,
  sparkModel: 'Local AI',
  sparkStatus: 'Offline',
  sparkCpu: 0,
  sparkRam: 0,
  sparkRamUsed: '0G',
  sparkRamTotal: '0G',
  sparkRamRatio: '0G/0G',
  sparkTotalTokens: '0',
  sparkTodayTokens: '0',
  sparkSavedCost: '$0',
  sparkSavedThb: '฿0',
  sparkSpeed: '',
  sparkContainers: [],
  sparkC1Name: '',
  sparkC1Cpu: '',
  sparkC2Name: '',
  sparkC2Cpu: '',
  sparkLastUpdated: null,
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
  if (fs.existsSync(CLAUDE_CREDS_FILE)) {
    const creds = JSON.parse(fs.readFileSync(CLAUDE_CREDS_FILE, 'utf8')).claudeAiOauth;
    if (creds && creds.subscriptionType) quotaStore.ccPlanType = creds.subscriptionType;
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
  if (!isPrimaryForExternalUsage()) return; // เครื่อง Secondary: ไม่ยิง API ซ้ำกับเครื่องอื่น
  const now = Date.now();
  if (now < ccRetryUntil) return;
  if (now - lastCcUsageFetch < CLAUDE_USAGE_INTERVAL) return;
  lastCcUsageFetch = now;

  try {
    const creds = JSON.parse(fs.readFileSync(CLAUDE_CREDS_FILE, 'utf8')).claudeAiOauth;
    if (creds && creds.subscriptionType) quotaStore.ccPlanType = creds.subscriptionType;
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
// OpenAI Codex: ดึงโควต้าจาก endpoint เดียวกับที่ Codex CLI ตัวจริงเรียกภายใน
// (ไม่มีเอกสารทางการ พบจาก reverse-engineering ตัว CLI: GET .../wham/usage ด้วย
// access token + account id จาก ~/.codex/auth.json)
// โครงสร้าง response: rate_limit.primary_window / secondary_window แต่ละอันมี
// used_percent (0-100) และ reset_at (unix seconds) — แพลน Free จะมีแค่ primary_window
// (หน้าต่าง 30 วัน) ส่วน secondary_window เป็น null จนกว่าจะอัปเกรดเป็น Plus/Pro
// =========================================================================
let lastCodexUsageFetch = 0;
let codexRetryUntil = 0;

async function fetchCodexUsage() {
  if (!isPrimaryForExternalUsage()) return; // เครื่อง Secondary: ไม่ยิง API ซ้ำกับเครื่องอื่น
  const now = Date.now();
  if (now < codexRetryUntil) return;
  if (now - lastCodexUsageFetch < CODEX_USAGE_INTERVAL) return;
  lastCodexUsageFetch = now;

  try {
    const auth = JSON.parse(fs.readFileSync(CODEX_AUTH_FILE, 'utf8'));
    const accessToken = auth?.tokens?.access_token;
    const accountId = auth?.tokens?.account_id;
    if (!accessToken) return; // ยังไม่ได้ login codex

    const res = await fetch('https://chatgpt.com/backend-api/wham/usage', {
      headers: {
        'Authorization': `Bearer ${accessToken}`,
        'ChatGPT-Account-Id': accountId || ''
      },
      signal: AbortSignal.timeout(5000)
    });

    if (!res.ok) {
      if (res.status === 429) {
        const retrySec = parseInt(res.headers.get('retry-after') || '1800', 10);
        codexRetryUntil = Date.now() + (retrySec * 1000);
        const retryTime = new Date(codexRetryUntil).toLocaleTimeString();
        process.stdout.write(`\n[${new Date().toLocaleTimeString()}] Codex API rate-limited (429). Pausing requests until ${retryTime} (~${Math.ceil(retrySec / 60)} min).\n`);
      }
      return;
    }

    const data = await res.json();
    quotaStore.codexConnected = true;
    quotaStore.codexPlanType = data.plan_type || null;

    // response รุ่นใหม่ (เช่น plan "prolite"): primary_window เป็นหน้าต่างรายสัปดาห์ (limit_window_seconds=604800),
    // secondary_window เป็น null, และมี chatpass.windows[] เป็นข้อมูลสำรอง -> ใช้เป็น fallback ถ้า rate_limit ว่าง
    const rl = data.rate_limit || {};
    const primaryWin = rl.primary_window || data.chatpass?.windows?.[0] || null;
    const secondaryWin = rl.secondary_window || null;
    const usedToLeft = (w) => (rl.limit_reached && w === rl.primary_window)
      ? 0 // ชนลิมิตแล้ว -> แสดง 0% แน่นอน แม้ used_percent จะยังไม่ถึง 100
      : Math.max(0, Math.min(100, Math.round(100 - (w.used_percent ?? 0))));
    const resetOf = (w) => {
      if (w.reset_at) return new Date(w.reset_at * 1000).toISOString();
      if (w.reset_after_seconds != null) return new Date(Date.now() + w.reset_after_seconds * 1000).toISOString();
      return null;
    };
    if (primaryWin) {
      quotaStore.codexPrimaryPercent = usedToLeft(primaryWin);
      quotaStore.codexPrimaryResetTime = resetOf(primaryWin);
    } else {
      quotaStore.codexPrimaryPercent = null;
      quotaStore.codexPrimaryResetTime = null;
    }
    if (secondaryWin) {
      quotaStore.codexSecondaryPercent = usedToLeft(secondaryWin);
      quotaStore.codexSecondaryResetTime = resetOf(secondaryWin);
    } else if (CODEX_SIMULATE_SECONDARY) {
      // ข้อมูลจำลอง (ไม่ใช่ของจริง) — เห็นตัวอย่างว่าถ้ามี secondary window (แบบ Plus/Pro) จอจะแสดงยังไง
      quotaStore.codexSecondaryPercent = 58;
      quotaStore.codexSecondaryResetTime = new Date(Date.now() + (4 * 24 + 6) * 3600 * 1000).toISOString();
    } else {
      quotaStore.codexSecondaryPercent = null;
      quotaStore.codexSecondaryResetTime = null;
    }

    quotaStore.codexLastUpdated = new Date().toLocaleTimeString('th-TH');

    try {
      fs.writeFileSync(CACHE_FILE, JSON.stringify(quotaStore, null, 2));
    } catch (e) {}
  } catch (e) {
    // ไม่มีไฟล์ auth.json (ยังไม่ได้ login codex) หรืออ่านไม่ได้ -> ถือว่ายังไม่เชื่อมต่อ เงียบไว้
  }
}

// =========================================================================
// Spark Local AI: ตรวจสอบสถานะ Ollama / Local AI (Port 8188 / 11434) และ % CPU
// =========================================================================
function formatOllamaModel(raw) {
  if (!raw) return 'Local AI';
  let name = raw.trim();
  // ตัดแท็ก :latest ออก เพื่อความสะอาดตา แต่คงแท็กอื่นเช่น :70b, :cloud ไว้
  name = name.replace(/:latest$/i, '');
  // ตัด prefix repository/path เช่น hf.co/.../ ถ้ามี
  if (name.includes('/')) {
    const parts = name.split('/');
    name = parts[parts.length - 1];
  }
  // จัดการกรณี HuggingFace tag ซ้ำซ้อน เช่น Model:Model-Q6_K.gguf
  if (name.includes(':')) {
    const [base, tag] = name.split(':');
    if (tag.endsWith('.gguf')) {
      name = tag.replace(/\.gguf$/i, '');
    }
  }
  name = name.replace(/\.gguf$/i, '');
  // ไม่ตัดทอนคำแล้ว รองรับชื่อเต็มได้ถึง 29 ตัวอักษรบนหน้าจอ TFT
  if (name.length > 29) {
    name = name.substring(0, 27) + '..';
  }
  return name;
}

async function fetchSparkStatus() {
  const now = Date.now();
  // Watchdog: ปลด lock อัตโนมัติหากมีคำสั่งก่อนหน้าค้างเกิน 15 วินาที
  if (isSparkPolling && (now - lastSparkPoll > 15000)) {
    isSparkPolling = false;
  }
  if (now - lastSparkPoll < SPARK_USAGE_INTERVAL) return;
  if (isSparkPolling) return;
  lastSparkPoll = now;
  isSparkPolling = true;

  try {
    // 1. ตรวจสอบ Ollama / Local AI บน Port 11434 เป็นหลัก (fallback 8188)
    const candidatePorts = [11434, 8188];
    let detectedModel = null;
    let modelStatus = 'Ready';
    let localAiOk = false;

    for (const port of candidatePorts) {
      try {
        // ตรวจสอบโมเดลที่กำลัง active ใน VRAM/Memory (/api/ps)
        const psRes = await fetch(`http://${SPARK_HOST}:${port}/api/ps`, { signal: AbortSignal.timeout(2000) });
        if (psRes.ok) {
          const psData = await psRes.json();
          if (Array.isArray(psData.models) && psData.models.length > 0) {
            localAiOk = true;
            modelStatus = 'Active';
            detectedModel = formatOllamaModel(psData.models[0].name || psData.models[0].model);
            break;
          }
        }

        // ตรวจสอบโมเดลที่ติดตั้งไว้ในคอนเทนเนอร์ (/api/tags)
        const tagsRes = await fetch(`http://${SPARK_HOST}:${port}/api/tags`, { signal: AbortSignal.timeout(2000) });
        if (tagsRes.ok) {
          const tagsData = await tagsRes.json();
          if (Array.isArray(tagsData.models) && tagsData.models.length > 0) {
            localAiOk = true;
            modelStatus = 'Ready';
            // เลือกโมเดลที่เป็น Local จริง (ไม่ลงท้ายด้วย :cloud) เป็นลำดับแรก
            const localM = tagsData.models.find(m => {
              const n = m.name || m.model || '';
              return !n.endsWith(':cloud');
            }) || tagsData.models[0];
            detectedModel = formatOllamaModel(localM.name || localM.model);
            break;
          }
        }
      } catch (e) {
        // พอร์ตนี้อาจไม่ใช่ Ollama ลองพอร์ตถัดไป
      }
    }

    // 2. ดึง % CPU และ RAM ผ่าน SSH (non-blocking พร้อม timeout และ keepalive ป้องกันค้าง)
    const sshCmd = `ssh -o BatchMode=yes -o ConnectTimeout=3 -o ServerAliveInterval=2 -o ServerAliveCountMax=2 ${SPARK_SSH_USER}@${SPARK_HOST} "top -bn1 | head -n 5; echo ===FREE===; free -m; echo ===TOKENS===; python3 /home/admin/spark_tokens.py 2>/dev/null || echo 0,0,0,0; echo ===DOCKER===; docker stats --no-stream --format '{{.Name}}:{{.CPUPerc}}' 2>/dev/null || true"`;
    exec(sshCmd, { timeout: 10000 }, (err, stdout) => {
      isSparkPolling = false;
      if (err) {
        quotaStore.sparkConnected = localAiOk;
        quotaStore.sparkStatus = localAiOk ? modelStatus : 'Offline';
        quotaStore.sparkContainers = [];
        if (localAiOk) quotaStore.sparkLastUpdated = new Date().toLocaleTimeString('th-TH');
      // Save cache on spark update (success)
      try {
        fs.writeFileSync(CACHE_FILE, JSON.stringify(quotaStore, null, 2));
      } catch (e) {}
      // Save cache on spark update
      try {
        fs.writeFileSync(CACHE_FILE, JSON.stringify(quotaStore, null, 2));
      } catch (e) {}
        return;
      }

      quotaStore.sparkConnected = true;
      const mCpu = stdout.match(/(\d+\.?\d*)\s*id/);
      quotaStore.sparkCpu = mCpu ? Math.max(0, Math.min(100, Math.round(100 - parseFloat(mCpu[1])))) : 0;

      const mMem = stdout.match(/Mem:\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)/);
      if (mMem) {
        const total = parseInt(mMem[1], 10);
        const used = parseInt(mMem[2], 10);
        quotaStore.sparkRam = Math.max(0, Math.min(100, Math.round((used / total) * 100)));
        const usedGb = Math.round(used / 1024) + 'G';
        let totalGb = Math.round(total / 1024) + 'G';
        if (total >= 115000 && total <= 135000) totalGb = '128G';
        else if (total >= 58000 && total <= 70000) totalGb = '64G';
        else if (total >= 28000 && total <= 35000) totalGb = '32G';
        quotaStore.sparkRamUsed = usedGb;
        quotaStore.sparkRamTotal = totalGb;
        quotaStore.sparkRamRatio = `${usedGb}/${totalGb}`;
      }

      // ดึงข้อมูล Tokens จาก Docker Logs
      const mTok = stdout.match(/===TOKENS===\s*([\d\.,]+)/);
      if (mTok) {
        const parts = mTok[1].trim().split(',');
        const totalTok = parseInt(parts[0], 10) || 0;
        const todayTok = parseInt(parts[1], 10) || 0;
        const savedUsd = parseFloat(parts[2]) || 0;
        const savedThb = parseInt(parts[3], 10) || 0;
        quotaStore.sparkTotalTokens = formatTokens(totalTok);
        quotaStore.sparkTodayTokens = formatTokens(todayTok);
        quotaStore.sparkSavedCost = savedUsd > 0 ? `~$${Math.round(savedUsd)}` : '$0';
        quotaStore.sparkSavedThb = savedThb > 0 ? `฿${formatTokens(savedThb)}` : '฿0';
        const speedVal = parseFloat(parts[4]) || 0;
        quotaStore.sparkSpeed = speedVal > 0 ? `${Math.round(speedVal)} t/s` : '';
      }

      
      // ดึงข้อมูล Docker Container CPU จาก docker stats
      const mDocker = stdout.match(/===DOCKER===\s*([\s\S]*)$/);
      if (mDocker && mDocker[1]) {
        const lines = mDocker[1].trim().split(/\r?\n/);
        const containers = [];
        for (const line of lines) {
          const trimmed = line.trim();
          if (!trimmed || trimmed.startsWith('===')) continue;
          const colonIdx = trimmed.indexOf(':');
          if (colonIdx > 0) {
            const name = trimmed.substring(0, colonIdx).trim();
            let cpu = trimmed.substring(colonIdx + 1).trim();
            if (cpu && !cpu.endsWith('%')) cpu += '%';
            containers.push({
              name,
              cpu: cpu || '0.00%',
              running: true
            });
          }
        }
        quotaStore.sparkContainers = containers;
        if (containers.length > 0) {
          quotaStore.sparkC1Name = containers[0].name;
          quotaStore.sparkC1Cpu = containers[0].cpu;
        } else {
          quotaStore.sparkC1Name = '';
          quotaStore.sparkC1Cpu = '';
        }
        if (containers.length > 1) {
          quotaStore.sparkC2Name = containers[1].name;
          quotaStore.sparkC2Cpu = containers[1].cpu;
        } else {
          quotaStore.sparkC2Name = '';
          quotaStore.sparkC2Cpu = '';
        }
      }

      quotaStore.sparkModel = detectedModel || quotaStore.sparkModel || 'Local AI';
      quotaStore.sparkStatus = localAiOk ? modelStatus : 'Offline';
      quotaStore.sparkLastUpdated = new Date().toLocaleTimeString('th-TH');
    });
  } catch (e) {
    isSparkPolling = false;
  }
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

  // Codex: ช่องไหนไม่เคยดึงสำเร็จ/แพลนไม่มีหน้าต่างนั้น (เช่น Free ไม่มี secondary) ส่ง -1 ให้จอรู้ว่าไม่ต้องวาด
  const codexP = calculateCountdown(quotaStore.codexPrimaryResetTime);
  const codexS = calculateCountdown(quotaStore.codexSecondaryResetTime);
  const codexPrimaryVal = quotaStore.codexPrimaryPercent;
  const codexSecondaryVal = quotaStore.codexSecondaryPercent;

  const codexRateLimitedNow = Date.now() < codexRetryUntil;
  let codexRateLimitReset = "";
  if (codexRateLimitedNow) {
    const mins = Math.max(1, Math.ceil((codexRetryUntil - Date.now()) / 60000));
    codexRateLimitReset = mins >= 60 ? `${Math.floor(mins / 60)}h ${mins % 60}m` : `${mins}m`;
  }

  const result = {
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
    ccPlanType: quotaStore.ccPlanType,
    // สถานะแจ้งเตือน 429: จอไหนอยากโชว์เตือนก็เช็ค ccRateLimited ได้เลย ไม่ต้องรู้เรื่อง Retry-After เอง
    ccRateLimited: ccRateLimitedNow,
    ccRateLimitReset: ccRateLimitReset,
    codexConnected: quotaStore.codexConnected,
    codexPlanType: quotaStore.codexPlanType,
    // -1 = ไม่มีข้อมูล (แพลนนี้ไม่มีหน้าต่างนี้ หรือยังไม่เคยดึงสำเร็จ) จอควรข้ามไม่วาดช่องนี้
    codexPrimaryPercent: (codexPrimaryVal === null || codexPrimaryVal === undefined) ? -1 : codexPrimaryVal,
    codexPrimaryReset: quotaStore.codexPrimaryResetTime ? codexP.short : "",
    codexSecondaryPercent: (codexSecondaryVal === null || codexSecondaryVal === undefined) ? -1 : codexSecondaryVal,
    codexSecondaryReset: quotaStore.codexSecondaryResetTime ? codexS.short : "",
    codexRateLimited: codexRateLimitedNow,
    codexRateLimitReset: codexRateLimitReset,
    codexLastUpdated: quotaStore.codexLastUpdated,
    sparkConnected: quotaStore.sparkConnected,
    sparkModel: quotaStore.sparkModel || 'Local AI',
    sparkStatus: quotaStore.sparkStatus || 'Offline',
    sparkCpu: quotaStore.sparkCpu ?? 0,
    sparkRam: quotaStore.sparkRam ?? 0,
    sparkRamUsed: quotaStore.sparkRamUsed || '0G',
    sparkRamTotal: quotaStore.sparkRamTotal || '0G',
    sparkRamRatio: quotaStore.sparkRamRatio || '0G/0G',
    sparkTotalTokens: quotaStore.sparkTotalTokens || '0',
    sparkTodayTokens: quotaStore.sparkTodayTokens || '0',
    sparkSavedCost: quotaStore.sparkSavedCost || '$0',
    sparkSavedThb: quotaStore.sparkSavedThb || '฿0',
    sparkSpeed: quotaStore.sparkSpeed || '',
    sparkContainers: quotaStore.sparkContainers || [],
    sparkC1Name: quotaStore.sparkC1Name || '',
    sparkC1Cpu: quotaStore.sparkC1Cpu || '',
    sparkC2Name: quotaStore.sparkC2Name || '',
    sparkC2Cpu: quotaStore.sparkC2Cpu || '',
    sparkLastUpdated: quotaStore.sparkLastUpdated || '',
    lastUpdated: quotaStore.lastUpdated,
    // "mirror" = กำลังอ่าน cc*/codex* จากเครื่องหลักผ่าน peerBridgeUrl สำเร็จอยู่
    // "local"  = เครื่องนี้ยิง API เอง (ไม่ได้ตั้ง peerBridgeUrl ไว้ หรือตั้งไว้แต่เอื้อมไม่ถึงเลย fallback มายิงเอง)
    dataSource: (getPeerBridgeUrl() && peerReachableNow) ? 'mirror' : 'local'
  };

  // ถ้าเพิ่งอ่านค่าจากเครื่องหลัก (peerBridgeUrl) สำเร็จรอบล่าสุด ใช้ค่านั้นทับ cc*/codex*
  // แทนค่าที่คำนวณเองในเครื่องนี้ (ซึ่งจะเก่า/ค้าง เพราะเครื่องนี้ไม่ได้ยิง API เอง)
  if (peerReachableNow && peerMirrorData) {
    Object.assign(result, peerMirrorData);
  }

  return result;
}

// Background Poller ตรวจสอบสถานะทุกๆ 10 วินาที
async function pollLoop() {
  const raw = await getAntigravityQuotaLive();
  if (raw) {
    processLiveData(raw);
  } else {
    quotaStore.ideRunning = false;
  }

  // ลองอ่านจากเครื่องหลักผ่าน peerBridgeUrl ก่อน (ถ้าตั้งไว้) เจอแล้วข้ามการยิง API เอง
  // ไม่เจอ (ปิดเครื่อง/VPN หลุด) หรือไม่ได้ตั้ง peerBridgeUrl ไว้ -> fallback ไปยิงเองตามปกติ
  const mirrored = await tryMirrorFromPeer();
  if (!mirrored) {
    await fetchClaudeCodeUsage();
    await fetchCodexUsage();
  }
  await fetchSparkStatus();
  try {
    scanClaudeCodeTokens();
  } catch (e) {}

  const current = buildDynamicResponse();
  const time = new Date().toLocaleTimeString();
  const ctxStr = current.ccActiveContext && current.ccActiveContext !== '0' ? ` (Ctx ${current.ccActiveContext})` : '';
  const mirrorTag = getPeerBridgeUrl() ? (mirrored ? ' [mirror]' : ' [fallback-self]') : '';
  const ccInfo = `CC ${current.cc5Hr}%/${current.ccWeekly}%${mirrorTag} | Tok ${current.ccWindowTokens}${ctxStr}`;
  const codexInfo = current.codexConnected
    ? ` | Codex ${current.codexPrimaryPercent}%${current.codexSecondaryPercent >= 0 ? `/${current.codexSecondaryPercent}%` : ''}`
    : '';
  const sparkInfo = current.sparkConnected
    ? ` | Spark ${current.sparkModel}(${current.sparkStatus}) CPU ${current.sparkCpu}% RAM ${current.sparkRam}%(${current.sparkRamRatio}) Tok ${current.sparkTotalTokens}(Save ${current.sparkSavedCost})`
    : '';
  if (current.ideRunning) {
    writeStatusLine(`[${time}] ON  | Gemini ${current.geminiWeekly}%(${current.gemini5Hr}%) | Claude ${current.claudeWeekly}%(${current.claude5Hr}%) | ${ccInfo}${codexInfo}${sparkInfo}`);
  } else {
    const sub = current.gemini5HrSubtext ? `5Hr ${current.gemini5HrSubtext}` : `Wk ${current.geminiWeeklySubtext}`;
    writeStatusLine(`[${time}] OFF | Reset ${sub} | ${ccInfo}${codexInfo}${sparkInfo}`);
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

server.on('error', (err) => {
  if (err.code === 'EADDRINUSE') {
    console.log(`[INFO] Port ${PORT} is already in use by another instance. Exiting gracefully.`);
    process.exit(0);
  }
  console.error(`[SERVER ERROR]`, err);
  process.exit(1);
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
  const peerUrl = getPeerBridgeUrl();
  if (!isPrimaryForExternalUsage()) {
    console.log(`🟡 Role: SECONDARY — skipping Claude Code / Codex API calls (see bridge.local.json)`);
  } else if (peerUrl) {
    console.log(`🔗 Role: MIRROR — try reading cc*/codex* from ${peerUrl} first, fallback to real API if unreachable`);
  } else {
    console.log(`🟢 Role: PRIMARY — fetching Claude Code / Codex usage from the real API`);
  }
  console.log(`=======================================================`);
});
