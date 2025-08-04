#include <Wire.h>
#include "MAX30100_PulseOximeter.h"

#define REPORTING_PERIOD_MS 1000

PulseOximeter pox;
uint32_t tsLastReport = 0;

void onBeatDetected() {
  Serial.println("💓 Beat detected!");
}

void setup() {
  Serial.begin(115200);
  while (!Serial) { delay(10); }

  Wire.begin(4, 5); // SDA = GPIO21, SCL = GPIO22
  Serial.println("Initializing MAX30100...");

  if (!pox.begin()) {
    Serial.println("❌ MAX30100 not found. Check wiring and power.");
    while (1) { delay(10); }
  }
  Serial.println("✅ MAX30100 detected!");

  // Optional: adjust LED current (default is 50 mA)
  pox.setIRLedCurrent(MAX30100_LED_CURR_7_6MA);

  // Register heartbeat callback
  pox.setOnBeatDetectedCallback(onBeatDetected);
}

void loop() {
  pox.update();

  if (millis() - tsLastReport > REPORTING_PERIOD_MS) {
    float hr = pox.getHeartRate();     // bpm
    float spo2 = pox.getSpO2();        // %
    Serial.print("Heart Rate: ");
    Serial.print(hr);
    Serial.print(" bpm | SpO2: ");
    Serial.print(spo2);
    Serial.println(" %");
    tsLastReport = millis();
  }
}
