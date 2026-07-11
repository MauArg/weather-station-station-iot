# station-iot

Repo **secondary** del proyecto Weather Station (ver [`../CLAUDE.md`](../CLAUDE.md) para contexto general, topología de repos y política de commits/push — aplica igual acá).

Firmware ESP32 (PlatformIO, `src/`) de la estación meteorológica, más **todo** el diseño de hardware — fuente única, no se duplica en el repo main: esquemáticos, proyectos Fritzing `.fzz` (raíz del repo), exports de fabricación en `PCB/` (gerbers/etch/silk en PDF, por versión de placa), y `componentes_y_conexiones.md`. También la infraestructura del broker MQTT que corre en la Raspberry Pi (`infra/docker-compose.yml` — Mosquitto). Ver `Readme.md` para operación del stack y flasheo OTA vía service mode.

Hay un issue de hardware sin resolver (bus I2C bloqueado, ver [`../i2c-bus-lockup-investigation.md`](../i2c-bus-lockup-investigation.md) y [`../STATUS.md`](../STATUS.md)) — sospechoso principal: AS5600 en JST5.
