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

```bash
# Desde la Raspberry Pi
docker exec -it mosquitto \
  mosquitto_pub -h localhost -u weather -P TU_PASSWORD \
  -t "station/01/cmd" -r \
  -m '{"cmd":"maintenance","timeout_min":15,"issued_at":"'$(date -u +%FT%TZ)'"}'
```

El ESP lo recibirá en el próximo ciclo de wake. Una vez en service mode, flashear desde PlatformIO:
```bash
pio run -e ota_upload -t upload
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