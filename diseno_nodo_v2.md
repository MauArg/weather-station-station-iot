# Nodo V2 — diseño tentativo inicial (2026-11-06)

> **Estado: borrador para empezar el esquemático en KiCad, no congelado.** Sale del cruce de tres análisis (firmware/GPIO, arquitectura de PCB, energía/componentes) sobre los PDFs de Heltec (`Wireless Tracker1.1.pdf`, `HTIT-Tracker_V0.5.pdf`, `Compact_Weather_Station_User_Guide.pdf`). Etiquetas: **[C]** citado de datasheet/esquemático, **[I]** inferido, **[?]** sin verificar. Las preguntas abiertas y las pruebas de banco que hay que hacer **antes de mandar a fabricar** están al final.

## Premisas (decididas por Mau)

- Módulo principal: **Heltec Wireless Tracker V1.2** (ESP32-S3FN8 + SX1262 + GNSS UC6580).
- **Una sola PCB**, 2 capas, fabricada en JLCPCB/PCBWay, **soldada a mano** (THT o SMD 0805+/SOT-23/SOIC/TSSOP; sensores en módulo).
- **El mismo diseño sirve para los dos despliegues**: reemplazar la V1 en casa, o una estación remota a ~2,5 km sólo con LoRa.
- Alcance: todo lo de la V1, más viento, GNSS activo y puertos de expansión.
- Conectores **GX12 en la base de la caja** en lugar de cables por prensaestopas (idea tomada del M12 de la Compact Weather Station de Heltec, y lección del granizo del 2026-10-31).

## 1. Energía

```
Panel 5,2 W ─ GX12 SOLAR ─ TVS SMBJ16CA (bidir.) ─ Schottky serie ─ CN3722 (MPPT por tensión, NTC, Vfloat ajustable)
                                                                         │
                                                         shunt INA226 ── VBAT ── pigtail 1,25 mm ─► JP3 (batería) de la Heltec
                                                                                                         │
                                                                     power-path interno (AO3401) ─ LDO CE6260 ─► 3V3 (pin header) ─► rails conmutados
```

- **La batería entra por el conector de batería de la Heltec (JP3).** Es el camino con el que se midió el reposo de 18,8 µA. **Se elimina el step-up de 5 V de la V1**, principal sospechoso del reposo de ~3,2 mA.
  - No alimentar por el pin 3V3: le empuja tensión a la salida del LDO y deja sin entrada al LDO de Vext, que alimenta GNSS y TFT [C, esquemático V0.5].
  - No alimentar por el pin 5V: la R1 de 10 k gasta ~0,4 mA permanentes y la batería queda fuera de especificación [C].
  - El TP4054 de la Heltec sólo se usa cuando se enchufa el USB en banco.
- **Cargador CN3722 integrado** (TSSOP-16 sin pad expuesto, LCSC C77905) [C]:
  - NTC: corta la carga por debajo de ~0 °C y por encima de ~50 °C.
  - **Vfloat ajustable**: 4,1 V alarga la vida de la celda dentro de una caja caliente.
  - Valores de partida: R_CS ≈ 0,4 Ω (~0,5 A), inductor blindado ≈ 47 µH con Isat ≥ 1 A, P-MOSFET AO3401A.
  - **Dejar en la placa un header de respaldo (IN± / BAT±) para enchufar el módulo CN3791 actual** si el CN3722 da problemas.
- **Presupuesto [I]: ~25–40 mAh/día, contra ~122 de la V1.** Con eso:
  - el MPPT no es crítico, y alcanza con un MPPT por tensión;
  - **no hace falta más capacidad**: 1500 mAh dan semanas de autonomía.
  - Sí importan tres cosas: lo que consume el cargador de noche, no cargar bajo 0 °C y no castigar la celda con calor.
- Batería: Li-ion 1S, idealmente **18650 de marca con PCM** (aguanta mejor el calor que el pouch). LiFePO4 queda descartada: su tensión de ~3,2 V deja el LDO en dropout.
- Medición: **INA226** en la línea de batería (power-down 0,5 µA y offset de ±10 µV, contra 6 µA y ±50 µV del INA219) [C]; opcionalmente un segundo para el panel. Los pines CHRG/DONE del CN3722, con pull-ups de 100 k, se pueden leer como pines del cargador.
- **Jumper de 2 pines en serie con la batería**, para poder medir el reposo con multímetro sin desoldar nada.

