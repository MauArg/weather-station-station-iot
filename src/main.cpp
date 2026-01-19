#include <Arduino.h>

#define LED_BUILTIN 8  // LED azul onboard ESP32-C3 SuperMini

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n=================================");
  Serial.println("ESP32-C3 SuperMini - Hello World!");
  Serial.println("=================================\n");
  
  pinMode(LED_BUILTIN, OUTPUT);
  
  Serial.println("✓ Board is working!");
  Serial.println("✓ USB connection OK");
  Serial.println("✓ Programming successful");
  Serial.println("\nStarting LED blink test...\n");
}

void loop() {
  digitalWrite(LED_BUILTIN, HIGH);
  Serial.println("LED ON");
  delay(500);
  
  digitalWrite(LED_BUILTIN, LOW);
  Serial.println("LED OFF");
  delay(1000);
}