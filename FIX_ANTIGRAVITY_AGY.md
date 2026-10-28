# แก้ Bridge อ่านโควต้า Antigravity ไม่ได้หลัง IDE อัปเดต (agy.exe)

วันที่แก้: 2026-10-28 · ไฟล์ที่แก้: `bridge_server.js` (ไฟล์เดียว)

## อาการ

- กล่อง ANTIGRAVITY บนจอไม่อัปเดต และ bridge ขึ้นสถานะ `OFF`
- `/api/quota` ตอบ `ideRunning: false` ทั้งที่เปิด Antigravity IDE อยู่

## สาเหตุ

Antigravity IDE รุ่นใหม่เปลี่ยน process ที่ทำหน้าที่ language server แต่ API ดึงโควต้า
`/exa.language_server_pb.LanguageServerService/RetrieveUserQuotaSummary` ยังเหมือนเดิม

| | รุ่นเก่า | รุ่นใหม่ |
|---|---|---|
| ชื่อ process | `language_server*.exe` | `agy.exe` (อยู่ที่ `%USERPROFILE%\.gemini\bin\agy.exe`) |
| flag ของ token | `--csrf_token <token>` (เว้นวรรค) | `--csrf_token=<token>` (เครื่องหมาย `=`) |
| พอร์ต | พอร์ต LISTENING ของ process | เหมือนเดิม: `--hub-port` ตอบแบบ HTTP ส่วนอีกพอร์ตตอบแบบ HTTPS |

ตัวอย่าง command line ใหม่:

```
C:\Users\<user>\.gemini\bin\agy.exe --hub --hub-port=64814 --app_data_dir=antigravity --csrf_token=xxxxxxxx-xxxx-... --add-dir=...
```

ยังมีปัญหาแฝงอีกจุด: response รุ่นใหม่มีกลุ่ม **"Gemini 4 Argon"** ต่อท้ายรายการ ชื่อกลุ่มมีคำว่า `gemini`
โค้ดเดิมจึงเอาค่าโควต้า Argon ไปเขียนทับ `geminiWeekly` ของกลุ่ม Gemini หลัก

## การแก้ไข (3 จุดใน `bridge_server.js`)

### 1. ฟังก์ชัน `getAntigravityQuotaLive()` ให้ค้นหา `agy.exe` ด้วย

ยังรองรับชื่อเดิม เผื่อเครื่องที่ยังไม่อัปเดต

```diff
-    const psCmd = `Get-CimInstance Win32_Process | Where-Object { $_.Name -like '*language_server*' } | Select-Object ProcessId, CommandLine | ConvertTo-Json`;
+    const psCmd = `Get-CimInstance Win32_Process | Where-Object { $_.Name -like '*language_server*' -or $_.Name -like 'agy*' } | Select-Object ProcessId, CommandLine | ConvertTo-Json`;
```

### 2. regex แกะ csrf token ให้รับได้ทั้งเว้นวรรคและ `=`

```diff
-      const tokenMatch = proc.CommandLine.match(/--csrf_token\s+([a-f0-9-]+)/);
+      const tokenMatch = proc.CommandLine.match(/--csrf_token[=\s]+([a-f0-9-]+)/);
```

### 3. ฟังก์ชัน `processLiveData()`: ให้กลุ่มแรกที่ตรงเป็นฝ่ายได้ค่า กันกลุ่ม Argon เขียนทับ

ก่อนลูป `for (const group of raw.groups)` ให้เพิ่ม:

```js
  // กลุ่มแรกที่ตรงชนะ: รุ่นใหม่มีกลุ่มแยกอย่าง "Gemini 4 Argon" ต่อท้าย ห้ามเขียนทับค่า Gemini หลัก
  const filled = new Set();
  const setSlot = (key, pct, reset) => {
    if (filled.has(key)) return;
    filled.add(key);
    quotaStore[key] = pct;
    quotaStore[key + 'ResetTime'] = reset || null;
  };
```

แล้วแทนบล็อก if/else ที่กำหนดค่า `quotaStore` ภายในลูป bucket ด้วย:

