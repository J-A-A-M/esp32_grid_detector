const int buttonPin = 2;

int lastState = LOW;
int currentState;
bool gridOnlineNotify;
bool gridOfflineNotify;
int gridStatus;
bool isInit = true;
unsigned long pressedTime = 0;

#define REACTION_TIME 2000

void setup() {
  Serial.begin(115200);
  pinMode(buttonPin, INPUT_PULLUP);
}

void loop() {
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