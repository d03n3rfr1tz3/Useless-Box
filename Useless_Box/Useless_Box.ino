
#include "Arduino.h"
#include "ArduinoOTA.h"
#include "AsyncMqttClient.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "ESP32Servo.h"
#include "RunningMedian.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "Tone32.h"
#include "WiFi.h"

#include "config.h"

RTC_DATA_ATTR volatile int triggerCount = 0;
AsyncMqttClient mqttClient;
Servo coverServo;
Servo handServo;
unsigned long startMillis = millis();
unsigned long idleMillis = millis();
volatile bool lastOTA = false;
volatile bool lastWifi = false;
volatile bool lastMQTT = false;
volatile int retryWifi = 0;
volatile int retryMQTT = 0;

RunningMedian batteryMedian = RunningMedian(BATTERY_MEDIAN);
TaskHandle_t triggerLightHandle;
TaskHandle_t triggerServoHandle;
TaskHandle_t peekServoHandle;
TaskHandle_t triggerBuzzerHandle;
TaskHandle_t peekBuzzerHandle;
static void IRAM_ATTR triggerSwitch(void);
static void IRAM_ATTR triggerRadar(void);
volatile bool isRadarActive = false;
volatile bool isSwitchActive = false;
volatile bool isLightTrigger = false;
volatile bool isLightTriggered = false;
volatile bool isBuzzerPeek = false;
volatile bool isBuzzerPeeked = false;
volatile bool isBuzzerTrigger = false;
volatile bool isBuzzerTriggered = false;
volatile bool isServoPeek = false;
volatile bool isServoPeeked = false;
volatile bool isServoTrigger = false;
volatile bool isServoTriggered = false;
volatile int number = 0;

/****************\
|*     ESP32    *|
\****************/

void setup() {
  Serial.begin(115200);
  randomSeed(analogRead(0));
  setCpuFrequencyMhz(80);
  ++triggerCount;
  
  initWiFi();
  initMQTT();
  initOTA();
  initRadar();
  initSwitch();
  initLight();
  initBuzzer();
  initServo();
}

void loop() {
  if (getElapsed(startMillis) > 15000) {
    startMillis = millis();
    
    checkWiFi(false);
    checkMQTT(false);
    checkOTA();
    loopMQTT();
  }
  
  if (getElapsed(idleMillis) > 120000) {
    disposeServo();
    disposeBuzzer();
    initSleep();
  } else if (isSwitchActive || lastOTA) {
    idleMillis = millis();
  }

  loopOTA();
  loopLight();
  loopServo();
  loopBuzzer();
  
  delay(25);
}

/****************\
|*    Sleep     *|
\****************/

void initSleep(void) {
  Serial.println(F("Preparing deep sleep now"));
  setCpuFrequencyMhz(20);

  rtc_gpio_pullup_dis(GPIO_SWITCH);
  rtc_gpio_pulldown_en(GPIO_SWITCH);
  esp_sleep_enable_ext0_wakeup(GPIO_SWITCH, HIGH);
  Serial.println("Set GPIO " + String(GPIO_SWITCH) + " for sleep-wakeup");

  esp_sleep_pd_config(ESP_PD_DOMAIN_MAX, ESP_PD_OPTION_OFF);
  Serial.println(F("Configured all RTC Peripherals to be powered down in deep sleep"));

  Serial.println(F("Going into deep sleep now"));
  esp_deep_sleep_start();
}

/****************\
|*     WiFi     *|
\****************/

void initWiFi(void) {
  Serial.println(F("Starting initialization of WiFi"));

  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoConnect(true);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(HOSTNAME);
  WiFi.persistent(true);
  WiFi.onEvent(connectedWifi, WiFiEvent_t::SYSTEM_EVENT_STA_GOT_IP);
  WiFi.begin(SSID, PASSWORD);
  
  Serial.println(F("Finished initialization of WiFi"));
}

void waitWiFi(void) {
  byte count = 0;
  while (WiFi.status() != WL_CONNECTED && count < 10)
  {
    count++;
    delay(500);
  }
}

