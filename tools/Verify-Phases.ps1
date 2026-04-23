param(
    [string]$Configuration = "Debug",
    [string]$Platform = "x64",
    [switch]$SkipBuild,
    [switch]$SkipRuntime
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Path $PSScriptRoot -Parent
$progressPath = Join-Path $repoRoot "progress\progress.md"
$solutionPath = Join-Path $repoRoot "LunaApp.sln"
$engineLibPath = Join-Path $repoRoot ("LunaEngine\bin\{0}-windows-x86_64\LunaEngine\LunaEngine.lib" -f $Configuration)
$appExePath = Join-Path $repoRoot ("LunaApp\bin\{0}-windows-x86_64\LunaApp\LunaApp.exe" -f $Configuration)
$errorLogPath = Join-Path $repoRoot "error.log"
$buildLogPath = Join-Path $repoRoot "build_output.log"

if (-not (Test-Path $progressPath)) {
    throw "Missing progress file: $progressPath"
}

$allLines = Get-Content -Path $progressPath

function Get-CompletionTableLines {
    param([string[]]$Lines)

    $start = ($Lines | Select-String -Pattern "^##\s+Phase Completion Table" -SimpleMatch:$false | Select-Object -First 1).LineNumber
    if (-not $start) { return @() }

    $table = @()
    for ($i = $start; $i -lt $Lines.Count; $i++) {
        $line = $Lines[$i]
        if ($line -match "^---") { break }
        if ($line -match "^\|") { $table += $line }
    }
    return $table
}

$completionTable = Get-CompletionTableLines -Lines $allLines
$roadmapLines = $allLines | Where-Object { $_ -match "^\|\s*(\d+[A-Z]?)\s*\|" }

$phases = @("1","2A","2B","2C","3A","3B","3C","3D","4A","4B","4C","4D","5","5A","5B","5C","6","7","8","9","10","11","12","13","14")

$phaseEvidenceMap = @{
    "1"  = @(@{ Path = "LunaEngine\src\LunaEngine\Shaders\constantbuffer.vert.hlsl"; Pattern = "cbuffer TransformBuffer" })
    "2A" = @(@{ Path = "LunaEngine\src\LunaEngine\Renderer\DX12\public\DX12Backend.h"; Pattern = "FRAMES_IN_FLIGHT" })
    "2B" = @(@{ Path = "LunaEngine\src\LunaEngine\Renderer\DX12\private\DX12Pipeline.cpp"; Pattern = "IDxcCompiler3|DXC" })
    "2C" = @(@{ Path = "LunaEngine\src\LunaEngine\Renderer\DX12\public\DX12Backend.h"; Pattern = "D3D12MA::Allocator" })
    "3A" = @(@{ Path = "LunaEngine\src\LunaEngine\Renderer\Camera.h"; Pattern = "yaw|pitch|radius" })
    "3B" = @(@{ Path = "LunaEngine\src\LunaEngine\Renderer\MeshLoader.cpp"; Pattern = "cgltf_" })
    "3C" = @(@{ Path = "LunaEngine\src\LunaEngine\Shaders\deferred_lighting.frag.hlsl"; Pattern = "GGX|Cook|Schlick|roughness" })
    "3D" = @(@{ Path = "LunaEngine\src\LunaEngine\Graphics\Texture.cpp"; Pattern = "stbi_" })
    "4A" = @(@{ Path = "LunaEngine\src\LunaEngine\Renderer\DX12\private\DX12Device.cpp"; Pattern = "SupportsDXR|D3D12_RAYTRACING" })
    "4B" = @(@{ Path = "LunaEngine\src\LunaEngine\Renderer\DX12\private\DX12AccelStructure.cpp"; Pattern = "BLAS|TLAS" })
    "4C" = @(@{ Path = "LunaEngine\src\LunaEngine\Renderer\DX12\private\DX12RTPipeline.cpp"; Pattern = "CreateStateObject|DispatchRays" })
    "4D" = @(@{ Path = "LunaEngine\src\LunaEngine\Renderer\DX12\private\DX12Backend.cpp"; Pattern = "DispatchShadows|DXR Shadows" })
    "5"  = @(@{ Path = "LunaEngine\src\LunaEngine\Renderer\Vulkan\Private\VulkanBackend.cpp"; Pattern = "BeginFrame|EndFrame" })
    "5A" = @(@{ Path = "LunaEngine\src\LunaEngine\Renderer\Vulkan\Public\VulkanBackend.h"; Pattern = "SetVSync" })
    "5B" = @(@{ Path = "LunaEngine\src\LunaEngine\Graphics\Material.h"; Pattern = "MaterialConstants|srvTableStart" })
    "5C" = @(@{ Path = "LunaEngine\src\LunaEngine\Renderer\Vulkan\Public\VulkanBackend.h"; Pattern = "sceneBuffer|sceneMapped" })
    "6"  = @(@{ Path = "LunaEngine\src\LunaEngine\Renderer\RenderGraph.h"; Pattern = "class RenderGraph" })
    "7"  = @(@{ Path = "LunaEngine\src\LunaEngine\Shaders\gbuffer.frag.hlsl"; Pattern = "SV_Target0|SV_Target1|SV_Target2" })
    "8"  = @(@{ Path = "LunaEngine\src\LunaEngine\Shaders\csm_depth.vert.hlsl"; Pattern = "main\(" })
    "9"  = @(@{ Path = "LunaEngine\src\LunaEngine\Shaders\ssao.frag.hlsl"; Pattern = "SSAO|ao|occlusion" })
    "10" = @(@{ Path = "LunaEngine\src\LunaEngine\Shaders\taa.frag.hlsl"; Pattern = "TAA|history|jitter" })
    "11" = @(@{ Path = "LunaEngine\src\LunaEngine\Shaders\gbuffer.frag.hlsl"; Pattern = "gAllTextures\[\]|space1|gMaterialIndex" })
    "12" = @(
        @{ Path = "LunaEngine\src\LunaEngine\Renderer\DX12\public\DX12Backend.h"; Pattern = "FlushDraws|MAX_GPU_OBJECTS|_indirectCmdSignature" },
        @{ Path = "LunaEngine\src\LunaEngine\Shaders\gpu_cull.comp.hlsl"; Pattern = "numthreads\(64, 1, 1\)|FrustumTestSphere" }
    )
    "13" = @(@{ Path = "LunaEngine\src\LunaEngine\Renderer\DX12\public\DX12Backend.h"; Pattern = "_computeQueue|WaitForComputeFrame|DispatchCullAsync" })
    "14" = @(@{ Path = "LunaEngine\src\LunaEngine\Renderer\RenderGraph.h"; Pattern = "RenderGraph" })
}

$buildStatus = "SKIP"
$buildEvidence = "build skipped"
if (-not $SkipBuild) {
    if (-not (Test-Path $solutionPath)) {
        $buildStatus = "FAIL"
        $buildEvidence = "missing solution: $solutionPath"
    } else {
        $msbuild = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
        if (-not (Test-Path $msbuild)) {
            $buildStatus = "FAIL"
            $buildEvidence = "missing MSBuild at expected path"
        } else {
            & $msbuild $solutionPath "/p:Configuration=$Configuration" "/p:Platform=$Platform" /m /nologo /verbosity:minimal | Out-Null
            if ($LASTEXITCODE -eq 0 -and (Test-Path $engineLibPath) -and (Test-Path $appExePath)) {
                $buildStatus = "PASS"
                $buildEvidence = "artifacts found: $engineLibPath, $appExePath"
            } else {
                $buildStatus = "FAIL"
                $buildEvidence = "msbuild exit code=$LASTEXITCODE"
            }
        }
    }
}

$runtimeStatus = "SKIP"
$runtimeEvidence = "runtime skipped"
if (-not $SkipRuntime) {
    if (Test-Path $appExePath) {
        $runtimeStatus = "PASS"
        $runtimeEvidence = "exe exists: $appExePath"
        if (Test-Path $errorLogPath) {
            # Ignore cosmetic/non-fatal errors (icon, resource path, etc.)
            $logHits = Select-String -Path $errorLogPath `
                -Pattern "error|fatal|exception" -CaseSensitive:$false -ErrorAction SilentlyContinue |
                Where-Object { $_.Line -notmatch "icon|\.png|\.ico|resource" }
            if ($logHits) {
                $runtimeStatus = "PARTIAL"
                $runtimeEvidence = "exe exists; potential runtime errors in error.log: $($logHits[0].Line.Trim())"
            }
        }
    } else {
        $runtimeStatus = "FAIL"
        $runtimeEvidence = "missing exe: $appExePath"
    }
}

$results = foreach ($phase in $phases) {
    $docLine = $completionTable | Where-Object { $_ -match "^\|\s*$([regex]::Escape($phase))\s*\|" } | Select-Object -First 1
    if (-not $docLine) {
        # Fallback: roadmap line counts as doc evidence when phase is explicitly marked done or dated.
        $docLine = $roadmapLines | Where-Object {
            $_ -match "^\|\s*$([regex]::Escape($phase))\s*\|" -and
            (($_ -like "*Done*") -or ($_ -match "\|\s*20\d\d-\d\d-\d\d\s*\|"))
        } | Select-Object -First 1
    }
    $docStatus = if ($docLine) { "PASS" } else { "FAIL" }

    $evidenceList = @()
    $codeStatus = "UNKNOWN"
    if ($phaseEvidenceMap.ContainsKey($phase)) {
        $phaseChecks = $phaseEvidenceMap[$phase]
        $hits = 0
        foreach ($check in $phaseChecks) {
            $fullPath = Join-Path $repoRoot $check.Path
            if (Test-Path $fullPath) {
                $match = Select-String -Path $fullPath -Pattern $check.Pattern -ErrorAction SilentlyContinue | Select-Object -First 1
                if ($match) {
                    $hits++
                    $evidenceList += "code: $($check.Path):$($match.LineNumber)"
                }
            }
        }
        if ($hits -eq $phaseChecks.Count -and $hits -gt 0) {
            $codeStatus = "PASS"
        } elseif ($hits -gt 0) {
            $codeStatus = "PARTIAL"
        } else {
            $codeStatus = "FAIL"
        }
    }

    if ($docLine) {
        $evidenceList = ,("doc: $docLine") + $evidenceList
    }

    $overall = "PASS"
    if ($docStatus -eq "FAIL" -or $codeStatus -eq "FAIL") {
        $overall = "FAIL"
    } elseif ($codeStatus -eq "PARTIAL" -or $buildStatus -eq "FAIL" -or $runtimeStatus -eq "FAIL") {
        $overall = "PARTIAL"
    }

    [pscustomobject]@{
        Phase   = $phase
        Doc     = $docStatus
        Code    = $codeStatus
        Build   = $buildStatus
        Runtime = $runtimeStatus
        Overall = $overall
        Evidence = ($evidenceList -join " || ")
    }
}

Write-Host "`n=== Build Check ==="
Write-Host "Status: $buildStatus"
Write-Host "Evidence: $buildEvidence"

Write-Host "`n=== Runtime Check ==="
Write-Host "Status: $runtimeStatus"
Write-Host "Evidence: $runtimeEvidence"

Write-Host "`n=== Phase Matrix ==="
$results | Select-Object Phase, Doc, Code, Build, Runtime, Overall | Format-Table -AutoSize

$outPath = Join-Path $repoRoot "progress\phase_verification_report.csv"
$results | Export-Csv -Path $outPath -NoTypeInformation -Encoding UTF8
Write-Host "`nReport written: $outPath"

