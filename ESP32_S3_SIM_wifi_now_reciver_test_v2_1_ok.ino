/**********************************************************
 * ESP32-S3 + SIM7670X (A7670)
 * ESP-NOW Receiver → MQTT (Waveshare Cloud)
 * FINAL VERSION – MQTT PUBLISH FIXED
 **********************************************************/

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Adafruit_NeoPixel.h>

/* ================= Hardware ================= */

#define SIM_RX      17
#define SIM_TX      18
#define SIM_BAUD    115200
#define SIM_PWRKEY  33

#define I2C_SDA     3
#define I2C_SCL     2

#define LED_PIN     38
#define LED_COUNT   1

/* ================= APN ================= */

#define APN "internet.fizz.ca"

/* ================= MQTT ================= */

#define MQTT_SERVER    "mqtt.waveshare.cloud"
#define MQTT_PORT      1883
#define CLIENT_ID      "9c493123"
#define MQTT_PUB_TOPIC "Pub/1412/37/9c493123"

/* ================= Objects ================= */

HardwareSerial sim7670(1);
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

/* ================= Data Struct ================= */

typedef struct {
  uint8_t r;
  uint8_t g;
  uint8_t b;
  float   temperature;
  bool    bootPressed;
} SensorData;

volatile bool dataReady = false;
SensorData rxData;

unsigned long lastRecvMillis = 0;
const unsigned long LINK_TIMEOUT = 5000;
bool ledOn = false;

/* ================= LTE Flags ================= */

volatile bool lteRegistered = false;
bool cfunDoneOnce = false;

/* ================= URC Parser ================= */

void parseURC(char c) {
  static String line;

  if (c == '\n') {
    line.trim();

    if (line.startsWith("+CEREG:")) {
      Serial.println("URC >> " + line);
      if (line.startsWith("+CEREG: 1") ||
          line.indexOf(",1") != -1 ||
          line.indexOf(",5") != -1) {
        lteRegistered = true;
        Serial.println(" LTE REGISTERED");
      }
    }

    if (line == "QCRDY") {
      Serial.println(" QCRDY RECEIVED");
      lteRegistered = false;
      sim7670.print("AT+CEREG=2\r\n");
    }

    line = "";
  } else if (c != '\r') {
    line += c;
  }
}

/* ================= SIM UART Drain ================= */

void drainSIM(uint32_t ms = 0) {
  uint32_t start = millis();
  do {
    while (sim7670.available()) {
      char c = sim7670.read();
      Serial.write(c);
      parseURC(c);
    }
    delay(1);
  } while (ms && millis() - start < ms);
}

/* ================= AT Helpers ================= */

void sendAT(const char *cmd) {
  Serial.print(">> ");
  Serial.println(cmd);
  sim7670.print(cmd);
  sim7670.print("\r\n");
}

bool waitFor(const char *key, uint32_t timeout = 8000) {
  String buf;
  uint32_t start = millis();

  while (millis() - start < timeout) {
    while (sim7670.available()) {
      char c = sim7670.read();
      buf += c;
      Serial.write(c);
      parseURC(c);
    }
    if (buf.indexOf(key) != -1) return true;
    delay(1);
  }
  return false;
}

/* ================= SIM Power ================= */

void simPowerOn() {
  pinMode(SIM_PWRKEY, OUTPUT);
  digitalWrite(SIM_PWRKEY, LOW);
  delay(1000);
  digitalWrite(SIM_PWRKEY, HIGH);
  delay(2000);
  digitalWrite(SIM_PWRKEY, LOW);
  delay(8000);
}

/* ================= SIM Init ================= */

bool simInit() {

  sendAT("AT");
  if (!waitFor("OK", 3000)) return false;

  sendAT("ATE0");
  waitFor("OK");

  sendAT("AT+CPIN?");
  if (!waitFor("READY", 8000)) return false;

  sendAT("AT+CNMP=38");
  waitFor("OK", 3000);

  sendAT("AT+CGDCONT=1,\"IP\",\"" APN "\"");
  waitFor("OK", 3000);

  sendAT("AT+CEREG=2");
  waitFor("OK", 3000);

  if (!cfunDoneOnce) {
    sendAT("AT+CFUN=1,1");
    cfunDoneOnce = true;
  }

  Serial.println("⏳ Waiting LTE registration...");
  return true;
}