## 2. Mapa de GPIOs (plan A: TFT retirado)

Hechos del ESP32-S3 verificados en el datasheet v2.2 [C]:
- strapping: GPIO0 tiene pull-up débil; GPIO45/46 tienen pull-down débil. **Si GPIO45 está en 1 durante el reset, VDD_SPI pasa a 1,8 V y no bootea.**
- RTC: GPIO0–21.
- ADC: ADC1 = GPIO1–10. ADC2 no sirve con WiFi activo.
- Deep sleep: el PCNT no cuenta. Consumo: 7–8 µA dormido, 170 µA con el ULP-FSM corriendo.

| GPIO | Señal | Notas |
|---|---|---|
| 0 | USER_SW (Heltec) | Botón de service. **No conectarle nada externo**: en 0 al arrancar entra en modo descarga. |
| 1 / 2 / 3 | VBAT_ADC / ADC_Ctrl / Vext_Ctrl | Propios de la Heltec, sin cambios. |
| 4 | RAIN_AO | ADC1 |
| 5 | LDR | ADC1. Si se reemplaza por un sensor de luz I2C, queda libre. |
| 6 | VANE_AO (salida analógica OUT del AS5600) | ADC1 + RTC. Si la veleta va por I2C, queda **libre** (es la única ADC1 de reserva). |
| 7 | PLUV (reed) | RTC; para el ULP o para despertar por ext. |
| 8–14 | LoRa | Heltec |
| 15 / 16 | **NO CONECTAR** | Cristal de 32 kHz (X1) en el esquemático. Un pad conectado lo desafina. |
| 17 | ANEMO (reed) | RTC, para el ULP. |
| 18 | LED blanco de estado (Heltec) | Al encender tiene un glitch en alto, así que no sirve como enable. |
| 19 / 20 | USB nativo | |
| 21 | EN_EXP (rail del puerto de expansión) | El R37 de 100 k a GND del backlight le hace de pull-down. |
| 26 | **libre** (reserva) | SPICS1 sin PSRAM en el FN8 [I]. Quedó libre al reemplazar el DHT22 por un SHT40 por I2C. |
| 33–36, 47, 48 | GNSS | Heltec. Con Vext apagado, no inicializar la UART del GNSS. |
| 37 | DS18B20 (1-Wire) | |
| 38 | DE del RS485 | Antes era TFT_CS. |
| 39 / 40 | I2C **EXT**: SDA / SCL | Bus hacia el Stevenson y el mástil (controlador I2C 1). |
| 41 / 42 | I2C **INT**: SDA / SCL | INA226 + SHT40 de la caja (controlador I2C 0). |
| 43 / 44 | UART de expansión (TX/RX) | La ROM escribe log en U0TXD en cada boot; con DE en bajo no sale al bus RS485. |
| 45 | EN_EXT (rail exterior) | **Sólo pull-down externo**, activo en alto. |
| 46 | EN_INT (rail de sensores internos) | Pull-down, activo en alto. |

**Dos buses I2C**: los tres análisis coinciden en separarlos. Así un cable exterior mojado o en corto, como el 31/10, no tumba la telemetría de energía.

**Si se quiere conservar el TFT** (por ejemplo, para diagnosticar en el sitio remoto sin laptop): libera los pines 38–42 y 21, pero obliga a un solo bus I2C (en 26/37), y el DS18B20 pasa a un sensor I2C. Entra, pero sin aislamiento entre buses y sin reserva.

## 3. Rails conmutados (reemplazan a los BC337 low-side)

- **Llave high-side TPS22917DBV** (SOT-23-6): Iq de 0,5 µA, bloqueo de corriente inversa, EN activo en alto [C].
  - Una por rail: `3V3_EXT` (EN_EXT), `3V3_INT` (EN_INT), `3V3_EXP` (EN_EXP).
  - Confirmar que el EN no tenga pull-up interno.
