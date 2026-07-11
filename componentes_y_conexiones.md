\# Estación Meteorológica Solar — Componentes y Conexiones

\*\*Embalse, Córdoba, Argentina · Rev 2.0\*\*

&#x20;

\---

&#x20;

\## Hardware principal

&#x20;

| Componente | Modelo | Notas |

|---|---|---|

| Microcontrolador | ESP32-C3 SuperMini | 5V via boost, 3.3V I/O |

| Sensor T+H exterior | SHT31-D | I2C 0x44, en plato exterior |

| Sensor presión | BMP180 | I2C 0x77, en plato exterior |

| Monitor corriente solar | INA219 | I2C 0x41 |

| Monitor corriente sistema | INA219 | I2C 0x40 |

| Cargador solar MPPT | CN3791 | Off-board, conectado por cable |

| Step-up boost | — | Calibrado a 5V, alimenta ESP32 |

| Batería | LiPo 1500mAh | Conecta directo a CN3791 (JST PH 1.25mm) |

| Panel solar | 5.2W \~24-cell | 210×165mm, diseñado para carga directa 12V, Voc=14.2V, Vmp≈11-12V |

| Caja estanca | — | 190×140×70mm |

| Mástil | PVC 3/4" marrón | — |

&#x20;

\---

&#x20;

\## GPIOs asignados — ESP32-C3 SuperMini

&#x20;

| GPIO | Señal | Tipo | Rail | Notas |

|---|---|---|---|---|

| GPIO0 | DHT11 DATA | Digital 1-wire | Rail B | — |

| GPIO1 | Pluviómetro SIGNAL | Interrupt | Always-on | Reed switch, pull-up interno |

| GPIO2 | Anemómetro SIGNAL | Interrupt | Always-on | Reed switch, normalmente abierto → safe boot |

| GPIO3 | Fotoresistor ADC | ADC1\_CH3 | Rail B | — |

| GPIO4 | Rain sensor AO | ADC1\_CH4 | Rail B | JTAG MTMS como entrada en boot → sin conflicto |

| GPIO5 | SCL | I2C clock | — | Bus I2C compartido |

| GPIO6 | SDA | I2C data | — | Bus I2C compartido |

| GPIO7 | Rail A NPN base | Digital out | — | BC337 low-side switch sensores primarios |

| GPIO8 | Rail B NPN base | Digital out | — | BC337 low-side switch sensores secundarios. GPIO8 LOW en boot → Rail B OFF al arrancar ✅ |

| GPIO10 | DS18B20 DATA | 1-Wire | Always-on | Pull-up incluido en módulo Sunfounder |

&#x20;

\*\*GPIOs libres:\*\* GPIO9 (⚠️ botón BOOT físico — no usar), GPIO20, GPIO21

&#x20;

\---

&#x20;

\## I2C bus

&#x20;

\- \*\*SDA:\*\* GPIO6 — cable verde (color UTP)

\- \*\*SCL:\*\* GPIO5 — cable verde/blanco (color UTP)

\- \*\*Pull-ups externos:\*\* 2× 4.92kΩ (SDA→3.3V y SCL→3.3V) soldados en placa principal

\- \*\*Motivo:\*\* pull-ups internos del ESP32-C3 insuficientes para la capacitancia del cable UTP de \~1.5m al plato exterior

\### Dispositivos I2C

&#x20;

| Dirección | Dispositivo | Ubicación |

|---|---|---|

| 0x40 | INA219 system | Placa principal |

| 0x41 | INA219 solar | Placa principal |

| 0x44 | SHT31-D | Plato exterior (via UTP) |

| 0x77 | BMP180 | Plato exterior (via UTP) |

| 0x36 | AS5600 veleta | Pendiente instalación |

&#x20;

\---

&#x20;

\## Switching de rails — BC337 NPN low-side

&#x20;

Dos transistores BC337 (hFE 250 y 263, pinout C-B-E con plano al frente) controlan el GND de los sensores.

&#x20;

\### Circuito (idéntico para Rail A y Rail B)

&#x20;

```

GPIO ──\[2.13kΩ]──┬── Base BC337

&#x20;                │

&#x20;             \[9.9kΩ]

&#x20;                │

&#x20;               GND

&#x20;

VCC sensores (siempre conectado)

&#x20;     │

&#x20; \[sensor]

&#x20;     │

&#x20; GND sensor ── Collector BC337

&#x20;                    │

&#x20;              Emitter BC337

&#x20;                    │

&#x20;                GND sistema

```

&#x20;

| Parámetro | Valor |

|---|---|

| R base | 2.13kΩ (nominal 2.2kΩ, tolerancia 3%) |

| R pull-down | 9.9kΩ (nominal 10kΩ, tolerancia 1%) |

| Ib | 1.22mA |

