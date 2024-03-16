#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Adafruit_AHTX0.h>

Adafruit_AHTX0 aht;
Adafruit_SH1107 display = Adafruit_SH1107(64, 128, &Wire);

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
uint32_t fanInTachCounter_1 = 0;
uint32_t fanInTachCounter_2 = 0;
int lastLightLevel = 0;
int lastFanLevel = 0;
const float ADC_RES = 4095.0f;
float tempSetPoint = 22; // setting temp to 22*C
float TEMP_DELTA = 0.5; // allows temp to be +/- *C
int inFanPwmLevel = 0;
uint32_t timeSinceCoolingUpdate = millis();
int16_t COOLING_UPDATE_INTERVAL = 5000; // in mS
String response = "X";
int lightLevel = 64; // default on startup
uint32_t lastLightSetTime = 0;
uint32_t lastTempSetTime = 0;
const uint8_t MAX_SETTABLE_TEMP = 26;
const uint8_t MIN_SETTABLE_TEMP = 18;


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
    if(tempSetPoint == MAX_SETTABLE_TEMP)
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
    if(tempSetPoint == MIN_SETTABLE_TEMP)
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
long calculateRpm(uint32_t &tachCount)
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

  if (! aht.begin()) {
  Serial.println("Could not find AHT10? Check wiring");
  while (1) delay(10);
  }
  Serial.println("AHT10 found");

  

  display.begin(0x3C, true); // Address 0x3C default
  display.display();
  delay(1000);


  display.clearDisplay();
  display.display();

  display.setRotation(3);
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0,0);
  display.display(); // actually display all of the above

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
    setLightLevel(lightLevel);
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
  sensors_event_t humidity, temp;
  aht.getEvent(&humidity, &temp);// populate temp and humidity objects with fresh data
  
  // Adjust cooling for temperature
  if((millis() - timeSinceCoolingUpdate) > COOLING_UPDATE_INTERVAL)
  {
    // every COOLING_UPDATE_INTERVAL 
    timeSinceCoolingUpdate = millis();
    response = adjustCooling(temp.temperature);
  }

  // update display
  Serial.print("Temperature: "); Serial.print(temp.temperature); Serial.println(" *C");
  Serial.print("Humidity: "); Serial.print(humidity.relative_humidity); Serial.println("% rH");
  display.clearDisplay();
  display.setCursor(0,0);
  display.print("Temp: "); display.print(temp.temperature); display.print(" *C\n");
  display.print("Humidity: "); display.print(humidity.relative_humidity); display.print("% rH\n\n");
  display.print("Light level: "); display.print(lightLevel); display.print("\n");
  //display.print("Fan level: "); display.print(fanLevel); display.print("\n");
  display.print("Fan 1 speed: "); display.print(rpm_1); display.print(" rpm"); display.print("\n");
  display.print("Fan 2 speed: "); display.print(rpm_2); display.print(" rpm"); display.print("\n");
  display.print("INFO: "); display.print(temp.temperature); display.print("/"); display.print(tempSetPoint); display.print(":"); display.println(response);
  display.display();
  delay(500);
}
