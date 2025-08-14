#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>  // Add this with other includes

// Telegram config (add with other constants)
const String BOT_TOKEN = "8264191106:AAEGIsLmFY3ZfPjU_uakh0eqAHMrG7JL0p4";  // From @BotFather
const String CHAT_ID = "7778326919";      // Get from /getUpdates

// WiFi credentials
const char* ssid = "OnePlus Nord CE3 5G";
const char* password = "0072512B";

// MPU6050 and WebServer instances
Adafruit_MPU6050 mpu;
WebServer server(80);

// Thresholds (g, °/s)
const float FREEFALL_G = 0.5;          // Freefall detection threshold (0.5g)
const float GYRO_FALL_DEG_S = 100;     // Gyro threshold for fall confirmation
const float SEIZURE_RMS_THR = 80;      // Seizure detection threshold (RMS)
const unsigned long DEBOUNCE_MS = 100; // Debounce time for fall detection
const unsigned long SAMPLE_DELAY_MS = 100; // Sampling interval (100ms)
const unsigned long SEIZURE_ALERT_MS = 13000; // 7 seconds for severe alert

// Buffers and status variables
float gyroBuffer[100];
int bufIndex = 0;
bool inFreefall = false;
unsigned long freefallStart = 0;
unsigned long seizureStart = 0;
int fallFlag = 0;      // Manual reset only
int seizureFlag = 0;   // 1 = detected (yellow, auto-reset), 2 = severe (red, manual reset)
bool mpuConnected = false;
bool wasSeizureDetected = false; // Track if we're in a seizure condition

void setup() {
  Serial.begin(115200);
  Wire.begin(22, 21);  // SDA=22, SCL=21
  Wire.setClock(100000);
  
  initializeMPU();
  connectToWiFi();
  
  // Server routes
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/resetFall", []() { fallFlag = 0; server.send(200, "text/plain", "OK"); });
  server.on("/resetSeizure", []() { seizureFlag = 0; seizureStart = 0; wasSeizureDetected = false; server.send(200, "text/plain", "OK"); });
  server.onNotFound(handleNotFound);
  server.enableCORS(true);
  server.begin();
}

void loop() {
  server.handleClient();  // Handle client requests
  static int prevFallFlag = 0;
  static int prevSeizureFlag = 0;

// Trigger alerts on state changes
  if (fallFlag == 1 && prevFallFlag == 0) {
    sendTelegramAlert("🚨 FALL DETECTED! Check patient immediately!");
  }
  if (seizureFlag == 2 && prevSeizureFlag != 2) {
    sendTelegramAlert("⚠️ SEVERE SEIZURE (13s+ detected)! Emergency!");
  }

  prevFallFlag = fallFlag;
  prevSeizureFlag = seizureFlag;
  // Handle seizure flag state
  if (seizureFlag == 1) {
    // Check if seizure condition is still active
    if (!wasSeizureDetected) {
      // If seizure condition ended before 7 seconds, auto-reset
      seizureFlag = 0;
      seizureStart = 0;
    } 
    else if (millis() - seizureStart >= SEIZURE_ALERT_MS) {
      // If seizure continues for 7+ seconds, upgrade to type 2
      seizureFlag = 2;
    }
  }
  // Only read sensor if MPU is connected
  if (mpuConnected) {
    if (!readSensorData()) {
      // If reading fails, attempt to reinitialize MPU
      delay(1000);
      initializeMPU();
    }
  }
  delay(SAMPLE_DELAY_MS);
}

bool readSensorData() {
  sensors_event_t accel, gyro;
  if (!mpu.getAccelerometerSensor()->getEvent(&accel) || !mpu.getGyroSensor()->getEvent(&gyro)) 
    return false;

  // Fall detection (unchanged)
  float ax = accel.acceleration.x / 9.80665;
  float ay = accel.acceleration.y / 9.80665;
  float az = accel.acceleration.z / 9.80665;
  float amag = sqrt(ax*ax + ay*ay + az*az);

  if (amag < FREEFALL_G) {
    if (!inFreefall) {
      inFreefall = true;
      freefallStart = millis();
    } 
    else if ((millis() - freefallStart >= DEBOUNCE_MS) && 
             (abs(gyro.gyro.x*57.2958) > GYRO_FALL_DEG_S || 
              abs(gyro.gyro.y*57.2958) > GYRO_FALL_DEG_S || 
              abs(gyro.gyro.z*57.2958) > GYRO_FALL_DEG_S)) {
      fallFlag = 1;
    }
  } else {
    inFreefall = false;
  }

  // Seizure detection
  float wmag = sqrt(pow(gyro.gyro.x*57.2958, 2) + pow(gyro.gyro.y*57.2958, 2) + pow(gyro.gyro.z*57.2958, 2));
  gyroBuffer[bufIndex++] = wmag;
  
  if (bufIndex >= 100) {
    bufIndex = 0;
    float sumSq = 0;
    for (int i=0; i<100; i++) sumSq += gyroBuffer[i] * gyroBuffer[i];
    float rms = sqrt(sumSq/100);
    
    wasSeizureDetected = (rms > SEIZURE_RMS_THR);
    
    if (wasSeizureDetected) {
      if (seizureFlag == 0) {
        // New seizure detected
        seizureFlag = 1;
        seizureStart = millis();
      }
    } else if (seizureFlag == 1) {
      // Seizure condition ended before reaching 7 seconds
      // Let the loop() handle the auto-reset
    }
  }

  Serial.printf("Fall: %d | Seizure: %d\n", fallFlag, seizureFlag);
  return true;
}

