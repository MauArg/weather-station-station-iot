# Captures the serial WITHOUT touching DTR or RTS.
#
# That's the whole point. On the ESP32-S3's USB-Serial-JTAG, DTR and RTS
# govern IO0 and EN with peripheral-specific logic, and any touch gets
# interpreted as a boot sequence. The previous attempt sent a pulse over RTS
# thinking it would reset the board and instead left it in `boot:0x23
# (DOWNLOAD)`, waiting for a flash that never came — which is why it never
# printed anything.
#
# The reset is done by esptool when the upload finishes, which does know the
# sequence. Here it just opens and reads.
#
# The port can take a while to appear: on reset, the USB device
# re-enumerates and Windows takes it down and back up. Hence the retry on open.
param(
    [string]$Port    = "COM7",
    [int]   $Minutes = 8,
    [string]$Out     = "sniffer.log"
)

$sp = $null
for ($i = 0; $i -lt 30 -and -not $sp; $i++) {
    try {
        $t = New-Object System.IO.Ports.SerialPort $Port, 115200, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
        $t.ReadTimeout = 1000
        $t.Open()
        $sp = $t
    } catch {
        Start-Sleep -Milliseconds 500
    }
}
if (-not $sp) { Write-Host "could not open $Port"; exit 1 }

$sw = New-Object System.IO.StreamWriter($Out, $false, [System.Text.Encoding]::UTF8)
$deadline = (Get-Date).AddMinutes($Minutes)
Write-Host "capturing $Port for $Minutes min -> $Out"

$n = 0
while ((Get-Date) -lt $deadline) {
    try {
        $line = $sp.ReadLine()
        $sw.WriteLine("$((Get-Date).ToString('HH:mm:ss.fff')) $line")
        $sw.Flush()
        $n++
    } catch [TimeoutException] {
        # the node sleeps ~60 s between bursts; silence is normal
    } catch {
        $sw.WriteLine("### error: $_"); $sw.Flush(); break
    }
}
$sp.Close(); $sw.Close()
Write-Host "capture finished: $n lines"