```js
        if (isGemini) {
          setSlot(isWeekly ? 'geminiWeekly' : 'gemini5Hr', pct, bucket.resetTime);
        } else if (isClaude) {
          setSlot(isWeekly ? 'claudeWeekly' : 'claude5Hr', pct, bucket.resetTime);
        }
```

## วิธีนำไปใช้ที่เครื่องอื่น

1. **ทางที่ง่ายที่สุด:** `git pull` ถ้าการแก้นี้ commit และ push ขึ้น repo แล้ว
2. **ถ้าไม่ได้ pull:** แก้ `bridge_server.js` เองตาม 3 จุดข้างบน แล้วเช็ก syntax:
   ```
   node --check bridge_server.js
   ```
3. รีสตาร์ท bridge (ต้องทำทุกครั้งหลังแก้ไฟล์)
   - ถ้ารันผ่าน `start_bridge_hidden.vbs` หรือ Scheduled Task "AIToken Bridge Server":
     ตัว launcher จะสั่งรันใหม่เองเมื่อ node ปิดตัวแบบ error ดังนั้น kill แค่ process node ก็พอ
     ```powershell
     Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -like 'node*bridge_server*' } |
       ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
     ```
     รอประมาณ 3–5 วินาที node จะกลับมาเอง
   - ถ้ารันเองในเทอร์มินัล: กด Ctrl+C แล้วรัน `node bridge_server.js` ใหม่

## วิธีตรวจว่าใช้ได้

1. ดูว่า IDE รุ่นใหม่มี `agy.exe` และดึง token มาได้:
   ```powershell
   Get-CimInstance Win32_Process | Where-Object { $_.Name -like 'agy*' } | Select-Object ProcessId, CommandLine
   ```
2. ดูบรรทัดสถานะท้าย `bridge.log` ต้องขึ้น `ON  | Gemini xx%(xx%) | Claude xx%(xx%) ...` ไม่ใช่ `OFF`
3. เรียก `http://127.0.0.1:5000/api/quota` ต้องได้ `"connected": true` และ `"ideRunning": true`
4. ถ้าจอ ESP32 ไม่เปลี่ยนภายในประมาณ 1 นาที ให้กดรีเซ็ตบอร์ด

## ทดสอบ API ตรง ๆ (สำหรับ debug)

```bash
# แทน <PORT> ด้วยค่า --hub-port และ <TOKEN> ด้วยค่า --csrf_token จาก command line ของ agy.exe
curl -s -X POST "http://127.0.0.1:<PORT>/exa.language_server_pb.LanguageServerService/RetrieveUserQuotaSummary" \
  -H "Content-Type: application/json" -H "x-codeium-csrf-token: <TOKEN>" -d "{}"
```

response ที่ถูกต้องมี `response.groups[]` ประกอบด้วย:

- `Gemini Models` มี bucket `gemini-weekly` และ `gemini-5h`
- `Claude and GPT models` มี bucket `3p-weekly` และ `3p-5h`
- `Gemini 4 Argon` (กลุ่มใหม่ ตอนนี้ bridge ไม่ใช้ค่านี้)

แต่ละ bucket มี `remainingFraction` (0–1) และ `resetTime`

---

# เพิ่มแถว Gemini 4 Argon บนจอ (ต่อจากการแก้ข้างบน)

วันที่แก้: 2026-10-28 · ไฟล์ที่แก้: `bridge_server.js` และ `AIToken.ino`

Antigravity รุ่นใหม่มีโควต้ากลุ่มที่สาม **"Gemini 4 Argon"** แยกออกมา มีเฉพาะรายสัปดาห์ ไม่มีแบบ 5 ชั่วโมง
(bucket `gemini-argon-wtu-weekly`) ตอนนี้ bridge แยกค่ากลุ่มนี้ไว้และส่งไปแสดงเป็นแถวที่ 3 ในแผง ANTIGRAVITY

## ฝั่ง Bridge (`bridge_server.js`)

