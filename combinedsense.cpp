#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <base64.h>
#include "MAX30100_PulseOximeter.h"
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>

// WiFi Configuration - REPLACE WITH YOUR CREDENTIALS
const char* ssid = "OnePlus Nord CE3 5G";
const char* password = "0072512B";

// Twilio Configuration - REPLACE WITH YOUR CREDENTIALS
const char* account_sid = "AC93541a9ee007412ac13158a925e5ab26";
const char* auth_token = "a06744b289ca173942666f2d6e65696f";
const char* twilio_phone_number = "+12566634185"; // +1234567890
const char* personal_phone_number = "+919661666199"; // +1234567890

// I2C Configuration for multiple sensors
TwoWire I2C_1 = TwoWire(0); // For MPU6050 (default I2C)
TwoWire I2C_2 = TwoWire(1); // For MAX30100 (custom I2C)

// Sensor objects
PulseOximeter pox;
Adafruit_MPU6050 mpu;

// Medical thresholds for alerts
struct MedicalThresholds {
  // Heart rate thresholds (BPM)
  float hr_sepsis_high = 100.0;
  float hr_sepsis_low = 60.0;
  float hr_seizure_high = 120.0;
  float hr_critical_high = 140.0;
  float hr_critical_low = 50.0;
  
  // SpO2 thresholds (%)
  float spo2_sepsis_low = 95.0;
  float spo2_seizure_low = 90.0;
  float spo2_critical_low = 85.0;
  
  // Motion thresholds
  float accel_seizure_threshold = 3.0;  // g
  float gyro_seizure_threshold = 50.0;  // deg/s
  float accel_critical_threshold = 5.0;  // g
  float gyro_critical_threshold = 100.0; // deg/s
  
  // Duration thresholds (milliseconds)
  unsigned long motion_duration_threshold = 10000; // 10 seconds
  unsigned long critical_motion_duration = 30000;  // 30 seconds
  
  // Alert cooldown periods (milliseconds)
  unsigned long sepsis_cooldown = 300000;  // 5 minutes
  unsigned long seizure_cooldown = 180000; // 3 minutes
  unsigned long critical_cooldown = 60000; // 1 minute
};

MedicalThresholds thresholds;

// Global variables for monitoring
float current_hr = 0;
float current_spo2 = 0;
float accel_magnitude = 0;
float gyro_magnitude = 0;
bool motion_detected = false;
unsigned long motion_start_time = 0;
unsigned long last_sepsis_alert = 0;
unsigned long last_seizure_alert = 0;
unsigned long last_critical_alert = 0;

// Status flags
bool sepsis_detected = false;
bool seizure_detected = false;
bool critical_condition = false;

void setup() {
  Serial.begin(115200);
  Serial.println("ESP32 Medical Alert System Starting...");
  
  // Initialize I2C buses
  I2C_1.begin(21, 22, 100000); // SDA=21, SCL=22 for MPU6050
  I2C_2.begin(4, 5, 100000);   // SDA=4, SCL=5 for MAX30100
  
  // Initialize WiFi
  initializeWiFi();
  
  // Initialize MAX30100
  initializeMAX30100();
  
  // Initialize MPU6050
  initializeMPU6050();
  
  Serial.println("System initialized successfully!");
  Serial.println("Monitoring for medical conditions...");
  
  // Send startup notification
  sendSMS("Medical Alert System Online", "SYSTEM");
}

void loop() {
  // Update sensor readings
  updateSensorReadings();
  
  // Check for medical conditions
  checkMedicalConditions();
  
  // Print status every 5 seconds
  static unsigned long last_print = 0;
  if (millis() - last_print > 5000) {
    printStatus();
    last_print = millis();
  }
  
  delay(100); // Small delay for stability
}

void initializeWiFi() {
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println();
  Serial.print("WiFi connected! IP address: ");
  Serial.println(WiFi.localIP());
}