bool checkWiFi(bool skipRetry) {
  if (WiFi.status() == WL_CONNECTED)
  {
    if (lastWifi == false) Serial.println(F("WiFi connected successfully"));
    lastWifi = true;
    retryWifi = 0;
    return true;
  }
  else
  {
    if (lastWifi == true) {
      Serial.println(F("WiFi connection failed or timed out"));
      mqttClient.disconnect(true);
      lastMQTT = false;
    }
    lastWifi = false;

    if (skipRetry) return false;
    
    if (retryWifi++ < 5) {
      Serial.println(F("WiFi connection currently lost. Trying to reconnect."));
      WiFi.reconnect();
      delay(250);
      return WiFi.status() == WL_CONNECTED;
    }
  }
}

void connectedWifi(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (lastWifi == false) Serial.println(F("WiFi connected successfully async"));
  lastWifi = true;
  retryWifi = 0;

  WiFi.setAutoConnect(true);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(HOSTNAME);
  WiFi.persistent(true);
  
  if (!mqttClient.connected())
    mqttClient.connect();
}

/****************\
|*     MQTT     *|
\****************/

void initMQTT(void) {
  Serial.println(F("Starting initialization of MQTT"));

  mqttClient.setKeepAlive(10);
  mqttClient.setClientId(HOSTNAME);
  mqttClient.setCredentials(MQTTUSER, MQTTPASS);
  mqttClient.setServer(MQTTHOST, 1883);
  mqttClient.setWill(TOPIC_STATE, 1, true, "offline");

  mqttClient.onConnect(connectedMQTT);
  mqttClient.onDisconnect(disconnectedMQTT);
  mqttClient.onMessage(receiveMQTT);
  if (lastWifi) mqttClient.connect();
  
  Serial.println(F("Finished initialization of MQTT"));
}

bool checkMQTT(bool skipRetry) {
  if (!lastWifi) return false;
  if (mqttClient.connected())
  {
    if (lastMQTT == false) Serial.println(F("MQTT connected successfully"));
    lastMQTT = true;
    retryMQTT = 0;
    return true;
  }
  else
  {
    if (lastMQTT == true) Serial.println(F("MQTT connection failed or timed out"));
    lastMQTT = false;

    if (skipRetry) return false;
    
    if (retryMQTT++ < 20) {
      Serial.println(F("MQTT connection currently lost. Trying to reconnect."));
      mqttClient.connect();
      delay(250);
      return mqttClient.connected();
    }
  }
}

void connectedMQTT(bool sessionPresent) {
  if (lastMQTT == false) Serial.println(F("MQTT connected successfully async"));
  lastMQTT = true;
  retryMQTT = 0;

  mqttClient.subscribe(MQTTTOPIC_LIGHT, 0);
  mqttClient.subscribe(MQTTTOPIC_BUZZER, 0);
  mqttClient.subscribe(MQTTTOPIC_SERVO, 0);
  mqttClient.publish(TOPIC_STATE, 1, true, "online");
  loopMQTT();
}

void disconnectedMQTT(AsyncMqttClientDisconnectReason reason) {
  if (lastMQTT == true) Serial.println(F("MQTT connection failed or timed out async"));
  lastMQTT = false;
}

