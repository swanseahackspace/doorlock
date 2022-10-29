#include <TimeLib.h>
#include <InputDebounce.h>
#include <Wiegand.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <ArduinoOTA.h>
#include <WiFiManager.h>
#include <ESP8266mDNS.h>
#include <FS.h>
#include <U8x8lib.h>


/* Board edge connector
 *
 * 1. Vcc 3.3v
 * 2. GPIO5   Door LED
 * 3. Gnd
 * 4. GPIO4   Buzzer
 * 5. GPIO12  Weigand WD0
 * 6. GPIO13  Weigand WD1
 * 7. GPIO14  Lock Sense
 * 8. GPIO15  Emergency Release Switch
 * 
 * MOSFET to GPIO16
 */

// MOSFET for door lock activation
// ON/HIGH == ground the output screw terminal
#define MOSFET D8

// Wiegand keyfob reader pins
#define WD0 D6
#define WD1 D7

// Door lock sense pin
#define SENSE D3

// emergency release switch
#define ERELEASE D4

// Buzzer/LED on keyfob control
#define BUZZER D5   // Gnd to beep
#define DOORLED D0  // low = green, otherwise red

// orientation of some signals
#define DLED_GREEN LOW
#define DLED_RED HIGH
#define LOCK_OPEN HIGH
#define LOCK_CLOSE LOW
#define BUZZ_ON LOW
#define BUZZ_OFF HIGH



/***********************
 * Configuration parameters
 */
// AP that it will apoear as for configuration
#define MANAGER_AP "DoorLock"

// Credentials required to reset or upload new info
// should contain #def's for www_username and www_password
#include "config.h"

// files to store card/fob data in
#define CARD_TMPFILE "/cards.tmp"
#define CARD_FILE "/cards.dat"
#define LOG_FILE "/log.dat"

// how long to hold the latch open in millis
#define LATCH_HOLD 5000

// webserver for configuration portnumber
#define CONFIG_PORT 80

// ntp server to use
#define NTP_SERVER "1.uk.pool.ntp.org"

// NTP retry time (mS)
#define NTP_RETRY 30000

/***************************
 * code below
 */

// OLED screen shield for extra debug info (optional)
U8X8_SSD1306_128X64_NONAME_SW_I2C u8x8(/* clock=*/ D1, /* data=*/ D2);

 
WIEGAND wg;
ESP8266WebServer server(CONFIG_PORT);

const unsigned int localPort = 2390;
IPAddress ntpServerIP;

const int NTP_PACKET_SIZE = 48; // NTP time stamp is in the first 48 bytes of the message

byte packetBuffer[ NTP_PACKET_SIZE]; //buffer to hold incoming and outgoing packets

WiFiUDP udp;


unsigned long ntp_lastset = 0;
unsigned long ntp_lasttry = 0;

bool ota_enabled = false;

/* User requests to enable OTA mode */
void enable_ota(void)
{
  if (!ota_enabled) {
    if (!server.authenticate(www_username, www_password))
      return server.requestAuthentication();
      
    Serial.println("Enabling OTA Mode.");
    ArduinoOTA.begin();
    ota_enabled = true;
  }
  handleRoot();
}

/* compose and send an NTP time request packet */
void ntp_send()
{
  if (ntpServerIP == INADDR_NONE) {
    if (WiFi.hostByName(NTP_SERVER, ntpServerIP) == 1) {
      if (ntpServerIP == IPAddress(1,0,0,0)) {
        Serial.println("DNS lookup failed for " NTP_SERVER " try again later.");
        ntpServerIP = INADDR_NONE;
        return;
      }
      Serial.print("Got NTP server " NTP_SERVER " address ");
      Serial.println(ntpServerIP);
    } else {
      Serial.println("DNS lookup of " NTP_SERVER " failed.");
      return;
    }
  }
  
  ntp_lasttry = millis();
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  // Initialize values needed to form NTP request
  // (see URL above for details on the packets)
  packetBuffer[0] = 0b11100011;   // LI, Version, Mode
  packetBuffer[1] = 0;     // Stratum, or type of clock
  packetBuffer[2] = 6;     // Polling Interval
  packetBuffer[3] = 0xEC;  // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  packetBuffer[12]  = 49;
  packetBuffer[13]  = 0x4E;
  packetBuffer[14]  = 49;
  packetBuffer[15]  = 52;

  // all NTP fields have been given values, now
  // you can send a packet requesting a timestamp:
  udp.beginPacket(ntpServerIP, 123); //NTP requests are to port 123
  udp.write(packetBuffer, NTP_PACKET_SIZE);
  udp.endPacket(); 

  Serial.println("Sending NTP request");
}

