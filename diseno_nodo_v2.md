# Nodo V2 — diseño tentativo inicial (rev. 2026-11-07b: PCBA)

> **Estado: borrador para empezar el esquemático en KiCad, no congelado.** Sale del cruce de tres análisis (firmware/GPIO, arquitectura de PCB, energía/componentes) sobre los PDFs de Heltec (`Wireless Tracker1.1.pdf`, `HTIT-Tracker_V0.5.pdf`, `Compact_Weather_Station_User_Guide.pdf`) y el datasheet del LDO (`CE6260.PDF`). Etiquetas: **[C]** citado de datasheet/esquemático, **[M]** medido, **[I]** inferido, **[?]** sin verificar. Las pruebas de banco que hay que hacer **antes de mandar a fabricar** están al final.
>
> Rev. 2026-11-07b: **la placa pasa a PCBA de JLCPCB** (todo el SMD en la cara superior, ensamblado Economic; Mau suelda sólo el THT). Se eliminan los módulos breakout, se suman dos placas satélite (cabezal de aire y veleta), el BMP390 se reemplaza por el BMP581 por falta de stock, y se incorporan el expansor I2C con la inhibición de carga, el BOM con números LCSC (§9) y el costo.
>
> Rev. 2026-11-07: el mapa de GPIOs pasa al **plan B** porque el TFT va soldado; el viento es **continuo** (el ULP pasa a ser obligatorio); la veleta es **AS5600 por salida analógica**; **todos los conectores son GX12-4**; se suman el CE6260 y las temperaturas reales de la caja.

## Premisas (decididas por Mau)

- Módulo principal: **Heltec Wireless Tracker V1.2** (ESP32-S3FN8 + SX1262 + GNSS UC6580). **El TFT va soldado y no se puede retirar.**
- **Una sola PCB principal**, 2 capas, **fabricada y ensamblada en JLCPCB (PCBA Economic, todo el SMD en top)**. Mau suelda sólo el THT: zócalo de la Heltec, JST-XH, headers, jumper de batería y conector de batería. **Sin módulos breakout**: los chips van en la placa. El CN3722 y los repuestos llegan desde LCSC en el mismo envío (ver §9).
- **Dos placas satélite propias**, también por PCBA: `sat_aire` (SHT45 + BMP581) para el Stevenson y `sat_veleta` (AS5600). **Decidido (2026-11-07): van en el mismo pedido que la principal**, y se aprovecha el envío para sumar todo lo que se pueda de LCSC (repuestos, conectores). Ojo con el tope de 3 unidades de la misma especie (§9).
- **El mismo diseño sirve para los dos despliegues**: reemplazar la V1 en casa, o estación remota a ~2,5 km sólo con LoRa.
- Alcance: todo lo de la V1 más **viento continuo** (promedio y ráfaga, contando dormido), GNSS activo y puertos de expansión.
- Conectores **GX12 de 4 pines** (Mau tiene machos y hembras, todos de 4) en la base de la caja, en lugar de cables por prensaestopas.
- Caja: **~15 × 20 cm de espacio útil**, a confirmar midiendo.

## 1. Energía

```
Panel 5,2 W ─ GX12 SOLAR ─ TVS SMBJ16CA (bidir.) ─ Schottky serie ─ CN3722 (MPPT por tensión, NTC, Vfloat ajustable)
                                                                         │
                                                         shunt INA226 ── VBAT ── pigtail 1,25 mm ─► JP3 (batería) de la Heltec
                                                                                                         │
                                                                     power-path interno (AO3401) ─ LDO CE6260 ─► 3V3 (pin header) ─► rails conmutados
```

- **La batería entra por el conector de batería de la Heltec (JP3).** Es el camino con el que se midió el reposo de 18,8 µA [M]. **Se elimina el step-up de 5 V de la V1.**
  - No alimentar por el pin 3V3: deja sin entrada al LDO de Vext (GNSS y TFT) [C, esquemático V0.5].
  - No alimentar por el pin 5V: la R1 de 10 k consume ~0,4 mA permanentes [C].
- **Cargador: CN3722 integrado** (TSSOP-16 sin pad expuesto, LCSC C77905) [C].
  - NTC para cortar la carga en frío y en calor.
  - **Vfloat ajustable**, a 4,1 V.
  - Punto de partida: R_CS ≈ 0,4 Ω (~0,5 A), inductor blindado de ≈ 47 µH con Isat ≥ 1 A, P-MOSFET AO3401A.
  - **Header de respaldo (IN± / BAT±)** para enchufar el módulo CN3791 actual.
