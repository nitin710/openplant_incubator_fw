#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Adafruit_AHTX0.h>
#include <Adafruit_SHTC3.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "secrets.h" // WiFi + MQTT broker settings. Copy secrets.h.example to secrets.h

#define FW_VERSION "0.2.0"

Adafruit_AHTX0 aht;
Adafruit_SHTC3 shtc3;
Adafruit_SH1107 display = Adafruit_SH1107(64, 128, &Wire);

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

const int lightAdcInput = A0;
const int fanAdcInput = A1;
const int PWM_LIGHT = 5;
const int PWM_FAN_IN_1 = 18;
const int PWM_FAN_IN_2 = 19;
const int TACH_1 = 21;
const int TACH_2 = A5;
const int PIN_COOL_1 = 27;
const int PIN_COOL_2 = 33;
const int BUTTON_LIGHT = 14;// button C on LED matrix feather wing
const int BUTTON_SET_TEMP_UP = 15;  // button B on LED matrix
const int BUTTON_SET_TEMP_DOWN = 32;  //button A on LED matrix



const int PWM_FREQ = 25000; // Run Fan PWM on 25KHz
const int PWM_RESOLUTION = 8; // 8 bit resolution should be enough
const int PWM_CHANNEL_FAN_IN_1 = 0;
const int PWM_CHANNEL_FAN_IN_2 = 1;
volatile uint32_t fanInTachCounter_1 = 0;
volatile uint32_t fanInTachCounter_2 = 0;
int lastLightLevel = 0;
int lastFanLevel = 0;
const float ADC_RES = 4095.0f;
volatile float tempSetPoint = 22; // setting temp to 22*C
float TEMP_DELTA = 0.5; // allows temp to be +/- *C
int inFanPwmLevel = 0;
uint32_t timeSinceCoolingUpdate = millis();
int16_t COOLING_UPDATE_INTERVAL = 5000; // in mS
String response = "X";
volatile int lightLevel = 64; // default on startup
volatile uint32_t lastLightSetTime = 0;
volatile uint32_t lastTempSetTime = 0;
const uint8_t MAX_SETTABLE_TEMP = 26;
const uint8_t MIN_SETTABLE_TEMP = 18;

// Network. The incubator must keep running without it, so nothing here may block for long.
const uint32_t WIFI_RETRY_INTERVAL = 30000; // in mS
const uint32_t MQTT_RETRY_INTERVAL = 15000; // in mS
const uint32_t MQTT_PUBLISH_INTERVAL = 30000; // in mS
const uint32_t NET_TIMEOUT_S = 2; // cap on each TCP connect / MQTT handshake
uint32_t lastWifiAttempt = 0;
uint32_t lastMqttAttempt = 0;
uint32_t lastMqttPublish = 0;
bool publishNow = false; // set when something changed or we just (re)connected

// Last values sent, to publish immediately when they change
float publishedSetPoint = -1;
int publishedLightLevel = -1;
int publishedCoolingStatus = -1;

// Device identity, built from the MAC so every unit runs the same binary
char deviceId[16];          // e.g. "inc-a1b2c3"
char topicStatus[64];       // openplant/incubator/<id>/status
char topicMeta[64];         // openplant/incubator/<id>/meta
char topicState[64];        // openplant/incubator/<id>/state
char topicCmdSetpoint[64];  // openplant/incubator/<id>/cmd/setpoint
char topicCmdLight[64];     // openplant/incubator/<id>/cmd/light


// Temperature/humidity sensors on the I2C bus. Each type has a fixed address, so a
// board can carry one of each. The order is the priority: the first sensor that reads
// controls cooling, the other one is reported for monitoring.
enum SensorType{
  SENSOR_AHT,
  SENSOR_SHTC3
};
struct TempSensor{
  SensorType type;
  const char *name;
  uint8_t addr;
  bool found;     // answered at boot
  bool ok;        // last read succeeded
  float temp;
  float humidity;
};
TempSensor sensors[] = {
  {SENSOR_AHT, "AHT", 0x38},
  {SENSOR_SHTC3, "SHTC3", 0x70},
};
const int NUM_SENSORS = sizeof(sensors) / sizeof(sensors[0]);
int controlSensor = -1;  // index of the sensor cooling runs on, -1 if none reads
int monitorSensor = -1;  // index of the other fitted sensor, -1 if only one is fitted
int publishedControlSensor = -2;