void receiveMQTT(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {
  Serial.print("MQTT message arrived in topic: ");
  Serial.println(topic);
 
  String packet = "";
  for (int i = index; i < len; i++) { packet += (char)payload[i]; }
  
  Serial.print("MQTT message payload:");
  Serial.println(packet);
  
  if (strncmp(TOPIC_LIGHT_TRIG, topic, strlen(TOPIC_LIGHT_TRIG)) == 0) {
    idleMillis = millis();
    number = (int)packet.toInt();
    xTaskCreatePinnedToCore(triggerLight, "TriggerLightTask", 1000, NULL, 0, &triggerLightHandle, 0);
  }

  if (strncmp(TOPIC_BUZZER_PEEK, topic, strlen(TOPIC_BUZZER_PEEK)) == 0) {
    idleMillis = millis();
    number = (int)packet.toInt();
    xTaskCreatePinnedToCore(peekBuzzer, "PeekBuzzerTask", 1000, NULL, 0, &peekBuzzerHandle, 0);
  }
  
  if (strncmp(TOPIC_BUZZER_TRIG, topic, strlen(TOPIC_BUZZER_TRIG)) == 0) {
    idleMillis = millis();
    number = (int)packet.toInt();
    xTaskCreatePinnedToCore(triggerBuzzer, "TriggerBuzzerTask", 1000, NULL, 0, &triggerBuzzerHandle, 0);
  }
  
  if (strncmp(TOPIC_SERVO_PEEK, topic, strlen(TOPIC_SERVO_PEEK)) == 0) {
    idleMillis = millis();
    number = (int)packet.toInt();
    xTaskCreatePinnedToCore(peekServo, "PeekServoTask", 1000, NULL, 0, &peekServoHandle, 1);
  }
  
  if (strncmp(TOPIC_SERVO_TRIG, topic, strlen(TOPIC_SERVO_TRIG)) == 0) {
    idleMillis = millis();
    number = (int)packet.toInt();
    xTaskCreatePinnedToCore(triggerServo, "TriggerServoTask", 1000, NULL, 0, &triggerServoHandle, 1);
  }
}

void loopMQTT(void) {
  float battery = getBattery();
  mqttClient.publish(TOPIC_TRIGGERS, 0, false, String(triggerCount).c_str());
  mqttClient.publish(TOPIC_BATTERY, 0, true, String(battery).c_str());
}

/****************\
|*     OTA      *|
\****************/

void initOTA(void) {
  ArduinoOTA.setPort(OTAPORT);
  ArduinoOTA.setHostname(OTAHOST);
  ArduinoOTA.setPasswordHash(OTAPASS);

  ArduinoOTA
    .onStart([]() {
      lastOTA = true;
      Serial.println("OTA: Update started");
    })
    .onEnd([]() {
      lastOTA = false;
      Serial.println("OTA: Update finished");
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("OTA:: %u%%\r\n", (progress / (total / 100)));
    })
    .onError([](ota_error_t error) {
      lastOTA = false;
      Serial.printf("OTA: Update failed because of %u\r\n", error);
    });
    
  ArduinoOTA.begin();
}

bool checkOTA() {
  if (!lastWifi) return false;
  ArduinoOTA.begin();
}

void loopOTA(void) {
  if (!lastWifi && !lastOTA) return;
  ArduinoOTA.handle();
}

/****************\
|*    Switch    *|
\****************/

void initSwitch(void) {
  Serial.println(F("Starting initialization of Switch"));
  
  pinMode(PIN_SWITCH, INPUT_PULLDOWN);
  attachInterrupt(digitalPinToInterrupt(PIN_SWITCH), triggerSwitch, CHANGE);
  isSwitchActive = digitalRead(PIN_SWITCH);
  
  Serial.println(F("Finished initialization of Switch"));
}

static void IRAM_ATTR triggerSwitch(void) {
  isSwitchActive = digitalRead(PIN_SWITCH);

  if (!isSwitchActive) {
    isLightTriggered = false;
    isBuzzerTriggered = false;
    isServoTriggered = false;
    isBuzzerPeeked = false;
    isServoPeeked = false;
  }
}

/****************\
|*    Radar    *|
\****************/

void initRadar(void) {
  Serial.println(F("Starting initialization of Radar"));
  
  pinMode(PIN_RADAR, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_RADAR), triggerRadar, CHANGE);
  isRadarActive = digitalRead(PIN_RADAR);
  
  Serial.println(F("Finished initialization of Radar"));
}

static void IRAM_ATTR triggerRadar(void) {
  isRadarActive = digitalRead(PIN_RADAR);

  if (!isRadarActive) {
    isBuzzerPeeked = false;
    isServoPeeked = false;
  }
}

/****************\
|*    Light     *|
\****************/

void initLight(void) {
  Serial.println(F("Starting initialization of Light"));
  
  pinMode(PIN_LIGHT, OUTPUT);
  
  Serial.println(F("Finished initialization of Light"));
}

void loopLight(void) {
  if (isSwitchActive && !isLightTrigger && !isLightTriggered) {
    number = 0;
    xTaskCreatePinnedToCore(triggerLight, "TriggerLightTask", 1000, NULL, 0, &triggerLightHandle, 0);
  } else if (!isSwitchActive && !isLightTrigger) {
    if (isRadarActive) {
      analogWrite(PIN_LIGHT, 1);
    } else {
      analogWrite(PIN_LIGHT, 0);
    }
  }
}

