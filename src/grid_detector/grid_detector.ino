#define ETHERNET 0
#define WIFI 0
#define GRID 1

const PROGMEM char* VERSION = "0.1";

#include <Preferences.h>

#if ETHERNET
#include <Ethernet.h>
#include <EthernetClient.h>
byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED }; 
EthernetClient  client;
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

int lastState = LOW;
int currentState;
bool gridOnlineNotify;
bool gridOfflineNotify;
int gridStatus;
bool isInit = true;
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

#if ETHERNET
void initEthernet(){
  Serial.println("Ethernet init");
  if (Ethernet.hardwareStatus() == EthernetNoHardware) {
    Serial.println("Ethernet shield was not found. Sorry, can't run without hardware. :(");
    while (true) {
      delay(1);
    }
  }
  if (Ethernet.linkStatus() == LinkOFF) {
    Serial.println("Ethernet cable is not connected.");
    while (true) {
      delay(1);
    }
  }
  if (Ethernet.begin(mac) == 0) {
    Serial.println("Failed to configure Ethernet using DHCP");
    while (true) {
      delay(1);
    }
  } else {
    Serial.println("Ethernet initialized with DHCP.");
    Serial.print("IP Address: ");
    Serial.println(Ethernet.localIP());
  }
}
#endif

#if WIFI
char* getLocalIP() {
  strcpy(localIP, WiFi.localIP().toString().c_str());
  return localIP;
}

void apCallback(WiFiManager* wifiManager) {
  const char* message = wifiManager->getConfigPortalSSID().c_str();
  Serial.print("connect to: ");
  Serial.println(message);
  WiFi.onEvent(wifiEvents);
}

void saveConfigCallback() {
  Serial.print("saved AP: ");
  Serial.println(wm.getWiFiSSID(true).c_str());
  delay(2000);
  rebootDevice();
}

static void wifiEvents(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED: {
      char softApIp[16];
      strcpy(softApIp, WiFi.softAPIP().toString().c_str());
      Serial.print("set in browser: ");
      Serial.println(softApIp);
      WiFi.removeEvent(wifiEvents);
      break;
    }
    default:
      break;
  }
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
}
#endif

#if GRID
void gridDetect() {
  currentState = digitalRead(settings.gridpin);

  if (!isInit){
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
  }else{
    isInit = false;
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
  Serial.println("");
  Serial.println("Setup start");
  pinMode(settings.gridpin, INPUT_PULLUP);
  initChipID();
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
  gridDetect();
  #endif

  delay(200);
}