#pragma once
#include <Arduino.h>
#include <PubSubClient.h>
#include "command.h"

// ─── Estado persistente en RTC memory ────────────────────────────────────────
// Sobrevive al deep sleep y a reinicios por software.
// Se pierde solo con power-off total (fallo de batería, etc.).
extern RTC_DATA_ATTR bool     rtc_inServiceMode;
extern RTC_DATA_ATTR int      rtc_serviceTimeoutMin;

// Segundos de service mode ya consumidos, acumulados entre reinicios.
//
// Existe porque el timeout no acotaba nada: si se caía MQTT, el nodo salía por
// deep sleep sin poder limpiar el comando retenido, y al despertar lo volvía a
// leer y arrancaba una sesión nueva con el timeout entero. Con un enlace que se
// cae seguido eso se repite indefinidamente — el nodo queda despierto a 50-140 mA
// en ciclos de un minuto sin que nada lo corte. Acumulando acá, un reinicio retoma
// el saldo en vez de estrenar presupuesto.
//
// Se pone en cero solo cuando la sesión termina de verdad, o sea cuando se logró
// limpiar el retenido y por lo tanto no puede haber re-entrada.
extern RTC_DATA_ATTR uint32_t rtc_serviceElapsedSec;

// ─── API pública ──────────────────────────────────────────────────────────────

// Llama al despertar ANTES de tomar decisiones.
// Evalúa el comando recibido + el estado RTC para determinar
// si corresponde entrar/continuar/salir del modo servicio.
void serviceMode_evaluate(PubSubClient& mqtt, const Command& cmd);

// Retorna true si el dispositivo está actualmente en modo servicio
// (puede ser de un ciclo anterior persistido en RTC).
bool serviceMode_isActive();

// Bloquea hasta que se reciba un firmware OTA, se agote el timeout,
// o el servidor limpie el comando. Llama a goToDeepSleep() al finalizar.
void serviceMode_run(PubSubClient& mqtt, int timeoutMin);

// Limpia el estado RTC y publica el topic retenido vacío para limpiar el broker.
// sessionSec son los segundos que duró esta sesión, que se suman al acumulado de
// RTC para que el timeout siga siendo absoluto si el nodo vuelve a entrar.
void serviceMode_exit(PubSubClient& mqtt, const char* reason, uint32_t sessionSec = 0);
