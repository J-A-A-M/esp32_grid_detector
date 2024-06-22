#define ETHERNET 0
#define WIFI 1
#define GRID 1

const PROGMEM char* VERSION = "0.0.1";

#include <Preferences.h>
#include <ArduinoWebsockets.h>
#include <async.h>
#include <map>
#include <ArduinoJson.h>
#include <HTTPUpdate.h>

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
#endif
WiFiClient      client;

using namespace   websockets;
WebsocketsClient  client_websocket;
Async             asyncEngine = Async(20);

struct Settings {
  const char*   apssid                = "GridDetector";
  const char*   softwareversion       = VERSION;
  int           gridpin               = 2;

  // ------- web config start
  char          identifier[51]        = "tr1-2";
  char          devicename[31]        = "Grid Detector";
  char          broadcastname[31]     = "griddetector";
  int           ws_alert_time         = 150000;
  int           ws_reboot_time        = 300000;
  char          serverhost[31]        = "alerts.net.ua";
  int           websocket_port        = 39447;
  int           updateport            = 9090;
  int           reaction_time         = 2000;
  // ------- web config end
};

Settings    settings;
Preferences preferences;

time_t  pressedTime = 0;
time_t  websocketLastPingTime = 0;
int     lastState = INIT;
int     gridStatus = INIT;
int     currentState;
bool    connected = false;
bool    gridOnlineNotify;
bool    gridOfflineNotify;
bool    apiConnected;
bool    websocketReconnect = false;
char    chipID[13];
char    localIP[16];
String  newFirmwareUrl = "";
long    updateTaskId = -1;

void rebootDevice(int time = 2000) {
  Serial.printf("reboot in %d seconds\n", time / 1000);
  delay(time);
  ESP.restart();
}

void initChipID() {
  uint64_t chipid = ESP.getEfuseMac();
  sprintf(chipID, "%04x%04x", (uint32_t)(chipid >> 32), (uint32_t)chipid);
  Serial.printf("ChipID inited: '%s'\n", chipID);
}

void initSettings() {
  Serial.print("init settings\n");
  preferences.begin("storage", true);

  preferences.getString("dn", settings.devicename, sizeof(settings.devicename));
  preferences.getString("bn", settings.broadcastname, sizeof(settings.broadcastname));
  preferences.getString("host", settings.serverhost, sizeof(settings.serverhost));
  preferences.getString("id", settings.identifier, sizeof(settings.identifier));
  settings.websocket_port   = preferences.getInt("wsp", settings.websocket_port);
  settings.updateport       = preferences.getInt("upport", settings.updateport);
  settings.ws_alert_time    = preferences.getInt("wsat", settings.ws_alert_time);
  settings.ws_reboot_time   = preferences.getInt("wsrt", settings.ws_reboot_time);
  settings.reaction_time    = preferences.getInt("rt", settings.reaction_time);

  preferences.end();

  Serial.printf("current firmware version: %s\n", VERSION);
}

String connectMode() {
  #if ETHERNET
    return "ethernet";
  #endif
  #if WIFI
    return "wifi";
  #endif
}

#if WIFI
char* getLocalIP() {
  strcpy(localIP, WiFi.localIP().toString().c_str());
  return localIP;
}

void apCallback(WiFiManager* wifiManager) {
  const char* message = wifiManager->getConfigPortalSSID().c_str();
  Serial.printf("connect to: %s\n", message);
  WiFi.onEvent(Events);
}

void saveConfigCallback() {
  Serial.printf("saved AP: %s\n", wm.getWiFiSSID(true).c_str());
  delay(2000);
  rebootDevice();
}

void initWifi() {
  Serial.print("init wifi\n");
  WiFi.mode(WIFI_STA); 

  wm.setHostname(settings.broadcastname);
  wm.setTitle(settings.devicename);
  wm.setConfigPortalBlocking(true);
  wm.setConnectTimeout(3);
  wm.setConnectRetries(10);
  wm.setAPCallback(apCallback);
  wm.setSaveConfigCallback(saveConfigCallback);
  wm.setConfigPortalTimeout(180);
  Serial.printf("connecting to: %s\n", wm.getWiFiSSID(true).c_str());
  if (!wm.autoConnect(settings.apssid)) {
    Serial.print("reboot\n");
    rebootDevice(5000);
    return;
  }
  Serial.print("wifi initialized with DHCP\n");
  wm.setHttpPort(80);
  wm.startWebPortal();
  Serial.printf("ip address: %s\n", getLocalIP());
  connected = true;
}
#endif

