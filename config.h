// WiFi network to use for connections
#define WIFI_SSID    "Hackspace"
#define WIFI_PASS    "electronics"

// This devices unique ID and shared auth secret
#define DEVICE_ID    "frontdoor"
#define SECRET       "abcdef"

// URL of the API 
#define AUTH_HOST    "swansea.hackspace.org.uk"
#define AUTH_URL     "/auth/log.php"

// MFRC522 Pins
#define RST_PIN    5   // MFRC522 Reset Pin
#define SS_PIN    15  // MFRC522 Select Pin

#define LED_PIN   2

#define LOCK_PIN  16
#define LOCK_TIME 3000  // unlock for 3 seconds

// how long to try to connect the wifi
#define WIFI_COUNT  20
#define WIFI_DELAY  250  // 20 x 250mS == 5s

// how often to retry the connection
#define WIFI_RETRY  60000

// filename on SPIFFS that stores key list
#define KEYS_FILE "/keys"



#undef DEBUG
