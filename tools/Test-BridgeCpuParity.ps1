#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Benchmark,
    [Parameter(Mandatory)][string]$ReferenceDll,
    [Parameter(Mandatory)][string]$CandidateDll,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [ValidateRange(3,21)][int]$Trials = 7,
    [ValidateRange(0,100)][double]$MaxRegressionPercent = 0
)
$ErrorActionPreference = 'Stop'
$bench = (Resolve-Path -LiteralPath $Benchmark).Path
$reference = (Resolve-Path -LiteralPath $ReferenceDll).Path
$candidate = (Resolve-Path -LiteralPath $CandidateDll).Path
$root = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $root) { throw 'OutputDirectory must be a new directory.' }
$null = New-Item -ItemType Directory -Path $root
$sources = [ordered]@{ reference = $reference; candidate = $candidate }
$identities = [ordered]@{}
foreach ($name in $sources.Keys)
{
    $stage = Join-Path $root $name
    $null = New-Item -ItemType Directory -Path $stage
    $target = Join-Path $stage 'Dx11FsrBridge.dll'
    Copy-Item -LiteralPath $sources[$name] -Destination $target
    $sourceHash = (Get-FileHash -LiteralPath $sources[$name] -Algorithm SHA256).Hash
    $copyHash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
    if ($sourceHash -ne $copyHash) { throw "DLL copy changed: $name" }
    $identities[$name] = [ordered]@{ source = $sources[$name]; sha256 = $copyHash }
}

function Get-Median([double[]]$Values)
{
    $sorted = @($Values | Sort-Object)
    if ($sorted.Count -eq 0) { throw 'A benchmark sample set is empty.' }
    $middle = [int][Math]::Floor($sorted.Count / 2)
    if ($sorted.Count % 2) { return $sorted[$middle] }
    return ($sorted[$middle - 1] + $sorted[$middle]) / 2
}

$runs = @()
$configurationHash = $null
for ($trial = 0; $trial -lt $Trials; ++$trial)
{
    # 2026-11-16：交替先后次序，保留原始样本；不在两个测量进程之间并发其它测试。
    $order = if ($trial % 2) { @('candidate','reference') } else { @('reference','candidate') }
    foreach ($name in $order)
    {
        $stage = Join-Path $root $name
        $stdout = Join-Path $stage ("trial-{0}.stdout.json" -f $trial)
        $stderr = Join-Path $stage ("trial-{0}.stderr.txt" -f $trial)
        $dll = Join-Path $stage 'Dx11FsrBridge.dll'
        $process = Start-Process -FilePath $bench -ArgumentList ('"{0}"' -f $dll) -WorkingDirectory $stage `
            -RedirectStandardOutput $stdout -RedirectStandardError $stderr -WindowStyle Hidden -PassThru
        $null = $process.Handle
        if (-not $process.WaitForExit(60000))
        {
            $process.Kill()
            $process.WaitForExit()
            throw "Benchmark timed out: $name trial $trial"
        }
        if ($process.ExitCode -ne 0)
        {
            throw "Benchmark failed: $name trial $trial exit=$($process.ExitCode); see $stderr"
        }
        $sample = Get-Content -LiteralPath $stdout -Raw | ConvertFrom-Json
        if (-not $sample.draw_owned -or -not $sample.indexed_owned -or $sample.iterations -le 0 -or
            $sample.rounds -le 0 -or @($sample.samples_ns).Count -ne 3 -or
            $sample.hook_checks -le 0)
        {
            throw "Benchmark did not prove the three production Draw paths: $name"
        }
        foreach ($mode in $sample.samples_ns)
        {
            if (@($mode).Count -ne $sample.rounds) { throw 'Round count does not match the samples.' }
            foreach ($value in $mode)
            {
                $number = [double]$value
                if ([double]::IsNaN($number) -or [double]::IsInfinity($number) -or $number -le 0)
                {
                    throw 'Invalid timing sample.'
                }
            }
        }
        $iniHash = (Get-FileHash -LiteralPath (Join-Path $stage 'Dx11FsrBridge.ini') -Algorithm SHA256).Hash
        if ($null -eq $configurationHash) { $configurationHash = $iniHash }
        elseif ($configurationHash -ne $iniHash) { throw 'Reference and candidate INI bytes differ.' }
        $runs += [pscustomobject]@{ side = $name; trial = $trial; data = $sample }
    }
}

$rows = @()
$names = @('Draw(0)', 'DrawIndexed(0)', 'Draw(3), no FSR inputs')
for ($mode = 0; $mode -lt $names.Count; ++$mode)
{
    $before = @($runs | Where-Object side -eq 'reference' | ForEach-Object { $_.data.samples_ns[$mode] })
    $after = @($runs | Where-Object side -eq 'candidate' | ForEach-Object { $_.data.samples_ns[$mode] })
    $beforeMedian = Get-Median $before
    $afterMedian = Get-Median $after
    $change = ($afterMedian / $beforeMedian - 1) * 100
    $rows += [pscustomobject]@{
        mode = $names[$mode]
        reference_ns = $beforeMedian
        candidate_ns = $afterMedian
        change_percent = $change
        pass = $change -le $MaxRegressionPercent
    }
}
$passed = @($rows | Where-Object { -not $_.pass }).Count -eq 0
$report = [ordered]@{
    scope = 'WARP CPU Draw entry cost only; no GPU, game FPS, startup, frame pacing or image-quality verdict'
    passed = $passed
    max_regression_percent = $MaxRegressionPercent
    configuration_sha256 = $configurationHash
    benchmark_sha256 = (Get-FileHash -LiteralPath $bench -Algorithm SHA256).Hash
    dlls = $identities
    rows = $rows
    runs = $runs
}
$report | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $root 'result.json') -Encoding UTF8
$rows | Format-Table -AutoSize
Write-Host ("CPU Draw parity={0}; receipt={1}" -f $passed, (Join-Path $root 'result.json'))
if (-not $passed) { exit 1 }