/* ================= PDP ================= */

bool activatePDP() {

  sendAT("AT+CGATT=1");
  waitFor("OK", 15000);

  sendAT("AT+CGACT=1,1");
  drainSIM(2000);

  sendAT("AT+CGPADDR=1");
  if (waitFor(".", 8000)) {
    Serial.println(" PDP ACTIVE");
    return true;
  }
  return false;
}

/* ================= MQTT ================= */

bool mqttConnect() {

  sendAT("AT+CMQTTSTOP");
  drainSIM(500);

  sendAT("AT+CMQTTSTART");
  if (!waitFor("+CMQTTSTART", 20000)) return false;

  sendAT("AT+CMQTTACCQ=0,\"" CLIENT_ID "\"");
  waitFor("OK", 5000);

  sendAT(
    "AT+CMQTTCONNECT=0,"
    "\"tcp://" MQTT_SERVER ":1883\",60,1"
  );

  return waitFor("OK", 30000);
}

/* =================  FIXED MQTT PUBLISH ================= */

void mqttPublish(const SensorData &d) {

  String payload =
    String("{\"data\":{") +
    "\"Red\":" + String(d.g) + "," +
    "\"Green\":" + String(d.r) + "," +
    "\"Blue\":" + String(d.b) + "," +
    "\"Sensor_Temperature\":" + String(d.temperature, 2) + "," +
    "\"Key1\":" + String(d.bootPressed ? 1 : 0) +
    "}}";

  // ---- TOPIC ----
  sendAT(("AT+CMQTTTOPIC=0," +
          String(strlen(MQTT_PUB_TOPIC))).c_str());
  if (!waitFor(">", 5000)) return;
  sim7670.print(MQTT_PUB_TOPIC);

  // ---- PAYLOAD ----
  sendAT(("AT+CMQTTPAYLOAD=0," +
          String(payload.length())).c_str());
  if (!waitFor(">", 5000)) return;
  sim7670.print(payload);

  // ---- PUBLISH ----
  sendAT("AT+CMQTTPUB=0,1,60");
  waitFor("OK", 10000);
}

/* ================= ESP-NOW Callback ================= */

void onReceive(const esp_now_recv_info *info,
               const uint8_t *incomingData,
               int len) {
  memcpy((void*)&rxData, incomingData, sizeof(rxData));
  lastRecvMillis = millis();
  dataReady = true;
}

/* ================= Arduino ================= */

void setup() {

  Serial.begin(115200);
  delay(1000);

  strip.begin();
  strip.setBrightness(25);
  strip.clear();
  strip.show();

  sim7670.begin(SIM_BAUD, SERIAL_8N1, SIM_RX, SIM_TX);
  Wire.begin(I2C_SDA, I2C_SCL);

  Serial.println("===== ESP32-S3 + SIM7670X START =====");

  simPowerOn();

  if (!simInit()) {
    Serial.println(" SIM INIT FAILED");
  }

  uint32_t t0 = millis();
  while (!lteRegistered && millis() - t0 < 60000) {
    drainSIM();
  }

  if (!lteRegistered) {
    Serial.println(" LTE REGISTER TIMEOUT");
  }

  if (!activatePDP()) {
    Serial.println(" PDP FAILED");
  }

  if (!mqttConnect()) {
    Serial.println(" MQTT CONNECT FAILED");
  }


  WiFi.mode(WIFI_STA);
  delay(100);
  esp_wifi_set_promiscuous(true); // lock wifi channel
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);


  esp_now_init();
  esp_now_register_recv_cb(onReceive);

  Serial.println("ESP-NOW READY");
}

void loop() {

  drainSIM();

  if (dataReady) {
    dataReady = false;
    mqttPublish(rxData);
    strip.setPixelColor(
      0,
      strip.Color(rxData.r, rxData.g, rxData.b)
    );
    strip.show();
    ledOn = true;
  }

  if (ledOn && (millis() - lastRecvMillis > LINK_TIMEOUT)) {
    SensorData rxData_1;
    rxData_1.temperature=0;
    rxData_1.bootPressed=0;
    rxData_1.r=0;
    rxData_1.g=0;
    rxData_1.b=0;
    mqttPublish(rxData_1);
    strip.clear();
    strip.show();
    ledOn = false;
  }

  delay(10);
}