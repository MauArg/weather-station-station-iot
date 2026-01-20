#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_BMP085.h>

Adafruit_BMP085 bmp;

void setup() {
  Serial.begin(115200);
  delay(2000); // Espera para abrir Serial Monitor
  
  Serial.println("=== Test BMP180 ===");
  
  if (!bmp.begin()) {
    Serial.println("ERROR: No se detectó el BMP180!");
    Serial.println("Verificá conexiones:");
    Serial.println("  VCC -> 3.3V");
    Serial.println("  GND -> GND");
    Serial.println("  SDA -> GPIO8");
    Serial.println("  SCL -> GPIO9");
    while (1) delay(1000);
  }
  
  Serial.println("BMP180 inicializado correctamente\n");
}

void loop() {
  float temperatura = bmp.readTemperature();
  float presion = bmp.readPressure() / 100.0F; // Convertir Pa a hPa
  float altitud = bmp.readAltitude();
  
  Serial.println("--- Lectura ---");
  Serial.print("Temperatura: ");
  Serial.print(temperatura);
  Serial.println(" °C");
  
  Serial.print("Presión:     ");
  Serial.print(presion);
  Serial.println(" hPa");
  
  Serial.print("Altitud:     ");
  Serial.print(altitud);
  Serial.println(" m");
  Serial.println();
  
  delay(2000);
}