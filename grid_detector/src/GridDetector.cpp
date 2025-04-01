#define ETHERNET 0
#define WIFI 1
#define GRID 1
#define ARDUINO_OTA_ENABLED 1

#include <Preferences.h>
#include <ArduinoWebsockets.h>
#include <async.h>
#include <map>
#include <ArduinoJson.h>
#include <HTTPUpdate.h>
#if ARDUINO_OTA_ENABLED
#include <ArduinoOTA.h>
#endif

const PROGMEM char* VERSION = "0.0.4";

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
  char          identifier[51]        = "test";
  char          devicename[31]        = "Grid Detector";
  char          broadcastname[31]     = "griddetector";
  int           ws_alert_time         = 150000;
  int           ws_reboot_time        = 300000;
  char          serverhost[31]        = "alerts.net.ua";
  int           websocket_port        = 39447;
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
  Serial.printf("Reboot in %d seconds\n", time / 1000);
  delay(time);
  ESP.restart();
}

void initChipID() {
  uint64_t chipid = ESP.getEfuseMac();
  sprintf(chipID, "%04x%04x", (uint32_t)(chipid >> 32), (uint32_t)chipid);
  Serial.printf("ChipID inited: '%s'\n", chipID);
}

void initSettings() {
  Serial.print("Init settings\n");
  preferences.begin("storage", true);

  preferences.getString("dn", settings.devicename, sizeof(settings.devicename));
  preferences.getString("bn", settings.broadcastname, sizeof(settings.broadcastname));
  preferences.getString("host", settings.serverhost, sizeof(settings.serverhost));
  preferences.getString("id", settings.identifier, sizeof(settings.identifier));
  settings.websocket_port   = preferences.getInt("wsp", settings.websocket_port);
  settings.ws_alert_time    = preferences.getInt("wsat", settings.ws_alert_time);
  settings.ws_reboot_time   = preferences.getInt("wsrt", settings.ws_reboot_time);
  settings.reaction_time    = preferences.getInt("rt", settings.reaction_time);

  preferences.end();

  Serial.printf("Node inited: %s\n", settings.identifier);
  Serial.printf("Current firmware version: %s\n", VERSION);

}

String connectMode() {
  #if ETHERNET
    return "ethernet";
  #endif
  #if WIFI
    return "wifi";
  #endif
}

