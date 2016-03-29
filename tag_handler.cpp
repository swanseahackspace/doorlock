#include <MFRC522.h>
#include <FS.h>
#include "config.h"

typedef uint8_t byte_t;

static const char * hex = "0123456789ABCDEF";


int hexto(char c)
{
  if (c >= '0' && c <='9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
}

static const char * cardid(const byte_t uid[], byte_t len)
{
  static char text[21];
  if (len > 10) return NULL;

  char * p = text;
  *(p++) = hex[ len ];   //DEBUG
  *(p++) = ':';          //DEBUG
  for (int i=0; i<len; i++) {
    *(p++) = hex[ uid[i] >> 4 ];
    *(p++) = hex[ uid[i] & 15 ];
  }
  *p = 0;
  return text;
}

const char * uid_print(const MFRC522::Uid uid)
{
  return cardid(uid.uidByte, uid.size);
}

void uid_copy(MFRC522::Uid &a, const MFRC522::Uid b)
{
  a.size = b.size;
  memcpy(a.uidByte, b.uidByte, b.size);
}

bool uid_compare(const MFRC522::Uid a, const MFRC522::Uid b)
{
  if (a.size != b.size) return 0;
  if (a.size == 0 || b.size == 0) return 0;
  return memcmp(a.uidByte, b.uidByte, a.size)==0?true:false;
}

void uid_zero(MFRC522::Uid &uid)
{
  uid.size = 0;
}

/* is this card id permitted */
bool uid_auth(const MFRC522::Uid uid)
{
  // TODO:  everything
 //  return card_auth(cardid(uid.uidByte, uid.size));
 return true;
}

bool uid_isset(const MFRC522::Uid uid)
{
  return (uid.size!=0);
}

bool uid_visible(MFRC522 &reader, const MFRC522::Uid master)
{
  MFRC522::Uid uid;

  // no tag given
  if (master.size == 0) return false;

  byte bufferATQA[2];
  byte bufferSize = sizeof(bufferATQA);
  byte status = 0;

  SPI.begin();
  
  // no tags in range to wakeup
  if (reader.PICC_WakeupA(bufferATQA, &bufferSize) == MFRC522::STATUS_TIMEOUT) {
      return false;
  }

  bool found = false;

  uid_copy(uid, master);
  if (reader.PICC_Select(&uid, uid.size * 8) == MFRC522::STATUS_OK) {
    // something answered, but was it the right one
    if (uid_compare(uid, master)) {
      // yes, match
      found=true;
    } else {
      // uid did not match
    }
  } else {
    // nothing answered
  }
  reader.PICC_HaltA();
  return found;
}


bool uid_fetch(MFRC522 &reader, MFRC522::Uid &uid, int count)
{
  SPI.begin();

  byte bufferATQA[2];
  byte bufferSize = sizeof(bufferATQA);
  byte status = 0;

  if (count == 0) {
      status = reader.PICC_WakeupA(bufferATQA, &bufferSize);
  } else {
      status = reader.PICC_RequestA(bufferATQA, &bufferSize);
  }

  uid_zero(uid);
  if (status != MFRC522::STATUS_OK && status != MFRC522::STATUS_COLLISION) {
      if (status == MFRC522::STATUS_TIMEOUT) {
        // Nobody listening
        return false;
      }
  }
 
  bool result = false;
  if ( (status = reader.PICC_Select(&uid, 0)) == MFRC522::STATUS_OK ) {
      result=true;
  }
  reader.PICC_HaltA();

  return result;
}


bool uid_read(File &f, MFRC522::Uid &uid)
{
  char line[40];

  int len = f.readBytesUntil('\n', line, 40);
  if (len < 0 || len > 24) {
    Serial.print("Invalid read: ");
    Serial.println(len);
    return false;
  }
  line[len]=0;

  uid_zero(uid);
  if (len == 0) {
    // end of file
    return false;
  }
  
  // minimum length is 4 bytes == 10 chars
  if (len < 10) {
    Serial.print(F("Read Card: bad line length="));
    Serial.print(len);
    Serial.print(" '");
    Serial.print(line);
    Serial.println("'");
    return false;
  }

  // invalid format
  if (line[1] != ':') {
    Serial.print(F("Read Card: bad format '"));
    Serial.print(line);
    Serial.println("'");
    return false;
  }
  
  int bytes = hexto(line[0]);
  
  // invalid lengths: 4, 7, and 10 are the standards
  if (bytes != 4 && bytes != 7 && bytes != 10) {
    Serial.print(F("Read Card: bad size="));
    Serial.print(bytes);
    Serial.print(" '");
    Serial.print(line);
    Serial.println("'");
    return false;
  }

  if (len < (bytes*2)+2) {
    Serial.print(F("Read card: line too short. len="));
    Serial.print(len);
    Serial.print(F(" size="));
    Serial.print(bytes);
    Serial.print(" '");
    Serial.print(line);
    Serial.println("'");
    return false;
  }
  
  uid.size = bytes;
  byte * p = uid.uidByte;
  int in = 2;
  
  for (;bytes > 0;bytes--) {
    int a,b;
    a = hexto(line[in++]);
    b = hexto(line[in++]);
    if (a == -1 || b == -1) {
      uid_zero(uid);

      Serial.print(F("Read Card: Non Hex char in line '"));
      Serial.print(line);
      Serial.println("'");
      return false;
    } 
    *(p++) = a<<4 | b;
  }

#ifdef DEBUG
  Serial.print(F("Loaded card '"));
  Serial.print(line);
  Serial.print("' as ");
  Serial.println(uid_print(uid));
#endif

  return true;
}

bool uid_valid(MFRC522::Uid &uid)
{
#ifdef DEBUG
    Serial.print(F("Opening file "));
    Serial.println(KEYS_FILE);
#endif
    
    File f = SPIFFS.open(KEYS_FILE, "r");
    // cant open the file, so card isnt there
    if (!f) {
      Serial.println(F("Cannot open cards file"));
      return false;
    }

    MFRC522::Uid entry;
    while ( uid_read(f, entry) ) {
      if (uid_compare(uid, entry)) {
        f.close();
#ifdef DEBUG
        Serial.println(F("Found card"));
#endif
        return true;
      }
    }

#ifdef DEBUG
    Serial.println(F("Card not found"));
#endif
    f.close();
    return false;
}

