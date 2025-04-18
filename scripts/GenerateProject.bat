@echo off
echo =====================================
echo      Generating Visual Studio Project
echo =====================================

pushd ..
vendor\bin\premake5.exe vs2022
popd

echo Premake generation complete!
pause