void triggerLight(void *parameter) {
  isLightTrigger = true;
  if (number == 0) isLightTriggered = true;

  int num = number;
  if (num == 0) num = random(5);

  Serial.print(F("Triggered Light with num: "));
  Serial.println(num);

  switch(num) {
    case 0: triggerLight1(); break;
    case 1: triggerLight1(); break;
    case 2: triggerLight2(); break;
    case 3: triggerLight3(); break;
    case 4: triggerLight4(); break;
  }

  if (number > 0) delay(1000);
  isLightTrigger = false;
  vTaskDelete(NULL);
}

/* instantly light up */
void triggerLight1() {
  analogWrite(PIN_LIGHT, 255);
}

/* fast dimming the light up */
void triggerLight2() {
  for (int i = 0; i < 250; i++) {
    analogWrite(PIN_LIGHT, i);
    delay(2);
  }
  delay(25);
  
  analogWrite(PIN_LIGHT, 255);
}

/* fast flickering the light and then light up */
void triggerLight3() {
  for (int i = 0; i < 10; i++) {
    analogWrite(PIN_LIGHT, 255);
    delay(25);
    
    analogWrite(PIN_LIGHT, 0);
    delay(25);
  }
  delay(75);
  
  analogWrite(PIN_LIGHT, 255);
}

/* slow flickering the light, then wait a moment, then fast flickering shortly and then light up */
void triggerLight4() {
  for (int i = 0; i < 5; i++) {
    analogWrite(PIN_LIGHT, 255);
    delay(25 + (i * 10));
    
    analogWrite(PIN_LIGHT, 0);
    delay(75 - (i * 10));
  }

  analogWrite(PIN_LIGHT, 255);
  delay(750);
  
  for (int i = 0; i < 3; i++) {
    analogWrite(PIN_LIGHT, 255);
    delay(50 - (i * 5));
    
    analogWrite(PIN_LIGHT, 0);
    delay(25 + (i * 5));
  }
  delay(75);
  
  analogWrite(PIN_LIGHT, 255);
}

/****************\
|*    Servo     *|
\****************/

void initServo(void) {
  Serial.println(F("Starting initialization of Servo"));

  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  pinMode(PIN_SERVO_GATE, OUTPUT);
  digitalWrite(PIN_SERVO_GATE, HIGH);
  delay(100);
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 1);
  
  coverServo.attach(PIN_COVER);
  handServo.attach(PIN_HAND);
  
  coverServo.write(getCoverDegree(0));
  handServo.write(getHandDegree(0));
  
  Serial.println(F("Finished initialization of Servo"));
}

void disposeServo(void) {
  coverServo.write(getCoverDegree(0));
  handServo.write(getHandDegree(0));
  delay(250);
  
  digitalWrite(PIN_SERVO_GATE, LOW);
  delay(25);
  
  coverServo.detach();
  handServo.detach();
}

void loopServo(void) {
  if (isSwitchActive && !isServoTrigger && !isServoTriggered) {
    number = 0;
    xTaskCreatePinnedToCore(triggerServo, "TriggerServoTask", 2000, NULL, 0, &triggerServoHandle, 1);
  }

  if (isRadarActive && !isSwitchActive && !isServoTrigger && !isServoPeek && !isServoPeeked) {
    number = 0;
    xTaskCreatePinnedToCore(peekServo, "PeekServoTask", 1000, NULL, 0, &peekServoHandle, 1);
  }
}

void triggerServo(void *parameter) {
  isServoTrigger = true;
  if (number == 0) isServoTriggered = true;

  int num = number;
  if (num == 0 && triggerCount > 0 && triggerCount <= 10) num = triggerCount;
  if (num == 0) num = random(1, 11);
  ++triggerCount;

  Serial.print(F("Triggered Servo with num: "));
  Serial.println(num);
  
  switch(num) {
    case 1: triggerServo1(); break;
    case 2: triggerServo2(); break;
    case 3: triggerServo3(); break;
    case 4: triggerServo4(); break;
    case 5: triggerServo5(); break;
    case 6: triggerServo6(); break;
    case 7: triggerServo7(); break;
    case 8: triggerServo8(); break;
    case 9: triggerServo9(); break;
    case 10: triggerServo10(); break;
  }

  if (number > 0) delay(1000);
  isServoTrigger = false;
  vTaskDelete(NULL);
}

