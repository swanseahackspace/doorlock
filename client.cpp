#include <ESP8266WiFi.h>

#include "config.h"

static String http_get(const String url)
{
  WiFiClient client;
  if (!client.connect(AUTH_HOST, 80)) {
    Serial.println("connection failed");
    return "";
  }

  client.print(String("GET ") + url + " HTTP/1.1\r\n" +
               "Host: " + AUTH_HOST + "\r\n" +
               "Connection: close\r\n" +
               "\r\n");
  delay(10);

  String answer;
  while (client.available()) {
    String line = client.readStringUntil('\r');
    answer += line;
  }        
  return answer;
}

bool log_attempt(const char *uid, bool success)
{
  String url = AUTH_URL;
  url += "?device=";
  url += DEVICE_ID;
  url += "&uid=";
  url += uid;
  url += "&action=access";
  if (success) {
    url += "&success=true";
  } else {
    url += "&success=false";  
  }

  String answer = http_get(url);
  if (answer == "") return false;
  return true;
}

