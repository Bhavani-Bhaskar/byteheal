#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <WiFi.h>
#include <WebServer.h>

// WiFi credentials - REPLACE WITH YOUR NETWORK INFO
const char* ssid = "OnePlus Nord CE3 5G";
const char* password = "0072512B";

// Create MPU6050 instance
Adafruit_MPU6050 mpu;

// Create WebServer instance on port 80
WebServer server(80);

// Thresholds (g, °/s, ms)
const float FREEFALL_G = 0.5;
const float GYRO_FALL_DEG_S = 100;
const float SEIZURE_RMS_THR = 80;
const unsigned long DEBOUNCE_MS = 100;
const unsigned long FLAG_MS = 5000;

const int SAMPLE_HZ = 100;
const unsigned long INT_MS = 1000 / SAMPLE_HZ;

// Buffers for gyro RMS
float gyroBuffer[100];
int bufIndex = 0;

// Status variables
unsigned long freefallStart = 0;
unsigned long flagRaisedAt = 0;
bool inFreefall = false;
int fallFlag = 0;
int seizureFlag = 0;
bool mpuConnected = false;

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  Serial.println("\nESP32 MPU6050 Sensor with Web Server");

  // Initialize I2C with custom pins and slower clock speed
  Wire.begin(22, 21);  // SDA=22, SCL=21
  Wire.setClock(100000);  // Reduced to 100kHz for reliability

  // Initialize MPU6050 with retry logic
  initializeMPU();

  // Connect to WiFi
  connectToWiFi();

  // Set up server routes
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.onNotFound(handleNotFound);

  // Enable CORS for all routes
  server.enableCORS(true);

  // Start server
  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  server.handleClient();  // Handle client requests

  // Only read sensor if MPU is connected
  if (mpuConnected) {
    if (!readSensorData()) {
      // If reading fails, attempt to reinitialize MPU
      delay(1000);
      initializeMPU();
    }
  }

  delay(INT_MS);
}

bool initializeMPU() {
  Serial.println("Initializing MPU6050...");
  mpuConnected = false;
  
  for (int i = 0; i < 5; i++) {  // Retry up to 5 times
    if (mpu.begin()) {
      mpu.setAccelerometerRange(MPU6050_RANGE_2_G);
      mpu.setGyroRange(MPU6050_RANGE_250_DEG);
      mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
      mpuConnected = true;
      Serial.println("MPU6050 initialized successfully");
      return true;
    }
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nFailed to initialize MPU6050");
  return false;
}

void connectToWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nFailed to connect to WiFi");
    // Continue anyway - AP might connect later
  }
}

bool readSensorData() {
  sensors_event_t accel, gyro;
  
  // Read accelerometer with error handling
  if (!mpu.getAccelerometerSensor()->getEvent(&accel)) {
    Serial.println("Failed to read accelerometer");
    return false;
  }

  // Read gyroscope with error handling
  if (!mpu.getGyroSensor()->getEvent(&gyro)) {
    Serial.println("Failed to read gyroscope");
    return false;
  }

  // Convert accel to g
  float ax = accel.acceleration.x / 9.80665;
  float ay = accel.acceleration.y / 9.80665;
  float az = accel.acceleration.z / 9.80665;
  float amag = sqrt(ax * ax + ay * ay + az * az);

  // Convert gyro to deg/s and compute magnitude
  float wx = gyro.gyro.x * 57.2958;  // rad/s → deg/s
  float wy = gyro.gyro.y * 57.2958;
  float wz = gyro.gyro.z * 57.2958;
  float wmag = sqrt(wx * wx + wy * wy + wz * wz);

  unsigned long now = millis();

  // --- Fall detection extended with gyro ---
  if (amag < FREEFALL_G) {
    if (!inFreefall) {
      inFreefall = true;
      freefallStart = now;
    } else if ((now - freefallStart >= DEBOUNCE_MS) &&
               (abs(wx) > GYRO_FALL_DEG_S || abs(wy) > GYRO_FALL_DEG_S || abs(wz) > GYRO_FALL_DEG_S) &&
               (fallFlag == 0)) {
      fallFlag = 1;
      flagRaisedAt = now;
    }
  } else {
    inFreefall = false;
  }

  if (fallFlag && now - flagRaisedAt >= FLAG_MS) {
    fallFlag = 0;
  }

  // --- Seizure detection via gyro RMS ---
  gyroBuffer[bufIndex++] = wmag;
  if (bufIndex >= SAMPLE_HZ) {
    bufIndex = 0;
    // Compute RMS over last 1s
    float sumSq = 0;
    for (int i = 0; i < SAMPLE_HZ; i++) {
      sumSq += gyroBuffer[i] * gyroBuffer[i];
    }
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

  return true;
}

void handleRoot() {
  String html = R"=====(
<!DOCTYPE html>
<html>
<head>
  <title>ESP32 MPU6050 Monitor</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: Arial, sans-serif; text-align: center; margin: 20px; }
    .flag { font-weight: bold; font-size: 1.5em; }
    .status { margin: 20px; padding: 10px; border-radius: 5px; }
    .normal { background-color: #d4edda; color: #155724; }
    .warning { background-color: #fff3cd; color: #856404; }
    .alert { background-color: #f8d7da; color: #721c24; }
  </style>
</head>
<body>
  <h1>ESP32 MPU6050 Status</h1>
  <div id="fallStatus" class="status normal">
    <h2>Fall Detection</h2>
    <p class="flag" id="fallFlag">0</p>
  </div>
  <div id="seizureStatus" class="status normal">
    <h2>Seizure Detection</h2>
    <p class="flag" id="seizureFlag">0</p>
  </div>
  <p>Last update: <span id="updateTime"></span></p>
  <p><a href="/data">View raw JSON data</a></p>
  <script>
    function updateData() {
      fetch('/data')
        .then(response => response.json())
        .then(data => {
          document.getElementById('fallFlag').textContent = data.fallFlag;
          document.getElementById('seizureFlag').textContent = data.seizureFlag;
          document.getElementById('updateTime').textContent = new Date().toLocaleString();
          
          // Update status colors
          updateStatus('fallStatus', data.fallFlag);
          updateStatus('seizureStatus', data.seizureFlag);
        })
        .catch(error => console.error('Error:', error));
    }
    
    function updateStatus(elementId, flagValue) {
      const element = document.getElementById(elementId);
      element.className = 'status ' + (flagValue ? 'alert' : 'normal');
    }
    
    // Update immediately and every second
    updateData();
    setInterval(updateData, 1000);
  </script>
</body>
</html>
)=====";

  server.send(200, "text/html", html);
}

void handleData() {
  // Create JSON response
  String json = "{";
  json += "\"fallFlag\":" + String(fallFlag) + ",";
  json += "\"seizureFlag\":" + String(seizureFlag);
  json += "}";
  
  // Send with CORS headers
  server.send(200, "application/json", json);
}

void handleNotFound() {
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  
  for (uint8_t i = 0; i < server.args(); i++) {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  
  server.send(404, "text/plain", message);
}