- **Presupuesto [I]: ~25–40 mAh/día con el ULP incluido, contra ~122 de la V1.** Con esa diferencia:
  - un MPPT por tensión alcanza;
  - 1500 mAh dan semanas de autonomía.
- **Batería**: Li-ion 1S, idealmente una **18650 de marca con PCM**. LiFePO4 queda descartada (ver 1.1).
- **Medición**: **INA226** en la línea de batería (power-down de 0,5 µA, offset de ±10 µV) [C]; opcional, un segundo para el panel. Los pines CHRG y DONE del CN3722 se leen por I2C o se dejan como test points (no quedan GPIOs, ver §2).
- **Jumper de 2 pines en serie con la batería**, para medir el reposo con multímetro.

### 1.1 El LDO de la Heltec: CE6260B33M [C, datasheet V1.5]

| Parámetro | Valor |
|---|---|
| Iq | **6 µA** típ.; shutdown 0,1 µA (encaja con los 18,8 µA medidos: LDO de 3V3 encendido y LDO de Vext apagado) |
| Dropout | **120 mV a 100 mA**, **650 mV a 500 mA** (≈ 1,2–1,3 Ω efectivos) |
| Entrada | 2,0–6,0 V (abs. 8 V); límite de corriente 650 mA; −40…85 °C; SOT-23 con Pd de 250 mW |

Consecuencias [I]:
- **TX LoRa a 22 dBm (~178 mA)**:
  - El dropout es de ~0,23 V, más ~15 mV del AO3401 del power-path.
  - El 3V3 se sostiene **hasta VBAT ≈ 3,55 V**.
  - Por debajo, el 3V3 cae a VBAT − 0,25 V. El S3 y el SX1262 siguen andando hasta ~3,0 V.
- **WiFi (picos de 300–500 mA)**: hace falta **VBAT ≳ 3,9 V** para regular limpio.
  - **Regla de firmware**: sesiones WiFi/OTA (despliegue en casa) sólo con VBAT ≥ 3,8 V.
  - LoRa funciona en todo el rango útil.
- **Piso de batería por firmware**:
  - < 3,5 V: intervalo largo;
  - < 3,4 V: sólo heartbeat;
  - < 3,3 V: dormir sin transmitir.
  - El PCM de la celda queda como última barrera.
- **LiFePO4 queda descartada con dato**: la meseta de ~3,2 V tendría el 3V3 en dropout todo el tiempo.
- Los rails de sensores (pocos mA) cuelgan del mismo LDO. Hay margen de sobra contra los 500 mA.

### 1.2 Temperatura real de la caja [M, InfluxDB, 2026-04-12 → hoy]

El histórico empieza en abril, así que **todavía no hay un verano registrado**.

| | Valor |
|---|---|
| Máxima creíble de la caja (DS18B20) | **40,0 °C** el 2026-09-27 (aire 34,9 °C). Otros días: 39,0 °C el 24/09 (aire 35,4 °C) y 36,5 °C el 29/10 |
| Caja − aire (máximos diarios) | mediana **+4,8 °C**, p90 **+8,4 °C** |
| Mínima de la caja | **−7,0 °C** el 2026-08-09 (aire −6,2 °C). **39 días con la caja bajo 0 °C** |
| Descartado | Días de mayo y junio con el DS18B20 en 59–83 °C y aire de 16–20 °C: son lecturas basura de la época de juntas frías, no calor |

Lectura [I]:
- **Verano**: con aire de 38–40 °C, la caja llegaría a **~45–48 °C**. Es el borde de la carga permitida de una Li-ion típica (0–45 °C; confirmar en el datasheet de la celda elegida).
  - El corte por NTC cerca de 45–50 °C va a pausar la carga en las tardes de verano.
  - No es un problema: en la V1 la carga real ocurre a la mañana, y a mediodía el pack ya está lleno.
  - Ubicar la batería abajo en la caja y lejos del sol directo.
- **Invierno**: **el corte por frío no es opcional.** Con 39 días bajo 0 °C, el sol de las 9–10 h empezaría a cargar con la celda todavía helada, y eso hace plating de litio. Con el CN3722 + NTC eso queda resuelto en hardware. La V1 no lo tiene.
- **NTC pegado a la batería**, no a la PCB.

## 2. Mapa de GPIOs — plan B (TFT soldado)

Hechos del ESP32-S3 [C, datasheet v2.2]:
- **Strapping**:
  - GPIO0: pull-up débil.
  - GPIO45/46: pull-down débil.
  - **GPIO45 en 1 durante el reset pone VDD_SPI a 1,8 V y no bootea.**