void peekServo(void *parameter) {
  isServoPeek = true;
  if (number == 0) isServoPeeked = true;
  
  int num = number;
  if (num == 0) num = num = random(3);
  
  Serial.print(F("Peeked Servo with num: "));
  Serial.println(num);
  
  switch(num) {
    case 1: peekServo1(); break;
    case 2: peekServo2(); break;
  }

  if (number > 0) delay(1000);
  isServoPeek = false;
  vTaskDelete(NULL);
}

/* full speed peek raising half height */
void peekServo1(void) {
  coverServo.write(getCoverDegree(50));
  delay(2500);
  
  coverServo.write(getCoverDegree(0));
}

/* slow peek raising less than half height */
void peekServo2(void) {
  for (int i = getCoverDegree(0); i < getCoverDegree(45); i++) {
    coverServo.write(i);
    delay(1);
  }
  delay(2500);
  
  coverServo.write(getCoverDegree(0));
}

/* full speed raising cover | full speed moving hand */
void triggerServo1() {
  coverServo.write(getCoverDegree(75));
  delay(250);
  
  handServo.write(getHandDegree(180));
  delay(400);
  
  handServo.write(getHandDegree(0));
  delayServo(400);
  
  coverServo.write(getCoverDegree(0));
}

/* full speed raising cover | slowly moving hand near switch, then fast onto switch and full speed back */
void triggerServo2() {
  coverServo.write(getCoverDegree(75));
  delay(300);
  
  for (int i = getHandDegree(0); i > getHandDegree(160); i--) {
    handServo.write(i);
    delay(i < getHandDegree(110) ? 2 : 1);
  }
  delay(1000);
  
  handServo.write(getHandDegree(180));
  delay(300);
  
  handServo.write(getHandDegree(0));
  delayServo(750);
  
  coverServo.write(getCoverDegree(0));
}

/* fast shaking cover then full speed raising cover | slowly moving hand */
void triggerServo3() {
  coverServo.write(getCoverDegree(35));
  delay(75);
  
  for (int i = 0; i < 10; i++) {
    coverServo.write(getCoverDegree(42));
    delay(50);
    
    coverServo.write(getCoverDegree(35));
    delay(50);
  }
  delay(75);
  
  coverServo.write(getCoverDegree(75));
  delay(1000);

  for (int i = getHandDegree(0); i > getHandDegree(160); i--) {
    handServo.write(i);
    delay(i < getHandDegree(110) ? 3 : 1);
  }
  delay(50);

  handServo.write(getHandDegree(180));
  delay(300);
  
  handServo.write(getHandDegree(165));
  delay(2000);
  
  handServo.write(getHandDegree(0));
  delayServo(1000);
  
  coverServo.write(getCoverDegree(0));
}

/* full speed raising cover | full speed moving hand near switch, then back a little, then slowly forward, then fast onto switch and full speed back */
void triggerServo4() {
  coverServo.write(getCoverDegree(75));
  delay(300);
  
  handServo.write(getHandDegree(160));
  delay(1000);

  for (int i = getHandDegree(160); i < getHandDegree(110); i++) {
    handServo.write(i);
    delay(2);
  }
  delay(500);

  for (int i = getHandDegree(110); i > getHandDegree(150); i--) {
    handServo.write(i);
    delay(1);
  }
  delay(1000);
  
  handServo.write(getHandDegree(180));
  delay(300);
  
  handServo.write(getHandDegree(0));
  delayServo(750);
  
  coverServo.write(getCoverDegree(0));
}

/* full speed raising cover then shaking it slow | full speed moving hand */
void triggerServo5() {
  coverServo.write(getCoverDegree(75));
  delay(300);

  for (int i = 0; i < 5; i++) {
    coverServo.write(getCoverDegree(65));
    delay(100);
    
    coverServo.write(getCoverDegree(75));
    delay(100);
  }
  
  handServo.write(getHandDegree(180));
  delay(500);
  
  handServo.write(getHandDegree(165));
  delay(500);
  
  handServo.write(getHandDegree(0));
  delayServo(1000);
  
  coverServo.write(getCoverDegree(0));
}