| Ic máxima estimada | \~5mA |

| hFE necesario | 4 → BC337 en saturación profunda ✅ |

&#x20;

\### Rail A — sensores primarios (GPIO7)

Activo en tiers 1, 2 y 3. Se apaga en tier 4.

\- SHT31-D + BMP180 (via UTP)

\### Rail B — sensores secundarios (GPIO8)

Activo solo en tier 1. Se apaga en tier 2.

\- DHT11

\- Rain sensor (placa sensora directa, sin módulo Sunfounder)

\- Fotoresistor

\- AS5600 veleta (pendiente instalación)

\### Always-on (sin switching)

\- DS18B20 (temperatura enclosure)

\- Anemómetro (reed switch)

\- Pluviómetro (reed switch)

\---

&#x20;

\## Tiers de energía por voltaje de batería

&#x20;

| Tier | Voltaje | Estado |

|---|---|---|

| 1 | ≥ 4.10V | Operación completa. Rail A + Rail B ON |

| 2 | 3.90–4.10V | Rail A ON · Rail B OFF |

| 3 | 3.75–3.90V | Solo SHT31+BMP180 cada 5min |

| 4 | < 3.75V | Heartbeat cada 30min. Todo OFF |

&#x20;

Tiers configurables via MQTT `config` sin reflash.

&#x20;

\---

&#x20;

\## Circuito rain sensor (sin módulo Sunfounder)

&#x20;

Se prescinde del módulo completo. Solo se usa la placa sensora conectada directamente.

&#x20;

```

3V3

&#x20;│

\[R1 9.9kΩ]─┐

\[R2 9.9kΩ]─┴──── nodo señal+ ──── GPIO4 (ADC)

(paralelo = 4.95kΩ)   │

&#x20;                   \[C1 100nF]

&#x20;                      │

&#x20;                   Rail B GND ──── placa sensora pin 2

```

&#x20;

\- R1 ∥ R2 = 4.95kΩ pull-up (impedancia fuente óptima para ADC)

\- C1 100nF cerámico (marcado "104"): filtra ruido 50Hz del cable de 1.5m

\- Salida: solo AO analógica. DO descartado (redundante con lectura ADC por firmware)

\---

&#x20;

\## Placa principal — 31×26 holes

&#x20;

\### Zonas

&#x20;

| Zona | Cols | Rows | Contenido |

|---|---|---|---|

| INA219 #1 Solar | cols 1–9 | rows 1–10 | Header 6× hembra col 9, rows 3–8 — \*\*I2C 0x41\*\* |

| INA219 #2 System | cols 10–18 | rows 1–10 | Header 6× hembra col 18, rows 3–8 — \*\*I2C 0x40\*\* |

| JST Solar + CN3791 | 19–31 | 1–10 | 2× JST 2p acceso superior derecho |

| Step-up boost | 1–14 | 11–16 | 4 pines (VIN col 1, VOUT col 14) |

| ESP32-C3 SuperMini | 15–21 | 11–18 | 8× hembra col 15 + 8× hembra col 21 |

| NPN A + resistencias | 23–27 | 12–14 | BC337 Rail A |

| NPN B + resistencias | 23–27 | 16–18 | BC337 Rail B |

| Pull-ups I2C | 28–29 | 22–25 | R\_SCL (28) y R\_SDA (29) |

| Buses | — | 22–25 | SCL·SDA·GND·3V3 full width |

| Header inter-board | 10–21 | 26 | Macho 1×12, borde inferior |

&#x20;

\### Buses

&#x20;

| Fila | Señal |

|---|---|

| 22 | SCL |

| 23 | SDA |

| 24 | GND |

| 25 | 3V3 |

| 26 | Header inter-board 1×12 (cols 10–21) |

&#x20;

\---

&#x20;

\## Placa auxiliar JST — 24×10 holes

&#x20;

\### Conectores JST

&#x20;

| # | Conector | Pines | Pinout | Rail | Ubicación |

|---|---|---|---|---|---|

| 1 | UTP SHT31+BMP180 | 4 | VCC · GND\_A · SCL · SDA | Rail A | cols 2–5, fila 3 |

| 2 | Rain sensor | 2 | señal+ · señal− | Rail B | cols 7–8, fila 3 |

| 3 | DHT11 | 3 | VCC · GND\_B · DATA | Rail B | cols 11–13, fila 3 |

| 4 | Fotoresistor | 3 | VCC · GND\_B · ADC | Rail B | cols 15–17, fila 3 |

| 5 | Veleta AS5600 | 5 | VCC · GND\_B · SCL · SDA · DIR | Rail B | cols 19–23, fila 3 |

| 6 | DS18B20 | 3 | VCC · GND · DATA | Always-on | cols 2–4, fila 7 |

