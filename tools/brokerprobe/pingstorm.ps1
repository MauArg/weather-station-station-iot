# pingstorm — bounds the node's reachability window without touching the firmware.
#
# The node answers ICMP during its awake window (~275 ms to ~2400 ms of each
# ~63 s cycle). Probing every 150 ms shows the first and last reachable
# instant of each cycle, and that bounds WHEN the network path dies on
# cycles whose telemetry doesn't arrive.
#
# System.Net.NetworkInformation.Ping is used instead of ping.exe because
# ping.exe has a fixed 1 s interval (no flag to lower it) and at that
# resolution the whole window is 2 samples. The .NET class doesn't need
# admin privileges, unlike opening a raw socket.
#
# NOTE — observer effect, and it's deliberate: every probe is traffic that
# wakes the radio out of modem sleep. If the loss DISAPPEARS while this
# runs, that doesn't invalidate the measurement, it IS the result: it points
# to the ~1.7 s gap with no traffic between the handshake and the telemetry
# publish. Run in parallel with brokerprobe and cross-reference by wall clock.

#
# It also alternates the probe payload SIZE from probe to probe. The ICMP
# reply travels over the uplink, which is exactly the suspect direction, so
# a 470 B payload reproduces a frame the same size as the telemetry PUBLISH
# (503 B) while the 32 B one serves as a control at the same instant and
# over the same path. If the large ones fail and the small ones get
# through, the problem is frame size, and that's demonstrated without
# touching the firmware.

param(
    [string]$Target   = "192.168.18.105",
    [int]   $EveryMs  = 150,
    [int]   $TimeoutMs = 130,
    [int]   $Minutes  = 25,
    [int[]] $Sizes    = @(32, 470),
    [string]$Out      = "pingstorm.ndjson"
)

$ping = New-Object System.Net.NetworkInformation.Ping
# One buffer per size, created once.
$buffers = @{}
foreach ($s in $Sizes) { $buffers[$s] = New-Object byte[] $s }
$sizeIdx = 0
$deadline = (Get-Date).AddMinutes($Minutes)
$sw = New-Object System.IO.StreamWriter($Out, $false, [System.Text.Encoding]::UTF8)

# State for reporting windows live
$winFirst = $null
$winLast  = $null
$winCount = 0
$windows  = New-Object System.Collections.ArrayList

Write-Host "probing $Target every ${EveryMs}ms for $Minutes min -> $Out"
Write-Host ""

while ((Get-Date) -lt $deadline) {
    $t = Get-Date
    $size = $Sizes[$sizeIdx % $Sizes.Count]
    $sizeIdx++
    try   { $r = $ping.Send($Target, $TimeoutMs, $buffers[$size]); $st = $r.Status.ToString(); $rtt = $r.RoundtripTime }
    catch { $st = "Exception"; $rtt = -1 }

    $alive = ($st -eq "Success")
    $sw.WriteLine((@{
        t     = $t.ToString("o")
        alive = $alive
        st    = $st
        rtt   = $rtt
        size  = $size
    } | ConvertTo-Json -Compress))

    if ($alive) {
        if ($null -eq $winFirst) {
            $winFirst = $t
            Write-Host ("[{0}] woke up" -f $t.ToString("HH:mm:ss.fff"))
        }
        $winLast = $t
        $winCount++
    }
    elseif ($null -ne $winFirst -and ($t - $winLast).TotalSeconds -gt 5) {
        # The node sleeps ~60 s and lives ~2.2 s: a 5 s gap closes the
        # window unambiguously.
        $dur = [math]::Round(($winLast - $winFirst).TotalMilliseconds)
        Write-Host ("[{0}] window: {1} -> {2}   {3} ms reachable, {4} probes" -f `
            $winLast.ToString("HH:mm:ss"), $winFirst.ToString("HH:mm:ss.fff"), `
            $winLast.ToString("HH:mm:ss.fff"), $dur, $winCount)
        [void]$windows.Add([pscustomobject]@{ First = $winFirst; Last = $winLast; Ms = $dur; N = $winCount })
        $winFirst = $null; $winCount = 0
    }

    $sw.Flush()
    $sleep = $EveryMs - [int]((Get-Date) - $t).TotalMilliseconds
    if ($sleep -gt 0) { Start-Sleep -Milliseconds $sleep }
}
$sw.Close()

Write-Host ""
Write-Host ("=" * 70)
Write-Host ("{0} reachability windows" -f $windows.Count)
Write-Host ("=" * 70)
$prev = $null
foreach ($w in $windows) {
    $gap = ""
    if ($null -ne $prev) { $gap = "  (cycle of {0:N1} s)" -f ($w.First - $prev).TotalSeconds }
    Write-Host ("  {0} -> {1}   {2,6} ms{3}" -f $w.First.ToString("HH:mm:ss.fff"), `
        $w.Last.ToString("HH:mm:ss.fff"), $w.Ms, $gap)
    $prev = $w.First
}
if ($windows.Count -gt 1) {
    $m = ($windows | Measure-Object -Property Ms -Average -Minimum -Maximum)
    Write-Host ""
    Write-Host ("reachable duration: min {0} ms / avg {1:N0} ms / max {2} ms" -f `
        $m.Minimum, $m.Average, $m.Maximum)
    Write-Host "Cross-reference with telemetry arrivals: if the short windows are the"
    Write-Host "lost cycles, the association drops; if they all measure the same, the"
    Write-Host "association holds and what's lost is the telemetry frame."
}
