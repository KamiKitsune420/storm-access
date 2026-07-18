# build-and-install.ps1
# Builds the Storm 4 accessibility mod (xinput9_1_0.dll proxy) and installs the
# runtime files into the game folder. Defaults to the GENUINE Steam install.
#
# Usage:
#   pwsh ./build-and-install.ps1                 # build + install to Steam copy
#   pwsh ./build-and-install.ps1 -SkipBuild      # just (re)install existing build
#   pwsh ./build-and-install.ps1 -GameRoot "X:\path\to\game"   # install elsewhere
#
# NOTE: the game must NOT be running when this copies the DLL.

param(
    [string]$GameRoot = "C:\Program Files (x86)\Steam\steamapps\common\NARUTO SHIPPUDEN Ultimate Ninja STORM 4",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$ScriptDir = $PSScriptRoot

$MSBuild  = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe"
$Project  = Join-Path $ScriptDir "ns4moddingapi\d3dcompiler_47_og\d3dcompiler_47_og.vcxproj"
$BuiltDll = Join-Path $ScriptDir "ns4moddingapi\d3dcompiler_47_og\x64\Release\xinput9_1_0.dll"
$LibDir   = Join-Path $ScriptDir "lib"

# ---- Guard: game must not be running -------------------------------------
if (Get-Process -Name "NSUNS4" -ErrorAction SilentlyContinue) {
    throw "NSUNS4.exe is running. Close the game before installing (the DLL is locked while it runs)."
}

# ---- Build ---------------------------------------------------------------
if (-not $SkipBuild) {
    Write-Host "==> Building mod..." -ForegroundColor Cyan
    # This machine ships v143 + SDK 10.0.26100; the project's stock v141 / 10.0.17763 aren't installed.
    & $MSBuild $Project `
        /p:Configuration=Release `
        /p:Platform=x64 `
        /p:PlatformToolset=v143 `
        /p:WindowsTargetPlatformVersion=10.0.26100.0 `
        /v:minimal
    if ($LASTEXITCODE -ne 0) { throw "MSBuild failed (exit $LASTEXITCODE)." }
}

if (-not (Test-Path $BuiltDll)) { throw "Built DLL not found: $BuiltDll (build first, drop -SkipBuild)." }
if (-not (Test-Path $GameRoot)) { throw "Game folder not found: $GameRoot" }

# ---- Install (additive) --------------------------------------------------
Write-Host "==> Installing to: $GameRoot" -ForegroundColor Cyan

# 1) our proxy
Copy-Item $BuiltDll (Join-Path $GameRoot "xinput9_1_0.dll") -Force
Write-Host "    xinput9_1_0.dll (mod proxy)"

# 2) real xinput the proxy chain-loads (only if missing — never overwrite)
$realXinput = Join-Path $GameRoot "xinput9_1_0_o.dll"
if (-not (Test-Path $realXinput)) {
    Copy-Item "C:\Windows\System32\xinput9_1_0.dll" $realXinput -Force
    Write-Host "    xinput9_1_0_o.dll (real xinput, from System32)"
} else {
    Write-Host "    xinput9_1_0_o.dll (already present)"
}

# 3) speech libs
foreach ($dll in "Tolk.dll","nvdaControllerClient64.dll","SAAPI64.dll") {
    Copy-Item (Join-Path $LibDir $dll) (Join-Path $GameRoot $dll) -Force
    Write-Host "    $dll"
}

# 4) modding-api config + required empty mods dir
$modApi = Join-Path $GameRoot "moddingapi"
New-Item -ItemType Directory -Force -Path (Join-Path $modApi "mods") | Out-Null
$cfg = Join-Path $modApi "config.ini"
if (-not (Test-Path $cfg)) {
    Set-Content -Path $cfg -Value "[General]`r`nEnableConsole=0`r`nEnableModList=0" -Encoding ascii
}
Write-Host "    moddingapi\config.ini + moddingapi\mods\"

Write-Host "==> Done. Launch the game; check $GameRoot\storm_access.log for 'Storm accessibility loaded'." -ForegroundColor Green