void initializeMAX30100() {
  Serial.print("Initializing MAX30100...");
  
  if (!pox.begin()) {
    Serial.println("FAILED!");
    Serial.println("Check MAX30100 wiring and restart ESP32");
    while(1);
  }
  
  Serial.println("SUCCESS");
  
  // Configure MAX30100
  pox.setIRLedCurrent(MAX30100_LED_CURR_7_6MA);
  pox.setOnBeatDetectedCallback(onBeatDetected);
}

void initializeMPU6050() {
  Serial.print("Initializing MPU6050...");
  
  if (!mpu.begin(0x68, &I2C_1)) {
    Serial.println("FAILED!");
    Serial.println("Check MPU6050 wiring and restart ESP32");
    while(1);
  }
  
  Serial.println("SUCCESS");
  
  // Configure MPU6050
  mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
  mpu.setGyroRange(MPU6050_RANGE_500_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
}

void updateSensorReadings() {
  // Update MAX30100 readings
  pox.update();
  current_hr = pox.getHeartRate();
  current_spo2 = pox.getSpO2();
  
  // Update MPU6050 readings
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);
  
  // Calculate motion magnitudes
  accel_magnitude = sqrt(a.acceleration.x * a.acceleration.x + 
                        a.acceleration.y * a.acceleration.y + 
                        a.acceleration.z * a.acceleration.z);
  
  gyro_magnitude = sqrt(g.gyro.x * g.gyro.x + 
                       g.gyro.y * g.gyro.y + 
                       g.gyro.z * g.gyro.z) * 180.0 / PI; // Convert to deg/s
  
  // Check for significant motion
  if (accel_magnitude > thresholds.accel_seizure_threshold || 
      gyro_magnitude > thresholds.gyro_seizure_threshold) {
    if (!motion_detected) {
      motion_detected = true;
      motion_start_time = millis();
    }
  } else {
    motion_detected = false;
    motion_start_time = 0;
  }
}

void checkMedicalConditions() {
  unsigned long current_time = millis();
  bool send_alert = false;
  String alert_message = "";
  String alert_type = "";
  
  // Reset condition flags
  sepsis_detected = false;
  seizure_detected = false;
  critical_condition = false;
  
  // Check for CRITICAL conditions first
  if ((current_hr > thresholds.hr_critical_high || current_hr < thresholds.hr_critical_low) ||
      (current_spo2 < thresholds.spo2_critical_low) ||
      (accel_magnitude > thresholds.accel_critical_threshold) ||
      (gyro_magnitude > thresholds.gyro_critical_threshold) ||
      (motion_detected && (current_time - motion_start_time) > thresholds.critical_motion_duration)) {
    
    critical_condition = true;
    
    if (current_time - last_critical_alert > thresholds.critical_cooldown) {
      alert_message = "CRITICAL ALERT! ";
      if (current_hr > thresholds.hr_critical_high) alert_message += "Extreme Tachycardia ";
      if (current_hr < thresholds.hr_critical_low) alert_message += "Extreme Bradycardia ";
      if (current_spo2 < thresholds.spo2_critical_low) alert_message += "Severe Hypoxemia ";
      if (accel_magnitude > thresholds.accel_critical_threshold) alert_message += "Violent Motion ";
      if (motion_detected && (current_time - motion_start_time) > thresholds.critical_motion_duration) {
        alert_message += "Prolonged Seizure Activity ";
      }
      alert_message += "HR:" + String(current_hr) + " SpO2:" + String(current_spo2);
      alert_type = "CRITICAL";
      send_alert = true;
      last_critical_alert = current_time;
    }
  }
  
  // Check for SEIZURE conditions
  else if ((current_hr > thresholds.hr_seizure_high) ||
           (current_spo2 < thresholds.spo2_seizure_low) ||
           (motion_detected && (current_time - motion_start_time) > thresholds.motion_duration_threshold)) {
    
    seizure_detected = true;
    
    if (current_time - last_seizure_alert > thresholds.seizure_cooldown) {
      alert_message = "SEIZURE ALERT! ";
      if (current_hr > thresholds.hr_seizure_high) alert_message += "High HR ";
      if (current_spo2 < thresholds.spo2_seizure_low) alert_message += "Low SpO2 ";
      if (motion_detected) alert_message += "Prolonged Motion ";
      alert_message += "HR:" + String(current_hr) + " SpO2:" + String(current_spo2);
      alert_type = "SEIZURE";
      send_alert = true;
      last_seizure_alert = current_time;
    }
  }
  
  // Check for SEPSIS conditions
  else if ((current_hr > thresholds.hr_sepsis_high || current_hr < thresholds.hr_sepsis_low) ||
           (current_spo2 < thresholds.spo2_sepsis_low)) {
    
    sepsis_detected = true;
    
    if (current_time - last_sepsis_alert > thresholds.sepsis_cooldown) {
      alert_message = "SEPSIS ALERT! ";
      if (current_hr > thresholds.hr_sepsis_high) alert_message += "Tachycardia ";
      if (current_hr < thresholds.hr_sepsis_low) alert_message += "Bradycardia ";
      if (current_spo2 < thresholds.spo2_sepsis_low) alert_message += "Hypoxemia ";
      alert_message += "HR:" + String(current_hr) + " SpO2:" + String(current_spo2);
      alert_type = "SEPSIS";
      send_alert = true;
      last_sepsis_alert = current_time;
    }
  }
  
  // Send SMS alert if conditions are met
  if (send_alert) {
    Serial.println("MEDICAL ALERT TRIGGERED!");
    Serial.println("Alert Type: " + alert_type);
    Serial.println("Message: " + alert_message);
    sendSMS(alert_message, alert_type);
  }
}