- **Pines**: RTC en GPIO0–21; ADC1 en GPIO1–10 (el ADC2 no sirve con WiFi).
- **Deep sleep**: el PCNT no cuenta; el consumo es de 7–8 µA, o **170 µA con el ULP-FSM corriendo**.

Con el TFT conectado, los pines 38–42 quedan para el TFT. Usarlos para otra cosa es riesgoso: una línea en alto con Vext apagado alimentaría el TFT y el GNSS a través de sus protecciones [I]. **A cambio, el TFT sirve para diagnóstico en sitio** en service mode, sin laptop.

| GPIO | Señal | Notas |
|---|---|---|
| 0 | USER_SW (Heltec) | Botón de service. **No conectarle nada externo** (en 0 al arrancar entra en modo descarga). |
| 1 / 2 / 3 | VBAT_ADC / ADC_Ctrl / Vext_Ctrl | Heltec, sin cambios. |
| 4 | RAIN_AO | ADC1 |
| 5 | **DS18B20** (1-Wire) | Se mantiene la serie `ds18b20_c`; va pegado a la batería. Queda libre porque el LDR pasa a I2C. |
| 6 | VANE_AO (salida OUT del AS5600) | ADC1 + RTC. |
| 7 | PLUV (reed) | RTC: lo cuenta el ULP. |
| 8–14 | LoRa | Heltec |
| 15 / 16 | **NO CONECTAR** | Cristal de 32 kHz (X1) en el esquemático. |
| 17 | ANEMO (reed) | RTC: lo cuenta el ULP. |
| 18 | DE del RS485 | Tiene el LED blanco de la Heltec, que se prende al transmitir (diagnóstico gratis). Al encender da un glitch en alto, inofensivo como DE. |
| 19 / 20 | USB nativo | |
| 21 | EN_EXP (rail de expansión) | La R37 de 100 k a GND del backlight hace de pull-down. Si Vext está encendido, el backlight se prende junto con el rail (ver §8). |
| 26 | I2C SDA | SPICS1 sin PSRAM en el FN8 [I]. **Probarlo en banco.** |
| 33–36, 47, 48 | GNSS | Heltec. |
| 37 | I2C SCL | Libre en el FN8 [C §2.3.5]. |
| 38–42 | TFT | Heltec, sin cambios. |
| 43 / 44 | UART de expansión (TX/RX) | La ROM escribe su log en U0TXD en cada boot; con DE en bajo no sale nada al bus. |
| 45 | EN_EXT | **Sólo pull-down externo**, activo en alto. |
| 46 | EN_INT | Pull-down, activo en alto. |

**Libres: ninguno.** El margen se recupera así:
- desoldando el LED de GPIO18 y moviendo el DE a una salida I2C;
- con un expansor I2C (PCF8574/TCA9534) para enables y señales lentas, como CHRG/DONE.

**Un solo controlador I2C, con el segmento exterior aislado por un repetidor** con EN (familia TCA9517DR, SOIC-8 [C, SCPS242D]). El EN del repetidor se ata a EN_EXT, así que no gasta GPIO.
- Si el cable exterior se moja o queda en corto (como el 31/10), con el repetidor deshabilitado el bus interno sigue andando.
- Verificado en el datasheet [C, SCPS242D]:
  - "High-Impedance I2C Pins When Powered-Off".
  - **Conexión**: lado A al ESP32 (VCCA = 3V3 permanente); lado B al exterior (VCCB = 3V3_EXT, pull-ups a 3V3_EXT). Con el rail apagado se apagan juntos el chip, los pull-ups y los sensores. Eso además protege al BMP581, que se daña si se le mantiene un pin en alto con VDDIO apagado [C].
  - **Límite**: con el cable en corto y el repetidor **habilitado**, el lado A también cae. El aislamiento existe sólo con EN en bajo o con VCCB apagado. El firmware tiene que detectar el bus trabado y cortar 3V3_EXT.
  - **Lado B**: VOL de 0,45–0,6 V; los esclavos tienen que tener VIL < 0,4 V. Queda justo con el SHT4x: medir el ACK con osciloscopio.
- Reemplaza la idea de dos controladores, que no entra con el TFT puesto.

**Mapa de direcciones (un solo espacio)**:

