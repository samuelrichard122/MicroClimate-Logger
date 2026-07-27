//This file is going to be converted to binary, bare-metal instructions, and stored on the Raspberry PI. It will execute it when powered on.
// The #includes will also be converted to bare-metal, the chip doesn't have C++ on it. 
#include <SPI.h> //SPI shared bus object
#include <SD.h> //File system logic object
#include <DHT.h> //timing logic for DHT22 object

// Telemetry Libraries ---
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <HTTPClient.h> // library for HTTP web requests
#include "secrets.h" //wifi data

//Next, we want to map the Raspberry PI pins to our other components.
// --- DHT22 Sensor Configuration ---
#define DHTPIN 15 
#define DHTTYPE DHT22 
DHT dht(DHTPIN, DHTTYPE); 

// --- MicroSD SPI1 Configuration ---
const int chipSelect = 13; 

// --- Wi-Fi & MQTT Configuration ---
const char* ssid = SECRET_SSID;
const char* password = SECRET_PASSWORD;
const char* mqtt_server = SECRET_MQTT_SERVER;

WiFiClient espClient;
PubSubClient client(espClient);

// --- Network & Time Functions ---
void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Connecting to Wi-Fi: ");
  Serial.println(ssid);

  // Ensure Wi-Fi starts in station mode before connecting
  WiFi.mode(WIFI_STA); 
  WiFi.begin(ssid, password);

  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 20) {
    delay(500);
    Serial.print(".");
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("");
    Serial.println("WiFi connected successfully!");
    Serial.print("Pico IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWi-Fi Connection Failed!");
  }
}

// Fetch time via HTTP GET to bypass UDP port blocks
unsigned long get_http_time_ms() {
  if (WiFi.status() != WL_CONNECTED) {
    return 0; // No Wi-Fi, return 0 to trigger Telegraf fallback
  }

  Serial.println("Fetching time from WorldTimeAPI...");
  HTTPClient http;
  
  // Initialize connection to public time API
  http.begin(espClient, "http://worldtimeapi.org/api/timezone/Etc/UTC");
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    
    // Parse "unixtime":1721000000 from the JSON response
    int timeIdx = payload.indexOf("\"unixtime\":");
    if (timeIdx != -1) {
      int start = timeIdx + 11;
      int end = payload.indexOf(',', start);
      String epochStr = payload.substring(start, end);
      unsigned long epochSeconds = strtoul(epochStr.c_str(), NULL, 10);
      
      http.end();
      Serial.println("HTTP Time Sync Success!");
      return epochSeconds * 1000ULL;
    }
  }
  
  Serial.println("HTTP Time Sync Failed. Defaulting to system time.");
  http.end();
  return 0; 
}

bool reconnect() {
  int attempts = 0;
  while (!client.connected() && attempts < 3) {
    Serial.print("Attempting MQTT connection to laptop (Attempt ");
    Serial.print(attempts + 1);
    Serial.println(")...");
    
    if (client.connect("PicoMicroclimateNode")) {
      Serial.println("Connected to Mosquitto Broker!");
      return true;
    } else {
      Serial.print("Failed, state code = ");
      Serial.print(client.state());
      Serial.println(". Retrying in 2 seconds...");
      delay(2000);
      attempts++;
    }
  }
  return client.connected();
}

void flush_sd_cache() {
  if (!SD.exists("data.csv")) {
    return; 
  }

  File logFile = SD.open("data.csv", FILE_READ);
  if (!logFile) {
    Serial.println("Failed to open data.csv for flushing.");
    return;
  }

  if (logFile.size() <= 45) {
    logFile.close();
    return; 
  }

  Serial.println("Offline data detected! Flushing cache to MQTT...");

  while (logFile.available()) {
    String line = logFile.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    
    if (line.startsWith("Timestamp")) continue;

    int firstComma = line.indexOf(',');
    int secondComma = line.indexOf(',', firstComma + 1);

    if (firstComma != -1 && secondComma != -1) {
      String msStr = line.substring(0, firstComma);
      String tempStr = line.substring(firstComma + 1, secondComma);
      String humStr = line.substring(secondComma + 1);

      String payload = "{\"device\":\"node_01\",\"temperature\":" + tempStr + ",\"humidity\":" + humStr;
      
      if (msStr != "0") {
        payload += ",\"timestamp_ms\":" + msStr;
      }
      payload += "}";

      if (client.publish("env/microclimate", payload.c_str())) {
        Serial.println("Flushed offline record: " + payload);
        delay(50); 
      }
    }
  }

  logFile.close();
  SD.remove("data.csv");
  
  File newFile = SD.open("data.csv", FILE_WRITE);
  if (newFile) {
    newFile.println("Timestamp(ms),Temperature(C),Humidity(%)");
    newFile.close();
  }
  
  Serial.println("SD card cache successfully flushed and cleared.");
}

void setup() {
  Serial.begin(115200); 
  delay(3000); 
  Serial.println("Initializing Microclimate Logger..."); 

  dht.begin(); 

  SPI1.setRX(12);
  SPI1.setTX(11);  
  SPI1.setSCK(10);  

  if (!SD.begin(chipSelect, SPI1)) { 
    Serial.println("CRITICAL ERROR: SD Card initialization failed!");
    return;
  }
  
  File dataFile = SD.open("data.csv", FILE_WRITE);
  if (dataFile) {
    if (dataFile.size() == 0) {
      dataFile.println("Timestamp(ms),Temperature(C),Humidity(%)");
    }
    dataFile.close();
  }
  
  client.setServer(mqtt_server, 1883);
}

void loop() {
  // 1. Take the sensor readings first
  float h = dht.readHumidity();
  float t = dht.readTemperature();

  if (isnan(h) || isnan(t)) {
    Serial.println("Failed to read from DHT sensor! Check GP15 wiring.");
    delay(5000); 
    return;
  }

  // 2. Wake up Wi-Fi FIRST so we can query the internet for the time
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Waking up Wi-Fi and reconnecting...");
    setup_wifi();
  }

  // 3. Get the Unix timestamp via HTTP request
  unsigned long currentTimestamp = get_http_time_ms();

  // --- Package data into JSON payload for MQTT ---
  StaticJsonDocument<200> doc;
  doc["device"] = "node_01";
  doc["temperature"] = t;
  doc["humidity"] = h;
  
  if (currentTimestamp > 0) {
    doc["timestamp_ms"] = currentTimestamp;
  }
  
  char jsonBuffer[512];
  serializeJson(doc, jsonBuffer);

  // 4. Attempt MQTT connection
  if (WiFi.status() == WL_CONNECTED) {
    if (!client.connected()) {
      reconnect();
    }
    if (client.connected()) {
      client.loop(); 
      flush_sd_cache();
    }
  }

  // --- Publish to Server OR Failover to SD Card ---
  if (client.connected()) {
    client.publish("env/microclimate", jsonBuffer);
    Serial.println("SUCCESS: Payload published to MQTT: " + String(jsonBuffer));
  } else {
    Serial.println("NETWORK DOWN: Falling back to local SD card logging...");
    String dataString = String(currentTimestamp) + "," + String(t) + "," + String(h);

    File dataFile = SD.open("data.csv", FILE_WRITE);
    if (dataFile) {
      dataFile.println(dataString);
      dataFile.close();
      Serial.println("Logged successfully to SD: " + dataString);
    } else {
      Serial.println("CRITICAL ERROR: Failed to open data.csv on SD card");
    }
  }

  // 5. Force disconnect and sleep
  if (client.connected()) {
    client.disconnect();
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.println("Radio powered down. Entering low-power sleep for 5 minutes...");

  delay(300000); 
}