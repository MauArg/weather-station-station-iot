#pragma once
#include <Arduino.h>
#include <PubSubClient.h>
#include "command.h"

// ─── Estado persistente en RTC memory ────────────────────────────────────────
// Sobrevive al deep sleep y a reinicios por software.
// Se pierde solo con power-off total (fallo de batería, etc.).
extern RTC_DATA_ATTR bool     rtc_inServiceMode;
extern RTC_DATA_ATTR uint32_t rtc_serviceStartEpoch;   // Unix timestamp de entrada
extern RTC_DATA_ATTR int      rtc_serviceTimeoutMin;

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
void serviceMode_exit(PubSubClient& mqtt, const char* reason);
