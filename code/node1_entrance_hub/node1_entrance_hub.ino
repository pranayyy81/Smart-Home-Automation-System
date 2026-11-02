/*
  NODE 1 – ENTRANCE HUB (Security + Smart Door Lock)
  Smart Home Automation System – ESP32 + Blynk IoT (3 nodes)
  ===========================================================
  This node is the "brain" of the house. It holds the HOUSE MODE and tells
  the other two nodes about it, and about alerts.

  HOUSE MODES (Segmented Switch in the app)
    HOME  : security off. Door auto-locks 20 s after it is closed.
    AWAY  : door locks, armed after 10 s. Door forced open OR motion -> intruder alarm.
    NIGHT : door locks, armed after 10 s. Door forced open -> alarm
            (motion ignored, because people are moving inside).

  SMART FEATURES
    - Smart lock (servo): lock/unlock from the app, auto-lock when door closed.
    - Remote guest entry: unlock from the app while AWAY -> no alarm, re-locks
      and re-arms automatically after the guest closes the door.
    - Smart doorbell: button press -> phone notification + Living Room lights blink.
    - Intruder: buzzer + phone alert + Living Room lights flash red.
    - Emergency from Kitchen (gas/fire): door UNLOCKS automatically for escape.

  Wiring
    Reed switch   : one wire -> GPIO14, other -> GND      (magnet on the door)
    IR sensor     : VCC -> 3V3, GND -> GND, OUT -> GPIO27
    Doorbell btn  : one leg -> GPIO13, other leg -> GND   (push button)
    Servo (SG90)  : brown -> GND, red -> VIN (5V), orange -> GPIO18   (door lock)
    Buzzer        : (+) -> GPIO25, (-) -> GND             (active buzzer)
    Green LED     : GPIO21 -> 220 ohm -> LED -> GND       (unlocked)
    Red LED       : GPIO22 -> 220 ohm -> LED -> GND       (locked)
    Armed LED     : on-board LED (GPIO2)

  Blynk template "Entrance Hub" – datastreams (Virtual pins)
    V0 House mode  Integer 0-2   -> Segmented Switch (0 Home, 1 Away, 2 Night)
    V1 Door lock   Integer 0-1   -> Switch (1 = locked)
    V2 Door open   Integer 0-1   -> LED widget
    V3 Motion      Integer 0-1   -> LED widget
    V4 Siren       Integer 0-1   -> Switch (ON = panic siren, OFF = silence alarm)
    V5 Status      String        -> Label
    V6 Emergency   Integer 0-1   -> (no widget needed – set by the Kitchen node)
  Events (notifications ON): intruder (Critical), doorbell (Info)

  Libraries: Blynk, ESP32Servo (by Kevin Harrington)
*/

// ---- From this node's device in Blynk (Device Info) ----
#define BLYNK_TEMPLATE_ID   "TMPLxxxxxxxx"
#define BLYNK_TEMPLATE_NAME "Entrance Hub"
#define BLYNK_AUTH_TOKEN    "EntranceHubTokenHere"

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
const char* LIVING_TOKEN   = "LivingRoomTokenHere";
const char* KITCHEN_TOKEN  = "KitchenTokenHere";
// Your Blynk server region – shown in the Blynk console (e.g. blr1, sgp1, fra1, ny3)
const char* BLYNK_API_HOST = "blr1.blynk.cloud";

// ---- Pins ----
#define REED_PIN    14
#define IR_PIN      27
#define BELL_PIN    13
#define SERVO_PIN   18
#define BUZZER_PIN  25
#define GREEN_LED   21
#define RED_LED     22
#define ARMED_LED   2

const bool IR_ACTIVE_LOW = true;        // FC-51 IR: LOW = detected. HC-SR501 PIR: false

const int LOCKED_ANGLE   = 90;          // servo angle when locked
const int UNLOCKED_ANGLE = 0;           // servo angle when unlocked

const unsigned long EXIT_DELAY_MS   = 10000;   // time to leave after locking/arming
const unsigned long AUTO_LOCK_MS    = 20000;   // auto-lock after door closed this long
const unsigned long DOORBELL_GAP_MS = 5000;    // ignore repeated presses

enum Mode  { HOME = 0, AWAY = 1, NIGHT = 2 };
enum Alert { ALERT_NONE = 0, ALERT_INTRUDER = 1, ALERT_EMERGENCY = 2, ALERT_DOORBELL = 3 };
const char* modeNames[] = { "HOME", "AWAY", "NIGHT" };

Servo lockServo;
BlynkTimer timer;

Mode mode = HOME;
bool locked = false, doorOpen = false, motion = false;
bool alarmOn = false, siren = false, emergency = false;
unsigned long armedAt = 0, doorClosedAt = 0, lastBell = 0, chimeUntil = 0;
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
void setLock(bool lock) {
  locked = lock;
  lockServo.write(lock ? LOCKED_ANGLE : UNLOCKED_ANGLE);
  digitalWrite(RED_LED, lock);
  digitalWrite(GREEN_LED, !lock);
  Blynk.virtualWrite(V1, lock ? 1 : 0);
  if (lock) armedAt = millis();        // (re)start exit delay
  else      doorClosedAt = millis();   // restart auto-lock timer
}