1. เพิ่มชื่อฟิลด์ `'argonWeekly'` และ `'argonWeeklyReset'` ใน `LOCAL_ANTIGRAVITY_FIELDS`
   เพื่อให้ค่านี้อ่านจาก IDE ในเครื่องตัวเองเสมอ ไม่ mirror มาจาก peer
2. เพิ่มใน `quotaStore`:
   ```js
   argonWeekly: null,          // null = IDE รุ่นนี้ไม่มีกลุ่ม Gemini 4 Argon
   argonWeeklyResetTime: null,
   ```
3. ใน `processLiveData()`: เช็กว่าเป็นกลุ่ม Argon ก่อน เพราะชื่อกลุ่มมีคำว่า gemini ด้วย
   ```js
   const isArgon = name.includes('argon'); // "Gemini 4 Argon" โควต้าแยก มีแค่รายสัปดาห์
   const isGemini = !isArgon && name.includes('gemini');
   ...
   if (isArgon) {
     if (isWeekly) setSlot('argonWeekly', pct, bucket.resetTime);
   } else if (isGemini) {
   ```
4. ใน `buildDynamicResponse()`: คำนวณค่าที่จะส่ง และส่งฟิลด์ใหม่ไปที่จอ
   ```js
   const aWeekly = calculateCountdown(quotaStore.argonWeeklyResetTime);
   const argonWeeklyVal = (quotaStore.argonWeekly === null || quotaStore.argonWeekly === undefined) ? -1
     : (aWeekly.isExpired && quotaStore.argonWeekly < 100) ? 100 : quotaStore.argonWeekly;
   ...
   argonWeekly: argonWeeklyVal, // -1 = ไม่มีกลุ่ม Argon ให้จอซ่อนแถว
   argonWeeklyReset: (argonWeeklyVal >= 0 && argonWeeklyVal < 100) ? aWeekly.short : "",
   ```

ฟิลด์ใหม่ใน `/api/quota`:

| ฟิลด์ | ความหมาย |
|---|---|
| `argonWeekly` | % โควต้า Argon ที่เหลือ (0–100) หรือ `-1` ถ้า IDE ไม่มีกลุ่มนี้ |
| `argonWeeklyReset` | เวลาที่เหลือก่อนรีเซ็ต แบบย่อ เช่น `6d 23h` (ว่างเมื่อเหลือ 100%) |

## ฝั่งจอ TFT (`AIToken.ino`)

1. ปรับ layout: แผง Antigravity สูงขึ้น 6px โดยลดแผง Spark ลง 6px ขอบล่างของจอยังเท่าเดิม
   ```cpp
   const int PANEL_Y[PANELS]  = { 1, 60, 113, 150 };
   const int PANEL_H[PANELS]  = { 56, 50, 34, 88 };
   ```
2. เพิ่มตัวแปร `int argonWeekly = -1; String argonWeeklyReset = "";`
   และอ่านค่าจาก JSON ต่อจากบรรทัดที่อ่าน `claudeWeeklyReset`
3. ใน `drawAntigravityBody()`:
   - เปลี่ยนระยะแถวเป็น `bodyTop + 3`, `+16`, `+29` (เดิม +3 และ +19)
   - เพิ่มแถวที่ 3 `Argon [แถบ] xx% รีเซ็ต` ใช้คอลัมน์เดียวกับแถว Gemini/Claude
     (แถบที่ x=50, ตัวเลข % ที่ x=94, เวลารีเซ็ตที่ x=122) ถ้า `argonWeekly < 0` จะแสดง `-` แทน
   - ข้อความตอน IDE ปิด ขยับเป็น `bodyTop + 13` และ `+27` ให้อยู่กลางแผงที่สูงขึ้น

## นำไปใช้

1. `git pull`
2. รีสตาร์ท bridge ตามขั้นตอนในส่วนแรกของไฟล์นี้
3. Upload `AIToken.ino` ลงบอร์ด ESP32 ใหม่ ต้องอัปโหลดเฟิร์มแวร์ใหม่ด้วยถึงจะเห็นแถว Argon
4. ตรวจ: `/api/quota` ต้องมี `argonWeekly` (เช่น `97`) และจอต้องแสดงแถว `Argon`
