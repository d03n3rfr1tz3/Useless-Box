#ifndef CONFIG_H
#define CONFIG_H

#define SSID              ""
#define PASSWORD          ""
#define HOSTNAME          "UselessBox"

#define MQTTHOST          ""
#define MQTTUSER          ""
#define MQTTPASS          ""
#define MQTTTOPIC_LIGHT   "UselessBox/Light/#"
#define MQTTTOPIC_BUZZER  "UselessBox/Buzzer/#"
#define MQTTTOPIC_SERVO   "UselessBox/Servo/#"

#define OTAPORT           3232
#define OTAHOST           "UselessBox"
#define OTAPASS           ""

#define GPIO_SWITCH       GPIO_NUM_27
#define PIN_SWITCH        27
#define PIN_RADAR         32
#define PIN_SERVO_GATE    14
#define PIN_LIGHT         12
#define PIN_COVER         25
#define PIN_HAND          26
#define PIN_BUZZER        33
#define PIN_BATTERY       39

#define BATTERY_MEDIAN    50

#define TOPIC_STATE       "UselessBox/state"
#define TOPIC_TRIGGERS    "UselessBox/triggers"
#define TOPIC_BATTERY     "UselessBox/battery"
#define TOPIC_LIGHT_TRIG  "UselessBox/Light/Trigger"
#define TOPIC_BUZZER_PEEK "UselessBox/Buzzer/Peek"
#define TOPIC_BUZZER_TRIG "UselessBox/Buzzer/Trigger"
#define TOPIC_SERVO_PEEK  "UselessBox/Servo/Peek"
#define TOPIC_SERVO_TRIG  "UselessBox/Servo/Trigger"

#endif