void Events(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED: {
      char softApIp[16];
      strcpy(softApIp, WiFi.softAPIP().toString().c_str());
      Serial.printf("set in browser: %s\n", softApIp);
      WiFi.removeEvent(Events);
      break;
    }
    #if ETHERNET
    case 18:
      Serial.print("ETH started\n");
      //set eth hostname here
      ETH.setHostname("esp32-ethernet");
      break;
    case 20:
      Serial.print("ETH connected\n");
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
      Serial.println(" Mbps");
      connected = true;
      break;
    case 21:
      Serial.print("ETH disconnected\n");
      connected = false;
      break;
    case SYSTEM_EVENT_ETH_STOP:
      Serial.print("ETH stopped\n");
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
  while(!connected) {
    delay(1000);
  }
  connected = true;
}
#endif

void initUpdate() {
  Serial.println("init update");
  httpUpdate.onStart([]() {
    Serial.println("update start");
    client_websocket.send("update:started");
  });
  httpUpdate.onEnd([]() {
    Serial.println("update done");
    client_websocket.send("update:done");
    rebootDevice();
  });
  httpUpdate.onProgress([](int progress, int total) {
    Serial.printf("update progress: %d%%\n", (progress * 100) / total);
  });
  httpUpdate.onError([](int error) {
    client_websocket.send(String("update:error_") + error);
    Serial.printf("error: %d\n", error);
    updateTaskId = -1;
    newFirmwareUrl.clear();
  });
}

void updateFw() {
  if (newFirmwareUrl.isEmpty()) {
    Serial.println("no firmware url");
    return;
  }
  Serial.printf("Firmware url: %s\n", newFirmwareUrl.c_str());
  t_httpUpdate_return fwRet = httpUpdate.update(client, newFirmwareUrl.c_str(), VERSION);
  handleUpdateStatus(fwRet);
}

void handleUpdateStatus(t_httpUpdate_return ret) {
  Serial.println("Firmware update status:");
  switch (ret) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("Error Occurred. Error (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
      break;
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("HTTP_UPDATE_NO_UPDATES");
      break;
    case HTTP_UPDATE_OK:
      Serial.println("Firmware update successfully completed. Rebooting...");
      break;
  }
}

#if GRID
void gridDetect() {
  currentState = digitalRead(settings.gridpin);

  if (lastState == HIGH && currentState == LOW) {
    pressedTime = millis();
    gridOnlineNotify = true;
    gridOfflineNotify = false;
    Serial.print("on\n");
  }
  if (lastState == LOW && currentState == HIGH) {
    pressedTime = millis();
    gridOnlineNotify = false;
    gridOfflineNotify = true;
    Serial.print("off\n");
  }
  if (lastState == INIT) {
    pressedTime = millis();
    Serial.print("grid init\n");
    switch (currentState) {
      case HIGH: {
        Serial.print("init off\n");
        gridOnlineNotify = false;
        gridOfflineNotify = true;
        break;
      }
      case LOW: {
        Serial.print("init on\n");
        gridOnlineNotify = true;
        gridOfflineNotify = false;
        break;
      }
    }
  }

  long changeDuration = millis() - pressedTime;

  if (gridOnlineNotify && changeDuration > settings.reaction_time && currentState == LOW && gridStatus != HIGH) {
    Serial.print("grid ONLINE\n");
    gridOnlineNotify = false;
    gridStatus = HIGH;
    client_websocket.send("grid:online");
  }
  if (gridOfflineNotify && changeDuration > settings.reaction_time && currentState == HIGH && gridStatus != LOW) {
    Serial.print("grid OFFLINE\n");
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
    Serial.print("reconnecting...\n");
    socketConnect();
    if (client_websocket.available()) {
      lastState = INIT;
      gridStatus = INIT;
    }
  }
}

JsonDocument parseJson(const char* payload) {
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    Serial.printf("deserialization error: $s\n", error.f_str());
    return doc;
  } else {
    return doc;
  }
}