void sendSMS(String message, String alert_type) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, cannot send SMS");
    return;
  }
  
  HTTPClient http;
  http.begin("https://api.twilio.com/2010-04-01/Accounts/" + String(account_sid) + "/Messages.json");
  
  // Set headers
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  
  // Create basic auth header
  String auth = String(account_sid) + ":" + String(auth_token);
  String authEncoded = base64::encode(auth);
  http.addHeader("Authorization", "Basic " + authEncoded);
  
  // Create POST data
  String postData = "To=" + String(personal_phone_number) + 
                   "&From=" + String(twilio_phone_number) + 
                   "&Body=" + message;
  
  Serial.println("Sending SMS...");
  int httpResponseCode = http.POST(postData);
  
  if (httpResponseCode > 0) {
    String response = http.getString();
    Serial.println("SMS Response Code: " + String(httpResponseCode));
    if (httpResponseCode == 201) {
      Serial.println("SMS sent successfully!");
    } else {
      Serial.println("SMS Error Response: " + response);
    }
  } else {
    Serial.println("SMS Error Code: " + String(httpResponseCode));
  }
  
  http.end();
}

void onBeatDetected() {
  // This callback is triggered when MAX30100 detects a heartbeat
  // Can be used for additional processing if needed
}

void printStatus() {
  Serial.println("=== MEDICAL MONITORING STATUS ===");
  Serial.println("Heart Rate: " + String(current_hr) + " BPM");
  Serial.println("SpO2: " + String(current_spo2) + " %");
  Serial.println("Accel Magnitude: " + String(accel_magnitude) + " g");
  Serial.println("Gyro Magnitude: " + String(gyro_magnitude) + " deg/s");
  Serial.println("Motion Detected: " + String(motion_detected ? "YES" : "NO"));
  
  if (motion_detected) {
    unsigned long motion_duration = (millis() - motion_start_time) / 1000;
    Serial.println("Motion Duration: " + String(motion_duration) + " seconds");
  }
  
  // Status indicators
  Serial.print("Status: ");
  if (critical_condition) {
    Serial.println("CRITICAL CONDITION");
  } else if (seizure_detected) {
    Serial.println("SEIZURE DETECTED");
  } else if (sepsis_detected) {
    Serial.println("SEPSIS INDICATORS");
  } else {
    Serial.println("NORMAL");
  }
  
  Serial.println("WiFi: " + String(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected"));
  Serial.println("================================");
}