/* slowly raising cover then shaking it fast | full speed moving hand near switch, wait a moment, then full speed onto switch and full speed back */
void triggerServo6() {
  for (int i = getCoverDegree(0); i < getCoverDegree(75); i++) {
    coverServo.write(i);
    delay(3);
  }
  
  for (int i = 0; i < 10; i++) {
    coverServo.write(getCoverDegree(65));
    delay(50);
    
    coverServo.write(getCoverDegree(75));
    delay(50);
  }
  delay(25);

  handServo.write(getHandDegree(160));
  delay(2500);

  handServo.write(getHandDegree(180));
  delay(300);
  
  handServo.write(getHandDegree(0));
  delayServo(500);
  
  coverServo.write(getCoverDegree(0));
}

/* slowly raising cover | full speed moving hand then shake it fast | slowly lowering cover near closed, then full speed halfway open, then full speed closed */
void triggerServo7() {
  for (int i = getCoverDegree(0); i < getCoverDegree(75); i++) {
    coverServo.write(i);
    delay(2);
  }

  handServo.write(getHandDegree(180));
  delay(500);

  for (int i = 0; i < 10; i++) {
    handServo.write(getHandDegree(150));
    delay(50);
    
    handServo.write(getHandDegree(160));
    delay(50);
  }
  delay(100);
  
  handServo.write(getHandDegree(0));
  delayServo(500);

  for (int i = getCoverDegree(75); i > getCoverDegree(40); i--) {
    coverServo.write(i);
    delay(1);
  }
  delay(100);

  coverServo.write(getCoverDegree(60));
  delay(2000);
  
  coverServo.write(getCoverDegree(0));
}

/* wait a moment | full speed raising cover | full speed moving hand and wait another moment */
void triggerServo8() {
  delay(2000);
  
  coverServo.write(getCoverDegree(75));
  delay(200);
  
  handServo.write(getHandDegree(180));
  delay(350);
  
  handServo.write(getHandDegree(0));
  delayServo(2000);
  
  coverServo.write(getCoverDegree(0));
}

/* full speed raising cover | full speed moving hand and holding onto switch | slowly lowering the cover halfway, then shaking it a little bit then full speed opening | full speed moving hand back and close cover */
void triggerServo9() {
  coverServo.write(getCoverDegree(75));
  delay(200);
  
  handServo.write(getHandDegree(180));
  delay(500);
  
  handServo.write(getHandDegree(170));
  delay(25);
  
  for (int i = getCoverDegree(75); i > getCoverDegree(60); i--) {
    coverServo.write(i);
    delay(1);
  }
  delay(100);

  for (int i = 0; i < 5; i++) {
    coverServo.write(getCoverDegree(50));
    delay(100);
    
    coverServo.write(getCoverDegree(60));
    delay(100);
  }
  delay(500);
  
  coverServo.write(getCoverDegree(75));
  delay(250);
  
  handServo.write(getHandDegree(0));
  delayServo(500);
  
  coverServo.write(getCoverDegree(0));
}

/* slowly raising cover, waiting a moment | full speed moving hand onto switch, then shake hand and cover a little bit, then move hand very slow halfway, waiting a moment | full speed moving hand back and close cover */
void triggerServo10() {
  for (int i = getCoverDegree(0); i < getCoverDegree(70); i++) {
    coverServo.write(i);
    delay(3);
  }
  delay(2000);

  handServo.write(getHandDegree(180));
  delay(500);

  for (int i = 0; i < 5; i++) {
    coverServo.write(getCoverDegree(75));
    handServo.write(getHandDegree(150));
    delay(50);
    
    coverServo.write(getCoverDegree(65));
    handServo.write(getHandDegree(160));
    delay(50);
  }

  int i = 0;
  int currentCover = getCoverDegree(75);
  int currentHand = getHandDegree(170);
  int maxCover = getCoverDegree(52);
  int maxHand = getHandDegree(135);
  while (currentCover > maxCover || currentHand < maxHand) {
    coverServo.write(currentCover);
    handServo.write(currentHand);
    delay(3);
    
    if (currentCover > maxCover && i % 2 == 0) currentCover--;
    if (currentHand < maxHand) currentHand++;
    i++;
  }
  delay(2000);

  coverServo.write(getCoverDegree(75));
  delay(75);
  
  handServo.write(getHandDegree(0));
  delayServo(300);
  
  coverServo.write(getCoverDegree(0));
}

