#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>

Adafruit_MPU6050 mpu;

// Thresholds and timing (milliseconds)
const float FREEFALL_THRESH_G   = 0.75;    // sum-vector threshold in g
const unsigned long DEBOUNCE_MS = 100;     // min free-fall duration
const unsigned long FLAG_MS     = 5000;    // flag high duration

// Sampling rate
const int SAMPLE_RATE_HZ        = 100;
const unsigned long SAMPLE_MS   = 1000 / SAMPLE_RATE_HZ;

unsigned long freefallStart = 0;
unsigned long flagRaisedAt  = 0;
bool inFreefall             = false;
int  fallFlag               = 0;

void setup() {
  Serial.begin(115200);
  Wire.begin();

  if (!mpu.begin()) {
    Serial.println("Failed to find MPU6050 chip");
    while (1) { delay(10); }
  }

  // Configure accelerometer range to ±2 g
  mpu.setAccelerometerRange(MPU6050_RANGE_2_G);
  // Use default filter and sample rate
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
}

void loop() {
  // 1. Read accelerometer event
  sensors_event_t accel;
  mpu.getAccelerometerSensor()->getEvent(&accel);

  // 2. Ax, Ay, Az in m/s² → convert to g
  float ax_g = accel.acceleration.x / 9.80665;
  float ay_g = accel.acceleration.y / 9.80665;
  float az_g = accel.acceleration.z / 9.80665;

  // 3. Compute sum-vector magnitude (g)
  float amag = sqrt(ax_g*ax_g + ay_g*ay_g + az_g*az_g);

  unsigned long now = millis();

  // 4. Free-fall detection debounce
  if (amag < FREEFALL_THRESH_G) {
    if (!inFreefall) {
      inFreefall = true;
      freefallStart = now;
    } else if ((now - freefallStart >= DEBOUNCE_MS) && (fallFlag == 0)) {
      // 5. Raise flag
      fallFlag = 1;
      flagRaisedAt = now;
    }
  } else {
    // Exit free-fall state if above threshold
    inFreefall = false;
  }

  // 6. Lower flag after FLAG_MS
  if ((fallFlag == 1) && (now - flagRaisedAt >= FLAG_MS)) {
    fallFlag = 0;
  }

  // 7. Debug output
  Serial.print("Flag=");
  Serial.print(fallFlag);
  Serial.print(" | A_g=");
  Serial.print(amag, 2);
  Serial.print("  (");
  Serial.print(ax_g, 2); Serial.print(", ");
  Serial.print(ay_g, 2); Serial.print(", ");
  Serial.print(az_g, 2); Serial.print(") g");
  Serial.println();

  delay(SAMPLE_MS);
}