| Dispositivo | Dirección | Segmento |
|---|---|---|
| SHT45 | 0x44 | exterior |
| BMP581 | 0x47 | exterior (reemplaza al BMP390, que no tiene stock en JLC) |
| SHT40-**BD1B** | **0x45** | interior (no puede ser 0x44); en el 3V3 permanente |
| TCA9534 (expansor) | 0x20 | interior: CHRG, DONE, CHG_INH, reservas |
| INA226 batería / panel | 0x40 / 0x41 | interior |
| VEML7700 (luz, reemplaza al LDR) | 0x10 | interior; en el 3V3 permanente. **Es de vista lateral**: mira paralelo a la PCB |

**Conteo de viento continuo**:
- El ULP-FSM sondea GPIO17 y GPIO7 con debounce por muestras, y los contadores quedan en la memoria RTC.
- Calcula promedio y ráfaga entre uplinks.
- Costo: ~170 µA, unos 4 mAh/día [C/I], ya incluidos en el presupuesto.
- **Riesgo principal del proyecto: que el ULP compile y corra con PlatformIO + Arduino** [?]. Es la prueba de banco n.º 1 (§7).
- Si falla, el fallback es un framework `arduino, espidf` o IDF.
- El fallback por hardware sería un contador I2C externo, sin GPIO extra.

## 3. Rails conmutados

- **TPS22917DBV** (SOT-23-6, Iq 0,5 µA, bloqueo de corriente inversa, EN activo en alto) [C]. Uno por rail:

  | Rail | Enable | Alimenta |
  |---|---|---|
  | `3V3_EXT` | EN_EXT | Stevenson, AS5600, repetidor I2C |
  | `3V3_INT` | EN_INT | rain sensor y DS18B20 |
  | `3V3_EXP` | EN_EXP | RS485 y expansión |

- **Confirmar que el EN no tenga pull-up interno.**
- Pull-down externo de 100 k en cada EN, para que el rail arranque apagado en boot, brown-out y deep sleep.
- Antes de llamar a `gpio_deep_sleep_hold_en()`, poner los EN en 0.
- Al 3V3 permanente van el INA226 (en power-down entre ciclos), el **SHT40 y el VEML7700** (≈ 0,4 y 0,5 µA en reposo) y el TCA9534. Si colgaran de un rail conmutado, los pull-ups del bus interno los alimentarían por SDA/SCL.
- El pin ON del TPS22917 tiene "Smart Pulldown" pero **no se puede dejar flotando** [C]: se mantienen los 100 k externos.
- **Pull-ups de I2C del lado conmutado** de cada segmento. Antes de cortar el rail, el firmware pone SDA/SCL en alta impedancia.
- GND común a todo.

## 4. Conectores al exterior — todos GX12-4

Los GX12 van montados en la base de la caja, mirando hacia abajo, con pigtail a **JST-XH en la PCB**. Llevan tapa los que no se usan, y la caja lleva **venteo de membrana**.

**Convención de pines de los puertos de sensor**: `1 GND · 2 VPORT (3V3 conmutado) · 3 SIG_A · 4 SIG_B`. Si se cruzan dos puertos de sensor no se rompe nada.

**Keying por género** (todo es GX12-4, así que el género es lo único que evita conectar mal):
- **Puertos de sensor**: la caja lleva **hembra**, porque entrega tensión y no conviene dejar pines vivos expuestos. El cable del sensor lleva macho.
- **SOLAR**: la caja lleva **macho** y el cable del panel lleva hembra. Así el cable del panel no entra en ningún puerto de sensor, y un sensor no entra en SOLAR.
- Además: etiqueta y color distinto en la caja.
- SOLAR usa **dos pines en paralelo por polo** (1+2 GND, 3+4 PV+) para repartir la corriente.

| Conector (caja) | Pin 3 / Pin 4 | Notas |
|---|---|---|
| AIRE (hembra) | SDA / SCL (segmento I2C exterior) | Placa `sat_aire` (SHT45 + BMP581). VPORT = 3V3_EXT. |
| LLUVIA (hembra) | PLUV (reed a GND) / RAIN_AO | PLUV con pull-up a 3V3 permanente en la PCB; rain sensor en 3V3_INT. |
| VIENTO (hembra) | ANEMO (reed a GND) / VANE_AO | AS5600 por su **salida analógica OUT**, alimentado por VPORT = 3V3_EXT sólo en la ventana despierta. El reed del anemómetro tiene pull-up permanente en la PCB, independiente de VPORT, para que el ULP cuente dormido. DIR del AS5600 fijo en la cabeza. |
| EXP (hembra) | A / B (RS485) | VPORT = 3V3_EXP. |
| SOLAR (**macho**) | — | PV− en 1+2, PV+ en 3+4. |
| LoRa | — | SMA bulkhead, pigtail u.FL lo más corto posible. |

