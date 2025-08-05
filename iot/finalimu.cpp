#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>

// Create MPU6050 instance
Adafruit_MPU6050 mpu;

// Thresholds (g, °/s, ms)
const float  FREEFALL_G       = 0.5;
const float  GYRO_FALL_DEG_S  = 100;
const float  SEIZURE_RMS_THR  = 80;
const unsigned long DEBOUNCE_MS = 100;
const unsigned long FLAG_MS     = 5000;

const int SAMPLE_HZ      = 100;
const unsigned long INT_MS = 1000 / SAMPLE_HZ;

// Buffers for gyro RMS
float gyroBuffer[100];
int   bufIndex = 0;

unsigned long freefallStart = 0;
unsigned long flagRaisedAt  = 0;
bool inFreefall             = false;
int  fallFlag               = 0;
int  seizureFlag            = 0;

void setup() {
  Serial.begin(115200);

  // Initialize I2C with custom pins SDA = 22, SCL = 21
  Wire.begin(22, 21);
  
  if (!mpu.begin()) {
    Serial.println("Failed to find MPU6050 chip");
    while (1) { delay(10); }
  }

  mpu.setAccelerometerRange(MPU6050_RANGE_2_G);
  mpu.setGyroRange(MPU6050_RANGE_250_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
}

void loop() {
  sensors_event_t accel, gyro;
  mpu.getAccelerometerSensor()->getEvent(&accel);
  mpu.getGyroSensor()->getEvent(&gyro);

  // Convert accel to g
  float ax = accel.acceleration.x / 9.80665;
  float ay = accel.acceleration.y / 9.80665;
  float az = accel.acceleration.z / 9.80665;
  float amag = sqrt(ax*ax + ay*ay + az*az);

  // Convert gyro to deg/s and compute magnitude
  float wx = gyro.gyro.x * 57.2958; // rad/s → deg/s
  float wy = gyro.gyro.y * 57.2958;
  float wz = gyro.gyro.z * 57.2958;
  float wmag = sqrt(wx*wx + wy*wy + wz*wz);

  unsigned long now = millis();

  // --- Fall detection extended with gyro ---
  if (amag < FREEFALL_G) {
    if (!inFreefall) {
      inFreefall = true;
      freefallStart = now;
    } else if ((now - freefallStart >= DEBOUNCE_MS)
               && (abs(wx) > GYRO_FALL_DEG_S || abs(wy) > GYRO_FALL_DEG_S || abs(wz) > GYRO_FALL_DEG_S)
               && fallFlag == 0) {
      fallFlag = 1;
      flagRaisedAt = now;
    }
  } else {
    inFreefall = false;
  }

  if (fallFlag && now - flagRaisedAt >= FLAG_MS) fallFlag = 0;

  // --- Seizure detection via gyro RMS ---
  gyroBuffer[bufIndex++] = wmag;
  if (bufIndex >= SAMPLE_HZ) {
    bufIndex = 0;
    // Compute RMS over last 1s
    float sumSq = 0;
    for (int i = 0; i < SAMPLE_HZ; i++) sumSq += gyroBuffer[i] * gyroBuffer[i];
    float rms = sqrt(sumSq / SAMPLE_HZ);
    seizureFlag = (rms > SEIZURE_RMS_THR) ? 1 : 0;
  }

  // Debug output
  Serial.print("FallFlag=");
  Serial.print(fallFlag);
  Serial.print(" SeizureFlag=");
  Serial.print(seizureFlag);
  Serial.print(" A_g=");
  Serial.print(amag, 2);
  Serial.print(" W_deg/s=");
  Serial.print(wmag, 1);
  Serial.println();

  delay(INT_MS);
}
