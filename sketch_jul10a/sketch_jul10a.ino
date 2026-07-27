//This file is going to be converted to binary, bare-metal instructions, and stored on the Raspberry PI. It will execute it when powered on.
// The #includes will also be converted to bare-metal, the chip doesn't have C++ on it. 
#include <SPI.h> //SPI shared bus object
#include <SD.h> //File system logic object
#include <DHT.h> //timing logic for DHT22 object
#include <time.h> // NEW: Time library for NTP sync

// Telemetry Libraries ---
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "secrets.h" //wifi data

//Next, we want to map the Raspberry PI pins to our other components.
// --- DHT22 Sensor Configuration ---
#define DHTPIN 15 //The pin GP15 (physical pin is 20. Since not all physical pins are used by the computer, such as ground, the numbering isn't the same )
#define DHTTYPE DHT22 
DHT dht(DHTPIN, DHTTYPE); //We instantiate our DHT object for later

// --- MicroSD SPI1 Configuration ---
const int chipSelect = 13; //We're saying that (in the SPI1 neighbourhood), we want to select the chip who is currently connected to GP13. As many chips can be connected in SPI1). 

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

// NEW: Sync time with NTP server
void setup_time() {
  Serial.println("Syncing time with NTP server...");
  // Configures NTP for UTC time (0 offset, 0 daylight savings offset)
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  // Wait up to 10 seconds for valid NTP time (year > 2020)
  time_t now = time(nullptr);
  int retries = 0;
  while (now < 1600000000 && retries < 20) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
    retries++;
  }

  if (now > 1600000000) {
    Serial.println("\nTime synchronized successfully!");
  } else {
    Serial.println("\nNTP sync timed out. Defaulting to system time.");
  }
}

// FIXED: Helper to get true Unix timestamp in milliseconds
unsigned long get_unix_time_ms() {
  time_t now = time(nullptr);
  // 1600000000 is September 2020. If now is less than this, NTP hasn't synced yet.
  if (now < 1600000000) {
    return 0; // Return 0 as a fallback signal
  }
  return (unsigned long)now * 1000ULL;
}

// Fixed: Reconnect with a retry limit to prevent locking up the Pico if Wi-Fi drops
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

// FIXED: Flush cached offline SD card readings to MQTT safely
void flush_sd_cache() {
  if (!SD.exists("data.csv")) {
    return; // No offline data to flush
  }

  File logFile = SD.open("data.csv", FILE_READ);
  if (!logFile) {
    Serial.println("Failed to open data.csv for flushing.");
    return;
  }

  // If the file only contains the header row (approx. 40 bytes) or is empty, skip flushing
  if (logFile.size() <= 45) {
    logFile.close();
    return; 
  }

  Serial.println("Offline data detected! Flushing cache to MQTT...");

  while (logFile.available()) {
    String line = logFile.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    
    // Skip the CSV header row to prevent sending bad data types to InfluxDB
    if (line.startsWith("Timestamp")) continue;

    // Parse CSV row: timestamp_ms, temperature, humidity
    int firstComma = line.indexOf(',');
    int secondComma = line.indexOf(',', firstComma + 1);

    if (firstComma != -1 && secondComma != -1) {
      String msStr = line.substring(0, firstComma);
      String tempStr = line.substring(firstComma + 1, secondComma);
      String humStr = line.substring(secondComma + 1);

      // Reconstruct MQTT JSON payload, omitting timestamp if it was 0
      String payload = "{\"device\":\"node_01\",\"temperature\":" + tempStr + ",\"humidity\":" + humStr;
      
      if (msStr != "0") {
        payload += ",\"timestamp_ms\":" + msStr;
      }
      payload += "}";

      // Publish cached record to broker
      if (client.publish("env/microclimate", payload.c_str())) {
        Serial.println("Flushed offline record: " + payload);
        delay(50); // Short delay to prevent overwhelming Mosquitto socket
      }
    }
  }

  logFile.close();

  // Wipe the file after successful transfer
  SD.remove("data.csv");
  
  // Re-create the file and write the header row immediately so it's ready for next time
  File newFile = SD.open("data.csv", FILE_WRITE);
  if (newFile) {
    newFile.println("Timestamp(ms),Temperature(C),Humidity(%)");
    newFile.close();
  }
  
  Serial.println("SD card cache successfully flushed and cleared.");
}