**AS5600 por OUT analógica** [?, confirmar en el datasheet]:
- Con la configuración de fábrica, la salida analógica cubre 0–100 % de VDD en 360°, sin programar nada por I2C.
- El ADC del S3 alinealiza cerca de los extremos: o se calibra, o se configura el AS5600 al rango 10–90 %. Esa configuración pide I2C una vez en banco, y el burn de OTP es opcional.
- Con un cable de mástil de varios metros: RC de 1 k + 100 nF en la PCB, y promediar varias lecturas.

**Front-end de cada puerto** (hoja jerárquica reutilizada en KiCad): R serie de 47–100 Ω más ESD PESD5V0S2BT (SOT-23) pegado al JST, y RC según el tipo de señal:

| Señal | Pull-up | RC | Notas |
|---|---|---|---|
| Anemómetro | 47–100 k a 3V3 permanente | 1 k + ~1–10 nF (τ ≈ 0,1 ms; ~230 Hz a 50 m/s) | |
| Pluviómetro | igual | τ de 1–10 ms | |
| ADCs | — | 1 k + 100 nF | |
| I2C exterior | 2,2–4,7 k a 3V3_EXT | — | 100 kHz, sin extensor |

## 5. Sensores

| Función | V1 | V2 propuesto |
|---|---|---|
| T/HR exterior | SHT31 | **SHT45-AD1F** (membrana + heater) en `sat_aire`, 0x44 |
| Presión | BMP180/085 | **BMP581** (C5362283) en `sat_aire`, 0x47. El BMP390 tiene stock 0 en JLC; alternativas: BMP388 (0x77) o pre-order del BMP390 |
| T/HR caja | DHT22 | **SHT40-BD1B**, 0x45 |
| Luz | LDR (ADC) | **VEML7700** (lux calibrados), 0x10. Libera el GPIO del DS18B20. |
| Temp. batería/caja | DS18B20 | DS18B20 (GPIO5, misma serie) + NTC 10 k B3950 del cargador, los dos pegados a la batería |
| Energía | 2× INA219 | 1–2× **INA226** |
| Viento | — | Reeds (anemómetro + pluviómetro) por ULP; veleta AS5600 analógica |

## 5b. Inhibición de carga por temperatura (dos capas)

- **Hardware, sin firmware**: el NTC 10 k B3950 (de gota, con cable, **pegado a la batería** y llegando por un JST de 2 pines) va al pin TEMP del CN3722. Con su pull-up de 50 µA y umbrales de 0,175 V y 1,61 V [C], la carga queda permitida entre **~1 °C y ~51 °C** [I]. Cubre lo crítico, el frío (39 días bajo 0 °C), aunque el firmware esté colgado.
- **Firmware**: el 51 °C está por encima de los 45 °C de una Li-ion típica. El TCA9534 maneja `CHG_INH` → 2N7002 → tira el pin TEMP a GND y corta la carga [C, CN3722 fig. 3]. El firmware lo activa con DS18B20 > 45 °C. El mismo expansor lee CHRG y DONE.
- **El cargador propio de la Heltec (TP4054) no tiene NTC**: con el USB enchufado carga la celda en paralelo. **Regla operativa**: nada de service por USB con la caja bajo 0 °C. Si se puede, medir la corriente en JP3 con el USB conectado.

## 6. PCB

- **Hojas**:

  | Hoja | Contenido |
  |---|---|
  | `01_power_solar` | TVS, Schottky, CN3722 con inductor, AO3401A y R_CS; JST del NTC; header de respaldo para el CN3791; 2N7002 de CHG_INH |
  | `01b_power_meas` | INA226 con shunt Kelvin; jumper de batería |
  | `02_heltec` | zócalo, conector a JP3, test points, keepouts |
  | `03_rails` | 3× TPS22917, TCA9534 |
  | `04_port` (×4) | front-end de puerto; `04a_i2c_ext` agrega el TCA9517 en AIRE |
  | `05_sensors_int` | península del SHT40, VEML7700, conector del DS18B20 |
  | `06_expansion` | SP3485, header I2C/UART |
  | `07_mech` | agujeros, ranuras, versión |

  Los satélites `sat_aire` y `sat_veleta` van como **proyectos KiCad aparte**.