enum CoolingStatus{
  OFF,
  ON
}coolingStatus;

int getLightLevel()
{
  int adcVal = analogRead(lightAdcInput);
  int val = ((float)adcVal/ADC_RES)*255;
  return val;

}
int getFanLevel()
{
  int adcVal = analogRead(fanAdcInput);
  int val = ((float)adcVal/ADC_RES)*255;
  return val;
}
void setLightLevel(int val)
{

  Serial.print("Setting light to: "); Serial.println(val);
  analogWrite(PWM_LIGHT, val);
}
void setFanLevel(int val)
{
  Serial.print("Setting fan to: "); Serial.println(val);
  inFanPwmLevel = val;
  //analogWrite(PWM_FAN_IN_1, val);
  //analogWrite(PWM_FAN_IN_2, val);
  ledcWrite(PWM_CHANNEL_FAN_IN_1, val);
  ledcWrite(PWM_CHANNEL_FAN_IN_2, val);
}


void IRAM_ATTR countTach1Pulse() {
	// increment tach counter on every pusle
  fanInTachCounter_1++;
}

void IRAM_ATTR countTach2Pulse() {
	// increment tach counter on every pusle
  fanInTachCounter_2++;
}

void IRAM_ATTR updateLightSetting()
{
  // Discard multiple erroneous button presses (debouncer)
  if(millis() - lastLightSetTime > 100)
  {
    lastLightSetTime = millis();
    // increase light level in steps of 32
    if(lightLevel >= 128)
    { // wrap around
      lightLevel = 32;
    }
    else
    {
      lightLevel += 32;
    }
  }
}

void IRAM_ATTR increaseSetTemp()
{
  // increase by 1*C
  if(millis() - lastTempSetTime > 100)
  {
    lastTempSetTime = millis();
    if(tempSetPoint >= MAX_SETTABLE_TEMP)
    {
      // wrap around to to max
      tempSetPoint = MIN_SETTABLE_TEMP;
    }
    else
    {
      tempSetPoint+=1;
    }
  }
}

void IRAM_ATTR decreaseSetTemp()
{
  // decrease by 1*c
  if(millis() - lastTempSetTime > 100)
  {
    lastTempSetTime = millis();
    if(tempSetPoint <= MIN_SETTABLE_TEMP)
    {
      // wrap around to to max
      tempSetPoint = MAX_SETTABLE_TEMP;
    }
    else
    {
      tempSetPoint-=1;
    }
  }
}
long calculateRpm(volatile uint32_t &tachCount)
{
  long rpm = (tachCount * 60 * 2) / 2;
  Serial.print("tachCounter: "); Serial.println(tachCount);
  Serial.print("RPM:");
  Serial.println(rpm);
  tachCount = 0;
  return rpm;
}

