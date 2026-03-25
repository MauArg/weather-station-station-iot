# Infraestructura — Weather Station

Stack de servicios que corre en la Raspberry Pi.

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

## Próximos servicios

En `docker-compose.yml` están comentados los bloques de **InfluxDB** y **N8N**.
Cuando corresponda, descomentar, agregar las variables en `.env` y:
```bash
docker compose up -d
```