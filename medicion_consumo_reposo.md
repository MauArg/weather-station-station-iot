# Medición del consumo de reposo — procedimiento de banco

**Estado: pendiente de ejecución.** Preparado el 2026-08-11, para hacer con multímetro de mano en banco.

Es la medición de mayor valor pendiente del proyecto: decide si tiene sentido optimizar la ventana activa o el reposo. Ver `componentes_y_conexiones.md` → "El reposo pesa más que la ventana activa" para el contexto que la motiva, y `../STATUS.md` → "Power management".

---

## 1. Qué se busca y qué número lo decide

El presupuesto energético actual está estimado indirectamente, desde la caída de tensión de la batería durante la noche (5 noches, mediana 5,10 mA de consumo medio total):

| | mA medios | mAh/día | cómo se sabe |
|---|---|---|---|
| Ventana activa | 1,86 | 44,7 | **medido** — 51,2 mA × 3,64% de ciclo de trabajo |
| Reposo | ~3,2 | 77,7 | **por diferencia, nunca medido directo** |
| **Total** | **5,10** | **122,4** | estimado de la caída nocturna |

El 63% del presupuesto está sin explicar, y **no hay cambio de firmware que lo resuelva**: el INA219 se apaga al dormir, y aunque no se apagara, no hay nadie despierto para leerlo.

### La predicción falsable

Si la estimación indirecta es correcta, **el nodo dormido tiene que consumir ~3,2 mA de la batería**. Ese es el número a confirmar o desmentir en la medición M2. Si sale muy distinto, lo que está mal es el método de la caída nocturna (la conversión volts→mAh usa la curva de SoC del backend, que es un indicador y no una contabilidad).

### Sospechosos, en orden

1. **Reposo del step-up boost** — el principal. Tiene que estar siempre encendido porque alimenta al ESP32 que despierta, y los módulos de esta clase se citan en 0,5–2 mA, empeorando a carga liviana.
2. **El regulador propio de la SuperMini** más el deep sleep del ESP32-C3.
3. **El CN3791 de noche** — nunca cuantificado. Sus dos LEDs ya están descartados: sólo encienden cuando el panel entrega (confirmado 2026-08-09).
4. Los dos INA219 en power-down son ~12 µA entre ambos y no explican nada. Los LEDs de los módulos están todos desoldados.

---

## 2. La topología, y los dos regalos del PCB

```
Panel ──[INA219 solar 0x41]──▶ CN3791 ──┬──▶ batería LiPo 1500 mAh (JST PH 1.25)
                                        │
                                        └──[INA219 sys 0x40]──▶ boost 5V ──▶ ESP32-C3
                                                                              │
                                                                    LDO onboard ──▶ bus 3V3
                                                                                     ├─ 2× INA219 Vcc
                                                                                     └─ pull-ups I2C
```

El CN3791 no tiene salida de carga separada, así que **batería, cargador y carga comparten un solo nodo**. Eso define los dos puntos de inserción útiles:

- **En la línea de batería** → consumo *total* (incluye al CN3791).
- **En la entrada del boost** (o del INA219 de sistema) → sólo la rama de carga, *excluye* al CN3791.

Dos cosas del armado hacen esto mucho más fácil de lo que parecía:

- **La SuperMini está sobre headers hembra** (`componentes_y_conexiones.md`, zona ESP32-C3, 8× hembra por lado): se puede **desenchufar sin desoldar**. Con el ESP32 afuera no hay ráfagas de WiFi, y la corriente es DC estable — trivial de medir.
- **El power-down del INA219 no abre el shunt**, así que el camino de corriente es idéntico dormido y despierto. Nada que compensar.

---

## 3. El problema del burden voltage, y cómo esquivarlo

Un multímetro de mano en rango de mA mete un shunt de ~1–10 Ω en serie:

| estado | corriente | caída con 10 Ω | consecuencia |
|---|---|---|---|
| dormido | ~3 mA | ~30 mV | ✅ irrelevante |
| despierto | 51 mA | ~510 mV | ⚠️ tolerable |
| ráfaga de asociación WiFi | cientos de mA | volts | ❌ **brownout / reset** |

Un nodo que se resetea en loop no está en deep sleep, así que la lectura no valdría nada.

**Solución: un puente en paralelo con el multímetro.** Un cable con pinzas cocodrilo cruzado sobre las dos puntas.

- **Puente cerrado** → la corriente pasa por el cable, el multímetro no ve nada, el nodo funciona sin burden.
- **Puente abierto** → toda la corriente pasa por el multímetro y se lee.

Se abre sólo durante la ventana de sueño y se cierra antes del próximo despertar.

### Cómo saber cuándo está dormido