void setup() {
  Serial.begin(115200); //opens up the communication pipeline between the Pico and my laptop's Serial Monitor so I can view diagnostic messages and debug errors in real-time. I get 115200 bits per second
  delay(3000); //pauses code execution for 3 seconds. To give the serial Monitor a second to send stuff to our monitor and let us known if anything's wrong.
  Serial.println("Initializing Microclimate Logger..."); //Send a message to my PC

  dht.begin(); //Initailizes our DHT sensor, using the DHTPIN we set earlier.

  // Route SPI1 to the specific hardware pins we wired
  SPI1.setRX(12);// Sets lane GP12 to the shared bus RX. Note: If we tried to instead do SPI1.setTX(12), it wouldnt work (as physically, the pins just arent connected to that) and an error would ensue. 
  SPI1.setTX(11);  // Sets lane GP12 to the shared bus TX  
  SPI1.setSCK(10);  // Sets lane GP12 to the shared bus SCK

  // Initialize the SD card on SPI1 pin chipSelect and check for errors
  if (!SD.begin(chipSelect, SPI1)) { 
    Serial.println("CRITICAL ERROR: SD Card initialization failed!");
    return;
  }
  
  // Create or open the CSV file and write the header row
  File dataFile = SD.open("data.csv", FILE_WRITE);
  if (dataFile) {
    if (dataFile.size() == 0) {
      dataFile.println("Timestamp(ms),Temperature(C),Humidity(%)");
    }
    dataFile.close();
  }

  // --- Initialize Network & Time ---
  setup_wifi();
  
  // Only attempt to sync time if the network successfully connected
  if (WiFi.status() == WL_CONNECTED) {
    setup_time();
  }
  
  client.setServer(mqtt_server, 1883);
}

void loop() {
  // 1. Take the sensor readings first
  float h = dht.readHumidity();
  float t = dht.readTemperature();

  if (isnan(h) || isnan(t)) {
    Serial.println("Failed to read from DHT sensor! Check GP15 wiring.");
    delay(5000); // Short delay to prevent terminal spam if the sensor unplugs
    return;
  }

  // NEW: Get the absolute Unix timestamp instead of board uptime
  unsigned long currentTimestamp = get_unix_time_ms();

  // --- Package data into JSON payload for MQTT ---
  StaticJsonDocument<200> doc;
  doc["device"] = "node_01";
  doc["temperature"] = t;
  doc["humidity"] = h;
  
  // FIXED: Only attach timestamp if NTP sync was successful
  if (currentTimestamp > 0) {
    doc["timestamp_ms"] = currentTimestamp;
  }
  
  char jsonBuffer[512];
  serializeJson(doc, jsonBuffer);

  // 2. Wake up Wi-Fi FIRST before checking MQTT connection
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Waking up Wi-Fi and reconnecting...");
    setup_wifi();
  }

  // 3. Attempt MQTT connection only if Wi-Fi is connected successfully
  if (WiFi.status() == WL_CONNECTED) {
    if (!client.connected()) {
      reconnect();
    }
    if (client.connected()) {
      client.loop(); // Keep the MQTT connection active
      
      // NEW: Flush any offline data stored on the SD card before sending the current reading
      flush_sd_cache();
    }
  }

  // --- Publish to Server OR Failover to SD Card ---
  if (client.connected()) {
    // If the network is up, fire it to the Mosquitto broker
    client.publish("env/microclimate", jsonBuffer);
    Serial.println("SUCCESS: Payload published to MQTT: " + String(jsonBuffer));
  } else {
    // If the network drops, fallback to your original SD saving logic
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

  // 4. Force disconnect and completely power down the Wi-Fi radio
  if (client.connected()) {
    client.disconnect();
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.println("Radio powered down. Entering low-power sleep for 5 minutes...");

  // 5. Sleep the board.
  delay(300000); 
}