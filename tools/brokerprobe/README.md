# brokerprobe — medir la pérdida de telemetría desde la LAN

Herramientas de la investigación del 2026-07-29/31, que terminó en el fix de
`WIFI_FORCE_11B` (ver [`../../../STATUS.md`](../../../STATUS.md)).

Se versionan porque **cambiaron el costo de medir**: antes, saber cuánta
telemetría se perdía costaba una captura de logs en el nodo, una sesión de
service mode y una transferencia paginada — o sea una noche y una operación cara.
Con esto son 35 minutos desde la LAN, sin tocar el nodo. Si el enlace vuelve a
degradarse, esto es lo primero que se quiere tener a mano.

## `brokerprobe` (Go)

```
go build -o brokerprobe.exe .
./brokerprobe.exe -dur 35m -out captura.ndjson
```

Se suscribe al broker con un client-ID propio y registra cada telemetría con su
payload completo. Además lee los contadores `$SYS/broker/#` que publica Mosquitto,
y ahí está lo que no se puede obtener de otra forma:

`$SYS/broker/publish/messages/received` y `.../bytes/received` se incrementan en
`handle__publish`, **antes de rutear**, así que dicen si el broker *ingresó* el
mensaje con independencia de a quién se lo entregue. Un tercer suscriptor no
puede contestar eso: si el broker descarta en el ingreso, todos los suscriptores
pierden igual.

Mosquitto publica cada valor de `$SYS` **sólo cuando cambia**, así que la
secuencia de mensajes es la secuencia de transiciones con su timestamp.

## Análisis

| script | qué hace |
|---|---|
| `analyze.py` | tasa de pérdida por huecos de `boot_count`, y la aritmética de bytes que localiza en qué punto del ciclo se cortó |
| `linkstate.py` | lee los campos `pv_*` (si el firmware los publica) y separa cómo cerraron los ciclos perdidos de los que llegaron |
| `crossref.py` | cruza una captura del sniffer 802.11 con las llegadas al broker: RSSI y reintentos de los ciclos perdidos contra los que llegaron |

## `pingstorm.ps1`

Sondeo ICMP a intervalo corto. El nodo contesta ping durante su ventana despierta,
así que la duración de esa ventana acota **cuándo** dejó de estar en la red dentro
del ciclo. Alterna tamaños de payload para separar fallas dependientes del tamaño
de frame.

Usa `System.Net.NetworkInformation.Ping` y no `ping.exe` porque éste tiene el
intervalo fijo en 1 s y a esa resolución la ventana entera son 2 muestras.

## `serialcap.ps1`

Captura el serial del sniffer a un archivo, acotada en tiempo. **No toca DTR ni
RTS**: en el USB-Serial-JTAG del ESP32-S3 esos pines gobiernan IO0 y EN con lógica
propia del periférico, y cualquier toque deja la placa en modo descarga esperando
un flasheo que nunca llega. Costó un rato descubrirlo.

## Trampas que ya se pagaron

- **Las ventanas tomadas en horarios distintos no son comparables.** La tasa de
  reintentos se movió de 29% a 54% *dentro de una misma hora*. Para un A/B que
  importe, alternar los brazos en la misma franja o aceptar que sólo se detectan
  efectos grandes.
- **La posición del sniffer es parte de su calibración.** 23 cm de altura (casi
  dos longitudes de onda a 2,4 GHz) lo llevaron de oír al nodo a no oírlo en
  absoluto, e invalidaron una conclusión que se había dado por buena.
- **PowerShell resuelve las rutas relativas contra el directorio del proceso**,
  no contra `Set-Location`. Pasar rutas absolutas a `-Out` o el archivo aparece
  en cualquier lado.
