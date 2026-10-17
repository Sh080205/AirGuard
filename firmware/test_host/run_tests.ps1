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

# FW-3/FW-4: pure C modules; only the esp_err.h stub is needed.
$pureSuites = @(
    @{ Name = 'csv_row'; Component = 'storage'; Source = 'csv_row.c' },
    @{ Name = 'aht21_parse'; Component = 'aht21'; Source = 'aht21_parse.c' },
    # FW-4: settime parser, row timestamp checks, stats
    @{ Name = 'logutil'; Component = 'logutil'; Source = 'logutil.c' }
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

# FW-5a: ENS160 parse/convert/hold (pure C); the hold rule reuses
# logutil_frame_fresh, so logutil.c is linked in as well.
$ensDir = Join-Path $PSScriptRoot '../components/ens160'
$logDir = Join-Path $PSScriptRoot '../components/logutil'
$binary = Join-Path $outputDir 'test_ens160_parse.exe'
& gcc @commonArgs "-I$ensDir/include" "-I$logDir/include" "$ensDir/ens160_parse.c" `
    "$logDir/logutil.c" "$PSScriptRoot/test_ens160_parse.c" -o $binary
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $binary
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# FW-7: Mahalanobis score (pure C) against the ML-3 header's golden vectors;
# the CSV round-trip check links csv_row.c.
$mahalDir = Join-Path $PSScriptRoot '../components/mahal'
$storageDir = Join-Path $PSScriptRoot '../components/storage'
$binary = Join-Path $outputDir 'test_mahal.exe'
& gcc @commonArgs "-I$mahalDir/include" "-I$mahalDir/model" "-I$storageDir/include" `
    "$mahalDir/ag_mahal.c" "$storageDir/csv_row.c" "$PSScriptRoot/test_mahal.c" -lm -o $binary
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $binary
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
exit 0