String adjustCooling(float temp)
{
  String response = "";
  Serial.print("AdjustingTemp: "); Serial.print(temp); Serial.print("/"); Serial.println(tempSetPoint);
  float rangeMin = tempSetPoint - TEMP_DELTA;
  float rangeMax = tempSetPoint + TEMP_DELTA;

  if (temp > rangeMax)
  {
    // temp is higher than expected.
    // check if cooling unit is ON
    if(coolingStatus == CoolingStatus::ON)
    {
      // cooling unit is ON
      // set IN FANS to max speed
      //inFanPwmLevel = 255;
      setFanLevel(255); // ToDO: remove the hardcoded number
    }
    else
    {
      // peltier is OFF. switch it ON
      coolingStatus = CoolingStatus::ON;
      digitalWrite(PIN_COOL_1, HIGH);
      digitalWrite(PIN_COOL_2, HIGH);
      // maybe we should have a time based checl?
      // If it has been >Xmins since peltier switch on, increase fan speed
    }
    response = "+";
  }
  else if (temp < rangeMin)
  {
    if(inFanPwmLevel != 0)
    {
      //inFanPwmLevel = 0;
      setFanLevel(0); // set inside fan to min speed
    }
    else
    {
      // fans already on lowest speed.
      // cut power to peltier and out fan
      coolingStatus = CoolingStatus::OFF;
      digitalWrite(PIN_COOL_1, LOW);
      digitalWrite(PIN_COOL_2, LOW);
    }
    response = "-";
  }
  else
  {
      // within range. Shut down peltier. and reduce fan speed
      //coolingStatus = CoolingStatus::OFF;
      //digitalWrite(PIN_COOL_1, LOW);
      //digitalWrite(PIN_COOL_2, LOW);
      //inFanPwmLevel = 128;
      setFanLevel(128); // cruise at half speed
      response = "o";
  }
  return response;
}

void setupSensors()
{
  for(int i = 0; i < NUM_SENSORS; i++)
  {
    TempSensor &s = sensors[i];
    s.found = s.type == SENSOR_AHT ? aht.begin() : shtc3.begin();
    Serial.print(s.name); Serial.println(s.found ? " found" : " not found");
  }
}