- **PCBA Economic, todo el SMD en F.Cu** [C, capacidades de JLC al 2026-11-07]: una cara, placa de 0,8–1,6 mm, sin rieles ni fiduciales, de 2 a 50 placas ensambladas.
  - Pasivos de **0603 como mínimo**, para poder retrabajar. R_CS, shunt y capacitores de bulk en 0805/1206.
  - Unificar valores para minimizar las partes Extended.
  - El THT lleva **Exclude from BOM/POS** (o DNP).
  - Dejar **≥ 2–3 mm entre pads THT y SMD chico**, para que entre la punta del soldador.
  - **Nada SMD caliente bajo el zócalo** de la Heltec. Mejor dejarlo libre.
- **Reglas de diseño**:
  - Mínimos de 0,15/0,15 mm (el mínimo de JLC es 0,10).
  - Una sola vía de 0,6/0,3 mm.
  - Cobre a borde de 0,5 mm.
  - **Texto de serigrafía de 1,0 mm con línea de 0,15**: hoy el proyecto está en 0,8 mm y **hay que cambiarlo**.
- **SHT40 en una península**: ranura fresada en U de 1–2 mm, lengüeta de ~10×12 mm sin plano de GND, en la esquina más fría, en diagonal opuesta al CN3722 y al inductor. Ensayo: cargar a 0,5 A con la caja cerrada y comparar contra el DS18B20.
- **VEML7700 (vista lateral)**: va en el **borde de la placa que enfrenta la ventana** de la caja, o en una pestaña o plaquita aparte si la ventana queda en otro plano. **Depende de la geometría de la caja.**
- **Flujo en KiCad**:
  - Campo `LCSC` en cada símbolo, cargado desde la Tabla de campos de símbolo.
  - Plugin **kicad-jlcpcb-tools** (Bouni, compatible con KiCad 10), que genera Gerber, BOM y CPL. Correrlo con todo commiteado.
  - **Antes de pagar, revisar en la vista previa de JLC la rotación de cada IC, diodo y TVS.** Hace falta una marca de pin 1 y de cátodo en la serigrafía.
  - Librería propia del proyecto (`ws_v2`) para las huellas importadas de LCSC/EasyEDA y la de la Heltec corregida. `LIB_TR3BOL_ESENCIAL` no se toca.
- **Heltec en zócalo** (tiras hembra 1×8 + 1×16 de pines torneados), sujeta con brida o clip. Altura ≈ 23 mm. **El TFT queda a la vista**, así que la PCB se orienta para poder leerlo con la tapa abierta.
- **Correcciones a la huella actual** `miHeltecV1.2_DIP_25_LongPads`:
  - (a) Borrar los pads `NC_42` / `NC_9`, que caen debajo de **la antena WiFi de resorte**.
  - (b) Rehacer el keepout como área de reglas en **todas las capas** (sin pistas, vías ni relleno), debajo del resorte (~12×10 mm) y del parche GNSS.
  - (c) Esa fila contra el borde de la placa, o con caladura.
- USB-C hacia un borde accesible. RST y GPIO0 repetidos en test points.
- B.Cu como plano de GND sólido con cosido de vías. El switching del CN3722 va **lejos de la Heltec y del u.FL**, en el extremo opuesto.
- **Clases de red**: Default 0,25 mm · PWR 0,6–1 mm · PV 1 mm. Vía de 0,6/0,3 mm.
- **Test points**: PV_IN, VBAT, 3V3, Vext, cada rail, SDA/SCL de los dos segmentos, UART, CHRG, DONE y GND doble.
- **Serigrafía**:
  - en cada JST: nombre de pin, pin 1, nombre del GX y color del hilo;
  - polaridad de batería y panel;
  - versión y fecha;
  - huellas oficiales de KiCad, revisadas en el visor 3D.
- **Barniz**: marcar en `User.x` lo que se enmascara.
- 4× M3 NPTH lejos de las antenas.
- **Tamaño**: objetivo ≤ 100×100 mm; con ~15×20 cm útiles hay margen hasta ~100×140 si el cargador lo pide.

## 7. Pruebas de banco ANTES de fabricar (en orden)

1. **Spike del ULP en lora-lab** (bloqueante): contar 0–300 Hz en GPIO17 y pulsos lentos en GPIO7 con la Tracker dormida, y medir la corriente.
2. **Camino de alimentación de la V1.2 contra el esquemático V0.5**:
   - continuidad entre la entrada de U4 y la de U6, y entre el pin 5V y VDD_5V;
   - **paso y polaridad del conector de batería** ("1.25X2P").
