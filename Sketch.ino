#include "freertos/FreeRTOS.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <string>
#include <cstddef>
#include <Wire.h>
#include <Preferences.h>
using namespace std;

/* ===== compile settings ===== */
#define MAX_CH 14       // 1 - 14 channels (1-11 for US, 1-13 for EU and 1-14 for Japan)
#define SNAP_LEN 2324   // max len of each recieved packet

#define BUTTON_PIN 5    // button to change the channel

#define USE_DISPLAY     // comment out if you don't want to use the OLED display
#define FLIP_DISPLAY    // comment out if you don't like to flip it
#define SDA_PIN 21
#define SCL_PIN 22
#define MAX_X 128
#define MAX_Y 51

#if CONFIG_FREERTOS_UNICORE
#define RUNNING_CORE 0
#else
#define RUNNING_CORE 1
#endif

#ifdef USE_DISPLAY
#include "SSD1306.h"

#endif

#include "FS.h"
#include <SD.h>
#include <SPI.h>
#include "Buffer.h"

esp_err_t event_handler(void* ctx, system_event_t* event) {
  return ESP_OK;
}

/* ===== run-time variables ===== */
Buffer sdBuffer;
#ifdef USE_DISPLAY
// SH1106 display(0x3c, SDA_PIN, SCL_PIN);
SSD1306 display(0x3c, SDA_PIN, SCL_PIN);


#endif
Preferences preferences;

bool useSD = false;
bool buttonPressed = false;
bool buttonEnabled = true;
uint32_t lastDrawTime;
uint32_t lastButtonTime;
uint32_t tmpPacketCounter;
uint32_t pkts[MAX_X];       // here the packets per second will be saved
uint32_t deauths = 0;       // deauth frames per second
unsigned int ch = 1;        // current 802.11 channel
int rssiSum;

/* ===== functions ===== */
double getMultiplicator() {
  uint32_t maxVal = 1;
  for (int i = 0; i < MAX_X; i++) {
    if (pkts[i] > maxVal) maxVal = pkts[i];
  }
  if (maxVal > MAX_Y) return (double)MAX_Y / (double)maxVal;
  else return 1;
}

void setChannel(int newChannel) {
  ch = newChannel;
  if (ch > MAX_CH || ch < 1) ch = 1;

  preferences.begin("packetmonitor32", false);
  preferences.putUInt("channel", ch);
  preferences.end();

  esp_wifi_set_promiscuous(false);
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous_rx_cb(&wifi_promiscuous);
  esp_wifi_set_promiscuous(true);
}

bool setupSD() {
  if (!SD.begin()) {
    Serial.println("Card Mount Failed");
    return false;
  } if (SD.begin()) {
    Serial.println("Card Mounted Successfully");
    Serial.print("SD Card initialized.\n");
    return true;
  }

  // return true;

  uint8_t cardType = SD.cardType();
  
  if (cardType == CARD_NONE) {
    Serial.println("No SD_MMC card attached");
    return false;
  }

  Serial.print("SD_MMC Card Type: ");
  if (cardType == CARD_MMC) {
    Serial.println("MMC");
  } else if (cardType == CARD_SD) {
    Serial.println("SDSC");
  } else if (cardType == CARD_SDHC) {
    Serial.println("SDHC");
  } else {
    Serial.println("UNKNOWN");
  }

  uint64_t cardSize = SD.cardSize() / (1024 * 1024);
  Serial.printf("SD Card Size: %lluMB\n", cardSize);

  return true;
}



void wifi_promiscuous(void* buf, wifi_promiscuous_pkt_type_t type) {
  wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
  wifi_pkt_rx_ctrl_t ctrl = (wifi_pkt_rx_ctrl_t)pkt->rx_ctrl;

  if (type == WIFI_PKT_MGMT && (pkt->payload[0] == 0xA0 || pkt->payload[0] == 0xC0 )) deauths++;

  if (type == WIFI_PKT_MISC) return;             // wrong packet type
  if (ctrl.sig_len > SNAP_LEN) return;           // packet too long

  uint32_t packetLength = ctrl.sig_len;
  if (type == WIFI_PKT_MGMT) packetLength -= 4;  // fix for known bug in the IDF https://github.com/espressif/esp-idf/issues/886

  //Serial.print(".");
  tmpPacketCounter++;
  rssiSum += ctrl.rssi;

  if (useSD) sdBuffer.addPacket(pkt->payload, packetLength);
}

void draw() {
#ifdef USE_DISPLAY
  double multiplicator = getMultiplicator();
  int len;
  int rssi;

  if (pkts[MAX_X - 1] > 0) rssi = rssiSum / (int)pkts[MAX_X - 1];
  else rssi = rssiSum;

  display.clear();

  display.setTextAlignment(TEXT_ALIGN_RIGHT);
  display.drawString( 10, 0, (String)ch);
  display.drawString( 14, 0, ("|"));
  display.drawString( 30, 0, (String)rssi);
  display.drawString( 34, 0, ("|"));
  display.drawString( 82, 0, (String)tmpPacketCounter);
  display.drawString( 87, 0, ("["));
  display.drawString(106, 0, (String)deauths);
  display.drawString(110, 0, ("]"));
  display.drawString(114, 0, ("|"));
  display.drawString(128, 0, (useSD ? "SD" : ""));
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawString( 36,  0, ("Pkts:"));

  display.drawLine(0, 63 - MAX_Y, MAX_X, 63 - MAX_Y);
  for (int i = 0; i < MAX_X; i++) {
    len = pkts[i] * multiplicator;
    display.drawLine(i, 63, i, 63 - (len > MAX_Y ? MAX_Y : len));
    if (i < MAX_X - 1) pkts[i] = pkts[i + 1];
  }
  display.display();
#endif
}

