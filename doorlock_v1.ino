#include <SPI.h>
#include <ESP8266WiFi.h>
#include <MFRC522.h>
#include <Adafruit_NeoPixel.h>
#include <FS.h>


#include "config.h"
#include "tag_handler.h"
#include "client.h"

MFRC522 mfrc522(SS_PIN, RST_PIN);

Adafruit_NeoPixel led = Adafruit_NeoPixel(9, LED_PIN, NEO_GRB + NEO_KHZ800);

// when did we last try to connect to wifi
unsigned long last_try = 0;

// is the wifi up and connected
bool wifi_connected() 
{
  if (WiFi.status() == WL_CONNECTED) return true;
  return false;
}

// try and connect us, but give up after a while
bool wifi_connect()
{
  int count = 0;
  int pulse = 0;
  int rate = 32;

  if (WiFi.status() == WL_CONNECTED) return true;


  Serial.print(F("WiFi Connecting to "));
  Serial.println(WIFI_SSID);
  
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED && count < WIFI_COUNT) {
    Serial.print(".");
    led.setPixelColor(0, 0, 0, pulse);
    led.show();
    pulse += rate;
    if (pulse <= 0 || pulse >= 192) rate=-rate;
    delay(WIFI_DELAY);
    count++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("Connected. IP="));
    Serial.println(WiFi.localIP());
    return true;
  }
  Serial.println(F("Wifi connect failed."));
  return false;
}

// unrecoverable error state
// pulse red
void error_loop()
{
  int pulse = 0;
  int rate = 8;
  while (1) {
    led.setPixelColor(0, pulse, 0, 0);
    led.show();
    pulse += rate;
    if (pulse <= 0 || pulse >= 256) rate=-rate;
    delay(50);
  }
}


// Lock is idle, colour indicates online or not
void led_status(bool has_wifi)
{
  // must be neatly divisible to work
  static int pulse = 32;
  static int rate = -2;
  
  if (has_wifi) {
    led.setPixelColor(0, pulse, pulse, pulse);
  } else {
    led.setPixelColor(0, pulse, pulse, 0);
  }
  led.show();

  pulse += rate;
  if (pulse == 0 || pulse == 32) rate=-rate;
}

void setup() {
  SPI.begin();
  
  Serial.begin(9600);
  Serial.println(F("\n\nRFID Doorlock. v1"));

  pinMode(LOCK_PIN, OUTPUT);
  digitalWrite(LOCK_PIN, LOW);

  // Enable and clear LEDs
  pinMode(LED_PIN, OUTPUT);
  led.begin();
  led.setBrightness(64);
  led.show();

  // Test comms with the RFID board
  mfrc522.PCD_Init();
  byte v = mfrc522.PCD_ReadRegister(mfrc522.VersionReg);

  if (v == 0) {
    Serial.println(F("RFID Init failed"));
    error_loop();
  }
  Serial.print(F("MFRC Rev 0x"));
  Serial.println(v, HEX);

  // connect and indicate
  led_status( wifi_connect() );
  last_try = millis();

  // check if we have a keyfile
  SPIFFS.begin();

  if (SPIFFS.exists(KEYS_FILE)) {
    Serial.print(F("Found keys file "));
    Serial.println(KEYS_FILE);
  } else {
    Serial.print(F("Keys file "));
    Serial.print(KEYS_FILE);
    Serial.println(F(" not found"));
  }


  ESP.wdtEnable(5000);
}

void open_lock()
{
  led.setPixelColor(0, 0, 255, 0);
  led.show();
  digitalWrite(LOCK_PIN, HIGH);
  delay(3000);
  digitalWrite(LOCK_PIN, LOW);
  led.setPixelColor(0, 0, 0, 0);
  led.show();
}

void fail_lock() 
{
  led.setPixelColor(0, 255, 0, 0);
  led.show();
  delay(3000);
  led.setPixelColor(0, 0, 0, 0);
  led.show();
  
}

void loop() {
  MFRC522::Uid uid;
  int count = 0;
  bool success = false;

  ESP.wdtFeed();
  while (uid_fetch(mfrc522, uid, count)) {
    Serial.print("Card: ");
    Serial.print(uid_print(uid));
    
    if (uid_valid(uid)) {
      Serial.println(" MATCH");
      open_lock();
      success = true;
    } else {
      Serial.println(" FAILED");
      fail_lock();
    }
    
    // wifi is up, report it
    if (wifi_connected()) {
      log_attempt(uid_print(uid), success);
    }
  }
  
  led_status( wifi_connected() );

  // wifi isnt connected, time to try again ?
  if (!wifi_connected()) {
    unsigned int now = millis();
    if ( last_try + WIFI_RETRY < now) {
      Serial.print("now="); Serial.print(now);
      Serial.print(" last="); Serial.print(last_try);
      Serial.print(" then="); Serial.println(last_try + WIFI_RETRY);
      led_status( wifi_connect() );
      last_try = now;
    }
  }
  
  // dont spin too fast
  delay(100);

}
