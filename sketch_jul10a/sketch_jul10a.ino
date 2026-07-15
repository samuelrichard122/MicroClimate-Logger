//This file is going to be converted to binary, bare-metal instructions, and stored on the Raspberry PI. It will execute it when powered on.
// The #includes will also be converted to bare-metal, the chip doesn't have C++ on it. 
#include <SPI.h> //SPI shared bus object
#include <SD.h> //File system logic object
#include <DHT.h> //timing logic for DHT22 object

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

// --- Network Functions ---
void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Connecting to Wi-Fi: ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("WiFi connected successfully!");
  Serial.print("Pico IP address: ");
  Serial.println(WiFi.localIP());
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection to laptop...");
    if (client.connect("PicoMicroclimateNode")) {
      Serial.println("Connected to Mosquitto Broker!");
    } else {
      Serial.print("Failed, state code = ");
      Serial.print(client.state());
      Serial.println(". Trying again in 5 seconds.");
      delay(5000);
    }
  }
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

  // --- Initialize Network ---
  setup_wifi();
  client.setServer(mqtt_server, 1883);
}

void loop() {
  // Ensure we stay connected to the Mosquitto broker
  if (WiFi.status() == WL_CONNECTED && !client.connected()) {
    reconnect();
  }
  if (client.connected()) {
    client.loop(); // Keep the MQTT connection active
  }

  delay(5000); 

  float h = dht.readHumidity();
  float t = dht.readTemperature();

  if (isnan(h) || isnan(t)) {
    Serial.println("Failed to read from DHT sensor! Check GP15 wiring.");
    return;
  }

  unsigned long currentMillis = millis();

  // --- Package data into JSON payload for MQTT ---
  StaticJsonDocument<200> doc;
  doc["device"] = "node_01";
  doc["temperature"] = t;
  doc["humidity"] = h;
  doc["timestamp_ms"] = currentMillis;
  
  char jsonBuffer[512];
  serializeJson(doc, jsonBuffer);

  // --- Publish to Server OR Failover to SD Card ---
  if (client.connected()) {
    // If the network is up, fire it to the Mosquitto broker
    client.publish("env/microclimate", jsonBuffer);
    Serial.println("SUCCESS: Payload published to MQTT: " + String(jsonBuffer));
  } else {
    // If the network drops, fallback to your original SD saving logic
    Serial.println("NETWORK DOWN: Falling back to local SD card logging...");
    String dataString = String(currentMillis) + "," + String(t) + "," + String(h);

    File dataFile = SD.open("data.csv", FILE_WRITE);
    if (dataFile) {
      dataFile.println(dataString);
      dataFile.close();
      Serial.println("Logged successfully to SD: " + dataString);
    } else {
      Serial.println("CRITICAL ERROR: Failed to open data.csv on SD card");
    }
  }
}