3. **GPIO 26 y 37 como I2C**, con el TFT funcionando.
4. **¿X1 (32 kHz) está montado en la V1.2?**
5. **Repetidor I2C**: aislamiento con el lado B sin alimentar y con el lado B en corto a GND.
6. **AS5600 por OUT analógica** contra el ADC del S3: linealidad y ruido con un cable del largo real.
7. Duración del cierre del reed (analizador lógico).
8. Si se puede, medir el reposo de la V1 (M1 de `medicion_consumo_reposo.md`).

## 8b. Respuestas de Mau del 2026-11-07 (tarde)

- **Satélites en el mismo pedido.** Todo lo que se pueda va en un solo envío.
- **JP3 (conector de batería de la Heltec) está en la cara de abajo**, la opuesta a la pantalla. Con el módulo en el zócalo, **el conector queda mirando a la PCB principal**. Consecuencias [I]:
  - Debajo de JP3, en la PCB principal, va un **keepout de componentes** con altura para el plug de 1,25 mm y la salida del cable. Hay que medir el plug y el radio del cable con la Heltec en la mano.
  - Con el zócalo de ~8,5 mm puede alcanzar. Si no, usar tiras de zócalo más altas, o una **ventana fresada en la PCB** bajo JP3 por donde pase el cable.
  - La batería se enchufa a la Heltec antes de insertarla en el zócalo. Hay que tenerlo en cuenta en el procedimiento de service.
  - Alternativa: un cable corto desde JP3 a un JST de la PCB principal ubicado justo al lado, para no tener que desenchufar debajo del módulo.
- **Ventana de la caja**: Mau la describe como "en la parte media, delantera, y asoma por abajo de la caja".
  - **[pendiente de confirmar]**: ¿está en la cara frontal, en su parte baja, o en la base (mirando hacia abajo)?
  - Si está en la cara frontal y la PCB va paralela al frente, el VEML7700, que es de vista lateral, no la ve desde la placa principal. Lo resolvería una pestaña o una plaquita en ángulo recto.
  - Si mira hacia abajo, mediría luz reflejada del suelo: es un índice relativo, no lux de cielo.
  - Decidir con una foto o un croquis de la caja.

### BMP581 contra BMP390 [C, datasheets BST-BMP581-DS004-13 y BST-BMP390-DS002-07]

| | BMP390 | BMP581 |
|---|---|---|
| Absoluta | ±50 Pa típ., ±0,5 hPa (0–65 °C) | ±75 Pa en **todo el rango −40…85 °C, ya incluida la deriva de soldadura** |
| Relativa | ±3 Pa (±0,03 hPa, 25–40 °C) | similar o mejor |
| Coef. térmico del offset | ±0,62 Pa/K | ±0,5 Pa/K |
| Deriva a largo plazo | ±0,16 hPa/año | **±10 Pa/año** |
| Ruido | ~1–2 Pa RMS sin filtro (aprox.) | **< 0,1 Pa RMS** nativo |
| Elemento sensor | piezorresistivo [C] | capacitivo (de memoria [?]) |
| Dirección I2C | 0x76/0x77 | **0x46/0x47** |
| Stock JLC hoy | **0** | 9388 |
| Software | librerías Arduino maduras (Adafruit) | Bosch BMP5 Sensor API, SparkFun; driver nuevo |
| Cuidado | — | Se daña con un pin en alto y VDDIO apagado [C]: lo cubre el TCA9517 apagando todo junto |

**Lectura**:
- Para una estación (QNH, tendencia, marea barométrica), los dos sobran contra el BMP180/085 de la V1. El BMP581 es la generación nueva: menos ruido, menos deriva y especificación honesta en todo el rango de temperatura del sitio, incluido −7 °C.
- La única contra real es el driver nuevo. Es una tarea de firmware chica, no un riesgo.
- **Recomendación: BMP581.**

## 9. PCBA: BOM, costo y envío [stock del API de JLC al 2026-11-07]

