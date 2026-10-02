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
    $vippTestBinary = 'output/vipp4_diagnostics_test.exe'
    if ($AddressSanitizer) {
        $vippTestArgs += '-fsanitize=address', '-fno-omit-frame-pointer'
        $vippTestBinary = 'output/vipp4_diagnostics_test_asan.exe'
    }
    & $vippCompilerPath @vippTestArgs 'tests/vipp4_diagnostics_test.c' '-o' $vippTestBinary
    if ($LASTEXITCODE -ne 0) { throw 'VIPP4 test compilation failed' }
    & (Join-Path $vippTestRoot $vippTestBinary)
    if ($LASTEXITCODE -ne 0) { throw 'VIPP4 test failed' }
} finally {
    $env:PATH = $vippOriginalPath
    Pop-Location
}
