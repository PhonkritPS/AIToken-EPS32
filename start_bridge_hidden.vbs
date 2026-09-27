' รัน bridge_server.js แบบซ่อนหน้าต่าง (ใช้กับ Task Scheduler ตอนเปิดเครื่อง)
' log ของรอบล่าสุดอยู่ที่ bridge.log (เขียนทับทุกครั้งที่เริ่มใหม่)
Set fso = CreateObject("Scripting.FileSystemObject")
Set sh = CreateObject("WScript.Shell")
sh.CurrentDirectory = fso.GetParentFolderName(WScript.ScriptFullName)

' 0 = ซ่อนหน้าต่าง, True = รอจน node จบ เพื่อให้ Task Scheduler รู้ว่าหยุดทำงานและรันใหม่ได้
WScript.Quit sh.Run("cmd /c node bridge_server.js > bridge.log 2>&1", 0, True)
