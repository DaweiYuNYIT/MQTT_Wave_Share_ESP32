#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Adafruit_NeoPixel.h>
NEW SKETCH


#define BOOT_PIN 0

/* ====== 本地 RGB LED ====== */
#define LED_PIN    48      // LED one wire pin
#define LED_COUNT  1

Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

/* ====== MAC ====== */
uint8_t receiverMAC[] = {0x80, 0xF3, 0xDA, 0x61, 0x51, 0xA8};  
/* ====== data payload ====== */
typedef struct {
  uint8_t r;
  uint8_t g;
  uint8_t b;
  float temperature;
  bool bootPressed;
} SensorData;

SensorData data;

/* ====== color matrix ====== */
const uint8_t colors[][3] = {
  {255,   0,   0},   // Red
  {0, 255,   0},   // Green
  {0,     0, 255}    // Blue
};
const uint8_t COLOR_COUNT = 3;
uint8_t colorIndex = 0;

/* Sending status */
volatile bool lastSendOK = false;

/* ESP-NOW callback */
void onSent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
  lastSendOK = (status == ESP_NOW_SEND_SUCCESS);

  Serial.print("Send Status: ");
  Serial.println(lastSendOK ? "OK" : "FAIL");

  if (lastSendOK) {
 // only light LED if communication is good.
    strip.setPixelColor(
      0,
      strip.Color(data.g, data.r, data.b)  // change according to the led type
    );
    strip.show();

    /* next color */
    colorIndex = (colorIndex + 1) % COLOR_COUNT;
  } else {

    strip.clear();
    strip.show();
  }
}


void printMacRaw() {
  uint8_t mac[6];
  esp_wifi_get_mac(WIFI_IF_STA, mac);

  Serial.print("My STA MAC: ");
  for (int i = 0; i < 6; i++) {
    Serial.printf("%02X", mac[i]);
    if (i < 5) Serial.print(":");
  }
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("ESP32-S3 SENSOR NODE START");

  pinMode(BOOT_PIN, INPUT_PULLUP);

  /*  RGB  */
  strip.begin();
  strip.setBrightness(5);   // 0~255，lower number for darker LED light.
  strip.clear();
  strip.show();

  WiFi.mode(WIFI_STA);
  delay(100);
  printMacRaw();
  Serial.print("My MAC: ");
  Serial.println(WiFi.macAddress());

    esp_wifi_set_protocol(WIFI_IF_STA,
                       WIFI_PROTOCOL_11B |
                       WIFI_PROTOCOL_11G |
                       WIFI_PROTOCOL_11N |
                       WIFI_PROTOCOL_LR);

  esp_wifi_set_promiscuous(true);  /// lock channel
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);



  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW INIT FAILED");
    return;
  }

  esp_now_register_send_cb(onSent);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, receiverMAC, 6);
  peer.channel = 0;
  peer.encrypt = false;

  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("ADD PEER FAILED");
    return;
  }
}

void loop() {
  data.bootPressed = (digitalRead(BOOT_PIN) == LOW);
  data.temperature = (int)temperatureRead(); // remove the 0.1 degree


  data.r = colors[colorIndex][0];  
  data.g = colors[colorIndex][1];
  data.b = colors[colorIndex][2];


  esp_now_send(receiverMAC, (uint8_t *)&data, sizeof(data));

  Serial.println("---- SEND ----");
  Serial.printf("RGB: %d %d %d\n", data.r, data.g, data.b);
  Serial.printf("TEMP: %.2f\n", data.temperature);
  Serial.printf("BOOT: %d\n", data.bootPressed);
  Serial.println("--------------");

  delay(5000);
}