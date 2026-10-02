param(
    [string]$Compiler = 'clang',
    [switch]$AddressSanitizer
)

# Compile real application/config with fake media modules; no board access.
$ErrorActionPreference = 'Stop'
$appTestRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$appCompilerPath = (Get-Command $Compiler -CommandType Application).Source
$appOriginalPath = $env:PATH
Push-Location $appTestRoot
try {
    $env:PATH = (Split-Path -Parent $appCompilerPath) + ';' + $env:PATH
    New-Item -ItemType Directory -Path 'output' -Force | Out-Null
    $appTestArgs = @('-std=gnu11', '-g', '-Wall', '-Wextra', '-pthread',
                     '-DAWCHIP=0x1886', '-Itests/mp4_stubs', '-Itests/stubs',
                     '-Isample/ipc_camera/include', '-Isample/common')
    foreach ($appMakeLine in (Get-Content -Encoding UTF8 'Makefile')) {
        if ($appMakeLine -match '^USER_INC_BASE_DIR\s*\+=\s*\$\(ROOT_DIR\)/(.*)$') {
            $appIncludeRoot = $Matches[1].Trim()
            if (Test-Path -LiteralPath $appIncludeRoot -PathType Container) {
                $appTestArgs += '-I' + $appIncludeRoot
                foreach ($appIncludeDir in (Get-ChildItem -LiteralPath $appIncludeRoot -Recurse -Directory)) {
                    $appTestArgs += '-I' + $appIncludeDir.FullName
                }
            }
        }
    }
    $appTestBinary = 'output/application_test.exe'
    if ($AddressSanitizer) {
        $appTestArgs += '-fsanitize=address', '-fno-omit-frame-pointer'
        $appTestBinary = 'output/application_test_asan.exe'
    }
    & $appCompilerPath @appTestArgs 'tests/application_test.c' '-o' $appTestBinary
    if ($LASTEXITCODE -ne 0) { throw 'Application test compilation failed' }
    & (Join-Path $appTestRoot $appTestBinary)
    if ($LASTEXITCODE -ne 0) { throw 'Application test failed' }
} finally {
    $env:PATH = $appOriginalPath
    Pop-Location
}