/* request a time update from NTP and parse the result */
time_t ntp_fetch()
{
  while (udp.parsePacket() > 0); // discard old udp packets
  ntp_send();

  uint32_t beginWait = millis();

  while (millis() - beginWait < 2500) {
    int size = udp.parsePacket();
    if (size >= NTP_PACKET_SIZE) {
      udp.read(packetBuffer, NTP_PACKET_SIZE);
  
      // this is NTP time (seconds since Jan 1 1900):
      unsigned long secsSince1900 = packetBuffer[40] << 24 | packetBuffer[41] << 16 | packetBuffer[42] << 8 | packetBuffer[43];
      const unsigned long seventyYears = 2208988800UL;
      time_t unixtime = secsSince1900 - seventyYears;

      ntp_lastset = millis();
      Serial.print("NTP update unixtime=");
      Serial.println(unixtime);
      return unixtime;
    }
  }
  Serial.println("No NTP response");
  return 0;
}

/* how big is a file */
int fileSize(const char *filename)
{
  int ret = -1;
  File file = SPIFFS.open(filename, "r");
  if (file) {
    ret = file.size();
    file.close();
  }
  return ret;
}


/* HTTP page request for /  */
void handleRoot()
{
  char mtime[16];
  int sec = millis() / 1000;
  int mi = sec / 60;
  int hr = mi / 60;
  int day = hr / 24;
  
  snprintf(mtime, 16, "%dd %02d:%02d:%02d", day, hr % 24, mi % 60, sec % 60);
    
  String out = "<html>\
  <head>\
    <title>Door Lock</title>\
    <style>\
      body { background-color: #cccccc; font-family: Arial, Helvetica, Sans-Serif; }\
    </style>\
  </head>\
  <body>\
    <h1>Door Lock!</h1>\
    <p>Uptime: " + (String)mtime + "</p>\n";

  if (timeStatus() == timeSet) {
    time_t when = now();
    out += "<p>Time now: " + getDate(when) + " " + getTime(when) + "</p>\n";
  }


  FSInfo fs_info;
  if (SPIFFS.info(fs_info)) {
    out += "<p>Onboard Flash disk: - Size:"+String(fs_info.totalBytes)+" Used:"+String(fs_info.usedBytes)+"</p>\n";
  }

  out += "<p>Lock is currently ";
  if (digitalRead(SENSE) == HIGH) out += "LOCKED"; else out += "OPEN";
  out += "</p>\n";

  if (SPIFFS.exists(CARD_FILE)) {
    
     out += "<p>Cardfile: " + String(CARD_FILE) + " is " + fileSize(CARD_FILE) + " bytes";
     int count = sanityCheck(CARD_FILE);
     if (count <= 0) {
      out += ", in an invalid file";
     } else {
      out += ", contains " + String(count) + " keyfob IDs";
      out += " - <a href=\"/download\">Download</a>";
     }
     
     out += ".</p>";
  }

  out += "<ul>\
      <li><a href=\"/reset\">Reset Configuration</a>\
      <li><a href=\"/upload\">Upload Cardlist</a>";

  if (SPIFFS.exists(LOG_FILE)) {
    out += "<li><a href=\"/wipelog\">Wipe log file</a>";
    out += "<li><a href=\"/viewlog?count=30\">View entry log</a>";
    out += "<li><a href=\"/download_logfile\">Download full logfile</a>";
  }

  if (ota_enabled) {
    out += "<li>OTA Updates ENABLED.";
  } else {
    out += "<li><a href=\"/enable_ota\">Enable OTA Updates</a>";
  }
  
  out += "</ul>";
  out += "</body>\
</html>";

  server.send( 200, "text/html", out);
}

void handleViewLog()
{
  String out = "<html>";
  out += "<head>";
  out += "<title>Card entry log</title>";
  out += "</head>";
  out += "<body>";

  int count = 10;
  if (server.hasArg("count")) {
    count = server.arg("count").toInt();
  }
  out += printLog(count);

  out += "</body></html>";
  server.send(200, "text/html", out);
}

void handleDownload()
{
  if (!server.authenticate(www_username, www_password))
    return server.requestAuthentication();

  if (!SPIFFS.exists(CARD_FILE)) {
    server.send(404, "text/plain", "Card file not found");
    return;
  }

  File f = SPIFFS.open(CARD_FILE, "r");
  server.streamFile(f, "text/csv");
  f.close();
}