bool i2cPresent(uint8_t addr)
{
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

void readSensors()
{
  controlSensor = -1;
  monitorSensor = -1;
  for(int i = 0; i < NUM_SENSORS; i++)
  {
    TempSensor &s = sensors[i];
    s.ok = false;
    if(!s.found) continue;
    // Both libraries retry forever if the sensor drops off the bus mid-read, so check it answers first
    if(i2cPresent(s.addr))
    {
      sensors_event_t humidity, temp;
      bool read = s.type == SENSOR_AHT ? aht.getEvent(&humidity, &temp) : shtc3.getEvent(&humidity, &temp);
      if(read && !isnan(temp.temperature))
      {
        s.ok = true;
        s.temp = temp.temperature;
        s.humidity = humidity.relative_humidity;
      }
    }
    if(!s.ok) { Serial.print(s.name); Serial.println(" read failed"); }

    if(s.ok && controlSensor < 0) controlSensor = i;
  }
  // Report whichever fitted sensor isn't controlling, including a failed one so the fault shows up.
  // With none reading, the first fitted sensor keeps the control slot.
  int controlSlot = controlSensor;
  for(int i = 0; i < NUM_SENSORS && controlSlot < 0; i++)
  {
    if(sensors[i].found) controlSlot = i;
  }
  for(int i = 0; i < NUM_SENSORS; i++)
  {
    if(sensors[i].found && i != controlSlot && monitorSensor < 0) monitorSensor = i;
  }
}

void coolingFailSafe()
{
  // No working sensor: we can't tell if the chamber is too cold, so stop cooling
  if(coolingStatus == CoolingStatus::ON)
  {
    Serial.println("No temperature sensor, cooling off");
  }
  coolingStatus = CoolingStatus::OFF;
  digitalWrite(PIN_COOL_1, LOW);
  digitalWrite(PIN_COOL_2, LOW);
}

// JSON number, or null when the reading isn't valid
String jsonNum(float val, bool ok)
{
  return ok ? String(val, 2) : String("null");
}

void setupDeviceId()
{
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(deviceId, sizeof(deviceId), "inc-%02x%02x%02x", mac[3], mac[4], mac[5]);
  snprintf(topicStatus, sizeof(topicStatus), "openplant/incubator/%s/status", deviceId);
  snprintf(topicMeta, sizeof(topicMeta), "openplant/incubator/%s/meta", deviceId);
  snprintf(topicState, sizeof(topicState), "openplant/incubator/%s/state", deviceId);
  snprintf(topicCmdSetpoint, sizeof(topicCmdSetpoint), "openplant/incubator/%s/cmd/setpoint", deviceId);
  snprintf(topicCmdLight, sizeof(topicCmdLight), "openplant/incubator/%s/cmd/light", deviceId);
  Serial.print("Device ID: "); Serial.println(deviceId);
}

void mqttCallback(char* topic, byte* payload, unsigned int length)
{
  char msg[16];
  unsigned int n = length < sizeof(msg) - 1 ? length : sizeof(msg) - 1;
  memcpy(msg, payload, n);
  msg[n] = '\0';
  Serial.print("MQTT cmd ["); Serial.print(topic); Serial.print("]: "); Serial.println(msg);

  char *end;
  float val = strtof(msg, &end);
  if(end == msg)
  {
    Serial.println("Ignored: not a number");
    return;
  }

  if(strcmp(topic, topicCmdSetpoint) == 0)
  {
    // whole degrees only, so the buttons keep stepping through 18..26
    long setPoint = lroundf(val);
    if(setPoint > MAX_SETTABLE_TEMP) setPoint = MAX_SETTABLE_TEMP;
    if(setPoint < MIN_SETTABLE_TEMP) setPoint = MIN_SETTABLE_TEMP;
    tempSetPoint = setPoint;
    Serial.print("Setpoint -> "); Serial.println(tempSetPoint);
  }
  else if(strcmp(topic, topicCmdLight) == 0)
  {
    // same levels the light button cycles through
    int level = (int)val;
    if(level == 32 || level == 64 || level == 96 || level == 128)
    {
      lightLevel = level;
      Serial.print("Light -> "); Serial.println(lightLevel);
    }
    else
    {
      Serial.println("Ignored: light must be 32, 64, 96 or 128");
    }
  }
}

void maintainWifi()
{
  // ESP32 auto-reconnects most of the time; this restarts it if it gave up
  if(WiFi.status() == WL_CONNECTED) return;
  if(millis() - lastWifiAttempt < WIFI_RETRY_INTERVAL) return;
  lastWifiAttempt = millis();
  Serial.println("WiFi not connected, retrying");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void maintainMqtt()
{
  if(WiFi.status() != WL_CONNECTED) return;
  if(mqttClient.connected())
  {
    mqttClient.loop();
    return;
  }
  if(lastMqttAttempt != 0 && millis() - lastMqttAttempt < MQTT_RETRY_INTERVAL) return;
  lastMqttAttempt = millis();

  Serial.print("Connecting to MQTT...");
  // An empty MQTT_USER means the broker allows anonymous clients
  const char *user = strlen(MQTT_USER) > 0 ? MQTT_USER : nullptr;
  const char *pass = strlen(MQTT_USER) > 0 ? MQTT_PASSWORD : nullptr;
  // The broker publishes "offline" for us (retained) if we drop off without saying goodbye
  if(mqttClient.connect(deviceId, user, pass, topicStatus, 0, true, "offline"))
  {
    Serial.println("connected");
    mqttClient.publish(topicStatus, "online", true);
    String meta = "{";
    meta += "\"id\":\"" + String(deviceId) + "\",";
    meta += "\"fw\":\"" FW_VERSION "\",";
    meta += "\"mac\":\"" + WiFi.macAddress() + "\",";
    meta += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    meta += "\"sensors\":[";
    bool first = true;
    for(int i = 0; i < NUM_SENSORS; i++)
    {
      if(!sensors[i].found) continue;
      if(!first) meta += ",";
      meta += "\"" + String(sensors[i].name) + "\"";
      first = false;
    }
    meta += "]";
    meta += "}";
    mqttClient.publish(topicMeta, meta.c_str(), true);
    mqttClient.subscribe(topicCmdSetpoint);
    mqttClient.subscribe(topicCmdLight);
    publishNow = true;
  }
  else
  {
    Serial.print("failed, rc="); Serial.println(mqttClient.state());
  }
}

void publishState(long rpm_1, long rpm_2)
{
  if(!mqttClient.connected()) return;

  if(tempSetPoint != publishedSetPoint || lightLevel != publishedLightLevel || (int)coolingStatus != publishedCoolingStatus || controlSensor != publishedControlSensor)
  {
    publishNow = true;
  }
  if(!publishNow && millis() - lastMqttPublish < MQTT_PUBLISH_INTERVAL) return;

  String payload = "{";
  // temp/humidity come from the sensor cooling runs on; temp2/humidity2 from the other one, if fitted
  TempSensor *ctrl = controlSensor >= 0 ? &sensors[controlSensor] : nullptr;
  payload += "\"temp\":" + jsonNum(ctrl ? ctrl->temp : 0, ctrl) + ",";
  payload += "\"humidity\":" + jsonNum(ctrl ? ctrl->humidity : 0, ctrl) + ",";
  payload += "\"sensor\":\"" + String(ctrl ? ctrl->name : "none") + "\",";
  if(monitorSensor >= 0)
  {
    TempSensor &mon = sensors[monitorSensor];
    payload += "\"temp2\":" + jsonNum(mon.temp, mon.ok) + ",";
    payload += "\"humidity2\":" + jsonNum(mon.humidity, mon.ok) + ",";
    payload += "\"sensor2\":\"" + String(mon.name) + "\",";
  }
  payload += "\"fan1_rpm\":" + String(rpm_1) + ",";
  payload += "\"fan2_rpm\":" + String(rpm_2) + ",";
  payload += "\"light\":" + String(lightLevel) + ",";
  payload += "\"setpoint\":" + String(tempSetPoint, 1) + ",";
  payload += "\"cooling\":\"" + String(coolingStatus == CoolingStatus::ON ? "ON" : "OFF") + "\",";
  payload += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  payload += "\"uptime\":" + String(millis() / 1000);
  payload += "}";
  if(mqttClient.publish(topicState, payload.c_str()))
  {
    Serial.println("MQTT published: " + payload);
    lastMqttPublish = millis();
    publishNow = false;
    publishedSetPoint = tempSetPoint;
    publishedLightLevel = lightLevel;
    publishedCoolingStatus = (int)coolingStatus;
    publishedControlSensor = controlSensor;
  }
}

void setup() {
  // put your setup code here, to run once:
  pinMode(lightAdcInput, INPUT);
  pinMode(fanAdcInput, INPUT);
  //pinMode(PWM_FAN_IN_1, OUTPUT);
  //pinMode(PWM_FAN_IN_2, OUTPUT);
  ledcSetup(PWM_CHANNEL_FAN_IN_1, PWM_FREQ, PWM_RESOLUTION);
  ledcAttachPin(PWM_FAN_IN_1, PWM_CHANNEL_FAN_IN_1);
  ledcSetup(PWM_CHANNEL_FAN_IN_2, PWM_FREQ, PWM_RESOLUTION);
  ledcAttachPin(PWM_FAN_IN_2, PWM_CHANNEL_FAN_IN_2);
  pinMode(PWM_LIGHT, OUTPUT);
  pinMode(TACH_1, INPUT);
  pinMode(TACH_2, INPUT);
  pinMode(PIN_COOL_1, OUTPUT);
  pinMode(PIN_COOL_2, OUTPUT);
  pinMode(BUTTON_LIGHT, INPUT_PULLUP);
  pinMode(BUTTON_SET_TEMP_UP, INPUT_PULLUP);
  pinMode(BUTTON_SET_TEMP_DOWN, INPUT);

  digitalWrite(PIN_COOL_1, HIGH);  // TODO: implement logic to control cooling unit ON/OFF
  digitalWrite(PIN_COOL_2, HIGH);  // TODO: implement logic to control cooling unit ON/OFF
  coolingStatus = CoolingStatus::ON;
  Serial.begin(115200);
  // attach interrupt
  attachInterrupt(TACH_1, countTach1Pulse, RISING);
  attachInterrupt(TACH_2, countTach2Pulse, RISING);
  attachInterrupt(BUTTON_LIGHT, updateLightSetting, FALLING);
  attachInterrupt(BUTTON_SET_TEMP_UP, decreaseSetTemp, FALLING);
  attachInterrupt(BUTTON_SET_TEMP_DOWN, increaseSetTemp, FALLING);

  // Without a sensor we keep running (cooling off, "!" on the OLED) so the fault shows up over MQTT
  setupSensors();



  display.begin(0x3C, true); // Address 0x3C default
  display.display();
  delay(1000);


  display.clearDisplay();
  display.display();

  display.setRotation(1);
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0,0);
  display.display(); // actually display all of the above

  // WiFi + MQTT. Don't wait for the connection; loop() keeps retrying in the background
  WiFi.mode(WIFI_STA);
  setupDeviceId();
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiAttempt = millis();
  wifiClient.setTimeout(NET_TIMEOUT_S);
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setSocketTimeout(NET_TIMEOUT_S);
  mqttClient.setBufferSize(512);
}

void loop() {
  // put your main code here, to run repeatedly:
  // int lightLevel = getLightLevel();
  // if(lightLevel != lastLightLevel)
  // {
  //   lastLightLevel = lightLevel;
  //   setLightLevel(lightLevel);
  // }
  if(lastLightLevel != lightLevel)
  {
    lastLightLevel = lightLevel;
    setLightLevel(lastLightLevel);
  }

  // int fanLevel = getFanLevel();
  // if(fanLevel != lastFanLevel)
  // {
  //   lastFanLevel = fanLevel;
  //   setFanLevel(fanLevel);
  // }


  // calculate RPM
  long rpm_1 = calculateRpm(fanInTachCounter_1);
  long rpm_2 = calculateRpm(fanInTachCounter_2);



  // TEMP
  readSensors();
  TempSensor *ctrl = controlSensor >= 0 ? &sensors[controlSensor] : nullptr;

  // Adjust cooling for temperature
  if(!ctrl)
  {
    coolingFailSafe();
    response = "!";
  }
  else if((millis() - timeSinceCoolingUpdate) > COOLING_UPDATE_INTERVAL)
  {
    // every COOLING_UPDATE_INTERVAL
    timeSinceCoolingUpdate = millis();
    response = adjustCooling(ctrl->temp);
  }

  // update display
  display.clearDisplay();
  display.setCursor(0,0);
  if(ctrl)
  {
    Serial.print("Temperature: "); Serial.print(ctrl->temp); Serial.print(" *C ("); Serial.print(ctrl->name); Serial.println(")");
    Serial.print("Humidity: "); Serial.print(ctrl->humidity); Serial.println("% rH");
    display.print("Temp: "); display.print(ctrl->temp); display.print(" *C "); display.print(ctrl->name); display.print("\n");
    display.print("Humidity: "); display.print(ctrl->humidity); display.print("% rH\n");
  }
  else
  {
    display.print("Temp: ! no sensor\n");
    display.print("Humidity: -\n");
  }
  if(monitorSensor >= 0)
  {
    TempSensor &mon = sensors[monitorSensor];
    display.print("Mon: ");
    if(mon.ok)
    {
      display.print(mon.temp, 1); display.print("C "); display.print(mon.humidity, 0); display.print("% ");
    }
    else
    {
      display.print("! ");
    }
    display.print(mon.name);
  }
  display.print("\n");
  display.print("Light level: "); display.print(lightLevel); display.print("\n");
  //display.print("Fan level: "); display.print(fanLevel); display.print("\n");
  display.print("Fan 1 speed: "); display.print(rpm_1); display.print(" rpm"); display.print("\n");
  display.print("Fan 2 speed: "); display.print(rpm_2); display.print(" rpm"); display.print("\n");
  display.print("INFO: "); if(ctrl) display.print(ctrl->temp); else display.print("-"); display.print("/"); display.print(tempSetPoint); display.print(":"); display.println(response);
  display.print(deviceId);
  display.print(" W:"); display.print(WiFi.status() == WL_CONNECTED ? "y" : "n");
  display.print(" M:"); display.print(mqttClient.connected() ? "y" : "n");
  display.display();

  // Network (cooling above runs regardless of WiFi/MQTT)
  maintainWifi();
  maintainMqtt();
  publishState(rpm_1, rpm_2);

  delay(500);
}
