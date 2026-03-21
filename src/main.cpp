#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_SHT31.h>
#include <Adafruit_BMP085.h>

#define SDA_PIN 6
#define SCL_PIN 5

// Altitud de Embalse en metros
#define ALTITUD_M 780.0

Adafruit_SHT31 sht31;
Adafruit_BMP085 bmp;
bool sht_ok, bmp_ok;

void setup() {
  Serial.begin(115200);
  delay(2000);
  Wire.begin(SDA_PIN, SCL_PIN);

  sht_ok = sht31.begin(0x44);
  bmp_ok = bmp.begin();

  Serial.printf("[SHT31]  %s\n", sht_ok ? "OK" : "NO DETECTADO");
  Serial.printf("[BMP180] %s\n", bmp_ok ? "OK" : "NO DETECTADO");
  Serial.println();
}

void loop() {
  if (sht_ok) {
    float t = sht31.readTemperature();
    float h = sht31.readHumidity();
    if (!isnan(t) && !isnan(h))
      Serial.printf("SHT31  | %.2f °C | %.1f %%HR\n", t, h);
    else
      Serial.println("SHT31  | ERROR de lectura");
  }

  if (bmp_ok) {
    float t   = bmp.readTemperature();
    float pres_abs = bmp.readPressure() / 100.0;  // hPa local

    // Dentro del loop, después de leer el BMP180:
    float pres_qnh = pres_abs / pow(1.0 - (ALTITUD_M / 44330.0), 5.255);
    Serial.printf("BMP180 | %.2f °C | %.2f hPa (abs) | %.2f hPa (QNH)\n", bmp.readTemperature(), pres_abs, pres_qnh);
  }

  // Cross-check de temperatura entre sensores
  if (sht_ok && bmp_ok) {
    float delta = abs(sht31.readTemperature() - bmp.readTemperature());
    if (delta > 3.0)
      Serial.printf("  ⚠ Delta temp: %.1f °C — revisar posicionamiento\n", delta);
  }

  Serial.println("---");
  delay(2000);
}