void handleWipelog()
{
  if (!server.authenticate(www_username, www_password))
    return server.requestAuthentication();

  SPIFFS.remove(LOG_FILE);
  server.send(200, "text/plain", "logfile deleted");
}

// need to do this chunked. 
// https://github.com/luc-github/ESP3D/blob/master/esp3d/webinterface.cpp#L214-L394
void handleDownloadLogfile()
{
  if (!server.authenticate(www_username, www_password))
    return server.requestAuthentication();

  File f = SPIFFS.open(LOG_FILE, "r");
  if (!f) {
    server.send(404, "text/plain", "logfile not found");
    return;
  } 

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Content-Type", "text/csv", true);
  server.sendHeader("Cache-Control", "no-cache");
  server.send(200);
  
  unsigned char entry[8];
  uint32_t * data = (uint32_t *)entry;

  while (f.available()) {
    String out;
    f.read(entry, 8);
    out += getDate( data[0] );
    out += " ";
    out += getTime( data[0] );
    out += "," + String(data[1]) + ",";
    
    if (data[1] == 0) {
      out += "Emergency Release";
    } else {
      String whom = findKeyfob(data[1]);
      if (whom == "") {
        out += "Unknown keyfob";
      } else {
        out += whom;
      }
    }
    out += "\n";
    server.sendContent(out);
  }
  f.close();
  server.sendContent("");  
}

void handleNotFound() {
  String out = "File Not found\n\n";
  server.send(404, "text/plain", out);
}

// User wants to reset config
void handleReset() {
  if (!server.authenticate(www_username, www_password))
    return server.requestAuthentication();

  server.send(200, "text/plain", "Rebooting to config manager...\n\n");

  WiFiManager wfm;
  wfm.resetSettings();
  WiFi.disconnect();
  ESP.reset();
  delay(5000);
}

void handleUploadRequest() {
  String out = "<html><head><title>Upload Keyfob list</title></head><body>\
<form enctype=\"multipart/form-data\" action=\"/upload\" method=\"POST\">\
<input type=\"hidden\" name=\"MAX_FILE_SIZE\" value=\"32000\" />\
Select file to upload: <input name=\"file\" type=\"file\" />\
<input type=\"submit\" value=\"Upload file\" />\
</form></body></html>";
  server.send(200, "text/html", out);
}

File uploadFile;

String upload_error;
int upload_code = 200;

void handleFileUpload()
{
  if (server.uri() != "/upload") return;

  if (!server.authenticate(www_username, www_password))
    return server.requestAuthentication();

  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    upload_error = "";
    upload_code = 200;
    uploadFile = SPIFFS.open(CARD_TMPFILE, "w");
    if (!uploadFile) {
      upload_error = "error opening file";
      Serial.println("Opening tmpfile failed!");
      upload_code = 403;
    }
  }else
  if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) {
      if (uploadFile.write(upload.buf, upload.currentSize) != upload.currentSize) {
        upload_error = "write error";
        upload_code = 409;
      }
    }
  }else
  if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) {
      uploadFile.close();
    }
  }
}

void handleUploadComplete()
{
  String out = "Upload finished.";
  if (upload_code != 200) {
    out += "Error: "+upload_error;
  } else {
    out += " Success";
    // upload with no errors, replace old one
    SPIFFS.remove(CARD_FILE);
    SPIFFS.rename(CARD_TMPFILE, CARD_FILE);
  }
  out += "</p><a href=\"/\">Back</a>";
  server.send(upload_code, "text/plain", out);
}


void returnOK() {
  server.send(200, "text/plain", "");
}

String getTime(time_t when)
{
  String ans;
  int h = hour(when);
  int m = minute(when);
  int s = second(when);

  if (h<10) ans += "0";
  ans += String(h) + ":";
  if (m<10) ans += "0";
  ans += String(m) + ":";
  if (s<10) ans += "0";
  ans += String(s);

  return ans;
}

String getDate(time_t when)
{
  String ans;

  ans += String(year(when)) + "-" + String(month(when)) + "-" + String(day(when));
  return ans;
}


int sanityCheck(const char * filename)
{
  int count = 0;
  
  File f = SPIFFS.open(filename, "r");
  if (!f) {
    Serial.print("Sanity Check: Could not open ");
    Serial.println(filename);
    return -1;
  }
  while (f.available()) {
    char c = f.peek();
    // skip comment lines
    if (c == '#') {
      f.find("\n");
      continue;
    }

    String wcode = f.readStringUntil(',');
    String wname = f.readStringUntil('\n');
    unsigned int newcode = wcode.toInt();

    if (newcode != 0) count++;
  }
  f.close();

  return count; 
}

