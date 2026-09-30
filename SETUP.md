# ติดตั้ง Bridge Server บนเครื่องใหม่ (Setup Guide)

คู่มือนี้สำหรับ**เครื่องคอมพิวเตอร์ที่จะรัน `bridge_server.js`** (เครื่องที่เปิด Antigravity IDE และ/หรือ Claude Code) ทำตามลำดับข้อ 1-7 แล้วจอ ESP32 จะแสดงข้อมูลได้เอง

---

## 1. ตรวจสอบสิ่งที่ต้องมีก่อน

| สิ่งที่ต้องมี | ตรวจสอบด้วยคำสั่ง | เกณฑ์ผ่าน |
|---|---|---|
| Node.js v16+ | `node -v` | ขึ้นเลขเวอร์ชัน เช่น `v20.x.x` |
| Git | `git --version` | ขึ้นเลขเวอร์ชัน |

ถ้า `node -v` ใช้ไม่ได้ ให้ติดตั้ง Node.js จาก https://nodejs.org ก่อน (เลือกเวอร์ชัน LTS)

---

## 2. Clone / Pull โค้ด

**ถ้ายังไม่เคยมีโฟลเดอร์นี้ในเครื่อง:**
```powershell
git clone https://github.com/PhonkritPS/AIToken-EPS32.git
cd AIToken-EPS32
```

**ถ้ามีโฟลเดอร์อยู่แล้ว:**
```powershell
cd D:\GitHub\AIToken
git pull origin main
```

---

## 3. Login Claude Code (จำเป็นถ้าต้องการแสดงโควต้า/token ของ Claude Code)

ถ้าเครื่องนี้ยังไม่เคยใช้ Claude Code CLI มาก่อน ให้ล็อกอินก่อน (ครั้งเดียว):
```powershell
claude
```
แล้วทำตามขั้นตอนล็อกอินในเบราว์เซอร์ให้เสร็จ ปิด Claude Code ได้เลยหลังล็อกอินสำเร็จ

**ตรวจสอบว่าล็อกอินสำเร็จ:**
```powershell
Test-Path "$env:USERPROFILE\.claude\.credentials.json"
```
ต้องได้ `True`

> ถ้าข้ามขั้นตอนนี้ ส่วนของ Claude Code บนจอจะไม่มีข้อมูล (แต่ส่วน Gemini/Claude ผ่าน Antigravity ยังทำงานได้ปกติ)

---

## ⚠️ สำคัญ: ถ้ามี bridge รันมากกว่า 1 เครื่องที่ login บัญชีเดียวกัน

ถ้าคุณรัน `bridge_server.js` พร้อมกันหลายเครื่อง (เช่น คอมที่ทำงาน + คอมที่บ้าน) โดยที่ทั้งสองเครื่อง
login Claude Code / Codex ด้วย**บัญชีเดียวกัน** — ทุกเครื่องจะต่างคนต่างยิง API เช็คโควต้าของบัญชีนั้น
พร้อมกัน ทำให้อัตราการยิงจริงต่อบัญชีเพิ่มเป็นหลายเท่าโดยไม่รู้ตัว **เสี่ยงโดน 429 Rate Limit ง่ายขึ้นมาก**

มี 2 วิธีแก้ เลือกตามสถานการณ์:

### วิธีที่ 1: อ่านค่าข้ามเครื่องอัตโนมัติผ่าน VPN/LAN (แนะนำถ้าเครื่องคุยกันได้)

ถ้าเครื่อง "รอง" (เช่นคอมที่บ้าน) เชื่อมต่อไปหาเครื่อง "หลัก" (เช่นคอมที่ทำงาน) ได้ผ่าน VPN/LAN อยู่แล้ว
ให้ตั้งเครื่องรองอ่านค่าจากเครื่องหลักแทนการยิง API เอง — **ถ้าเครื่องหลักปิดหรือ VPN หลุด จะ fallback
มายิง API เองอัตโนมัติทันที ไม่ต้องทำอะไรเพิ่ม** เครื่องหลักไม่ต้องตั้งค่าอะไรเลย ยิง API ของตัวเองตามปกติเสมอ

