# Captura del serial SIN tocar DTR ni RTS.
#
# Ese es todo el punto. En el USB-Serial-JTAG del ESP32-S3, DTR y RTS gobiernan
# IO0 y EN con una lógica propia del periférico, y cualquier toque los interpreta
# como una secuencia de arranque. El intento anterior mandó un pulso por RTS
# creyendo que reseteaba y la dejó en `boot:0x23 (DOWNLOAD)`, esperando un
# flasheo que nunca llegaba — de ahí que no imprimiera nada nunca.
#
# El reset lo hace esptool al terminar el upload, que sí conoce la secuencia. Acá
# sólo se abre y se lee.
#
# El puerto puede tardar en aparecer: al resetear, el dispositivo USB se
# re-enumera y Windows lo da de baja y de alta. De ahí el reintento de apertura.
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
if (-not $sp) { Write-Host "no se pudo abrir $Port"; exit 1 }

$sw = New-Object System.IO.StreamWriter($Out, $false, [System.Text.Encoding]::UTF8)
$deadline = (Get-Date).AddMinutes($Minutes)
Write-Host "capturando $Port durante $Minutes min -> $Out"

$n = 0
while ((Get-Date) -lt $deadline) {
    try {
        $line = $sp.ReadLine()
        $sw.WriteLine("$((Get-Date).ToString('HH:mm:ss.fff')) $line")
        $sw.Flush()
        $n++
    } catch [TimeoutException] {
        # el nodo duerme ~60 s entre ráfagas; el silencio es lo normal
    } catch {
        $sw.WriteLine("### error: $_"); $sw.Flush(); break
    }
}
$sp.Close(); $sw.Close()
Write-Host "captura terminada: $n lineas"