**El dashboard tiene la cuenta regresiva al próximo publish** y es exacta desde el backend `1.3.2` (el nodo reporta `next_s` = 60 y el backend calcula 64 s con el overhead de despertar). El ciclo es ~2,3 s despierto de cada ~64: hay casi un minuto de ventana tranquila.

> Mantené el nodo publicando durante la prueba — el dashboard es el reloj. Si le sacás el WiFi, `connectWiFi()` agota reintentos y se queda **hasta 45 s despierto** tirando 51 mA, que es lo contrario de lo que se quiere.

Protocolo: esperá a ver un publish nuevo → contá ~10 s → abrí el puente → leé → cerrá el puente antes de que la cuenta llegue a cero.

---

## 4. Las tres mediciones, de la más fácil a la más difícil

**El doc original proponía empezar por el total y después descomponer. Conviene al revés**: las mediciones sin ráfagas son las que aíslan al sospechoso principal, y no necesitan puente ni cronometrar nada.

| | punto de inserción | ESP32 | qué mide | ¿ráfagas? |
|---|---|---|---|---|
| **M1** | línea de batería | **fuera del zócalo** | CN3791 + boost en vacío | no |
| **M3** | entrada del boost | **fuera del zócalo** | boost en vacío solo | no |
| **M2** | línea de batería | dentro, nodo dormido | **reposo total** | sí → puente |

Las restas:

```
M1 − M3  =  CN3791 de noche
M2 − M1  =  rama del ESP32 (deep sleep + LDO + INA219 + pull-ups)
M2       =  el número que valida (o rompe) la estimación de 3,2 mA
```

**M3 es opcional**: depende de si la entrada del boost es accesible sin desoldar. Si el módulo está soldado a la placa, saltealo — M1 y M2 solas ya contestan la pregunta principal.

### Un factor de conversión que hay que aplicar

Cualquier corriente medida **del lado de 5 V** (después del boost) se refiere a la batería multiplicando por ~1,5:

```
I_batería ≈ (5 V × I_5V) / (3,9 V × η)     con η ≈ 0,85  →  ~1,5×
```

Las tres mediciones de arriba son todas del lado de la batería, así que no hace falta — pero si alguna vez medís entre el boost y la SuperMini, acordate.

---

## 5. Antes de tocar nada

### Seguridad

> ⚠️ **El error clásico de multímetro, y con una LiPo es serio:** dejar las puntas en los jacks de corriente (A / mA) y usarlo como voltímetro. Eso pone un shunt de ~1 Ω directo sobre la batería — cortocircuito franco. Quema el fusible en el mejor caso.
>
> **Regla: después de cada medición de corriente, devolvé la punta roja al jack de volts.**

- Desconectá el **panel** (JST solar) antes de empezar. En banco no hace falta, pero si por lo que sea hay luz sobre el panel, la corriente de carga se suma y la lectura pasa a ser *carga − consumo*, que no es lo que se busca.
- Conectá y desconectá siempre con la batería fuera del circuito primero. El JST PH 1.25 es chico y dos pines juntos sobre una LiPo de 1500 mAh no perdonan.
- No dejes el nodo desarmado con la batería conectada más de lo necesario.

### Qué tener a mano

- Multímetro con rango de mA (resolución de 0,01 mA o mejor).
- **Un cable con pinzas cocodrilo** para el puente. Es lo único no obvio de la lista y es lo que hace viable M2.
- Algo para intercalar en la línea de batería sin forzar el JST: un pigtail JST PH 1.25 macho-hembra cortado es lo ideal. Si no hay, ver la nota de abajo.
- El dashboard abierto, para la cuenta regresiva.

> **Punto a confirmar visualmente al abrir la caja**, que no pude resolver desde la documentación: cómo intercalar en la línea de batería sin pigtail. Los candidatos son el JST de la batería, los pines de entrada del CN3791, o los pads del PCB. Si ninguno es cómodo, decímelo con una foto y lo replanteamos — no fuerces el conector.

### Que el hueco en los datos sea esperado, no un susto

Mientras el nodo esté en el banco va a seguir publicando con el panel desconectado y los sensores adentro de la casa: `solar_v` ≈ 0, `photo_kohm` probablemente en el centinela 9999, temperatura y humedad de interior. **Es esperado.** El backend puede marcar estado energético raro y el dashboard mostrar cosas sin sentido meteorológico — no es una falla. Anotá la hora de inicio y fin para poder identificar la ventana después en InfluxDB.

Con `LIVE_AUTO_ENABLED=false` (que es como está) el backend no va a intentar armar live mode por su cuenta.

---

## 6. Procedimiento

### M1 — CN3791 + boost en vacío *(empezar acá)*

1. Panel desconectado. Batería desconectada.
2. **Sacá la SuperMini del zócalo.** Guardala aparte.
3. Multímetro en rango de mA, en serie en la línea de batería.
4. Reconectá la batería (a través del multímetro).
5. Esperá ~10 s a que se estabilice y anotá. **No hay ráfagas: la lectura tiene que ser un valor quieto.**

