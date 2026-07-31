# pingstorm — acota la ventana de alcanzabilidad del nodo sin tocar el firmware.
#
# El nodo contesta ICMP durante su ventana despierta (~275 ms a ~2400 ms de cada
# ciclo de ~63 s). Sondeando cada 150 ms se ve el primer y el último instante
# alcanzable de cada ciclo, y eso acota CUÁNDO muere el camino de red en los
# ciclos cuya telemetría no llega.
#
# Se usa System.Net.NetworkInformation.Ping y no ping.exe porque ping.exe tiene el
# intervalo fijo en 1 s (no hay flag para bajarlo) y a esa resolución la ventana
# entera son 2 muestras. La clase de .NET no necesita privilegios de admin, a
# diferencia de abrir un raw socket.
#
# OJO — efecto observador, y es deliberado: cada sonda es tráfico que despierta la
# radio del modem sleep. Si la pérdida DESAPARECE mientras esto corre, eso no
# invalida la medición, es el resultado: señala al hueco de ~1,7 s sin tráfico que
# hay entre el handshake y el publish de telemetría. Correr en paralelo con
# brokerprobe y cruzar por reloj de pared.

#
# Además alterna el TAMAÑO del payload sonda a sonda. La respuesta ICMP viaja por
# el uplink, que es justo la dirección sospechosa, así que un payload de 470 B
# reproduce un frame del mismo tamaño que el PUBLISH de telemetría (503 B) mientras
# el de 32 B sirve de control en el mismo instante y por el mismo camino. Si los
# grandes fallan y los chicos pasan, el problema es el tamaño del frame y queda
# demostrado sin tocar el firmware.

param(
    [string]$Target   = "192.168.18.105",
    [int]   $EveryMs  = 150,
    [int]   $TimeoutMs = 130,
    [int]   $Minutes  = 25,
    [int[]] $Sizes    = @(32, 470),
    [string]$Out      = "pingstorm.ndjson"
)

$ping = New-Object System.Net.NetworkInformation.Ping
# Un buffer por tamaño, creado una sola vez.
$buffers = @{}
foreach ($s in $Sizes) { $buffers[$s] = New-Object byte[] $s }
$sizeIdx = 0
$deadline = (Get-Date).AddMinutes($Minutes)
$sw = New-Object System.IO.StreamWriter($Out, $false, [System.Text.Encoding]::UTF8)

# Estado para reportar ventanas en vivo
$winFirst = $null
$winLast  = $null
$winCount = 0
$windows  = New-Object System.Collections.ArrayList

Write-Host "sondeando $Target cada ${EveryMs}ms durante $Minutes min -> $Out"
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
            Write-Host ("[{0}] despertó" -f $t.ToString("HH:mm:ss.fff"))
        }
        $winLast = $t
        $winCount++
    }
    elseif ($null -ne $winFirst -and ($t - $winLast).TotalSeconds -gt 5) {
        # El nodo duerme ~60 s y vive ~2,2 s: un hueco de 5 s cierra la ventana
        # sin ambigüedad.
        $dur = [math]::Round(($winLast - $winFirst).TotalMilliseconds)
        Write-Host ("[{0}] ventana: {1} -> {2}   {3} ms alcanzable, {4} sondas" -f `
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
Write-Host ("{0} ventanas de alcanzabilidad" -f $windows.Count)
Write-Host ("=" * 70)
$prev = $null
foreach ($w in $windows) {
    $gap = ""
    if ($null -ne $prev) { $gap = "  (ciclo de {0:N1} s)" -f ($w.First - $prev).TotalSeconds }
    Write-Host ("  {0} -> {1}   {2,6} ms{3}" -f $w.First.ToString("HH:mm:ss.fff"), `
        $w.Last.ToString("HH:mm:ss.fff"), $w.Ms, $gap)
    $prev = $w.First
}
if ($windows.Count -gt 1) {
    $m = ($windows | Measure-Object -Property Ms -Average -Minimum -Maximum)
    Write-Host ""
    Write-Host ("duración alcanzable: min {0} ms / prom {1:N0} ms / max {2} ms" -f `
        $m.Minimum, $m.Average, $m.Maximum)
    Write-Host "Cruzar con las llegadas de telemetría: si las ventanas cortas son las de"
    Write-Host "los ciclos perdidos, se cae la asociación; si todas miden igual, la"
    Write-Host "asociación aguanta y lo que se pierde es el frame de la telemetría."
}