String findKeyfob(unsigned int code)
{
  File f = SPIFFS.open(CARD_FILE, "r");
  if (!f) {
    Serial.println("Error opening card file " CARD_FILE);
    return "";
  }

  String answer = "";
  while (f.available()) {
    char c = f.peek();
    // skip comment lines
    if (c == '#') {
      f.find("\n");
      continue;
    }

    String wcode = f.readStringUntil(',');
    String wname = f.readStringUntil('\n');

    unsigned int newcode = wcode.toInt();

/* debug
    Serial.print("Line: code='");
    Serial.print(wcode);
    Serial.print("' (");
    Serial.print(newcode);
    Serial.print(") name='");
    Serial.print(wname);
    Serial.print("'");
*/
    if (code == newcode) {
   //   Serial.println(" - FOUND IT");
      answer = wname;
      break;
    }
    //Serial.println();
  }
  f.close();
  return answer;
}

// add an entry to the log
void logEntry(time_t when, uint32_t card)
{
  unsigned char entry[8];
  
  File f = SPIFFS.open(LOG_FILE, "a");
  if (!f) {
    Serial.println("Error opening log file");
    return;
  }

  // compose the record to write
  ((uint32_t *)entry)[0] = when;
  ((uint32_t *)entry)[1] = card;
  f.write(entry, 8);
  f.close();
}

// produce a copy of the log file
String printLog(int last)
{
  String out;
  File f = SPIFFS.open(LOG_FILE, "r");
  if (!f) return String("Could not open log file");

  unsigned char entry[8];
  uint32_t * data = (uint32_t *)entry;

  if (last != 0) {
    // print only the last N items
    int pos = f.size() / 8;
    if (pos > last) pos -= last; else pos = 0;
    f.seek( pos * 8, SeekSet);
    out += "Last " + String(last) + " log entries :-";
  }
  out += "<ul>";
  
  while (f.available()) {
    f.read(entry, 8);
    out += "<li> ";
    out += getDate( data[0] );
    out += " ";
    out += getTime( data[0] );
    out += " - ";
    
    if (data[1] == 0) {
      out += "<i>";
      out += "Emergency Release";
      out += "</i>";
    } else {
      String whom = findKeyfob(data[1]);
      if (whom == "") {
        out += "<i>by ";
        out += "Unknown keyfob";
        out += "</i>";
      } else {
        out += whom;
      }
      out += " (" + String(data[1]) + ")";
    }
    out += "\n";
  }
  f.close();
  out += "</ul>";
  return out;
}


static InputDebounce release_button;

/********************************************
 * Main setup routine
 */
