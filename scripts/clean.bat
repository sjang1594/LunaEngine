@echo off
setlocal enabledelayedexpansion

echo.
echo ============================
echo   Cleaning LunaEngine...
echo ============================

REM
set DIRS=bin bin-int

for %%D in (%DIRS%) do (
    if exist %%D (
        echo Removing folder: %%D
        rmdir /s /q %%D
    ) else (
        echo Skipping (not found): %%D
    )
)

REM 
set EXTENSIONS=*.sln *.vcxproj *.vcxproj.filters *.vcxproj.user

for %%E in (%EXTENSIONS%) do (
    for /r %%F in (%%E) do (
        echo Deleting file: %%F
        del /q "%%F"
    )
)

echo.
echo Clean complete!
pause