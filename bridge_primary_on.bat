@echo off
title AIToken Bridge - Set PRIMARY
cd /d "%~dp0"
echo Setting this machine as PRIMARY.
echo It WILL fetch Claude Code / Codex usage from the real API.
echo.
powershell -NoProfile -Command "'{ \"fetchExternalUsage\": true }' | Out-File -FilePath 'bridge.local.json' -Encoding utf8"
echo Done. Takes effect within ~10s, no restart needed.
echo (Only ONE machine using the same Claude/Codex login should be PRIMARY at a time.)
pause
