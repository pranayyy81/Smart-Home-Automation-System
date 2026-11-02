/*
  NODE 2 – LIVING ROOM (Comfort + Smart Lighting)
  Smart Home Automation System – ESP32 + Blynk IoT (3 nodes)
  ===========================================================
  Monitors : temperature, humidity (DHT11), room light level (LDR)
  Manages  : fan (LED, PWM speed), room light (LED, PWM dimming), RGB mood light

  AUTO mode
    HOME  : fan ON at 28 °C, speed 40–100 % rising with temperature.
            Light turns ON when dark; the darker the room, the brighter the light.
            RGB = soft green.
    NIGHT : quiet fan (max 50 %), dim 10 % night light when dark. RGB = soft blue.
    AWAY  : fan and light OFF (energy saving). RGB = soft purple.
  MANUAL mode : fan + speed and light + brightness from the app (still OFF in AWAY).

  REACTS TO OTHER ROOMS (sent by Entrance Hub / Kitchen)
    Intruder  : room light flashes, RGB flashes red
    Gas/fire  : all lights full ON (escape lighting), fan OFF, RGB flashes red
    Doorbell  : light + RGB blink yellow for 6 s (visual doorbell)

  Wiring
    DHT11      : VCC -> 3V3, GND -> GND, DATA -> GPIO4
    LDR module : VCC -> 3V3, GND -> GND, AO -> GPIO35
    Fan LED    : GPIO26 -> 220 ohm -> LED -> GND      (brightness = fan speed)
    Light LED  : GPIO27 -> 220 ohm -> LED -> GND      (white/yellow LED)
    RGB LED    : R -> GPIO21, G -> GPIO22, B -> GPIO23, each through 220 ohm,
                 common pin (longest leg) -> GND       (common-cathode RGB LED)

  Blynk template "Living Room" – datastreams (Virtual pins)
    V0 Temperature   Double  0-60 °C   -> Gauge / SuperChart
    V1 Humidity      Double  0-100 %   -> Gauge / SuperChart
    V2 Light level   Integer 0-100 %   -> Gauge
    V3 Fan           Integer 0-1       -> Switch
    V4 Fan speed     Integer 0-100 %   -> Slider
    V5 Light         Integer 0-1       -> Switch
    V6 Brightness    Integer 0-100 %   -> Slider
    V7 Auto mode     Integer 0-1       -> Switch
    V8 House mode    Integer 0-2       -> Value display (set by Entrance Hub)
    V9 Alert         Integer 0-3       -> Value display (set by other nodes)

  Libraries: Blynk, DHT sensor library (Adafruit) + Adafruit Unified Sensor
*/

// ---- From this node's device in Blynk (Device Info) ----
#define BLYNK_TEMPLATE_ID   "TMPLxxxxxxxx"
#define BLYNK_TEMPLATE_NAME "Living Room"
#define BLYNK_AUTH_TOKEN    "LivingRoomTokenHere"

#define BLYNK_PRINT Serial

#include <WiFi.h>
#include <WiFiClient.h>
#include <BlynkSimpleEsp32.h>
#include <DHT.h>

char ssid[] = "YourWiFiName";
char pass[] = "YourWiFiPassword";

// ---- Pins ----
#define DHT_PIN    4
#define DHT_TYPE   DHT11
#define LDR_PIN    35
#define FAN_PIN    26
#define LIGHT_PIN  27
#define RGB_R      21
#define RGB_G      22
#define RGB_B      23

const bool LDR_INVERT       = true;    // if light % goes UP when you cover the LDR, set false
const bool RGB_COMMON_ANODE = false;   // set true if your RGB LED's long leg goes to 3V3

// ---- Settings ----
const float FAN_ON_TEMP   = 28.0;      // auto fan starts here (°C)
const float FAN_MAX_TEMP  = 35.0;      // 100 % speed here
const int   DARK_LIMIT    = 40;        // light % below this = dark
const int   NIGHT_LIGHT   = 10;        // night-light brightness %
const int   NIGHT_FAN_MAX = 50;        // quiet fan limit at night %
const unsigned long DOORBELL_SHOW_MS = 6000;

