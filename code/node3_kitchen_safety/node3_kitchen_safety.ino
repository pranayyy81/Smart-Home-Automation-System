/*
  NODE 3 – KITCHEN (Gas & Fire Safety)
  Smart Home Automation System – ESP32 + Blynk IoT (3 nodes)
  ===========================================================
  Monitors : gas level (MQ-2), flame (flame sensor)
  Manages  : gas valve (servo), exhaust fan (LED)

  SMART SAFETY
    Gas leak  -> gas valve CLOSES, exhaust fan ON, buzzer, phone alert,
                 front door UNLOCKS (Entrance Hub), Living Room escape lights ON.
                 Exhaust keeps running 30 s after the gas clears (air purge).
    Fire      -> gas valve CLOSES, buzzer, phone alert, door unlocks, escape lights.
    AWAY mode -> gas valve closes automatically (no cooking when nobody is home).
    The valve can only be re-opened from the app when it is safe.

  Wiring
    MQ-2 module   : VCC -> VIN (5V), GND -> GND,
                    AO -> 10k resistor -> GPIO34, and GPIO34 -> 20k resistor -> GND
                    (voltage divider – MQ-2 gives up to 5V, ESP32 takes max 3.3V)
    Flame sensor  : VCC -> 3V3, GND -> GND, DO -> GPIO27  (LOW when flame seen)
    Servo (SG90)  : brown -> GND, red -> VIN (5V), orange -> GPIO18   (gas valve)
    Exhaust LED   : GPIO26 -> 220 ohm -> LED -> GND
    Buzzer        : (+) -> GPIO25, (-) -> GND             (active buzzer)
    Warning LED   : on-board LED (GPIO2) – ON when valve closed, blinks on danger

  Blynk template "Kitchen Safety" – datastreams (Virtual pins)
    V0 Gas level   Integer 0-100 %  -> Gauge / SuperChart
    V1 Flame       Integer 0-1      -> LED widget
    V2 Gas valve   Integer 0-1      -> Switch (1 = open)
    V3 Exhaust     Integer 0-1      -> Switch
    V4 Status      String           -> Label
    V5 House mode  Integer 0-2      -> Value display (set by Entrance Hub)
  Events (notifications ON): gas_leak (Critical), fire (Critical)

  Libraries: Blynk, ESP32Servo (by Kevin Harrington)
*/

// ---- From this node's device in Blynk (Device Info) ----
#define BLYNK_TEMPLATE_ID   "TMPLxxxxxxxx"
#define BLYNK_TEMPLATE_NAME "Kitchen Safety"
#define BLYNK_AUTH_TOKEN    "KitchenTokenHere"

#define BLYNK_PRINT Serial

#include <WiFi.h>
#include <WiFiClient.h>
#include <BlynkSimpleEsp32.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ESP32Servo.h>

char ssid[] = "YourWiFiName";
char pass[] = "YourWiFiPassword";

// ---- Other nodes (copy each device's Auth Token from Blynk) ----
const char* HUB_TOKEN      = "EntranceHubTokenHere";
const char* LIVING_TOKEN   = "LivingRoomTokenHere";
const char* BLYNK_API_HOST = "blr1.blynk.cloud";   // your region (blr1, sgp1, fra1, ny3 ...)

// ---- Pins ----
#define GAS_PIN      34
#define FLAME_PIN    27
#define SERVO_PIN    18
#define EXHAUST_PIN  26
#define BUZZER_PIN   25
#define WARN_LED     2

const bool FLAME_ACTIVE_LOW = true;    // most flame modules: LOW = flame detected

// ---- Settings ----
const int GAS_LIMIT = 40;              // % – gas leak starts here
const int GAS_CLEAR = 35;              // % – gas leak ends below this (hysteresis)
const int VALVE_OPEN_ANGLE   = 0;
const int VALVE_CLOSED_ANGLE = 90;
const unsigned long GAS_WARMUP_MS  = 60000;            // MQ-2 heat-up time
const unsigned long PURGE_MS       = 30000;            // exhaust runs on after gas clears
const unsigned long CLEAR_DELAY_MS = 5000;             // danger must be gone 5 s to clear
const unsigned long ALERT_GAP_MS   = 5UL * 60 * 1000;  // max one alert per type per 5 min

enum Mode  { HOME = 0, AWAY = 1, NIGHT = 2 };
enum Alert { ALERT_NONE = 0, ALERT_INTRUDER = 1, ALERT_EMERGENCY = 2, ALERT_DOORBELL = 3 };
const char* modeNames[] = { "HOME", "AWAY", "NIGHT" };

Servo valve;
BlynkTimer timer;

Mode houseMode = HOME;
int  gasPct = 0, sentGas = -100;
bool gasLeak = false, flame = false, danger = false;
bool valveOpen = false, exhaustOn = false, manualExhaust = false;
unsigned long lastDangerSeen = 0, lastGasSeen = 0, lastGasAlert = 0, lastFireAlert = 0;
bool beep = false;
String lastStatus = "";

// ---------------- Talk to other nodes (Blynk HTTP API) ----------------
void sendToNode(const char* token, const char* pin, int value) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = String("https://") + BLYNK_API_HOST + "/external/api/update?token=" +
               token + "&" + pin + "=" + String(value);
  http.setTimeout(4000);
  if (http.begin(client, url)) {
    int code = http.GET();
    Serial.printf("-> node %s=%d  (HTTP %d)\n", pin, value, code);   // 200 = OK
    http.end();
  }
}

