workspace "LunaEngineApp"
    architecture "x64"
    configurations = {"Debug", "Release", "Dist"}
    startproject "LunaEngineApp"

outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

include "LunaEngineExternal.lua"
include "LunaEAPP"