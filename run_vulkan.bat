@echo off
cd /d "C:\Users\Administrator\Documents\project\personal\LunaEngine-source\LunaApp\bin\Debug-windows-x86_64\LunaApp"
LunaApp.exe --vulkan > app_stdout.txt 2> app_stderr.txt
echo EXIT:%ERRORLEVEL%
