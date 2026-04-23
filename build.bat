@echo off
setlocal
set MSBUILD="C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
set SLN="C:\Users\Administrator\Documents\project\personal\LunaEngine-source\LunaApp.sln"
set OUT="C:\Users\Administrator\Documents\project\personal\LunaEngine-source\build_output.txt"

%MSBUILD% %SLN% /p:Configuration=Debug /p:Platform=x64 /t:LunaApp /m /v:minimal 1>%OUT% 2>&1
echo EXIT:%ERRORLEVEL% >>%OUT%
endlocal