Si oscila, algo más está conectado de lo que creés — pará y revisá antes de seguir.

### M3 — boost en vacío solo *(opcional, si la entrada es accesible)*

Igual que M1, pero con el multímetro en la entrada del boost en vez de en la línea de batería. La diferencia contra M1 es lo que consume el CN3791.

### M2 — reposo total *(la que necesita el puente)*

1. Batería desconectada. **Volvé a enchufar la SuperMini** en su zócalo, con cuidado de la orientación.
2. Multímetro en serie en la línea de batería, **con el puente de cocodrilos cerrado sobre sus puntas**.
3. Reconectá la batería. El nodo arranca; el puente lo protege del burden.
4. Confirmá en el dashboard que está publicando normal.
5. Esperá a ver un publish nuevo, contá ~10 s, **abrí el puente**.
6. Leé. Debería estabilizarse en pocos segundos.
7. **Cerrá el puente** antes de que la cuenta regresiva llegue a cero.
8. Repetí 3 veces y quedate con la mediana — es gratis y descarta una lectura tomada durante un transitorio.

---

## 7. Cómo interpretar el resultado

### Primero: ¿se sostiene la estimación indirecta?

**M2 ≈ 3,2 mA** → la estimación de la caída nocturna era buena, y el presupuesto de 122,4 mAh/día vale. Seguí con la descomposición.

**M2 muy distinto** (digamos fuera de 1,5–6 mA) → lo que hay que revisar es el método indirecto, no el hardware. La conversión volts→mAh depende de la curva de SoC, y la temperatura baja de noche sube la resistencia interna, así que parte de la caída podía ser térmica y no consumo.

### Después: ¿quién se lo lleva?

| resultado | lectura | qué sigue |
|---|---|---|
| **M1 ≈ M2** (la rama del ESP32 es chica) | El ESP32 duerme bien; se lo lleva el boost y/o el CN3791 | **Es el caso esperado.** La palanca es el boost — ver abajo |
| **M1 pequeño, M2 − M1 grande** | El boost está bien y el problema es el ESP32 o su LDO | Sospechar el LDO de la SuperMini; medir la rama de 5 V por separado |
| **M1 − M3 grande** | El CN3791 drena de noche más de lo que se suponía | Novedad — no estaba en la lista de sospechosos con peso |
| **Todo chico, M2 chico** | El reposo no es el problema | Vuelve a valer la pena optimizar la ventana activa, y la estimación nocturna estaba mal |

### Si el boost resulta ser el culpable

**No hay que decidirlo ahora, pero conviene saber que la palanca existe**: el boost está para llevar 3,7–4,2 V de la LiPo a 5 V, que la SuperMini vuelve a bajar a 3,3 V con su LDO. Son dos conversiones para terminar por debajo de donde arrancó. Si el reposo del boost resulta ser 2 de los 3,2 mA, la pregunta que se abre es si el boost tiene que existir.

**No investigué esa alternativa todavía** — depende de si la SuperMini expone 3V3 como entrada, del rango de dropout, y del comportamiento con la batería casi descargada. Es trabajo para cuando haya número, y toca hardware, así que va con su propia investigación antes de recomendar nada.

---

## 8. Dónde anotar el resultado

| medición | valor | fecha |
|---|---|---|
| M1 — CN3791 + boost en vacío | | |
| M3 — boost en vacío solo *(opcional)* | | |
| M2 — reposo total, nodo dormido | | |

Cuando estén, actualizar:

- `componentes_y_conexiones.md` → "El reposo pesa más que la ventana activa" (hoy dice "estimado, sin medir")
- `aprendizajes_y_roadmap.md` → el ítem "Medir el consumo de reposo con multímetro"
- `../STATUS.md` → sección de power management

---

## 9. Lo que sigue después

Con el número en mano se retoma **el promediado por hardware del INA219**, que quedó planificado en dos pasos (ver `aprendizajes_y_roadmap.md` → "Medir la energía de la ventana activa"):

- **Paso 1, sólo firmware**: despertar el INA219 al inicio de `setup()` y escribir el registro de configuración `0x00` después de cada `begin()`, para que `system_mA`/`system_v`/`system_mW` pasen a ser promedios de hardware en vez de un snapshot de 532 µs. Sin cambios de payload, backend ni N8N.
- **Paso 2**: integrar la ventana completa (`active_mAs` + `awake_ms`), que toca los cuatro eslabones de la cadena.

Cuatro hallazgos de la revisión de código del 2026-08-11 que corrigen premisas del diseño en papel están anotados en `aprendizajes_y_roadmap.md`, en esa misma sección.