void socketConnect() {
  Serial.print("websocket connection start\n");
  client_websocket.onMessage(onMessageCallback);
  client_websocket.onEvent(onEventsCallback);
  long startTime = millis();
  char webSocketUrl[100];
  sprintf(webSocketUrl, "ws://%s:%d/grid_detector", settings.serverhost, settings.websocket_port);
  Serial.printf("websoket url: %s\n", webSocketUrl);
  client_websocket.connect(webSocketUrl);
  if (client_websocket.available()) {
    long connectTime = millis() - startTime;
    char connectTime_c[12];
    sprintf(connectTime_c, "%ld", connectTime);
    Serial.printf("websocket connection time: %s ms\n", connectTime_c);
    char nodeInfo[100];
    sprintf(nodeInfo, "node:%s", settings.identifier);
    Serial.printf("node: %s\n", settings.identifier);
    client_websocket.send(nodeInfo);
    char firmwareInfo[100];
    sprintf(firmwareInfo, "firmware:%s", VERSION);
    Serial.printf("sent firmware info: %s\n", VERSION);
    client_websocket.send(firmwareInfo);
    char chipIdInfo[25];
    sprintf(chipIdInfo, "chip_id:%s", chipID);
    Serial.printf("chip_id: %s\n", chipID);
    client_websocket.send(chipIdInfo);
    char connectInfo[25];
    sprintf(connectInfo, "connect_mode:%s", connectMode());
    Serial.printf("connect_mode: %s\n", connectMode());
    client_websocket.send(connectInfo);

    client_websocket.ping();
    websocketReconnect = false;
    Serial.print("websocket connected\n");
  } else {
    Serial.print("websocket not connected\n");
  }
}

void onMessageCallback(WebsocketsMessage message) {
  JsonDocument data = parseJson(message.data().c_str());
  Serial.printf("got message: %s\n", message.data().c_str());
  String payload = data["payload"];
  if (!payload.isEmpty()) {
    if (payload == "ping") {
      Serial.print("heartbeat from server\n");
      websocketLastPingTime = millis();
    } else if (payload == "update") {
      Serial.println("update firmware");
      unsigned long delayTime = int(data["delay"]) * 1000;
      Serial.print("delay time: ");
      Serial.println(delayTime);
      newFirmwareUrl = data["url"].as<String>();
      Serial.print("new firmware url: ");
      Serial.println(newFirmwareUrl.c_str());
      if (updateTaskId != -1) {
        asyncEngine.clearInterval(updateTaskId);
      }
      updateTaskId = asyncEngine.setTimeout(updateFw, delayTime);
      Serial.printf("Scheduled update task with id: %d in %d seconds\n", updateTaskId, delayTime/1000);
    } else if (payload == "update_cancel") {
      Serial.println("update cancel");
      if (updateTaskId != -1) {
        asyncEngine.clearInterval(updateTaskId);
      }
      client_websocket.send("update:canceled");
    } else if (payload == "reboot") {
      Serial.println("rebooting...");
      rebootDevice(3000);
    }
  }
}

void onEventsCallback(WebsocketsEvent event, String data) {
  if (event == WebsocketsEvent::ConnectionOpened) {
    apiConnected = true;
    Serial.print("websocket connnection opened\n");
    websocketLastPingTime = millis();
  } else if (event == WebsocketsEvent::ConnectionClosed) {
    apiConnected = false;
    Serial.print("websocket connnection closed\n");
  } else if (event == WebsocketsEvent::GotPing) {
    Serial.print("got websocket ping\n");
    client_websocket.pong();
    client_websocket.send("pong");
    Serial.print("answered pong\n");
    websocketLastPingTime = millis();
  } else if (event == WebsocketsEvent::GotPong) {
    Serial.print("got websocket pong\n");
  }
}


void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.print("\n\nsetup start\n");
  pinMode(settings.gridpin, INPUT_PULLUP);
  initChipID();
  initSettings();
  WiFi.onEvent(Events);
  #if ETHERNET
  initEthernet();
  #endif
  #if WIFI
  initWifi();
  #endif
  initUpdate();

  Serial.print("setup complete\n");
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