/* ===== main program ===== */
void setup() {

  // Serial
  Serial.begin(115200);
  

  // Settings
  preferences.begin("wirelesswishpers", false);
  ch = preferences.getUInt("channel", 1);
  preferences.end();

  // System & WiFi
  nvs_flash_init();
  tcpip_adapter_init();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL));
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  //ESP_ERROR_CHECK(esp_wifi_set_country(WIFI_COUNTRY_EU));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
  ESP_ERROR_CHECK(esp_wifi_start());

  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);

  // SD card
  sdBuffer = Buffer();

  if (setupSD())
    sdBuffer.open(&SD);

  // I/O
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // display
  #ifdef USE_DISPLAY
    display.init();
  #ifdef FLIP_DISPLAY
    display.flipScreenVertically();
  #endif

  /* show start screen */
  display.clear();
  display.setFont(ArialMT_Plain_16);

  // Calculate the center position for "2.4 GHz Channel"
  int textWidth1 = display.getStringWidth("WiFi Rhapsody");
  int xPos1 = (display.width() - textWidth1) / 2;
  int yPos1 = (display.height() - 16) / 2; // Divide by 2 to vertically center the 16-pixel font
  display.drawString(xPos1, yPos1, "WiFi Rhapsody");

  display.setFont(ArialMT_Plain_10);
  // Calculate the center position for "Chorus"
  int textWidth2 = display.getStringWidth("** Packet -- Pulse **");
  int xPos2 = (display.width() - textWidth2) / 2;
  int yPos2 = yPos1 + 19; // Place "Chorus" just below "2.4 GHz Channel"
  display.drawString(xPos2, yPos2, "** Packet -- Pulse **");

  display.display();
  delay(1500); // Delay to show the first two lines for 2 seconds

  // Slide the previous content
  for (int i = 0; i > -46; i--) { // Adjust the value to control the sliding distance
    display.clear();
    display.setFont(ArialMT_Plain_16);
    display.drawString(xPos1, yPos1 + i, "WiFi Rhapsody");
    display.setFont(ArialMT_Plain_10);
    display.drawString(xPos2, yPos2 + i, "** Packet -- Pulse **");
    display.display();
    delay(30); // Adjust the delay to control the sliding speed
  }

  // Calculate the center position for "Tech-Ex" and "Softwarica College"
  display.clear();
  display.setFont(ArialMT_Plain_16);
  int textWidth3 = display.getStringWidth("Tech-Ex");
  int xPos3 = (display.width() - textWidth3) / 2;
  int yPos3 = (display.height() - 20) / 2; // Divide by 2 to vertically center the 20-pixel font
  display.drawString(xPos3, yPos3, "Tech-Ex");

  // Second Screen
  display.setFont(ArialMT_Plain_10);
  int textWidth4 = display.getStringWidth("Softwarica College");
  int xPos4 = (display.width() - textWidth4) / 2;
  int yPos4 = yPos3 + 19; // Place "Softwarica College" just below "Tech-Ex"
  display.drawString(xPos4, yPos4, "Softwarica College");
  display.display();

  delay(2000);
  #endif

  // second core
  xTaskCreatePinnedToCore(
    coreTask,               /* Function to implement the task */
    "coreTask",             /* Name of the task */
    2500,                   /* Stack size in words */
    NULL,                   /* Task input parameter */
    0,                      /* Priority of the task */
    NULL,                   /* Task handle. */
    RUNNING_CORE);          /* Core where the task should run */

  // start Wifi sniffer
  esp_wifi_set_promiscuous_rx_cb(&wifi_promiscuous);
  esp_wifi_set_promiscuous(true);
}

void loop() {
  vTaskDelay(portMAX_DELAY);
}

void coreTask( void * p ) {

  uint32_t currentTime;

  while (true) {

    currentTime = millis();

    /* bit of spaghetti code, have to clean this up later :D */

    // check button
    if (digitalRead(BUTTON_PIN) == LOW) {
      if (buttonEnabled) {
        if (!buttonPressed) {
          buttonPressed = true;
          lastButtonTime = currentTime;
        } else if (currentTime - lastButtonTime >= 2000) {
          if (useSD) {
            useSD = false;
            sdBuffer.close(&SD);
            draw();
          } else {
            if (setupSD())
              sdBuffer.open(&SD);
            draw();
          }
          buttonPressed = false;
          buttonEnabled = false;
        }
      }
    } else {
      if (buttonPressed) {
        setChannel(ch + 1);
        draw();
      }
      buttonPressed = false;
      buttonEnabled = true;
    }

    // save buffer to SD
    if (useSD)
      sdBuffer.save(&SD);

    // draw Display
    if ( currentTime - lastDrawTime > 1000 ) {
      lastDrawTime = currentTime;
      // Serial.printf("\nFree RAM %u %u\n", heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT), heap_caps_get_minimum_free_size(MALLOC_CAP_32BIT));// for debug purposes

      pkts[MAX_X - 1] = tmpPacketCounter;

      draw();

      Serial.println((String)pkts[MAX_X - 1]);

      tmpPacketCounter = 0;
      deauths = 0;
      rssiSum = 0;
    }

    // Serial input
    if (Serial.available()) {
      ch = Serial.readString().toInt();
      if (ch < 1 || ch > 14) ch = 1;
      setChannel(ch);
    }

  }

}
