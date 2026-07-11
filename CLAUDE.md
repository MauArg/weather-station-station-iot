# station-iot

Repo **secondary** del proyecto Weather Station (ver [`../CLAUDE.md`](../CLAUDE.md) para contexto general, topología de repos y política de commits/push — aplica igual acá).

Firmware ESP32 (PlatformIO, `src/`) de la estación meteorológica, más **todo** el diseño de hardware — fuente única, no se duplica en el repo main: esquemáticos, proyectos Fritzing `.fzz` (raíz del repo), exports de fabricación en `PCB/` (gerbers/etch/silk en PDF, por versión de placa), y `componentes_y_conexiones.md` (estado actual de hardware/campo — leer primero para cualquier trabajo de hardware). También la infraestructura del broker MQTT que corre en la Raspberry Pi (`infra/docker-compose.yml` — Mosquitto) y el pipeline completo de datos (ver `Readme.md`).

Ver [`aprendizajes_y_roadmap.md`](./aprendizajes_y_roadmap.md) para decisiones de diseño, aprendizajes técnicos y el roadmap del proyecto (subsistema de viento, mejoras pendientes, etc.).

Issue de hardware conocido y de baja severidad (bus I2C, causa real = cold solder joints en las perfboards del prototipo, ya mayormente resuelto): ver [`../i2c-bus-lockup-investigation.md`](../i2c-bus-lockup-investigation.md) y la sección "Estado actual en campo" de `componentes_y_conexiones.md`.