| 7 | Anemómetro | 3 | VCC · GND · SIGNAL | Always-on | cols 6–8, fila 7 |

| 8 | Pluviómetro | 3 | VCC · GND · SIGNAL | Always-on | cols 10–12, fila 7 |

&#x20;

\### Pulsadores de diagnóstico

&#x20;

| Pulsador | Cols | Rows | GPIO | En paralelo con |

|---|---|---|---|---|

| BTN\_ANE | 15–19 | 7–10 | GPIO2 | Anemómetro (JST7 SIGNAL) |

| BTN\_PLU | 20–24 | 7–10 | GPIO1 | Pluviómetro (JST8 SIGNAL) |

&#x20;

Footprint: 5 cols × 4 rows. Pines en las 4 esquinas.

P1 = señal GPIO. P2 = GND. Se cierra al presionar.

&#x20;

\### DIR veleta AS5600

\- Hardwired a GND vía puente de estaño en holes (23,4)→(23,5)

\- Pad de acceso externo en col 24 borde derecho

\- Para invertir: cortar puente con bisturí, conectar (23,4) a 3V3

\---

&#x20;

\## Header inter-board 1×12

&#x20;

Macho en placa principal (fila 26, cols 10–21).

Hembra en placa auxiliar (fila 1, cols 7–18).

&#x20;

| Pin | Col principal | Col auxiliar | Señal | GPIO |

|---|---|---|---|---|

| 1 | 10 | 7 | GND | — |

| 2 | 11 | 8 | 3V3 | — |

| 3 | 12 | 9 | SCL | GPIO5 |

| 4 | 13 | 10 | SDA | GPIO6 |

| 5 | 14 | 11 | Rail A GND | NPN A collector |

| 6 | 15 | 12 | Rail B GND | NPN B collector |

| 7 | 16 | 13 | Anemómetro | GPIO2 |

| 8 | 17 | 14 | Pluviómetro | GPIO1 |

| 9 | 18 | 15 | DHT11 | GPIO0 |

| 10 | 19 | 16 | DS18B20 | GPIO10 |

| 11 | 20 | 17 | Fotoresistor ADC | GPIO3 |

| 12 | 21 | 18 | Rain sensor AO | GPIO4 |

&#x20;

\---

&#x20;

\## Componentes pendientes de instalación

&#x20;

| Componente | Estado | Notas |

|---|---|---|

| AS5600 veleta | Por comprar | Módulo breakout I2C 0x36, 5 pines usados (VCC·GND·SCL·SDA·DIR) |

| 608ZZ bearings | Por comprar | Para anemómetro y veleta |

| Imán diametral grande | Por comprar | Para veleta AS5600 — necesita imán diametral específico para lectura angular |

| Chasis anemómetro+veleta | ✅ Impreso en PLA | STL "Weather Station One" (Printables/MakerWorld). Pendiente armado mecánico |

&#x20;

\### Materiales disponibles para armado

\- Reed switch del kit Sunfounder → apto para anemómetro (detección de pulsos)

\- Imanes pequeños disponibles → pueden funcionar para anemómetro (activar reed switch)

\- ⚠️ Falta imán diametral grande específico para AS5600 (veleta de dirección)

\### Pendiente de diseño

\- Definir cómo instalar imanes y sensores en base al diseño de la carcasa impresa

\- Verificar posición del reed switch relativa al rotor del anemómetro

\- Verificar posición del AS5600 relativa al eje de la veleta (distancia óptima al imán: 0.5–3mm)

\---

&#x20;

\## Notas de diseño importantes

&#x20;

\- \*\*`system\_mW` en InfluxDB no es consumo real\*\* — solo captura la fase WiFi activa (\~10s), no el deep sleep (\~54s). Consumo real inferir de estabilidad de voltaje de batería.

\- \*\*I2C sobre UTP requiere pull-ups externos\*\* — 4.92kΩ validados en prueba de campo de 46 horas.

\- \*\*CN3791 MPPT configurado para Vmp real del panel\*\* — verificar R1/R2 del divisor VMP físicamente. Mismatch con panel de 24 celdas (Vmp≈11.5V) inutiliza el MPPT.

\- \*\*GPIO8 LOW en boot\*\* — deshabilita mensajes ROM por UART0. Sin impacto en operación del firmware.

\- \*\*GPIO9 reservado\*\* — es el botón BOOT físico del ESP32-C3 SuperMini. No conectar.

\- \*\*Módulos Sunfounder: desoldar LEDs\*\* antes de montar en PCB — BMP180 (ya hecho), DS18B20, DHT11, fotoresistor (verificar si tiene).

\- \*\*Rain sensor: prescindir del módulo Sunfounder completo\*\* — conectar placa sensora directamente con pull-up 4.95kΩ y filtro 100nF en placa auxiliar.

