param(
    [string]$SourceRoot = $env:RAID_MANAGER_SOURCE_ROOT,
    [string]$EnvironmentScript
)
$ErrorActionPreference = 'Stop'
$env:PYTHONDONTWRITEBYTECODE = '1'
if ($SourceRoot) { $env:RAID_MANAGER_SOURCE_ROOT = $SourceRoot }
if (-not $EnvironmentScript) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Install VS C++ tools or pass -EnvironmentScript.' }
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if (-not $installation) { throw 'No compatible VS C++ installation was found.' }
    $EnvironmentScript = Join-Path $installation 'VC/Auxiliary/Build/vcvars64.bat'
}
if (-not (Test-Path -LiteralPath $EnvironmentScript)) { throw 'Developer environment script not found.' }
$runner = Join-Path $PSScriptRoot 'run_offline_tests.py'
$nativeCommand = 'call "{0}" >nul && python "{1}"' -f $EnvironmentScript, $runner
& $env:ComSpec /d /c $nativeCommand
exit $LASTEXITCODE