void Events(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED: {
      char softApIp[16];
      strcpy(softApIp, WiFi.softAPIP().toString().c_str());
      Serial.printf("Set in browser: %s\n", softApIp);
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

#if WIFI
char* getLocalIP() {
  strcpy(localIP, WiFi.localIP().toString().c_str());
  return localIP;
}

void apCallback(WiFiManager* wifiManager) {
  const char* message = wifiManager->getConfigPortalSSID().c_str();
  Serial.printf("Connect to: %s\n", message);
  WiFi.onEvent(Events);
}

void saveConfigCallback() {
  Serial.printf("Saved AP: %s\n", wm.getWiFiSSID(true).c_str());
  delay(2000);
  rebootDevice();
}

void initWifi() {
  Serial.print("Init WIFI\n");
  WiFi.mode(WIFI_STA); 

  wm.setHostname(settings.broadcastname);
  wm.setTitle(settings.devicename);
  wm.setConfigPortalBlocking(true);
  wm.setConnectTimeout(3);
  wm.setConnectRetries(10);
  wm.setAPCallback(apCallback);
  wm.setSaveConfigCallback(saveConfigCallback);
  wm.setConfigPortalTimeout(180);
  Serial.printf("Connecting to: %s\n", wm.getWiFiSSID(true).c_str());
  if (!wm.autoConnect(settings.apssid)) {
    Serial.print("Reboot\n");
    rebootDevice(5000);
    return;
  }
  Serial.print("WIFI initialized with DHCP\n");
  wm.setHttpPort(80);
  wm.startWebPortal();
  Serial.printf("IP address: %s\n", getLocalIP());
  connected = true;
}
#endif

#if ETHERNET
void initEthernet() {
  ETH.begin(ETH_ADDR, ETH_POWER_PIN, ETH_MDC_PIN, ETH_MDIO_PIN, ETH_TYPE, ETH_CLK_MODE);
  while(!connected) {
    delay(1000);
  }
  connected = true;
}
#endif

void initUpdates() {
  Serial.println("Init update");
  #if ARDUINO_OTA_ENABLED
  ArduinoOTA.onStart([]() {
    Serial.println("OTA update start");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("OTA update done");
    rebootDevice();
  });
  ArduinoOTA.onProgress([](int progress, int total) {
    Serial.printf("OTA update progress: %d%%\n", (progress * 100) / total);
  });
  ArduinoOTA.onError([](int error) {
    Serial.printf("OTA update error: %d\n", error);
  });
  ArduinoOTA.begin();
  #endif

  httpUpdate.onStart([]() {
    Serial.println("HTTP update start");
    client_websocket.send("update:started");
  });
  httpUpdate.onEnd([]() {
    Serial.println("HTTP update done");
    client_websocket.send("update:done");
    rebootDevice();
  });
  httpUpdate.onProgress([](int progress, int total) {
    Serial.printf("HTTP update progress: %d%%\n", (progress * 100) / total);
  });
  httpUpdate.onError([](int error) {
    client_websocket.send(String("update:error_") + error);
    Serial.printf("HTTP update error: %d\n", error);
    updateTaskId = -1;
    newFirmwareUrl.clear();
  });
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

void updateFw() {
  if (newFirmwareUrl.isEmpty()) {
    Serial.println("No firmware url");
    return;
  }
  Serial.printf("Firmware url: %s\n", newFirmwareUrl.c_str());
  t_httpUpdate_return fwRet = httpUpdate.update(client, newFirmwareUrl.c_str(), VERSION);
  handleUpdateStatus(fwRet);
}

#if GRID
void gridDetect() {
  currentState = digitalRead(settings.gridpin);

  if (lastState == HIGH && currentState == LOW) {
    pressedTime = millis();
    gridOnlineNotify = true;
    gridOfflineNotify = false;
    Serial.print("Grid on\n");
  }
  if (lastState == LOW && currentState == HIGH) {
    pressedTime = millis();
    gridOnlineNotify = false;
    gridOfflineNotify = true;
    Serial.print("Grid off\n");
  }
  if (lastState == INIT) {
    pressedTime = millis();
    Serial.print("Grid init\n");
    switch (currentState) {
      case HIGH: {
        Serial.print("Grid init off\n");
        gridOnlineNotify = false;
        gridOfflineNotify = true;
        break;
      }
      case LOW: {
        Serial.print("Grid init on\n");
        gridOnlineNotify = true;
        gridOfflineNotify = false;
        break;
      }
    }
  }

  long changeDuration = millis() - pressedTime;

  if (gridOnlineNotify && changeDuration > settings.reaction_time && currentState == LOW && gridStatus != HIGH) {
    gridOnlineNotify = false;
    gridStatus = HIGH;
    client_websocket.send("grid:online");
    Serial.print("Grid ONLINE event sended\n");
  }
  if (gridOfflineNotify && changeDuration > settings.reaction_time && currentState == HIGH && gridStatus != LOW) {
    gridOfflineNotify = false;
    gridStatus = LOW;
    client_websocket.send("grid:offline");
    Serial.print("grid OFFLINE event sended\n");
  }

  lastState = currentState;
}
#endif

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

void onMessageCallback(WebsocketsMessage message) {
  JsonDocument data = parseJson(message.data().c_str());
  Serial.printf("Got message: %s\n", message.data().c_str());
  String payload = data["payload"];
  if (!payload.isEmpty()) {
    if (payload == "ping") {
      Serial.print("Heartbeat from server\n");
      websocketLastPingTime = millis();
    } else if (payload == "update") {
      Serial.println("Update firmware");
      unsigned long delayTime = int(data["delay"]) * 1000;
      Serial.print("Delay time: ");
      Serial.println(delayTime);
      newFirmwareUrl = data["url"].as<String>();
      Serial.print("New firmware url: ");
      Serial.println(newFirmwareUrl.c_str());
      if (updateTaskId != -1) {
        asyncEngine.clearInterval(updateTaskId);
      }
      updateTaskId = asyncEngine.setTimeout(updateFw, delayTime);
      Serial.printf("Scheduled update task with id: %d in %d seconds\n", updateTaskId, delayTime/1000);
    } else if (payload == "update_cancel") {
      Serial.println("Update cancel");
      if (updateTaskId != -1) {
        asyncEngine.clearInterval(updateTaskId);
      }
      client_websocket.send("update:canceled");
    } else if (payload == "reboot") {
      Serial.println("Rebooting...");
      rebootDevice(3000);
    }
  }
}

void onEventsCallback(WebsocketsEvent event, String data) {
  if (event == WebsocketsEvent::ConnectionOpened) {
    apiConnected = true;
    Serial.print("Websocket connnection opened\n");
    websocketLastPingTime = millis();
  } else if (event == WebsocketsEvent::ConnectionClosed) {
    apiConnected = false;
    Serial.print("Websocket connnection closed\n");
  } else if (event == WebsocketsEvent::GotPing) {
    Serial.print("Got websocket ping\n");
    client_websocket.pong();
    client_websocket.send("pong");
    Serial.print("Answered pong\n");
    websocketLastPingTime = millis();
  } else if (event == WebsocketsEvent::GotPong) {
    Serial.print("Got websocket pong\n");
  }
}

void socketConnect() {
  Serial.print("Websocket connection start\n");
  client_websocket.onMessage(onMessageCallback);
  client_websocket.onEvent(onEventsCallback);
  long startTime = millis();
  char webSocketUrl[100];
  sprintf(webSocketUrl, "ws://%s:%d/grid_detector", settings.serverhost, settings.websocket_port);
  Serial.printf("Websoket URL: %s\n", webSocketUrl);
  client_websocket.connect(webSocketUrl);
  if (client_websocket.available()) {
    long connectTime = millis() - startTime;
    char connectTime_c[12];
    sprintf(connectTime_c, "%ld", connectTime);
    Serial.printf("Websocket connection time: %s ms\n", connectTime_c);
    char nodeInfo[100];
    sprintf(nodeInfo, "node:%s", settings.identifier);
    Serial.printf("Sent node info: %s\n", settings.identifier);
    client_websocket.send(nodeInfo);
    char firmwareInfo[100];
    sprintf(firmwareInfo, "firmware:%s", VERSION);
    Serial.printf("Sent firmware info: %s\n", VERSION);
    client_websocket.send(firmwareInfo);
    char chipIdInfo[25];
    sprintf(chipIdInfo, "chip_id:%s", chipID);
    Serial.printf("Sent chipID info: %s\n", chipID);
    client_websocket.send(chipIdInfo);
    char connectInfo[25];
    sprintf(connectInfo, "connect_mode:%s", connectMode());
    Serial.printf("Sent connect_mode info: %s\n", connectMode());
    client_websocket.send(connectInfo);

    client_websocket.ping();
    websocketReconnect = false;
    Serial.print("Websocket connected\n");
  } else {
    Serial.print("Websocket not connected\n");
  }
}

void websocketProcess() {
  if (millis() - websocketLastPingTime > settings.ws_alert_time) {
    websocketReconnect = true;
  }
  if (millis() - websocketLastPingTime > settings.ws_reboot_time) {
    rebootDevice(3000);
  }
  if (!client_websocket.available() or websocketReconnect) {
    Serial.print("Reconnecting...\n");
    socketConnect();
    if (client_websocket.available()) {
      lastState = INIT;
      gridStatus = INIT;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.print("\n-----\nSetup start\n-----\n\n");
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
  initUpdates();

  Serial.print("\n-----\nSetup complete\n-----\n\n");
  socketConnect();

  asyncEngine.setInterval(websocketProcess, 3000);
}

void loop() {
  asyncEngine.run();
  client_websocket.poll();

  #if WIFI
  wm.process();
  #endif

  #if ARDUINO_OTA_ENABLED
  ArduinoOTA.handle();
  #endif

  #if GRID
  if (connected) {
    gridDetect();
  }
  #endif

  delay(200);
}