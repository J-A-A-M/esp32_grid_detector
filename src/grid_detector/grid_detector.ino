#define ETHERNET 0
#define PING 0

#if ETHERNET
#include <Ethernet.h>
#include <EthernetClient.h>
byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED }; 
EthernetClient client;
#endif

#if PING
#include <ICMPPing.h>
IPAddress remote_ip(8, 8, 8, 8);
SOCKET pingSocket = 0;
ICMPPing ping(pingSocket, (uint16_t)random(0, 255));
#endif

const int buttonPin = 2;

int lastState = LOW;
int currentState;
bool gridOnlineNotify;
bool gridOfflineNotify;
int gridStatus;
bool isInit = true;
unsigned long pressedTime = 0;

#define REACTION_TIME 2000

#if ETHERNET
void ethernetInit(){
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


#if PING 
void googlePing(){
  ICMPEchoReply echoReply = ping(remote_ip, 4);

  switch (echoReply.status) {
    case SUCCESS:
      Serial.print("Ping to ");
      Serial.print(remote_ip);
      Serial.print(" successful, time=");
      Serial.print(echoReply.time);
      Serial.println("ms");
      break;
    case TIMED_OUT:
      Serial.println("Ping timed out");
      break;
    case DESTINATION_UNREACHABLE:
      Serial.println("Destination unreachable");
      break;
    case UNKNOWN:
    default:
      Serial.println("Ping failed");
      break;
  }
}
#endif


void setup() {
  Serial.begin(115200);
  pinMode(buttonPin, INPUT_PULLUP);
  #if ETHERNET
  ethernetInit();
  #endif

}

void loop() {

  #if PING 
  googlePing();
  #endif

  currentState = digitalRead(buttonPin);

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
  delay(200);
}