ที่**เครื่องรอง**เท่านั้น สร้างไฟล์ `bridge.local.json` (อยู่โฟลเดอร์เดียวกับ `bridge_server.js`):
```json
{ "peerBridgeUrl": "http://<IP เครื่องหลักผ่าน VPN/LAN>:5000/api/quota" }
```
เช่น ถ้าเครื่องหลักมี IP ผ่าน VPN เป็น `10.104.11.50`:
```json
{ "peerBridgeUrl": "http://10.104.11.50:5000/api/quota" }
```
มีผลภายในรอบโพลถัดไป (~10 วิ) ไม่ต้อง restart เช็คผลได้จาก `bridge.log`:
- `🔗 Role: MIRROR — ...` ตอน bridge เริ่มทำงาน (แปลว่าตั้งค่าถูกแล้ว)
- แต่ละบรรทัดสถานะจะมี `[mirror]` (อ่านจากเครื่องหลักสำเร็จ) หรือ `[fallback-self]` (เครื่องหลักไม่ตอบ เลยยิงเอง) ต่อท้าย `CC xx%/xx%`

> Gemini/Claude ผ่าน Antigravity และจำนวน Token ยังคำนวณจากเครื่องนั้นๆ เองเสมอ (ไม่ mirror) เพราะไม่ใช่ข้อมูล
> ที่เสี่ยง Rate Limit และเป็นข้อมูลเฉพาะเครื่องอยู่แล้ว — ที่ mirror มีแค่ % โควต้า Claude Code/Codex เท่านั้น

### วิธีที่ 2: สลับ Primary/Secondary ด้วยมือ (ถ้าเครื่องคุยกันไม่ได้)

ให้เลือกเพียง**เครื่องเดียว**เป็น **PRIMARY** (ยิง API จริง) ส่วนเครื่องอื่นตั้งเป็น **SECONDARY**
(ข้ามการยิง API จริง ใช้ค่าที่แคชไว้ล่าสุดแทน)

**เครื่องที่กำลังใช้งานอยู่ตอนนี้ (อยากได้ตัวเลขล่าสุด):**
```powershell
bridge_primary_on.bat
```

**เครื่องอื่นๆ ที่ล็อกอินบัญชีเดียวกันแต่ไม่ได้ใช้ตอนนี้:**
```powershell
bridge_primary_off.bat
```

มีผลภายใน ~10 วินาที ไม่ต้อง restart bridge หรือแฟลช ESP32 ใหม่ สลับกลับไปกลับมาได้ตลอดเวลาที่ย้ายที่ทำงาน
เช็คสถานะปัจจุบันได้จาก `bridge.log` (บรรทัด `🟢 Role: PRIMARY` หรือ `🟡 Role: SECONDARY`)

ทั้งสองวิธี: Gemini/Claude ผ่าน Antigravity ยังทำงานได้ปกติทุกเครื่องเสมอ เพราะดึงจาก Language Server
ในเครื่อง ไม่เกี่ยวกับ Rate Limit ของบัญชี

> ถ้ามีเครื่องเดียว ไม่ต้องทำอะไร (ค่าเริ่มต้นคือ PRIMARY อยู่แล้ว)

---

## 4. เปิด Antigravity IDE ทิ้งไว้ (จำเป็นถ้าต้องการแสดงโควต้า Gemini / Claude ผ่าน Antigravity)

เปิด Antigravity IDE ค้างไว้ในเครื่องนี้ (bridge จะหา process แล้วดึงข้อมูลอัตโนมัติทุก 10 วินาที) ถ้าปิด IDE ไป bridge จะยังนับเวลาถอยหลังต่อจากค่าล่าสุดที่เคยดึงได้

---

## 5. ทดสอบรัน bridge ด้วยมือก่อน 1 ครั้ง

```powershell
node bridge_server.js
```

ควรเห็นข้อความแบบนี้:
```
🚀 Antigravity AI Quota Bridge Server is RUNNING!
📡 Local API endpoint: http://localhost:5000/api/quota
```

เปิดเบราว์เซอร์ไปที่ `http://localhost:5000/api/quota` ต้องเห็นข้อมูล JSON ออกมา ถ้าเห็นแล้วให้กด `Ctrl+C` เพื่อปิดตัวทดสอบนี้ก่อนไปข้อถัดไป (ไม่งั้นจะชนพอร์ตกับ auto-start ในข้อ 7)

