#define ETHERNET 0
#define WIFI 1
#define GRID 1

const PROGMEM char* VERSION = "1.0";

#include <Preferences.h>
#include <ArduinoWebsockets.h>
#include <async.h>
#include <map>
#include <ArduinoJson.h>
#include <ArduinoWebsockets.h>

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

using namespace websockets;
WebsocketsClient  client_websocket;
Async             asyncEngine = Async(20);

struct Settings {
  const char*   apssid                = "GridDetector";
  const char*   softwareversion       = VERSION;
  int           gridpin               = 2;
  char          identifier[51]        = "test_01";
  char          devicename[31]        = "Grid Detector";
  char          broadcastname[31]     = "griddetector";
  int           ws_alert_time         = 150000;
  int           ws_reboot_time        = 300000;
  char          serverhost[31]        = "alerts.net.ua";
  int           websocket_port        = 39447;
};

struct Firmware {
  int major = 0;
  int minor = 0;
  int patch = 0;
  int betaBuild = 0;
  bool isBeta = false;
};

Settings settings;
Firmware currentFirmware;

static bool connected = false;
int lastState = INIT;
int currentState;
bool gridOnlineNotify;
bool gridOfflineNotify;
bool    apiConnected;
int gridStatus = INIT;
unsigned long pressedTime = 0;
char chipID[13];
char localIP[16];
bool    websocketReconnect = false;
time_t  websocketLastPingTime = 0;
char    currentFwVersion[25];

#define REACTION_TIME 2000

Firmware parseFirmwareVersion(const char *version) {

  Firmware firmware;

  char* versionCopy = strdup(version);
  char* token = strtok(versionCopy, ".-");

  while (token) {
    if (isdigit(token[0])) {
      if (firmware.major == 0)
        firmware.major = atoi(token);
      else if (firmware.minor == 0)
        firmware.minor = atoi(token);
      else if (firmware.patch == 0)
        firmware.patch = atoi(token);
    } else if (firmware.betaBuild == 0 && token[0] == 'b' && strcmp(token, "bin") != 0) {
      firmware.isBeta = true;
      firmware.betaBuild = atoi(token + 1); // Skip the 'b' character
    }
    token = strtok(NULL, ".-");
  }

  free(versionCopy);

  return firmware;
}

void fillFwVersion(char* result, Firmware firmware) {
  char patch[5] = "";
  if (firmware.patch > 0) {
    sprintf(patch, ".%d", firmware.patch);
  }
  char beta[5] = "";
  if (firmware.isBeta) {
    sprintf(beta, "-b%d", firmware.betaBuild);
  }
#if LITE
  sprintf(result, "%d.%d%s%s-lite", firmware.major, firmware.minor, patch, beta);
#else
  sprintf(result, "%d.%d%s%s", firmware.major, firmware.minor, patch, beta);
#endif

}

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
    client_websocket.send("grid:online");
  }
  if (gridOfflineNotify && changeDuration > REACTION_TIME && currentState == HIGH && gridStatus != LOW) {
    Serial.println("Grid OFFLINE");
    gridOfflineNotify = false;
    gridStatus = LOW;
    client_websocket.send("grid:offline");
  }

  lastState = currentState;
}
#endif

void websocketProcess() {
  if (millis() - websocketLastPingTime > settings.ws_alert_time) {
    websocketReconnect = true;
  }
  if (millis() - websocketLastPingTime > settings.ws_reboot_time) {
    rebootDevice(3000);
  }
  if (!client_websocket.available() or websocketReconnect) {
    Serial.println("Reconnecting...");
    socketConnect();
  }
}

JsonDocument parseJson(const char* payload) {
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    Serial.printf("Deserialization error: $s\n", error.f_str());
    return doc;
  } else {
    return doc;
  }
}

void socketConnect() {
  Serial.println("connection start...");
  client_websocket.onMessage(onMessageCallback);
  client_websocket.onEvent(onEventsCallback);
  long startTime = millis();
  char webSocketUrl[100];
  sprintf(webSocketUrl, "ws://%s:%d/grid_detector", settings.serverhost, settings.websocket_port);
  Serial.println(webSocketUrl);
  client_websocket.connect(webSocketUrl);
  if (client_websocket.available()) {
    Serial.print("connection time - ");
    Serial.print(millis() - startTime);
    Serial.println("ms");
    char firmwareInfo[100];
    sprintf(firmwareInfo, "firmware:%s_%s", currentFwVersion, settings.identifier);
    Serial.println(firmwareInfo);
    client_websocket.send(firmwareInfo);
    char chipIdInfo[25];
    sprintf(chipIdInfo, "chip_id:%s", chipID);
    Serial.println(chipIdInfo);
    client_websocket.send(chipIdInfo);

    client_websocket.ping();
    websocketReconnect = false;
    Serial.println("connected");
  } else {
    Serial.println("not connected");
  }
}

void onMessageCallback(WebsocketsMessage message) {
  Serial.print("Got Message: ");
  Serial.println(message.data());
  JsonDocument data = parseJson(message.data().c_str());
  String payload = data["payload"];
  if (!payload.isEmpty()) {
    if (payload == "ping") {
      Serial.println("Heartbeat from server");
      websocketLastPingTime = millis();
    }
  }
}

void onEventsCallback(WebsocketsEvent event, String data) {
  if (event == WebsocketsEvent::ConnectionOpened) {
    apiConnected = true;
    Serial.println("connnection opened");
    websocketLastPingTime = millis();
  } else if (event == WebsocketsEvent::ConnectionClosed) {
    apiConnected = false;
    Serial.println("connnection closed");
  } else if (event == WebsocketsEvent::GotPing) {
    Serial.println("got websocket ping");
    client_websocket.pong();
    client_websocket.send("pong");
    Serial.println("answered pong");
    websocketLastPingTime = millis();
  } else if (event == WebsocketsEvent::GotPong) {
    Serial.println("got websocket pong");
  }
}


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
  currentFirmware = parseFirmwareVersion(VERSION);
  fillFwVersion(currentFwVersion, currentFirmware);

  Serial.println("Setup complete");
  socketConnect();

  asyncEngine.setInterval(websocketProcess, 3000);
}

void loop() {
  asyncEngine.run();
  client_websocket.poll();
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