/* fast reaction if switch gets activated while hand is moving back */
void delayServo(int delayMilliseconds) {
  int pause = 50;
  int maximum = 350;
  delay(pause);
  
  int timeMoved = pause;
  int timeDelayed = 0;
  
  while (timeDelayed < delayMilliseconds) {
    if (!isServoTriggered && isSwitchActive) {
      int moving = timeMoved;
      if (moving > maximum) moving = maximum;
      
      handServo.write(getHandDegree(180));
      delay(moving + 100);
      
      handServo.write(getHandDegree(0));
      delay(pause);
      
      timeDelayed = (timeDelayed - timeMoved) + pause;
      if (timeDelayed < 0) timeDelayed = 0;
      timeMoved = pause;
    }
    
    delay(1);
    timeDelayed++;
    timeMoved++;
  }
}

int getCoverDegree(int degree) {
  return map(degree, 0, 180, 500, 2500);
}

int getHandDegree(int degree) {
  return map(degree, 0, 180, 2500, 500);
}

/****************\
|*    Buzzer    *|
\****************/

void initBuzzer(void) {
  Serial.println(F("Starting initialization of Buzzer"));
  
  pinMode(PIN_BUZZER, OUTPUT);
  noTone(PIN_BUZZER);
  
  Serial.println(F("Finished initialization of Buzzer"));
}

void disposeBuzzer(void) {
  noTone(PIN_BUZZER);
}

void loopBuzzer(void) {
  if (isSwitchActive && !isBuzzerTrigger && !isBuzzerTriggered) {
    number = 0;
    xTaskCreatePinnedToCore(triggerBuzzer, "TriggerBuzzerTask", 2000, NULL, 0, &triggerBuzzerHandle, 0);
  }

  if (isRadarActive && !isSwitchActive && !isBuzzerTrigger && !isBuzzerPeek && !isBuzzerPeeked) {
    number = 0;
    xTaskCreatePinnedToCore(peekBuzzer, "PeekBuzzerTask", 1000, NULL, 0, &peekBuzzerHandle, 0);
  }
}

void triggerBuzzer(void *parameter) {
  isBuzzerTrigger = true;
  if (number == 0) isBuzzerTriggered = true;

  int num = number;
  if (num == 0) num = num = random(8);

  Serial.print(F("Triggered Buzzer with num: "));
  Serial.println(num);
  
  switch(num) {
    case 1: triggerBuzzer1(); break;
    case 2: triggerBuzzer2(); break;
    case 3: triggerBuzzer3(); break;
    case 4: triggerBuzzer4(); break;
    case 5: triggerBuzzer5(); break;
    case 6: triggerBuzzer6(); break;
    case 7: triggerBuzzer7(); break;
  }
  
  if (number > 0) delay(1000);
  isBuzzerTrigger = false;
  vTaskDelete(NULL);
}

void peekBuzzer(void *parameter) {
  isBuzzerPeek = true;
  if (number == 0) isBuzzerPeeked = true;
  
  int num = number;
  if (num == 0) num = num = random(3);

  Serial.print(F("Peeked Buzzer with num: "));
  Serial.println(num);
  
  switch(num) {
    case 1: peekBuzzer1(); break;
    case 2: peekBuzzer2(); break;
  }
  
  if (number > 0) delay(1000);
  isBuzzerPeek = false;
  vTaskDelete(NULL);
}

void peekBuzzer1(void) {
  for (int i = 0; i < 32; i++) {
    tone(PIN_BUZZER, 4000 + (i * 16), 2);
  }
}

void peekBuzzer2(void) {
  for (int i = 0; i < 32; i++) {
    tone(PIN_BUZZER, 4500 - (i * 16), 2);
  }
}

/* fast beep going up | fast beep going down */
void triggerBuzzer1() {
  delay(500);
  
  for (int i = 0; i < 32; i++) {
    tone(PIN_BUZZER, 4000 + (i * 12), 6);
    delay(2);
  }
  for (int i = 0; i < 32; i++) {
    tone(PIN_BUZZER, 4000 - (i * 12), 6);
    delay(2);
  }
}

