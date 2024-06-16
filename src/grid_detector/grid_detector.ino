#define ETHERNET 1
#define WIFI 0
#define GRID 1

const PROGMEM char* VERSION = "0.1";

#include <Preferences.h>

#define INIT            0x3

#if ETHERNET
#include <ETH.h>
#define ETH_CLK_MODE    ETH_CLOCK_GPIO0_IN
#define ETH_POWER_PIN   16
#define ETH_TYPE        ETH_PHY_LAN8720
#define ETH_ADDR        1
#define ETH_MDC_PIN     23
#define ETH_MDIO_PIN    18
#endif

#if WIFI
#include <WiFiManager.h>
WiFiManager     wm;
WiFiClient      client;
#endif

struct Settings {
  const char*   apssid            = "GridDetector";
  const char*   softwareversion   = VERSION;
  int           gridpin           = 2;
  char          devicename[31]    = "Grid Detector";
  char          broadcastname[31] = "griddetector";
};

Settings settings;

static bool connected = false;
int lastState = INIT;
int currentState;
bool gridOnlineNotify;
bool gridOfflineNotify;
int gridStatus = INIT;
unsigned long pressedTime = 0;
char chipID[13];
char localIP[16];

#define REACTION_TIME 2000

void rebootDevice(int time = 2000) {
  Serial.print("reboot in: ");
  Serial.println(time);
  delay(time);
  ESP.restart();
}

void initChipID() {
  uint64_t chipid = ESP.getEfuseMac();
  sprintf(chipID, "%04x%04x", (uint32_t)(chipid >> 32), (uint32_t)chipid);
  Serial.printf("ChipID Inited: '%s'\n", chipID);
}

#if WIFI
char* getLocalIP() {
  strcpy(localIP, WiFi.localIP().toString().c_str());
  return localIP;
}

void apCallback(WiFiManager* wifiManager) {
  const char* message = wifiManager->getConfigPortalSSID().c_str();
  Serial.print("connect to: ");
  Serial.println(message);
  WiFi.onEvent(Events);
}

void saveConfigCallback() {
  Serial.print("saved AP: ");
  Serial.println(wm.getWiFiSSID(true).c_str());
  delay(2000);
  rebootDevice();
}

void initWifi() {
  Serial.println("Init Wifi");
  WiFi.mode(WIFI_STA); 

  wm.setHostname(settings.broadcastname);
  wm.setTitle(settings.devicename);
  wm.setConfigPortalBlocking(true);
  wm.setConnectTimeout(3);
  wm.setConnectRetries(10);
  wm.setAPCallback(apCallback);
  wm.setSaveConfigCallback(saveConfigCallback);
  wm.setConfigPortalTimeout(180);
  Serial.print("connecting to: ");
  Serial.println(wm.getWiFiSSID(true).c_str());
  if (!wm.autoConnect(settings.apssid)) {
    Serial.println("Reboot");
    rebootDevice(5000);
    return;
  }
  Serial.println("Wifi initialized with DHCP");
  wm.setHttpPort(80);
  wm.startWebPortal();
  Serial.print("IP Address: ");
  Serial.println(getLocalIP());
  connected = true;
}
#endif

void Events(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED: {
      char softApIp[16];
      strcpy(softApIp, WiFi.softAPIP().toString().c_str());
      Serial.print("set in browser: ");
      Serial.println(softApIp);
      WiFi.removeEvent(Events);
      break;
    }
    #if ETHERNET
    case 18:
      Serial.println("ETH Started");
      //set eth hostname here
      ETH.setHostname("esp32-ethernet");
      break;
    case 20:
      Serial.println("ETH Connected");
      break;
    case 22:
      Serial.print("ETH MAC: ");
      Serial.print(ETH.macAddress());
      Serial.print(", IPv4: ");
      Serial.print(ETH.localIP());
      if (ETH.fullDuplex()) {
        Serial.print(", FULL_DUPLEX");
      }
      Serial.print(", ");
      Serial.print(ETH.linkSpeed());
      Serial.println("Mbps");
      connected = true;
      break;
    case 21:
      Serial.println("ETH Disconnected");
      connected = false;
      break;
    case SYSTEM_EVENT_ETH_STOP:
      Serial.println("ETH Stopped");
      connected = false;
      break;
    #endif
    default:
      break;
  }
}

#if ETHERNET
void initEthernet() {
  ETH.begin(ETH_ADDR, ETH_POWER_PIN, ETH_MDC_PIN, ETH_MDIO_PIN, ETH_TYPE, ETH_CLK_MODE);
}
#endif

#if GRID
void gridDetect() {
  currentState = digitalRead(settings.gridpin);

  if (lastState == HIGH && currentState == LOW) {
    pressedTime = millis();
    gridOnlineNotify = true;
    gridOfflineNotify = false;
    Serial.println("on");
  }
  if (lastState == LOW && currentState == HIGH) {
    pressedTime = millis();
    gridOnlineNotify = false;
    gridOfflineNotify = true;
    Serial.println("off");
  }
  if (lastState == INIT) {
    pressedTime = millis();
    Serial.println("grid init");
    switch (currentState) {
      case HIGH: {
        Serial.println("init off");
        gridOnlineNotify = false;
        gridOfflineNotify = true;
        break;
      }
      case LOW: {
        Serial.println("init on");
        gridOnlineNotify = true;
        gridOfflineNotify = false;
        break;
      }
    }
  }

  long changeDuration = millis() - pressedTime;

  if (gridOnlineNotify && changeDuration > REACTION_TIME && currentState == LOW && gridStatus != HIGH) {
    Serial.println("Grid ONLINE");
    gridOnlineNotify = false;
    gridStatus = HIGH;
  }
  if (gridOfflineNotify && changeDuration > REACTION_TIME && currentState == HIGH && gridStatus != LOW) {
    Serial.println("Grid OFFLINE");
    gridOfflineNotify = false;
    gridStatus = LOW;
  }

  lastState = currentState;
}
#endif


void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("");
  Serial.println("Setup start");
  pinMode(settings.gridpin, INPUT_PULLUP);
  initChipID();
  WiFi.onEvent(Events);
  #if ETHERNET
  initEthernet();
  #endif
  #if WIFI
  initWifi();
  #endif
  Serial.println("Setup complete");
}

void loop() {

  #if WIFI
  wm.process();
  #endif

  #if GRID
  if (connected) {
    gridDetect();
  }
  #endif
  delay(200);
}