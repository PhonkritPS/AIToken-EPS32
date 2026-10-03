' Antigravity AI Quota Bridge Server Launcher
' Auto-restart on crash, graceful exit if already running, hidden background execution
Option Explicit
Dim fso, sh, ret
Set fso = CreateObject("Scripting.FileSystemObject")
Set sh = CreateObject("WScript.Shell")
sh.CurrentDirectory = fso.GetParentFolderName(WScript.ScriptFullName)

Do
    ret = sh.Run("cmd /c node bridge_server.js >> bridge.log 2>&1", 0, True)
    If ret = 0 Then WScript.Quit 0
    WScript.Sleep 3000
Loop