| Función | Part number | LCSC | Tipo | Encapsulado | Stock |
|---|---|---|---|---|---|
| Cargador | CN3722 | C77905 | Ext | TSSOP-16 | 3386 |
| P-MOSFET | AO3401A | C15127 | Basic | SOT-23 | 1,69 M |
| Schottky D1/D2 | SS34 | C8678 | Basic | SMA | 3 M (**riesgo de fuga, ver abajo**) |
| Inductor 47 µH | SWPA6045S470MT | C36414 | Ext | 6×6 | 10 716 (Isat [?]) |
| R_CS 0,4 Ω | 4× 0,1 Ω 1206 en serie | C25334 | Basic | 1206 | 661 k |
| TVS panel | SMBJ16CA | C151255 | Ext | SMB | 4864 |
| Monitor de batería | INA226AIDGSR | C49851 | Ext | VSSOP-10 | 16 063 |
| Shunt | 0,1 Ω 1206 | C25334 | Basic | 1206 | — |
| Load switch ×3 | TPS22917DBVR | C2681320 | Ext | SOT-23-6 | 3735 |
| Repetidor I2C | TCA9517DR | C182421 | Ext | SOIC-8 | 6073 |
| Expansor | TCA9534PWR | C783615 | Ext | TSSOP-16 | 8970 |
| CHG_INH | 2N7002 | C8545 | Basic | SOT-23 | 4,18 M |
| T/HR caja | SHT40-BD1B-R2 | C7461849 | Ext | DFN-4 | **2305** |
| Luz | VEML7700-TT | C1850416 | Ext | lateral | 6724 |
| RS485 | SP3485EN-L/TR | C8963 | Basic | SOIC-8 | 342 917 |
| ESD | PESD5V0S2BT | C49338 | Ext | SOT-23 | 8682 |
| Pasivos | 10 k C25804 · 100 k C25803 · 1 k C21190 · 4,7 k C23162 · 2,2 k C4190 · 100 nF C14663 · 1 µF/50 V 0603 C15849 · 10 µF/25 V 0805 C15850 · 10 µF/50 V 1206 C13585 · 1 nF C1588 · 10 nF C57112 | — | Basic | 0603–1206 | — |
| `sat_aire` | SHT45-AD1F-R2 · BMP581 | C5360602 · C5362283 | Ext | DFN / LGA | 3273 · 9388 |
| `sat_veleta` | AS5600-ASOT | C499458 | Ext | SOIC-8 | **352** |

- La placa principal tiene **10 partes Extended**, ≈ USD 31 de carga de feeders.
- **Revalidar el stock el día del pedido**: el SHT40-BD1B y el AS5600 tienen stock bajo.
- **Costo estimado** [I sobre la tarifa citada, actualizada el 22/10/2026], para 5 PCB con 2 ensambladas, sin la PCB desnuda ni el flete:
  - placa principal: ~USD 80–85;
  - con las dos satélites: ~USD 130–135.
  - La PCB, el flete y el IVA salen del cotizador.
- **Repuestos en el mismo envío**: LCSC permite combinar su pedido con el de JLC si tienen la misma moneda y el mismo customer ID [C]. Ahí van CN3722, AS5600 y conectores extra.
- **Aduana (courier, ARCA novedad 5939 del 06/10/2026)** [C]:
  - Franquicia de USD 400.
  - Tope de USD 3000 por envío.
  - **Hasta 3 unidades de la misma especie.** Hay que consultarle al courier cómo cuenta 5 PCB iguales.
- **Consumo de reposo del cargador** [C/I]: de noche la batería alimenta BAT/CSP del CN3722 (~10 µA) más la fuga inversa de D1/D2. Con la SS34 esa fuga es incierta en caliente. **Medir con el jumper, de noche y en caliente, antes de cerrar la elección del Schottky.** Si hace falta, elegir uno de baja fuga.

## 8. Preguntas abiertas

- Medidas internas exactas de la caja, tetones, y orientación de montaje (PCB vertical u horizontal), que afecta la vista de cielo del GNSS. **También dónde está la ventana**, que define dónde va el VEML7700 (vista lateral).
- Confirmar el BMP581 (comparación en §8b).
- Geometría exacta de la ventana (§8b).
- Medir el plug del JP3 y su cable para el keepout debajo de la Heltec.
- ¿Compra por LCSC junto con la PCB o por MercadoLibre?
- ¿La expansión RS485 tiene que alimentar sensores de 5–24 V? Si es así, va en un conector distinto del GX12-4.
- Largo del cable al mástil del viento.
- GPIO21 (EN_EXP) comparte gate con el backlight del TFT: con Vext encendido y EN_EXP en alto se prende la pantalla. Es aceptable, pero hay que tenerlo presente en el firmware.

## Datasheets pendientes de bajar

- ESP32-S3 Technical Reference Manual (ULP, pads en sleep).
- AS5600 (salida OUT, rangos).
- ~~TCA9517~~ (bajado por el agente); PCA9517A como segunda fuente.
- Sunlord SWPA6045S470MT (Isat).
- Sensirion SHT4x Handling Instructions / Design Guide.
- Vishay "Designing the VEML7700 Into an Application".
- Catálogo JST XH.
- Datasheet de la celda elegida (rango de temperatura de carga).
- ~~CE6260B33M~~ (bajado: `CE6260.PDF`).