enum Mode  { HOME = 0, AWAY = 1, NIGHT = 2 };
enum Alert { ALERT_NONE = 0, ALERT_INTRUDER = 1, ALERT_EMERGENCY = 2, ALERT_DOORBELL = 3 };
const char* modeNames[] = { "HOME", "AWAY", "NIGHT" };

DHT dht(DHT_PIN, DHT_TYPE);
BlynkTimer timer;

Mode  houseMode = HOME;
Alert alert = ALERT_NONE;
unsigned long alertUntil = 0;

bool autoMode = true;
bool manualFan = false, manualLight = false;
int  manualFanSpeed = 70, manualBrightness = 80;
int  fanSpeed = 0, brightness = 0;            // % currently applied

float temperature = NAN, humidity = NAN;
int   lightPct = 100;
float sentT = -100, sentH = -100;
int   sentL = -100;
bool  flash = false;

int pct2pwm(int p) { return map(constrain(p, 0, 100), 0, 100, 0, 255); }

void setRGB(int r, int g, int b) {
  if (RGB_COMMON_ANODE) { r = 255 - r; g = 255 - g; b = 255 - b; }
  analogWrite(RGB_R, r);
  analogWrite(RGB_G, g);
  analogWrite(RGB_B, b);
}

// ---------------- Outputs (update the app only when something changes) ----------------
void applyFan(int speed) {
  speed = constrain(speed, 0, 100);
  analogWrite(FAN_PIN, pct2pwm(speed));
  if ((speed > 0) != (fanSpeed > 0)) Blynk.virtualWrite(V3, speed > 0 ? 1 : 0);
  if (autoMode && speed > 0 && speed != fanSpeed) Blynk.virtualWrite(V4, speed);
  fanSpeed = speed;
}

void applyLight(int level) {
  level = constrain(level, 0, 100);
  analogWrite(LIGHT_PIN, pct2pwm(level));
  if ((level > 0) != (brightness > 0)) Blynk.virtualWrite(V5, level > 0 ? 1 : 0);
  if (autoMode && level > 0 && level != brightness) Blynk.virtualWrite(V6, level);
  brightness = level;
}

// ---------------- Monitoring (every 3 s) ----------------
void readSensors() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t) && !isnan(h)) {
    temperature = t;
    humidity = h;
    if (fabs(t - sentT) >= 0.5) { Blynk.virtualWrite(V0, t); sentT = t; }
    if (fabs(h - sentH) >= 2)   { Blynk.virtualWrite(V1, h); sentH = h; }
  } else {
    Serial.println("DHT11 read failed – check wiring");
  }

  int l = map(analogRead(LDR_PIN), 0, 4095, 0, 100);
  if (LDR_INVERT) l = 100 - l;
  lightPct = l;
  if (abs(l - sentL) >= 3) { Blynk.virtualWrite(V2, l); sentL = l; }

  Serial.printf("[%s] T %.1f  H %.0f  Light %d%%  Fan %d%%  Lamp %d%%  Alert %d\n",
                modeNames[houseMode], temperature, humidity, lightPct, fanSpeed, brightness, alert);
}

