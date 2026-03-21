#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_SHT31.h>
#include <Adafruit_BMP085.h>
#include <Adafruit_INA219.h>

// ─── WiFi ────────────────────────────────
const char* SSID     = "Ire y Mau";
const char* PASSWORD = "Lady-350!";

// ─── Sensores ────────────────────────────
Adafruit_SHT31   sht31;
Adafruit_BMP085  bmp;
Adafruit_INA219 ina219(0x41);

WebServer server(80);

// ─── Altitud Embalse ─────────────────────
const float ALTITUD_M = 780.0;

void handleRoot() {
  float temp_sht = sht31.readTemperature();
  float hum      = sht31.readHumidity();
  float temp_bmp = bmp.readTemperature();
  float pres_abs = bmp.readPressure() / 100.0;
  float pres_qnh = bmp.readSealevelPressure(ALTITUD_M) / 100.0;
  float voltage  = ina219.getBusVoltage_V();
  float current  = ina219.getCurrent_mA();
  float power    = ina219.getPower_mW();

  String body = "=== Estacion Meteorologica ===\n\n";

  body += "[SHT31]\n";
  body += "  Temperatura : " + String(temp_sht, 2) + " C\n";
  body += "  Humedad     : " + String(hum, 2) + " %\n\n";

  body += "[BMP180]\n";
  body += "  Temperatura : " + String(temp_bmp, 2) + " C\n";
  body += "  Presion abs : " + String(pres_abs, 1) + " hPa\n";
  body += "  Presion QNH : " + String(pres_qnh, 1) + " hPa\n\n";

  body += "[INA219]\n";
  body += "  Voltaje     : " + String(voltage, 3) + " V\n";
  body += "  Corriente   : " + String(current, 1) + " mA\n";
  body += "  Potencia    : " + String(power, 1) + " mW\n\n";

  body += "==============================\n";

  server.send(200, "text/plain", body);
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Wire.begin(6, 5);

  // Sensores
  sht31.begin(0x44)  ? Serial.println("[OK] SHT31")   : Serial.println("[ERROR] SHT31");
  bmp.begin()        ? Serial.println("[OK] BMP180")  : Serial.println("[ERROR] BMP180");
  ina219.begin()     ? Serial.println("[OK] INA219")  : Serial.println("[ERROR] INA219");

  // WiFi
  Serial.printf("Conectando a %s", SSID);
  WiFi.begin(SSID, PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[OK] WiFi conectado");
  Serial.print("[OK] IP: ");
  Serial.println(WiFi.localIP());

  server.on("/", handleRoot);
  server.begin();
  Serial.println("[OK] HTTP server iniciado");
}

void loop() {
  server.handleClient();
}