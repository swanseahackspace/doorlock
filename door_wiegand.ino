#include <InputDebounce.h>
#include <Wiegand.h>
#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <ESP8266mDNS.h>
#include <FS.h>

// MOSFET for door lock activation
// ON/HIGH == ground the output screw terminal
#define MOSFET 16

// Pin for LED / WS2812
#define STATUSLED 2

// Wiegand keyfob reader pins
#define WD0 12
#define WD1 13

// Door lock sense pin
#define SENSE 14

// emergency release switch
#define ERELEASE 15

// Buzzer/LED on keyfob control
#define BUZZER 4   // Gnd to beep
#define DOORLED 5  // low = green, otherwise red

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
#define www_username "admin"
#define www_password "wibble"

// files to store card/fob data in
#define CARD_TMPFILE "/cards.tmp"
#define CARD_FILE "/cards.dat"

// how long to hold the latch open in millis
#define LATCH_HOLD 5000

/***************************
 * code below
 */
WIEGAND wg;
ESP8266WebServer server(80);


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
    <p>Uptime: " + (String)mtime + "</p>";

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

  FSInfo fs_info;
  if (SPIFFS.info(fs_info)) {
    out += "<li><a href=\"/format\">Format SPIFFS</a> - Size:"+String(fs_info.totalBytes)+" Used:"+String(fs_info.usedBytes);
  }

  out += "<li>Lock is currently ";
  if (digitalRead(SENSE) == HIGH) out += "LOCKED"; else out += "OPEN";
      
  out += "</ul>\
  </body>\
</html>";

  server.send( 200, "text/html", out);
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
  server.send(upload_code, "text/plain", out);
}


void returnOK() {
  server.send(200, "text/plain", "");
}


static InputDebounce release_button;

void setup() {
  // some serial, for debug
  Serial.begin(115200);

  // The lock mechanism, set HIGH to turn on and connect ground to output pin
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

  Serial.println("DoorLock. Testing WiFi config...");

  // if we have no config, enter config mode
  WiFiManager wfm;
  wfm.autoConnect(MANAGER_AP);

  // we have config, enable web server
  server.on( "/", handleRoot );
  server.on( "/reset", handleReset );
  server.onFileUpload( handleFileUpload);
  server.on( "/upload", HTTP_GET, handleUploadRequest);
  server.on( "/upload", HTTP_POST, handleUploadComplete);
  server.on( "/download", handleDownload );
  server.onNotFound( handleNotFound );
  server.begin();

  // advertise we exist via MDNS
  if (!MDNS.begin("doorlock")) {
    Serial.println("Error setting up MDNS responder.");
  } else {
    MDNS.addService("http", "tcp", 80);
  }

  // enable internal flash filesystem
  SPIFFS.begin();

  // init wiegand keyfob reader
  Serial.println("Starting Wiegand test reader");
  wg.begin(WD0, WD0, WD1, WD1);

  // setup button debounce for the release switch
  release_button.setup(ERELEASE, 20, InputDebounce::PIM_EXT_PULL_DOWN_RES);
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
  }
  locktime = millis();
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

void loop() {
  unsigned long now = millis();

  // is the latch held open ?
  if (locktime != 0) {
    if (locktime + LATCH_HOLD < now) {
      locktime = 0;
      digitalWrite(MOSFET, LOCK_CLOSE);
      digitalWrite(DOORLED, DLED_RED);
    }
  }
  // handle web requests
  server.handleClient();

  unsigned int ertime = release_button.process(now);
  unsigned int count = release_button.getStateOnCount();
  static unsigned last_count = 0;
  if (ertime > 0) {
    if (count != last_count) {
      last_count = count;
      Serial.println("Door Release button triggered.");
      unlock_door();
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
    }
  }

}
