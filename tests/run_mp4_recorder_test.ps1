param(
    [string]$Compiler = 'clang',
    [switch]$AddressSanitizer
)

# Host tests use SDK types and fake MPP functions, never board drivers.
$ErrorActionPreference = 'Stop'
$mp4TestRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$mp4CompilerPath = (Get-Command $Compiler -CommandType Application).Source
$mp4OriginalPath = $env:PATH
Push-Location $mp4TestRoot
try {
    $env:PATH = (Split-Path -Parent $mp4CompilerPath) + ';' + $env:PATH
    New-Item -ItemType Directory -Path 'output' -Force | Out-Null
    $mp4TestArgs = @('-std=gnu11', '-g', '-pthread', '-DAWCHIP=0x1886',
                     '-Itests/mp4_stubs', '-Itests/stubs',
                     '-Isample/ipc_camera/include', '-Isample/common')
    # Collect SDK include paths from Makefile instead of hard-coding this PC.
    foreach ($mp4MakeLine in (Get-Content -Encoding UTF8 'Makefile')) {
        if ($mp4MakeLine -match '^USER_INC_BASE_DIR\s*\+=\s*\$\(ROOT_DIR\)/(.*)$') {
            $mp4IncludeRoot = $Matches[1].Trim()
            if (Test-Path -LiteralPath $mp4IncludeRoot -PathType Container) {
                $mp4TestArgs += '-I' + $mp4IncludeRoot
                foreach ($mp4IncludeDir in (Get-ChildItem -LiteralPath $mp4IncludeRoot -Recurse -Directory)) {
                    $mp4TestArgs += '-I' + $mp4IncludeDir.FullName
                }
            }
        }
    }
    $mp4TestBinary = 'output/mp4_recorder_test.exe'
    if ($AddressSanitizer) {
        $mp4TestArgs += '-fsanitize=address', '-fno-omit-frame-pointer'
        $mp4TestBinary = 'output/mp4_recorder_test_asan.exe'
    }
    & $mp4CompilerPath @mp4TestArgs 'tests/mp4_recorder_test.c' '-o' $mp4TestBinary
    if ($LASTEXITCODE -ne 0) { throw 'MP4 test compilation failed' }
    & (Join-Path $mp4TestRoot $mp4TestBinary)
    if ($LASTEXITCODE -ne 0) { throw 'MP4 test failed' }
} finally {
    $env:PATH = $mp4OriginalPath
    Pop-Location
}
