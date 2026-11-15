#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BuildDirectory,
    [Parameter(Mandatory)][string]$FfxDll,
    [Parameter(Mandatory)][string]$F6FixtureDirectory,
    [ValidateSet('ngx', 'ngx-resize', 'ngx-receipt', 'ngx-skip-control', 'ngx-v1', 'ngx-v1-resize', 'ngx-v1-skip-control',
        'ngx-v1-ao-on', 'ngx-v1-ao-off', 'ngx-nvidia-ao-on', 'ngx-nvidia-ao-off', 'ngx-v1-present', 'ngx-nvidia-present',
        'ngx-v1-dlaa', 'ngx-nvidia-dlaa',
        'native', 'native-async', 'native-resize', 'native-fsr3', 'native-fsr4', 'native-fsr4-fallback',
        'native-reject', 'native-fallback')][string[]]$Cases = @(
        'ngx', 'ngx-resize', 'ngx-receipt', 'native', 'native-async', 'native-resize', 'native-reject', 'native-fallback'),
    [ValidateRange(15,300)][int]$TimeoutSeconds = 65
)
$ErrorActionPreference = 'Stop'
$build = (Resolve-Path -LiteralPath $BuildDirectory).Path
$fixture = (Resolve-Path -LiteralPath $F6FixtureDirectory).Path
$ffx = (Resolve-Path -LiteralPath $FfxDll).Path
$root = Join-Path $build ('providers-' + (Get-Date -Format yyyyMMdd-HHmmss-fff))
$null = New-Item -ItemType Directory -Path $root

function Select-Lines([string]$Path, [string]$Pattern)
{
    if (Test-Path -LiteralPath $Path -PathType Leaf)
    {
        Select-String -LiteralPath $Path -Pattern $Pattern | ForEach-Object Line
    }
}

