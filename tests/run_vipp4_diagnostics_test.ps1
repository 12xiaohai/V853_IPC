param(
    [string]$Compiler = 'clang',
    [switch]$AddressSanitizer
)

# Host-only MPP substitutes. Product Makefile never includes these headers.
$ErrorActionPreference = 'Stop'
$vippTestRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$vippCompilerPath = (Get-Command $Compiler -CommandType Application).Source
$vippOriginalPath = $env:PATH
Push-Location $vippTestRoot
try {
    $env:PATH = (Split-Path -Parent $vippCompilerPath) + ';' + $env:PATH
    New-Item -ItemType Directory -Path 'output' -Force | Out-Null
    $vippTestArgs = @('-std=gnu11', '-Wall', '-Wextra', '-Werror', '-g',
                      '-pthread', '-Itests/vipp4_stubs',
                      '-Isample/ipc_camera/include')
    if ($AddressSanitizer) {
        $vippTestArgs += '-fsanitize=address', '-fno-omit-frame-pointer'
    }
    foreach ($vippTestName in @('vipp4_diagnostics_test', 'isp_3dnr_diagnostics_test')) {
        $vippTestSuffix = if ($AddressSanitizer) { '_asan' } else { '' }
        $vippTestBinary = "output/$vippTestName$vippTestSuffix.exe"
        & $vippCompilerPath @vippTestArgs "tests/$vippTestName.c" '-o' $vippTestBinary
        if ($LASTEXITCODE -ne 0) { throw "$vippTestName compilation failed" }
        & (Join-Path $vippTestRoot $vippTestBinary)
        if ($LASTEXITCODE -ne 0) { throw "$vippTestName failed" }
    }
} finally {
    $env:PATH = $vippOriginalPath
    Pop-Location
}
