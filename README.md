A self-contained, battery-powered Edge IoT node for environmental monitoring and telemetry streaming. The system collects real-time temperature and humidity data, stores measurements locally on microSD for redundancy, and simultaneously streams telemetry to a secure cloud-native data pipeline using MQTT. A containerized backend powered by Telegraf, InfluxDB, and Grafana enables real-time data ingestion, storage, visualization, and monitoring.

**Tech Stack:** Raspberry Pi Pico WH, Gravity DHT22, Adafruit MicroSD, JSON, MQTT, Eclipse Mosquitto, Telegraf, InfluxDB, Docker, Grafana.
