@echo off
echo =====================================
echo    LunaEngine Setup Script
echo =====================================

git submodule update --init --recursive
if errorlevel 1 (
    echo Failed to update main submodules.
    pause
    exit /b 1
)

cd ../..
echo Setup complete!
pause