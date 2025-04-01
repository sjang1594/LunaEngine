-- premake5.lua
workspace "LunaApp"
   architecture "x64"
   configurations { "Debug", "Release", "Dist" }
   startproject "LunaApp"

   outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"
   
   targetdir ("bin/" .. outputdir .. "/%{prj.name}")
   objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

include "LunaEngineExternal.lua"
include "LunaApp"

project "LunaApp"
   dependson "LunaEngine"