// ---------------- Outputs ----------------
void setValve(bool open) {
  valveOpen = open;
  valve.write(open ? VALVE_OPEN_ANGLE : VALVE_CLOSED_ANGLE);
  Blynk.virtualWrite(V2, open ? 1 : 0);
  Serial.println(open ? "Gas valve OPEN" : "Gas valve CLOSED");
}

void setExhaust(bool on) {
  digitalWrite(EXHAUST_PIN, on);
  if (on != exhaustOn) { exhaustOn = on; Blynk.virtualWrite(V3, on ? 1 : 0); }
}

bool purging() { return lastGasSeen != 0 && millis() - lastGasSeen < PURGE_MS; }

void updateStatus() {
  String s;
  if (millis() < GAS_WARMUP_MS) s += "Gas sensor warming | ";
  if (flame)        s += "FIRE! | ";
  if (gasLeak)      s += "GAS LEAK! | ";
  else if (purging()) s += "Purging air | ";
  s += "Valve " + String(valveOpen ? "OPEN" : "CLOSED");
  s += " | Exhaust " + String(exhaustOn ? "ON" : "OFF");
  s += " | House " + String(modeNames[houseMode]);
  if (s != lastStatus) { Blynk.virtualWrite(V4, s); lastStatus = s; }   // send only on change
}

// ---------------- Monitoring (every 500 ms) ----------------
void readSensors() {
  gasPct = map(analogRead(GAS_PIN), 0, 4095, 0, 100);
  if (abs(gasPct - sentGas) >= 2) { Blynk.virtualWrite(V0, gasPct); sentGas = gasPct; }

  if (millis() > GAS_WARMUP_MS) {
    gasLeak = gasLeak ? gasPct >= GAS_CLEAR : gasPct >= GAS_LIMIT;
  }

  bool raw = digitalRead(FLAME_PIN) == HIGH;
  bool f = FLAME_ACTIVE_LOW ? !raw : raw;
  if (f != flame) { flame = f; Blynk.virtualWrite(V1, f ? 1 : 0); }

  if (gasLeak) lastGasSeen = millis();
  if (gasLeak || flame) lastDangerSeen = millis();
  bool nowDanger = lastDangerSeen != 0 && millis() - lastDangerSeen < CLEAR_DELAY_MS;

  // Danger starts -> act here and tell the other rooms
  if (nowDanger && !danger) {
    danger = true;
    setValve(false);
    sendToNode(HUB_TOKEN, "V6", 1);                     // unlock front door
    sendToNode(LIVING_TOKEN, "V9", ALERT_EMERGENCY);    // escape lighting
  }
  // Danger over -> tell the other rooms (valve stays closed until reopened in the app)
  else if (!nowDanger && danger) {
    danger = false;
    sendToNode(HUB_TOKEN, "V6", 0);
    sendToNode(LIVING_TOKEN, "V9", ALERT_NONE);
  }

  // Phone notifications (rate-limited)
  if (gasLeak && (lastGasAlert == 0 || millis() - lastGasAlert > ALERT_GAP_MS)) {
    Blynk.logEvent("gas_leak", "Gas leak in the kitchen (" + String(gasPct) +
                   "%)! Valve closed, exhaust ON, door unlocked.");
    lastGasAlert = millis();
  }
  if (flame && (lastFireAlert == 0 || millis() - lastFireAlert > ALERT_GAP_MS)) {
    Blynk.logEvent("fire", "Fire detected in the kitchen! Gas valve closed, door unlocked.");
    lastFireAlert = millis();
  }
}

// ---------------- Automation (every 300 ms) ----------------
void automation() {
  beep = !beep;

  // Exhaust: forced ON for gas leak and purge; manual otherwise (not during fire)
  bool wantExhaust = gasLeak || purging() || (manualExhaust && !flame);
  setExhaust(wantExhaust);

  // AWAY safety: no gas supply when nobody is home
  if (houseMode == AWAY && valveOpen) setValve(false);

  digitalWrite(BUZZER_PIN, danger && beep);
  digitalWrite(WARN_LED, danger ? beep : !valveOpen);

  updateStatus();
}

// ---------------- Management: commands from the app ----------------
BLYNK_WRITE(V2) {                       // Gas valve
  bool wantOpen = param.asInt() == 1;
  if (wantOpen && (danger || houseMode == AWAY)) {   // unsafe -> refuse
    Blynk.virtualWrite(V2, 0);
    return;
  }
  setValve(wantOpen);
}

BLYNK_WRITE(V3) {                       // Exhaust fan (manual)
  manualExhaust = param.asInt() == 1;
  if (!manualExhaust && (gasLeak || purging())) Blynk.virtualWrite(V3, 1);   // forced ON
}

BLYNK_WRITE(V5) {                       // House mode (from Entrance Hub)
  houseMode = (Mode)constrain(param.asInt(), 0, 2);
  Serial.printf("House mode -> %s\n", modeNames[houseMode]);
}

BLYNK_CONNECTED() {
  Blynk.syncVirtual(V5, V2, V3);
}

void setup() {
  Serial.begin(115200);

  pinMode(FLAME_PIN, INPUT);
  pinMode(EXHAUST_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(WARN_LED, OUTPUT);
  digitalWrite(EXHAUST_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  valve.setPeriodHertz(50);
  valve.attach(SERVO_PIN, 500, 2400);
  valve.write(VALVE_CLOSED_ANGLE);      // start closed (safe); app sync re-opens if allowed

  analogReadResolution(12);

  Blynk.begin(BLYNK_AUTH_TOKEN, ssid, pass);

  timer.setInterval(500L, readSensors);
  timer.setInterval(300L, automation);
}

void loop() {
  Blynk.run();
  timer.run();
}