- Pull-down externo de 100 k en cada EN, para que los rails arranquen apagados en boot, en brown-out y en deep sleep.
  - Los GPIO 45/46 no son RTC: en sleep quedan flotando y el pull-down corta el rail.
  - **Ojo con `gpio_deep_sleep_hold_en()`**: hay que poner los EN en 0 antes de llamarlo.
- **Pull-ups de I2C del lado conmutado**, nunca al 3V3 permanente. Antes de cortar un rail, el firmware pone SDA/SCL en alta impedancia. Así se termina el back-powering y el GND "flotante" de la V1.
- GND común a todo.

## 4. Conectores al exterior (base de la caja)

Los GX12 van montados en la caja y bajan por pigtail a **JST-XH en la PCB**. No se sueldan a la PCB, para que el apriete de la tuerca no cargue las juntas. Van en la base mirando hacia abajo, con tapa en los que no se usan, y la caja lleva **venteo de membrana**.

**Convención única para todo GX12 de 4 pines**: `1 GND · 2 VPORT (3V3 conmutado) · 3 SIG_A · 4 SIG_B`. Con esa convención, si se cruzan dos GX12-4 se pueden leer datos basura, pero **no se rompe nada**, porque ningún GX12-4 lleva más de 3,3 V.

| Conector | Pin 3 / Pin 4 | Rail |
|---|---|---|
| AIRE (Stevenson: SHT45 + BMP390) | SDA / SCL (I2C EXT) | 3V3_EXT |
| LLUVIA | PLUV (reed a GND) / RAIN_AO | PLUV a 3V3 permanente con pull-up alto; rain sensor en 3V3_INT |
| VIENTO | ANEMO / VANE_AO | Válido si el AS5600 va por su **salida analógica OUT**. Si va por I2C o se usa la veleta óptica, hacen falta 5–6 hilos: usar GX12-5/6, y la diferencia de cantidad de pines sirve además de keying. |
| EXP (RS485 / futuro) | A / B | 3V3_EXP. Un sensor de 5–24 V va en otro tipo de conector. |
| SOLAR | — | **Nunca GX12-4**: 14 V entrando a un puerto de 3,3 V. Usar un GX12-2/3 **con el género invertido**. |
| LoRa | — | SMA bulkhead con pigtail u.FL lo más corto posible. |

Front-end de cada puerto, como **hoja jerárquica reutilizada** en KiCad: R serie de 47–100 Ω más ESD PESD5V0S2BT (SOT-23) pegado al JST, y RC según el tipo de señal:
- Anemómetro: 1 k serie, pull-up de 47–100 k a 3V3 permanente y ~1–10 nF (τ ≈ 0,1 ms; ~230 Hz a 50 m/s).
- Pluviómetro: igual, pero con τ de 1–10 ms.
- ADCs: 1 k + 100 nF.
- **Pull-up alto en los reeds**: con 10 k, un rotor parado con el imán sobre el reed consume 330 µA continuos.
- I2C EXT: pull-ups de 2,2–4,7 k a 100 kHz. **No hace falta extensor** para 1,5 m; lo que falló el 31/10 fue mecánico y por agua.

## 5. Sensores

| Función | V1 | V2 propuesto |
|---|---|---|
| T/HR exterior | SHT31 | **SHT45-AD1F** (con membrana y heater), en módulo. Alternativa: SHT31. |
| Presión | BMP180/085 | **BMP390** en módulo (±0,5 hPa abs.). Alternativa: BMP280. |
| T/HR caja | DHT22 | **SHT40** I2C (bus INT), libera un GPIO |
| Temp. caja/batería | DS18B20 | DS18B20 (telemetría) + NTC 10 k B3950 del cargador, pegado a la batería |
| Energía | 2× INA219 | 1–2× **INA226** |
| Viento | — | Reed + AS5600 (OUT analógica o I2C) o veleta óptica con PCF8574 en el mástil |

**Conteo de viento en deep sleep**: sólo es posible con el ULP-FSM corriendo con los reeds en pines RTC (~170 µA ≈ +4 mAh/día [C/I]). El riesgo principal es que el ULP compile con PlatformIO + Arduino [?]. La PCB deja abiertas todas las opciones: ULP, despertar por ext para el pluviómetro, o un contador I2C externo.

## 6. PCB