// ---------------- Automation engine (every 300 ms) ----------------
void automation() {
  flash = !flash;

  if (alert == ALERT_DOORBELL && millis() > alertUntil) {   // doorbell blink ends by itself
    alert = ALERT_NONE;
    Blynk.virtualWrite(V9, 0);
  }

  // 1) Normal targets
  int wantFan = 0, wantLight = 0;
  bool dark = brightness > 0 ? lightPct < DARK_LIMIT + 5 : lightPct < DARK_LIMIT;   // hysteresis

  if (houseMode == AWAY) {
    wantFan = 0;
    wantLight = 0;                                   // energy saving
  } else if (autoMode) {
    if (!isnan(temperature)) {
      bool hot = fanSpeed > 0 ? temperature > FAN_ON_TEMP - 1 : temperature >= FAN_ON_TEMP;
      if (hot) {
        wantFan = map((int)(temperature * 10), (int)(FAN_ON_TEMP * 10), (int)(FAN_MAX_TEMP * 10), 40, 100);
        wantFan = constrain(wantFan, 40, 100);
      }
    }
    if (dark) wantLight = constrain(map(lightPct, DARK_LIMIT, 0, 30, 100), 30, 100);   // darker = brighter

    if (houseMode == NIGHT) {
      if (wantFan > 0) wantFan = min(wantFan, NIGHT_FAN_MAX);
      wantLight = dark ? NIGHT_LIGHT : 0;
    }
  } else {
    wantFan   = manualFan   ? manualFanSpeed   : 0;
    wantLight = manualLight ? manualBrightness : 0;
  }

  // 2) Alerts from other rooms override everything
  switch (alert) {
    case ALERT_INTRUDER:
      applyFan(wantFan);
      analogWrite(LIGHT_PIN, flash ? 255 : 0);
      setRGB(flash ? 255 : 0, 0, 0);
      return;
    case ALERT_EMERGENCY:
      applyFan(0);                                   // fan off during gas / fire
      applyLight(100);                               // full escape lighting
      setRGB(flash ? 255 : 0, 0, 0);
      return;
    case ALERT_DOORBELL:
      applyFan(wantFan);
      analogWrite(LIGHT_PIN, flash ? 255 : pct2pwm(wantLight));
      setRGB(flash ? 255 : 0, flash ? 150 : 0, 0);   // yellow blink
      return;
    default:
      break;
  }

  // 3) Normal operation
  applyFan(wantFan);
  applyLight(wantLight);
  if (houseMode == HOME)       setRGB(0, 50, 0);     // soft green
  else if (houseMode == NIGHT) setRGB(0, 0, 50);     // soft blue
  else                         setRGB(30, 0, 30);    // soft purple (away)
}

// ---------------- Management: commands from the app ----------------
BLYNK_WRITE(V3) {                        // Fan switch
  if (autoMode || houseMode == AWAY) { Blynk.virtualWrite(V3, fanSpeed > 0 ? 1 : 0); return; }
  manualFan = param.asInt() == 1;
}

BLYNK_WRITE(V4) {                        // Fan speed
  if (autoMode) { Blynk.virtualWrite(V4, fanSpeed); return; }
  manualFanSpeed = constrain(param.asInt(), 0, 100);
}

BLYNK_WRITE(V5) {                        // Light switch
  if (autoMode || houseMode == AWAY) { Blynk.virtualWrite(V5, brightness > 0 ? 1 : 0); return; }
  manualLight = param.asInt() == 1;
}

BLYNK_WRITE(V6) {                        // Brightness
  if (autoMode) { Blynk.virtualWrite(V6, brightness); return; }
  manualBrightness = constrain(param.asInt(), 0, 100);
}

BLYNK_WRITE(V7) {                        // Auto / Manual
  autoMode = param.asInt() == 1;
  if (!autoMode) {                       // start manual mode from the current state
    manualFan = fanSpeed > 0;
    if (fanSpeed > 0) manualFanSpeed = fanSpeed;
    manualLight = brightness > 0;
    if (brightness > 0) manualBrightness = brightness;
    Blynk.virtualWrite(V4, manualFanSpeed);
    Blynk.virtualWrite(V6, manualBrightness);
  }
}

BLYNK_WRITE(V8) {                        // House mode (from Entrance Hub)
  houseMode = (Mode)constrain(param.asInt(), 0, 2);
  Serial.printf("House mode -> %s\n", modeNames[houseMode]);
}

BLYNK_WRITE(V9) {                        // Alert (from Entrance Hub / Kitchen)
  alert = (Alert)constrain(param.asInt(), 0, 3);
  if (alert == ALERT_DOORBELL) alertUntil = millis() + DOORBELL_SHOW_MS;
  Serial.printf("Alert -> %d\n", alert);
}

BLYNK_CONNECTED() {
  Blynk.syncVirtual(V7, V3, V4, V5, V6, V8, V9);
}

void setup() {
  Serial.begin(115200);
  pinMode(FAN_PIN, OUTPUT);
  pinMode(LIGHT_PIN, OUTPUT);
  pinMode(RGB_R, OUTPUT);
  pinMode(RGB_G, OUTPUT);
  pinMode(RGB_B, OUTPUT);
  analogWrite(FAN_PIN, 0);
  analogWrite(LIGHT_PIN, 0);
  setRGB(0, 0, 0);

  analogReadResolution(12);
  dht.begin();

  Blynk.begin(BLYNK_AUTH_TOKEN, ssid, pass);

  timer.setInterval(3000L, readSensors);
  timer.setInterval(300L,  automation);
}

void loop() {
  Blynk.run();
  timer.run();
}