---

## 6. หา IP ของเครื่องนี้ แล้วตั้งค่าให้ตรงกับ ESP32

```powershell
ipconfig | findstr /i "IPv4"
```
จด IP ที่ได้ (เช่น `192.168.10.99`)

เปิดไฟล์ [AIToken.ino](AIToken.ino) แล้วตรวจสอบ/แก้บรรทัดนี้ให้ตรงกับ IP ของเครื่องนี้:
```cpp
const char* apiUrl = "http://<IP_ของเครื่องนี้>:5000/api/quota";
```
ถ้าแก้ IP หรือ Wi-Fi (`ssid`/`password`) ต้อง**แฟลชโค้ดใหม่ลง ESP32** ด้วย Arduino IDE ถึงจะมีผล (ดูขั้นตอนแฟลชใน [README.md](README.md))

> ถ้า IP เครื่องนี้เหมือนเดิมกับที่ ESP32 ถูกตั้งไว้แล้ว ข้ามข้อนี้ได้เลย

---

## 7. ตั้งให้ bridge รันอัตโนมัติเบื้องหลังทุกครั้งที่ล็อกอิน

```powershell
powershell -ExecutionPolicy Bypass -File install_autostart.ps1
```

ควรเห็นข้อความ:
```
Registered scheduled task 'AIToken Bridge Server' (runs at logon, hidden).
```

**สั่งให้รันทันทีโดยไม่ต้องรอรีบูต/ล็อกอินใหม่:**
```powershell
Start-ScheduledTask -TaskName 'AIToken Bridge Server'
```

---

## 8. ตรวจสอบว่าทำงานถูกต้อง

```powershell
Start-Sleep 3
curl.exe -s http://localhost:5000/api/quota
```
ต้องได้ JSON กลับมา ไม่ error

ถ้าอยากดู log ล่าสุด:
```powershell
Get-Content bridge.log -Tail 30
```

---

## คำสั่งที่ใช้บ่อยหลังติดตั้งแล้ว

| ต้องการ | คำสั่ง |
|---|---|
| หยุดรันชั่วคราว | `Stop-ScheduledTask -TaskName 'AIToken Bridge Server'` |
| สั่งรันใหม่ทันที | `Start-ScheduledTask -TaskName 'AIToken Bridge Server'` |
| ดู log | `Get-Content bridge.log -Tail 30` |
| ถอนการ auto-start ออกทั้งหมด | `powershell -ExecutionPolicy Bypass -File install_autostart.ps1 -Uninstall` |

---

## แก้ปัญหาที่พบบ่อย

**`EADDRINUSE: address already in use 0.0.0.0:5000`**
มี bridge ตัวอื่นรันพอร์ต 5000 อยู่แล้ว (เช่นเปิด `start_bridge.bat` ค้างไว้ หรือลืมปิดตอนทดสอบข้อ 5) ให้หา process แล้วปิดก่อน:
```powershell
Get-CimInstance Win32_Process -Filter "Name='node.exe'" | Select ProcessId,CommandLine
Stop-Process -Id <ProcessId> -Force
```
แล้วสั่ง `Start-ScheduledTask -TaskName 'AIToken Bridge Server'` ใหม่อีกครั้ง

**ส่วน Claude Code บนจอไม่มีข้อมูล / ค้างที่ 100%**
ยังไม่ได้ทำข้อ 3 (login `claude` ในเครื่องนี้) หรือ token หมดอายุ ลองรัน `claude` แล้วเช็คสถานะล็อกอินอีกครั้ง

**ส่วน Gemini / Claude & GPT (Antigravity) ไม่มีข้อมูล**
ยังไม่ได้เปิด Antigravity IDE ในเครื่องนี้ (ข้อ 4) หรือ IDE เพิ่งปิดไปและกำลังรอนับถอยหลังต่อ

**จอ ESP32 ไม่อัปเดตเลย**
ตรวจสอบว่า `apiUrl` ใน `AIToken.ino` (ข้อ 6) ตรงกับ IP ของเครื่องที่รัน bridge จริง และ ESP32 กับเครื่องนี้อยู่ Wi-Fi วงเดียวกัน
