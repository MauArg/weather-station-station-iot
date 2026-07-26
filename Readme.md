# Infraestructura — Weather Station

Stack de servicios que corre en la Raspberry Pi.

Link al anemómetro impreso en 3D = https://makerworld.com/en/models/2052859-the-windicator-v1

## Setup inicial (primera vez)

### 1. Variables de entorno
```bash
cp .env.example .env
nano .env   # completar MQTT_PASSWORD
```

### 2. Crear el archivo de passwords de Mosquitto
```bash
docker run --rm -it eclipse-mosquitto:2 \
  mosquitto_passwd -c /tmp/passwd weather \
  | cat > ./mosquitto/config/passwd
```

> ⚠️ `passwd` está en `.gitignore`. Nunca lo commitees — tiene credenciales en texto plano.

### 3. Levantar
```bash
docker compose up -d
```

### 4. Verificar que Mosquitto está funcionando
```bash
# En una terminal — escuchar todos los topics de la estación
docker exec -it mosquitto \
  mosquitto_sub -h localhost -u weather -P TU_PASSWORD -t "station/#" -v

# En otra terminal — publicar mensaje de prueba
docker exec -it mosquitto \
  mosquitto_pub -h localhost -u weather -P TU_PASSWORD \
  -t "station/01/test" -m "hola"
```

Si ves `station/01/test hola` en la primera terminal, el broker está listo.

---

## Operación

```bash
# Ver logs en tiempo real
docker compose logs -f mosquitto

# Reiniciar broker
docker compose restart mosquitto

# Bajar todo
docker compose down

# Bajar y eliminar volúmenes (⚠️ borra datos persistidos)
docker compose down -v
```

---

## Activar service mode en el ESP32

**Vía recomendada: la UI.** El dashboard tiene una vista de *service mode* (botón en la barra superior) que arma la sesión, detecta sola cuándo el nodo está despierto y escuchando, verifica la versión después del flash y desarma. Ver `weather-station-frontend-dashboard/`.

Ahí también están el visor de payloads en vivo, el estado por sensor, el margen del buffer MQTT y el indicador de batería con semáforo de riesgo de flasheo.

### Por línea de comandos (fallback si la UI o el backend están caídos)

```bash
# Desde la Raspberry Pi
docker exec -it mosquitto \
  mosquitto_pub -h localhost -u weather -P TU_PASSWORD \
  -t "station/01/cmd" -r \
  -m '{"cmd":"maintenance","timeout_min":15,"issued_at":"'$(date -u +%FT%TZ)'"}'
```

El ESP lo recibirá en el próximo ciclo de wake. Para saber cuándo está listo **no hace falta hacerle ping**: publica en `station/01/status` un `service_mode_active` al entrar y un `service_mode_alive` cada 30 s con el `remaining_sec`, así que alcanza con escuchar ese topic:

```bash
docker exec -it mosquitto \
  mosquitto_sub -h localhost -u weather -P TU_PASSWORD -t "station/01/status" -v
```

Una vez en service mode, flashear desde PlatformIO:
```bash
pio run -e ota_production -t upload     # el que va a campo (LOG_LEVEL=0)
pio run -e ota_development -t upload    # debug (LOG_LEVEL=2, paga 2 s por wake)
```

Y al terminar, **limpiar el retenido** — si no, el nodo se queda despierto hasta que venza el timeout:
```bash
docker exec -it mosquitto \
  mosquitto_pub -h localhost -u weather -P TU_PASSWORD -t "station/01/cmd" -r -m ''
```

---

## Pipeline completo de datos (actualizado 2026-07-11)

Los bloques de **InfluxDB** y **N8N** en `docker-compose.yml` están comentados porque esos servicios **ya están corriendo, pero no acá** — no en la Raspberry Pi de la estación, sino repartidos así:

```
ESP32 ──MQTT──▶ Mosquitto (Docker, esta RPi)
                      │
                      ▼
                 N8N (host de esta RPi, NO en Docker)
                 MQTT Trigger → Code (Line Protocol) → HTTP Request
                      │
                      ▼
              InfluxDB v2 (Docker, NAS 192.168.18.251)
                      │
                      ▼
                Grafana (Docker, NAS 192.168.18.251, puerto 3000)
```

- **NAS (192.168.18.251)**: InfluxDB v2 (imagen fijada a `influxdb:2` — **nunca** `influxdb:latest`, la v3 Core tiene ventana de consulta de 72h, inutilizable para histórico meteorológico) y Grafana, ambos en Docker sobre una red interna `iot-net`, con datos persistidos en `/mnt/disco/influxdb` y `/mnt/disco/grafana`. Hay un override de systemd (drop-in) que hace que Docker espere a que el mount NFS esté listo antes de arrancar — sin eso, los contenedores fallan en boot si el NFS tarda en montar.
- **N8N** corre directo en el host de esta Raspberry Pi (no dockerizado), no en el `docker-compose.yml` de este repo.

⚠️ Los `docker-compose.yml` / configuración del lado del NAS (InfluxDB, Grafana) todavía no están versionados en ningún repo de este proyecto — viven solo en el NAS. Pendiente de incorporar a git si se quiere que el NAS también sea reproducible desde acá.