/* slow beep going up | fast beep going down */
void triggerBuzzer2() {
  delay(500);
  
  for (int i = 0; i < 48; i++) {
    tone(PIN_BUZZER, 4000 + (i * 16), 8);
    delay(4);
  }
  for (int i = 0; i < 32; i++) {
    tone(PIN_BUZZER, 4000 - (i * 10), 6);
    delay(2);
  }
}

/* fast beep going up | fast beep going down */
void triggerBuzzer3() {
  delay(500);
  
  for (int i = 0; i < 32; i++) {
    tone(PIN_BUZZER, 3500 + (i * 10), 6);
    delay(2);
  }
  for (int i = 0; i < 48; i++) {
    tone(PIN_BUZZER, 3500 - (i * 16), 8);
    delay(4);
  }
}

/* slow beep going up | slow beep going down */
void triggerBuzzer4() {
  delay(500);
  
  for (int i = 0; i < 48; i++) {
    tone(PIN_BUZZER, 4000 + (i * 12), 8);
    delay(4);
  }
  for (int i = 0; i < 48; i++) {
    tone(PIN_BUZZER, 4000 - (i * 12), 8);
    delay(4);
  }
}

/* very fast beep going up | very fast beep going up */
void triggerBuzzer5() {
  delay(500);
  
  for (int i = 0; i < 16; i++) {
    tone(PIN_BUZZER, 4000 + (i * 48), 6);
    delay(1);
  }
  delay(25);
  for (int i = 0; i < 16; i++) {
    tone(PIN_BUZZER, 4000 + (i * 48), 6);
    delay(1);
  }
}

/* slow beep going up | very fast beep going down | slow beep going up */
void triggerBuzzer6() {
  delay(500);
  
  for (int i = 0; i < 16; i++) {
    tone(PIN_BUZZER, 4000 + (i * 48), 6);
    delay(1);
  }
  delay(5);
  for (int i = 0; i < 12; i++) {
    tone(PIN_BUZZER, 4800 - (i * 16), 4);
  }
  delay(25);
  for (int i = 0; i < 16; i++) {
    tone(PIN_BUZZER, 4000 + (i * 48), 6);
    delay(1);
  }
}

/* very fast beep going up | very fast beep going down, then wait a moment | very fast beep going up | very fast beep going up */
void triggerBuzzer7() {
  delay(500);
  
  for (int i = 0; i < 16; i++) {
    tone(PIN_BUZZER, 4000 + (i * 48), 6);
    delay(1);
  }
  delay(5);
  for (int i = 0; i < 12; i++) {
    tone(PIN_BUZZER, 4800 - (i * 16), 4);
  }
  
  delay(1500);
  
  for (int i = 0; i < 16; i++) {
    tone(PIN_BUZZER, 4000 + (i * 48), 6);
    delay(1);
  }
  delay(25);
  for (int i = 0; i < 16; i++) {
    tone(PIN_BUZZER, 4000 + (i * 48), 6);
    delay(1);
  }
}

/****************\
|*     Other    *|
\****************/

long getElapsed(unsigned long compareMillis) {
  unsigned long currentMillis = millis();
  unsigned long elapsedMillis = currentMillis - compareMillis;
  return abs(elapsedMillis);
}

float getBattery(void) {
  float voltage = 0;

  analogSetClockDiv(8);
  pinMode(PIN_BATTERY, INPUT);
  
  for (int i = 0; i < BATTERY_MEDIAN; i++) {
    if (i > 0) delay(5);
    
    int batteryRaw = analogRead(PIN_BATTERY);
    if (batteryRaw > 0 && batteryRaw <= 4095) batteryMedian.add(batteryRaw + 0.00F);
  }

  int batteryRawMedian = batteryMedian.getAverage(5);
  if (batteryRawMedian != NAN && batteryRawMedian > 0 && batteryRawMedian <= 4095) {
    voltage = getVoltage(batteryRawMedian) * 1000 * ((220 + 100) / 100.0F);
    Serial.print(F("Battery voltage is: "));
    Serial.println(voltage);
  }
  
  return voltage;
}

double getVoltage(double value){
  if (value < 1 || value > 4095) return 0;
  return -0.000000000000016 * pow(value, 4) + 0.000000000118171 * pow(value, 3) - 0.000000301211691 * pow(value, 2) + 0.001109019271794 * value + 0.034143524634089;
}
