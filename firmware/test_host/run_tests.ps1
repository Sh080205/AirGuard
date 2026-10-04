$ErrorActionPreference = 'Stop'
$outputDir = Join-Path $PSScriptRoot '../build/test_host'
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$component = Join-Path $PSScriptRoot '../components/pms7003'
$commonArgs = @('-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic', '-O2',
    "-I$PSScriptRoot/stubs", "-I$component/include", "-I$PSScriptRoot/../main")
foreach ($suite in @('parse', 'uart')) {
    $binary = Join-Path $outputDir "test_pms7003_$suite.exe"
    $sources = @("$component/pms7003_parse.c", "$PSScriptRoot/test_pms7003_$suite.c")
    if ($suite -eq 'uart') {
        $sources += "$component/pms7003_uart.c"
    }
    & gcc @commonArgs @sources -o $binary
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $binary
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

# FW-3: pure C modules; only the esp_err.h stub is needed.
$pureSuites = @(
    @{ Name = 'csv_row'; Component = 'storage'; Source = 'csv_row.c' },
    @{ Name = 'aht21_parse'; Component = 'aht21'; Source = 'aht21_parse.c' }
)
foreach ($suite in $pureSuites) {
    $dir = Join-Path $PSScriptRoot "../components/$($suite.Component)"
    $binary = Join-Path $outputDir "test_$($suite.Name).exe"
    $sources = @("$dir/$($suite.Source)", "$PSScriptRoot/test_$($suite.Name).c")
    & gcc @commonArgs "-I$dir/include" @sources -o $binary
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $binary
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
exit 0