- **Hojas**: `01_power_solar`, `02_heltec`, `03_rails`, `04_port` (una instancia por GX12), `05_sensors_int`, `06_expansion` (transceptor RS485 3V3 SOIC-8 + header I2C/UART interno).
- **Heltec en zócalo** (tiras hembra 1×8 + 1×16 de pines torneados), sujeta con brida o clip. Altura total ≈ 23 mm.
- **Correcciones a la huella actual** `miHeltecV1.2_DIP_25_LongPads`:
  - (a) Borrar los pads `NC_42` / `NC_9`: caen en el hueco donde está **la antena WiFi de resorte**.
  - (b) El keepout actual está sólo en F.Cu, permite relleno y no cubre el resorte. Rehacerlo como área de reglas en **todas las capas**, sin pistas, vías ni relleno, debajo del resorte (~12×10 mm) y del parche GNSS.
  - (c) Esa fila contra el borde de la placa, o con caladura.
- USB-C de la Heltec hacia un borde accesible. RST y GPIO0 repetidos en test points.
- B.Cu como plano de GND sólido, con cosido de vías.
- Clases de red: Default 0,25 mm · PWR 0,6–1 mm · PV 1 mm. Vía de 0,6/0,3 mm.
- Test points: PV_IN, VBAT, 3V3, Vext, cada rail, SDA/SCL de los dos buses, UART, GND doble.
- **Serigrafía**:
  - en cada JST, nombre de pin, pin 1, número de GX y color de hilo;
  - polaridad de batería y panel;
  - versión y fecha.
  - Huellas oficiales de KiCad, revisadas en el visor 3D (lecciones de la V1: TO-92 de Fritzing con C/E invertidos y JST montados al revés).
- **Barniz**: marcar en `User.x` lo que se enmascara (zócalos, JST, u.FL, TP, jumpers), y agruparlo.
- 4× M3 NPTH en las esquinas, lejos de las antenas. Objetivo de tamaño: **≤ 100×100 mm**.

## 7. Pruebas de banco ANTES de fabricar

1. **Camino de alimentación de la V1.2 contra el esquemático V0.5**:
   - continuidad entre la entrada de U4 y la de U6, y entre el pin 5V y VDD_5V;
   - **paso y polaridad del conector de batería** ("1.25X2P", que no sería JST SH de 1,0 mm).
2. **GPIO 26, 37 y 38–42 usables**, con el TFT retirado. Si el TFT queda puesto: GPIO38 en alto con Vext apagado, medir si se alimenta por las protecciones de entrada.
3. **¿X1 (32 kHz) está montado en la V1.2?**
4. **Spike del ULP en lora-lab**: contar 0–300 Hz en deep sleep y medir la corriente.
5. Medir cuánto dura el cierre del reed (analizador lógico).
6. Si se puede, medir el reposo de la V1 (M1 de `medicion_consumo_reposo.md`): valida el consumo nocturno del cargador.

## 8. Preguntas abiertas para Mau

- ¿El TFT de la V1.2 se puede retirar? Define el plan A o B de GPIOs.
- ¿Viento continuo (promedio de 10 min y ráfaga, con ULP) o una muestra por ciclo?
- Veleta: ¿AS5600 por OUT analógica (entra en un GX12-4), por I2C o la óptica (GX12-5/6)?
- Caja: ¿medidas internas útiles, tetones, cómo queda montada (PCB vertical u horizontal, hacia dónde mira la tapa)? Afecta la vista de cielo del GNSS.
- GX12 que tenés: ¿macho o hembra de panel? ¿Hay de 2, 3, 5 o más pines? ¿Grado IP?
- ¿Compra por LCSC junto con la PCB, o MercadoLibre?
- ¿La expansión RS485 tiene que alimentar sensores de 5–24 V?
- Temperatura máxima de la caja el verano pasado: sale del histórico del DS18B20 y decide la ventana del NTC.

## Datasheets pendientes de bajar

- CE6260B33M (dropout/Iq: define el piso de batería).
- ESP32-S3 Technical Reference Manual (ULP, pads en sleep).
- AS5600 (confirmar la salida OUT analógica).
- LT3652 (alternativa al CN3722).
- Catálogo JST XH.
- Datasheet de la celda elegida.
