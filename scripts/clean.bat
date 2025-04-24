@echo off
echo ============================
echo   Cleaning LunaEngine...
echo ============================

cd /d "%~dp0\.."

REM LunaApp
if exist LunaApp\bin rd /s /q LunaApp\bin
if exist LunaApp\bin-int rd /s /q LunaApp\bin-int

REM LunaEngine
if exist LunaEngine\bin rd /s /q LunaEngine\bin
if exist LunaEngine\bin-int rd /s /q LunaEngine\bin-int

REM
for /r %%i in (*.sln *.vcxproj *.vcxproj.filters *.vcxproj.user) do (
    del /f /q "%%i"
)

echo Clean Complete
pause