void handleRoot() {
  server.send(200, "text/html", 
  R"=====(
  <!DOCTYPE html>
  <html>
  <head>
    <title>ESP32 Fall/Seizure Detector</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
      body { font-family: Arial, sans-serif; text-align: center; margin: 20px; }
      .flag { font-size: 2em; font-weight: bold; margin: 10px; }
      .status { padding: 15px; border-radius: 10px; margin: 20px; }
      .normal { background-color: #d4edda; color: #155724; }
      .warning { background-color: #fff3cd; color: #856404; }
      .alert { background-color: #f8d7da; color: #721c24; }
      button { 
        background-color: #dc3545; color: white; border: none; 
        padding: 10px 20px; border-radius: 5px; cursor: pointer;
        margin-top: 10px; font-size: 1em;
      }
    </style>
  </head>
  <body>
    <h1>ESP32 Health Monitor</h1>
    
    <div id="fallStatus" class="status normal">
      <h2>Fall Detection</h2>
      <p class="flag" id="fallFlag">0</p>
      <button id="resetFallButton" style="display:none;">Reset Fall Alert</button>
    </div>
    
    <div id="seizureStatus" class="status normal">
      <h2>Seizure Detection</h2>
      <p class="flag" id="seizureFlag">0</p>
      <button id="resetSeizureButton" style="display:none;">Reset Severe Seizure</button>
    </div>
    
    <script>
      function getStatusClass(flagValue) {
        if (flagValue == 2) return 'alert';
        if (flagValue == 1) return 'warning';
        return 'normal';
      }
      
      function updateData() {
        fetch('/data')
          .then(r => r.json())
          .then(data => {
            document.getElementById('fallFlag').textContent = data.fallFlag;
            document.getElementById('seizureFlag').textContent = data.seizureFlag;
            
            // Update UI
            document.getElementById('fallStatus').className = 
              `status ${data.fallFlag ? 'alert' : 'normal'}`;
            document.getElementById('seizureStatus').className = 
              `status ${getStatusClass(data.seizureFlag)}`;
              
            // Show/hide buttons
            document.getElementById('resetFallButton').style.display = 
              data.fallFlag ? 'inline-block' : 'none';
            document.getElementById('resetSeizureButton').style.display = 
              data.seizureFlag == 2 ? 'inline-block' : 'none';
          });
      }
      
      // Button handlers
      document.getElementById('resetFallButton').addEventListener('click', () => {
        fetch('/resetFall').then(updateData);
      });
      document.getElementById('resetSeizureButton').addEventListener('click', () => {
        fetch('/resetSeizure').then(updateData);
      });
      
      // Update every second
      setInterval(updateData, 1000);
      updateData();
    </script>
  </body>
  </html>
  )=====");
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

void sendTelegramAlert(String message) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi not connected - Telegram alert skipped");
      return;
    }
  
    HTTPClient http;
    String url = "https://api.telegram.org/bot" + BOT_TOKEN + "/sendMessage";
    String payload = "chat_id=" + CHAT_ID + "&text=" + message + "&disable_notification=false";
  
    http.begin(url);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    
    int httpCode = http.POST(payload);
    if (httpCode == HTTP_CODE_OK) {
      Serial.println("Telegram alert sent");
    } else {
      Serial.printf("Telegram failed: %d\n", httpCode);
    }
    http.end();
  }

void handleData() {
  String json = String("{\"fallFlag\":") + fallFlag + ",\"seizureFlag\":" + seizureFlag + "}";
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