$rows = @()
foreach ($case in $Cases)
{
    $path = Join-Path $root $case
    $null = New-Item -ItemType Directory -Path $path
    Copy-Item -LiteralPath "$build\Dx11FsrBridge.dll", "$build\ProviderFrameHost.exe" -Destination $path
    $mode = if ($case -eq 'native-fsr4-fallback') { 'native-fsr4' } else { $case }
    if ($case -eq 'ngx-receipt')
    {
        Copy-Item -LiteralPath "$build\F6ReceiptProvider.dll" -Destination $path
    }
    elseif ($case -like 'ngx-nvidia-*')
    {
        Copy-Item -LiteralPath "$fixture\nvngx_dlss.dll" -Destination $path
    }
    elseif ($case -like 'ngx*')
    {
        Copy-Item -LiteralPath "$fixture\OptiScaler.dll", "$fixture\OptiScaler.ini", "$fixture\nvngx_dlss.dll" -Destination $path
        if (Test-Path -LiteralPath "$fixture\streamline" -PathType Container)
        {
            Copy-Item -LiteralPath "$fixture\streamline" -Destination $path -Recurse
        }
        if ($case -in 'ngx-skip-control', 'ngx-v1-skip-control')
        {
            if ($case -eq 'ngx-skip-control') { $mode = 'ngx' }
            $ini = Join-Path $path 'OptiScaler.ini'
            $before = [IO.File]::ReadAllText($ini)
            $after = [regex]::Replace($before, '(?m)^(SkipFirstFrames[^\S\r\n]*=[^\S\r\n]*)[^\r\n;]*', '${1}2')
            if ($before -eq $after) { throw 'SkipFirstFrames key was absent or already 2 in the reference fixture.' }
            [IO.File]::WriteAllText($ini, $after, [Text.UTF8Encoding]::new($false))
        }
    }
    elseif ($case -eq 'native-reject')
    {
        Copy-Item -LiteralPath "$build\FsrRejectProvider.dll" -Destination $path
    }
    elseif ($case -ne 'native-fallback')
    {
        Copy-Item -LiteralPath $ffx -Destination (Join-Path $path 'amd_fidelityfx_upscaler_dx12.dll')
    }

    $stdout = Join-Path $path 'stdout.log'
    $stderr = Join-Path $path 'stderr.log'
    $process = Start-Process -FilePath "$path\ProviderFrameHost.exe" -ArgumentList $mode -WorkingDirectory $path `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr -WindowStyle Hidden -PassThru
    $null = $process.Handle # 保留句柄，PS 5.1 才能在进程结束后读取 ExitCode。
    $complete = $process.WaitForExit($TimeoutSeconds * 1000)
    if (-not $complete) { $process.Kill(); $process.WaitForExit() }
    $pixels = @(Select-Lines "$path\provider-receipt.txt" '^target=|^provider (host|export|dimensions):|^receipt host:|^native ao:|^present host:')
    $mock = if ($case -eq 'ngx-receipt') { @(Select-Lines $stdout '^receipt fixture:') } else { @() }
    $bridge = @(Select-Lines "$path\Dx11FsrBridge.log" 'ffx12_result|ngx_core (release_all|create_feature|provider resolved=)|ngx_in jitter|sdk_selected|ffx12_version|FFX : FSR|ngx_out')
    $f6 = @(Select-Lines "$path\OptiScaler.log" 'NVSDK_NGX_D3D11_.*Feature|Hotfix.SkipFirstFrames:|F6 evaluate receipt v1')
    $writes = @($bridge -match 'rc=DISPATCH_OK copy_enqueued=1').Count
    $samples = @($pixels -match '^target=')
    $selected = $null
    foreach ($line in $bridge)
    {
        if ($line -match '\bFFX : FSR(?<version>\d+(?:\.\d+)+)\b') { $selected = $Matches['version'] }
    }
    $frameCount = if ($case -in 'ngx-resize', 'ngx-v1-resize', 'native-resize') { 4 } elseif ($case -eq 'ngx-receipt') { 8 } else { 2 }
    # 日志由后台写入，进程退出前可能只落盘首帧；按宿主同步读回的当帧像素验收。
    $passed = $complete -and $process.ExitCode -eq 0 -and $samples.Count -eq (2 * $frameCount) -and
        @($samples -notmatch ' ok=1$').Count -eq 0
    if ($case -like 'ngx-v1*')
    {
        # 仅正确像素不足以证明走了扩展；桥也必须确认匹配同次调用的回执并排入写回。
        $passed = $passed -and @($pixels -eq 'provider export: v1=1 expected=1').Count -eq 1 -and
            @($bridge -match 'ngx_out copy .*evidence=f6_receipt_v1').Count -ge 1 -and
            @($bridge -match 'evidence=ngx_status_only').Count -eq 0
    }
    switch -Wildcard ($case)
    {
        'ngx-v1' { }
        'ngx-v1-dlaa' {
            $passed = $passed -and
                @($pixels -eq 'provider dimensions: render=512x512 display=512x512').Count -eq 1 -and
                @($bridge -match 'ngx_out copy .*evidence=f6_receipt_v1').Count -eq 2 -and
                @($f6 -match 'F6 evaluate receipt v1 .*status=2 result=1(?:\s|$)').Count -eq 2
        }
        'ngx-nvidia-dlaa' {
            $passed = $passed -and
                @($pixels -eq 'provider dimensions: render=512x512 display=512x512').Count -eq 1 -and
                @($bridge -match 'ngx_core provider resolved=nvidia_registry ').Count -eq 1 -and
                @($bridge -match 'ngx_out copy .*evidence=ngx_status_only').Count -eq 2 -and $f6.Count -eq 0
        }
        'ngx-v1-present' {
            $passed = $passed -and @($pixels -match '^present host: hr=00000000$').Count -eq 2 -and
                @($bridge -match 'ngx_out copy .*evidence=f6_receipt_v1').Count -eq 2 -and
                @($f6 -match 'F6 evaluate receipt v1 .*status=2 result=1(?:\s|$)').Count -eq 2
        }
        'ngx-nvidia-present' {
            $passed = $passed -and @($pixels -match '^present host: hr=00000000$').Count -eq 2 -and
                @($bridge -match 'ngx_core provider resolved=nvidia_registry ').Count -eq 1 -and
                @($bridge -match 'ngx_out copy .*evidence=ngx_status_only').Count -eq 2 -and $f6.Count -eq 0
        }
        'ngx-v1-ao-*' {
            # 像素负控不能代替真 F6 同次回执：两帧都必须由实际执行的 provider 交付。
            $enabled = if ($case -eq 'ngx-v1-ao-on') { 1 } else { 0 }
            $passed = $passed -and
                @($pixels -eq "native ao: enabled=$enabled provider=f6 neutral=1 contact=1").Count -eq 1 -and
                @($bridge -match 'ngx_out copy .*evidence=f6_receipt_v1').Count -eq 2 -and
                @($f6 -match 'F6 evaluate receipt v1 .*status=2 result=1(?:\s|$)').Count -eq 2
        }
        'ngx-nvidia-ao-*' {
            $enabled = if ($case -eq 'ngx-nvidia-ao-on') { 1 } else { 0 }
            $passed = $passed -and
                @($pixels -eq "native ao: enabled=$enabled provider=nvidia neutral=1 contact=1").Count -eq 1 -and
                @($bridge -match 'ngx_core provider resolved=nvidia_registry ').Count -eq 1 -and
                @($bridge -match 'ngx_out copy .*evidence=ngx_status_only').Count -eq 2 -and $f6.Count -eq 0
        }
        'ngx-v1-resize' {
            $passed = $passed -and @($bridge -match 'ngx_core create_feature ok=1 ').Count -eq 3
        }
        'ngx-v1-skip-control' {
            $passed = $passed -and @($f6 -match 'Hotfix.SkipFirstFrames: 2').Count -eq 1 -and
                @($bridge -match 'ngx_out not_delivered receipt=no_output request=1$').Count -eq 1 -and
                $samples[1] -match 'expected=native ok=1$' -and $samples[3] -match 'expected=provider ok=1$'
        }
        'ngx-skip-control' {
            # Receipt-free legacy F6 is the negative control: Success but no first-frame output.
            $passed = $complete -and $process.ExitCode -eq 1 -and
                @($bridge -match 'copy_enqueued=1 evidence=ngx_status_only').Count -ge 1 -and
                @($f6 -match 'Hotfix.SkipFirstFrames: 2').Count -eq 1 -and
                @($pixels -match 'target=1 pixel=0,0,0,0 expected=provider ok=0').Count -eq 1
        }
        'ngx' {
            $passed = $passed -and @($bridge -match 'copy_enqueued=1 evidence=ngx_status_only').Count -ge 1 -and
                @($f6 -match 'NVSDK_NGX_D3D11_EvaluateFeature Handle:').Count -eq 2
        }
        'ngx-receipt' {
            $passed = $passed -and @($mock -match '^receipt fixture:').Count -eq 8 -and
                @($mock -match '^receipt fixture: call=1 reset=1 rc=1 proof=1$').Count -eq 1 -and
                @($mock -match '^receipt fixture: call=[2-5] reset=1 rc=1 proof=0$').Count -eq 4 -and
                @($mock -match '^receipt fixture: call=6 reset=1 rc=BAD00000 proof=2$').Count -eq 1 -and
                @($mock -match '^receipt fixture: call=7 reset=1 rc=1 proof=2$').Count -eq 1 -and
                @($mock -match '^receipt fixture: call=8 reset=0 rc=1 proof=2$').Count -eq 1
        }
        'ngx-resize' {
            $passed = $passed -and @($bridge -match 'copy_enqueued=1 evidence=ngx_status_only').Count -ge 1 -and
                @($bridge -match 'ngx_core create_feature ok=1 ').Count -eq 3 -and
                @($f6 -match 'NVSDK_NGX_D3D11_EvaluateFeature Handle:').Count -eq 4
        }
        'native-fallback' { $passed = $passed -and $writes -eq 0 -and @($bridge -match 'rc=DISPATCH_FAILED').Count -ge 1 }
        'native-fsr3' { $passed = $passed -and $selected -match '^3\.' }
        'native-fsr4' { $passed = $passed -and $selected -match '^4\.' }
        'native-fsr4-fallback' { $passed = $passed -and $selected -match '^3\.' }
        'native-reject' { $passed = $passed -and $writes -eq 0 -and @($bridge -match 'rc=DISPATCH_FAILED').Count -ge 1 }
        default { $passed = $passed -and $selected -match '^2\.' }
    }
    $row = [pscustomobject]@{
        Case = $case; Path = $path; PID = $process.Id; Exit = $process.ExitCode; Timeout = -not $complete; Pass = $passed
        SelectedFsr = $selected; Pixels = $pixels; Mock = @($mock); Bridge = $bridge; F6 = $f6
        Identity = @(Get-ChildItem -LiteralPath $path -Recurse -File | Where-Object Extension -in '.dll','.exe','.ini' |
            ForEach-Object { [pscustomobject]@{ File = $_.FullName.Substring($path.Length + 1); SHA256 = (Get-FileHash -LiteralPath $_.FullName).Hash } })
    }
    $rows += $row
    ConvertTo-Json -InputObject $rows -Depth 6 | Set-Content -LiteralPath "$root\result.json" -Encoding utf8
    $process.Dispose()
    $row | Select-Object Case, PID, Exit, Pass, SelectedFsr, @{Name='Pixels';Expression={$_.Pixels -join '; '}}
}
Write-Host "Receipt: $root\result.json"
if (@($rows | Where-Object { -not $_.Pass }).Count -ne 0) { exit 1 }