void updateStatus() {
  String s;
  if (emergency) s += "EMERGENCY - door unlocked | ";
  if (alarmOn)   s += "INTRUDER! | ";
  if (mode != HOME && locked && millis() - armedAt < EXIT_DELAY_MS) s += "Arming... | ";
  s += modeNames[mode];
  s += locked ? " | LOCKED" : " | UNLOCKED";
  s += doorOpen ? " | Door OPEN" : " | Door closed";
  if (s != lastStatus) { Blynk.virtualWrite(V5, s); lastStatus = s; }   // send only on change
}

void triggerAlarm(const String& reason) {
  alarmOn = true;
  Blynk.virtualWrite(V4, 1);
  Blynk.logEvent("intruder", reason);
  Serial.println("ALARM: " + reason);
  sendToNode(LIVING_TOKEN, "V9", ALERT_INTRUDER);
  updateStatus();
}

void clearAlarm() {
  if (!alarmOn) return;
  alarmOn = false;
  sendToNode(LIVING_TOKEN, "V9", ALERT_NONE);
}

void setMode(Mode m) {
  mode = m;
  if (mode == HOME) clearAlarm();
  else if (!emergency) setLock(true);          // AWAY / NIGHT always lock the door
  digitalWrite(ARMED_LED, mode != HOME);
  sendToNode(LIVING_TOKEN,  "V8", mode);       // tell the other rooms
  sendToNode(KITCHEN_TOKEN, "V5", mode);
  updateStatus();
}

// ---------------- Monitoring (every 200 ms) ----------------
void readSensors() {
  bool irRaw = digitalRead(IR_PIN) == HIGH;
  bool m = IR_ACTIVE_LOW ? !irRaw : irRaw;
  bool d = digitalRead(REED_PIN) == HIGH;      // magnet away -> door open

  if (m != motion)   { motion = m; Blynk.virtualWrite(V3, m ? 1 : 0); }
  if (d != doorOpen) {
    doorOpen = d;
    Blynk.virtualWrite(V2, d ? 1 : 0);
    if (!d) doorClosedAt = millis();
    updateStatus();
  }

  // Intrusion: only while locked & armed (an app unlock = authorised guest entry)
  bool armed = (mode != HOME) && locked && (millis() - armedAt > EXIT_DELAY_MS);
  if (armed && !alarmOn && !emergency) {
    if (doorOpen)                   triggerAlarm(String("Door forced open in ") + modeNames[mode] + " mode!");
    else if (mode == AWAY && motion) triggerAlarm("Motion detected while you are away!");
  }

  // Smart doorbell
  if (digitalRead(BELL_PIN) == LOW && millis() - lastBell > DOORBELL_GAP_MS) {
    lastBell = millis();
    chimeUntil = millis() + 400;
    Serial.println("Doorbell pressed");
    Blynk.logEvent("doorbell", "Someone is at the door");
    sendToNode(LIVING_TOKEN, "V9", ALERT_DOORBELL);
  }
}

// ---------------- Automation (every 1 s) ----------------
void autoLock() {
  if (!locked && !doorOpen && !emergency && doorClosedAt != 0 &&
      millis() - doorClosedAt > AUTO_LOCK_MS) {
    Serial.println("Auto-lock");
    setLock(true);
  }
  updateStatus();
}

// Buzzer (every 300 ms): siren = continuous, alarm/emergency = beeping, doorbell = chime
void buzzerLoop() {
  beep = !beep;
  bool on;
  if (siren)                       on = true;
  else if (alarmOn || emergency)   on = beep;
  else                             on = millis() < chimeUntil;
  digitalWrite(BUZZER_PIN, on);
}

// ---------------- Management: commands from the app ----------------
BLYNK_WRITE(V0) {                       // House mode
  setMode((Mode)constrain(param.asInt(), 0, 2));
}

BLYNK_WRITE(V1) {                       // Lock / unlock
  bool wantLock = param.asInt() == 1;
  if (emergency && wantLock) { Blynk.virtualWrite(V1, 0); return; }   // keep escape route open
  setLock(wantLock);
  updateStatus();
}

BLYNK_WRITE(V4) {                       // Siren
  if (param.asInt() == 1) {
    siren = true;
  } else {
    siren = false;
    clearAlarm();
    armedAt = millis();                 // 10 s grace before re-arming
  }
  updateStatus();
}

BLYNK_WRITE(V6) {                       // Emergency signal from Kitchen node
  bool e = param.asInt() == 1;
  if (e && !emergency) {
    emergency = true;
    clearAlarm();
    setLock(false);                     // unlock door for escape
    Serial.println("EMERGENCY from kitchen - door unlocked");
  } else if (!e) {
    emergency = false;                  // door will auto-lock again once closed
  }
  updateStatus();
}

BLYNK_CONNECTED() {
  Blynk.syncVirtual(V0, V1, V6);        // restore mode, lock and emergency state
  Blynk.virtualWrite(V4, 0);
}

void setup() {
  Serial.begin(115200);

  pinMode(REED_PIN, INPUT_PULLUP);
  pinMode(IR_PIN, INPUT);
  pinMode(BELL_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(ARMED_LED, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  lockServo.setPeriodHertz(50);
  lockServo.attach(SERVO_PIN, 500, 2400);
  lockServo.write(UNLOCKED_ANGLE);
  digitalWrite(GREEN_LED, HIGH);

  Blynk.begin(BLYNK_AUTH_TOKEN, ssid, pass);

  timer.setInterval(200L,  readSensors);
  timer.setInterval(1000L, autoLock);
  timer.setInterval(300L,  buzzerLoop);
}

void loop() {
  Blynk.run();
  timer.run();
}