void setup() {
  // some serial, for debug
  Serial.begin(115200);
  Serial.println("Doorlock Mechanism. Wiegand Edition.");

  // The lock mechanism
  pinMode(MOSFET, OUTPUT);
  digitalWrite(MOSFET, LOCK_CLOSE);

  // lock sense microswitch
  pinMode(SENSE, INPUT_PULLUP);
  
  // emergency door release switch
  pinMode(ERELEASE, INPUT);

  // indicators on the keyfob reader
  pinMode(BUZZER, OUTPUT);
  pinMode(DOORLED, OUTPUT);
  digitalWrite(BUZZER, BUZZ_OFF);
  digitalWrite(DOORLED, DLED_RED);

  Serial.println("Init optional OLED Screen");
  u8x8.begin();
  u8x8.setFont(u8x8_font_chroma48medium8_r);
  u8x8.clearDisplay();
  u8x8.drawString(0, 0, "Doorlock");

  Serial.println("Testing WiFi config...");

  // if we have no config, enter config mode
  WiFiManager wfm;
  // Only wait in config mode for 3 minutes max
  wfm.setConfigPortalTimeout(180);
  // Try to connect to the old Ap for this long
  wfm.setConnectTimeout(60);
  // okay, lets try and connect...
  u8x8.drawString(0, 2, "WiFi Manager");
  wfm.autoConnect(MANAGER_AP);
  u8x8.drawString(0, 3, "Connected.");

  Serial.println("Configuring OTA update");
  ArduinoOTA.setPassword(www_password);
  ArduinoOTA.onStart([]() {
    Serial.println("Start");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  //ArduinoOTA.begin();

  
  Serial.println("Entering normal doorlock mode.");
  
  // we have config, enable web server
  server.on( "/", handleRoot );
  server.on( "/reset", handleReset );
  server.on( "/download", handleDownload );
  server.on( "/wipelog", handleWipelog );
  server.on( "/viewlog", handleViewLog );
  server.on( "/enable_ota", enable_ota );
  server.on( "/download_logfile", handleDownloadLogfile );
  server.onFileUpload( handleFileUpload);
  server.on( "/upload", HTTP_GET, handleUploadRequest);
  server.on( "/upload", HTTP_POST, handleUploadComplete);
  server.onNotFound( handleNotFound );
  server.begin();

  u8x8.setCursor(0, 4);
  u8x8.print("HTTP ");

  // advertise we exist via MDNS
  if (!MDNS.begin("doorlock")) {
    Serial.println("Error setting up MDNS responder.");
  } else {
    MDNS.addService("http", "tcp", 80);
  }
  u8x8.print("mDNS ");

  // enable internal flash filesystem
  SPIFFS.begin();
  u8x8.print("FS ");

  // init wiegand keyfob reader
  Serial.println("Configuring Wiegand keyfob reader");
  wg.begin(WD0, WD0, WD1, WD1);
  u8x8.print("WG ");
  
  // setup button debounce for the release switch
  release_button.setup(ERELEASE, 20, InputDebounce::PIM_EXT_PULL_DOWN_RES);

  Serial.println("Requesting time from network");
  // listener port for replies from NTP
  udp.begin(localPort);
  setSyncProvider(ntp_fetch);

  u8x8.setCursor(0, 6);
  u8x8.print("IP:");
  u8x8.print(WiFi.localIP());

  Serial.println("Hackspace doorlock v1.2 READY");
  u8x8.drawString(0, 7, "READY");
  // make sure its locked.
  digitalWrite(MOSFET, LOCK_CLOSE);
    digitalWrite(BUZZER, BUZZ_ON);
    delay(100);
    digitalWrite(BUZZER, BUZZ_OFF);
    delay(50);
    digitalWrite(BUZZER, BUZZ_ON);
    delay(100);
    digitalWrite(BUZZER, BUZZ_OFF);
    delay(50);
    digitalWrite(BUZZER, BUZZ_ON);
    delay(100);
    digitalWrite(BUZZER, BUZZ_OFF);
 }

unsigned long  locktime = 0;


void unlock_door()
{
  digitalWrite(DOORLED, DLED_GREEN);
  digitalWrite(MOSFET, LOCK_OPEN);
  if (locktime == 0) {
    digitalWrite(BUZZER, BUZZ_ON);
    delay(100);
    digitalWrite(BUZZER, BUZZ_OFF);
    delay(50);
    digitalWrite(BUZZER, BUZZ_ON);
    delay(100);
    digitalWrite(BUZZER, BUZZ_OFF);
    u8x8.drawString(0, 7, "Unlock");
  }
  locktime = millis();
}


void loop() {
  // is the latch held open ?
  if (locktime != 0) {
    if (locktime + LATCH_HOLD < millis()) {
      locktime = 0;
      digitalWrite(MOSFET, LOCK_CLOSE);
      digitalWrite(DOORLED, DLED_RED);
      u8x8.drawString(0, 7, "Locked");
    }
  }
  // handle web requests
  server.handleClient();
  if (ota_enabled) ArduinoOTA.handle();

  unsigned int ertime = release_button.process(millis());
  unsigned int count = release_button.getStatePressedCount();
  static unsigned last_count = 0;
  if (ertime > 0) {
    if (count != last_count) {
      last_count = count;
      Serial.println("Door Release button triggered.");
      unlock_door();
      logEntry(now(), 0);
    } else {
      // buttons is still pressed, do nothing
    }
  }

  // handle card swipes
  if (wg.available()) {
    unsigned long code = wg.getCode();
    
    Serial.print("wiegand HEX = ");
    Serial.print(code,HEX);
    Serial.print(", DECIMAL= ");
    Serial.print(code);
    Serial.print(", TYPE W");
    Serial.println(wg.getWiegandType());

    String who = findKeyfob(code);
    if (who != NULL) {
      Serial.print("Unlocking door for ");
      Serial.println(who);
      unlock_door();
      logEntry(now(), code);
    }
  }

  // has ntp failed, do we need to try again?
  if (ntp_lastset == 0 && ntp_lasttry + NTP_RETRY < millis()) {
    Serial.println("Ask Time service to try again");
    setSyncProvider(ntp_fetch);
  }
}
