@echo off
title AIToken Bridge - Set SECONDARY
cd /d "%~dp0"
echo Setting this machine as SECONDARY.
echo It will SKIP fetching Claude Code / Codex usage from the real API
echo (avoids double-polling the same account from two machines and
echo  triggering Rate Limit). Antigravity data keeps working normally
echo  if the IDE is running on this machine. Claude Code / Codex fields
echo  will just show the last cached values instead of live ones.
echo.
powershell -NoProfile -Command "'{ \"fetchExternalUsage\": false }' | Out-File -FilePath 'bridge.local.json' -Encoding utf8"
echo Done. Takes effect within ~10s, no restart needed.
pause
