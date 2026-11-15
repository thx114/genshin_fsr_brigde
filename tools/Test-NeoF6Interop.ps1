#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BuildDirectory,
    [Parameter(Mandatory)][string]$F6Dll,
    [Parameter(Mandatory)][string]$FixtureIni,
    [Parameter(Mandatory)][ValidateNotNullOrEmpty()][string]$F6Marker
)
$ErrorActionPreference = 'Stop'
$build = (Resolve-Path -LiteralPath $BuildDirectory).Path
$f6 = (Resolve-Path -LiteralPath $F6Dll).Path
$ini = (Resolve-Path -LiteralPath $FixtureIni).Path
$root = Join-Path $build ('mixed-neo-' + (Get-Date -Format yyyyMMdd-HHmmss-fff))
$null = New-Item -ItemType Directory -Path $root
Copy-Item -LiteralPath "$build\Dx11FsrBridge.dll", "$build\CreateRouteIntegrationTest.exe" -Destination $root
Copy-Item -LiteralPath $f6 -Destination "$root\OptiScaler.dll"
Copy-Item -LiteralPath $ini -Destination "$root\OptiScaler.fixture.ini"
Get-FileHash -LiteralPath "$root\Dx11FsrBridge.dll", "$root\OptiScaler.dll", "$root\OptiScaler.fixture.ini" |
    ConvertTo-Json | Set-Content -LiteralPath "$root\identity.json" -Encoding UTF8

function Read-Log([string]$Path)
{
    if (Test-Path -LiteralPath $Path) { return [string](Get-Content -LiteralPath $Path -Raw) }
    return ''
}

$rows = @()
foreach ($mode in @('hardware-for-hwnd-bridge-first-multithread', 'hardware-for-hwnd-bridge-first-returned',
                   'hardware-for-hwnd-device-first-multithread', 'hardware-for-hwnd-device-first-returned',
                   'hardware-for-hwnd-bridge-first-present', 'hardware-for-hwnd-device-first-present',
                   'hardware-for-hwnd-bridge-first-present1', 'hardware-for-hwnd-device-first-present1'))
{
    $out = "$root\$mode.stdout.log"
    $err = "$root\$mode.stderr.log"
    $arguments = @("$root\Dx11FsrBridge.dll", "$build\create-routing\First\d3d11.dll",
        "$build\create-routing\Second\d3d11.dll", "$build\CreateRouteImportsFirst.dll",
        "$build\CreateRouteImportsSecond.dll", $mode, "$root\OptiScaler.dll", "$root\OptiScaler.fixture.ini")
    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = "$root\CreateRouteIntegrationTest.exe"
    $start.Arguments = ($arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $start.WorkingDirectory = $root
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $process = [Diagnostics.Process]::Start($start)
    try
    {
        $outputTask = $process.StandardOutput.ReadToEndAsync()
        $errorTask = $process.StandardError.ReadToEndAsync()
        $ended = $process.WaitForExit(120000)
        if (!$ended) { $process.Kill(); $process.WaitForExit() }
        $stdout = $outputTask.GetAwaiter().GetResult()
        $stderr = $errorTask.GetAwaiter().GetResult()
        [IO.File]::WriteAllText($out, $stdout)
        [IO.File]::WriteAllText($err, $stderr)
        $run = "$root\create-routing-run-$($process.Id)"
        $bridge = Read-Log "$run\Dx11FsrBridge.log"
        $f6Log = Read-Log "$run\OptiScaler.log"
        $row = [ordered]@{
            Mode = $mode
            PID = $process.Id
            Exit = $process.ExitCode
            TimedOut = !$ended
            Neo = $bridge.Contains('runtime=neo observation_schema=neo.1')
            F6 = $f6Log.Contains($F6Marker)
            F6Factory = $f6Log -match 'hkCreateDXGIFactory1 Caller:\s+CreateRouteIntegrationTest\.exe'
            Dx11On12ForHwnd = $f6Log.Contains('Created Dx11wDx12SC HWND')
            F6GetDevice = $stdout.Contains('PASS: mixed ForHwnd returns an F6-owned GetDevice implementation')
            Candidate = $bridge.Contains('fsr2_translation_candidate render=64x64 output=128x128')
            Pixels = $stdout.Contains('PASS: selected immediate context DrawIndexed preserves real pixels') -and
                $stdout.Contains('PASS: selected immediate context Draw preserves real pixels')
            Rebind = !$mode.EndsWith('-multithread') -or
                $stdout.Contains('PASS: caller-held protection allows a nested switch')
            RunDirectory = $run
        }
        $row.Pass = $ended -and ($row.Exit -eq 0) -and $row.Neo -and $row.F6 -and $row.F6Factory -and
            $row.Dx11On12ForHwnd -and $row.F6GetDevice -and $row.Candidate -and $row.Pixels -and $row.Rebind
        $rows += [pscustomobject]$row
        $rows | ConvertTo-Json | Set-Content -LiteralPath "$root\mixed-result.json" -Encoding UTF8
        [pscustomobject]$row | Format-List
    }
    finally
    {
        $process.Dispose()
    }
}
Write-Host "Receipt: $root\mixed-result.json"
if (@($rows | Where-Object { !$_.Pass }).Count) { exit 1 }
