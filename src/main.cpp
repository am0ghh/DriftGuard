#include <Arduino.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#define VRX_PIN    13
#define VRY_PIN    14
#define SW_PIN     27
#define POT_PIN    26
#define BUZZER_PIN 33
#define LED_PIN    2   // onboard blue LED

// Buzzer PWM — low duty cycle = quiet
#define BUZZER_CHANNEL 0
#define BUZZER_FREQ    2000
#define BUZZER_RES     8
#define BUZZER_DUTY    25   // ~10% duty

#define SERVICE_UUID        "12345678-1234-1234-1234-123456789abc"
#define CHARACTERISTIC_UUID "abcd1234-ab12-ab12-ab12-abcdef123456"
#define COMMAND_UUID        "abcd1234-ab12-ab12-ab12-abcdef123457"

Preferences prefs;
BLEServer* pServer = nullptr;
BLECharacteristic* pCharacteristic = nullptr;
bool deviceConnected = false;

int xCenter, yCenter, xMin, xMax, yMin, yMax;
float xFiltered, yFiltered;
const float ALPHA = 0.2;

// =============================================================
// LED state machine (non-blocking)
// =============================================================
// Patterns: alternating ON/OFF durations in ms.
// Phase 0 always starts ON. Solid = count of 0 (no blinking).

enum LedState {
  LED_ADVERTISING,   // slow pulse      — waiting for connection
  LED_CONNECTED,     // solid on        — connected, idle
  LED_SCROLLING,     // fast blink      — scroll mode active
  LED_DWELL,         // double blink    — dwell countdown
  LED_CALIBRATING    // rapid flicker   — calibration in progress
};

const uint16_t PAT_ADVERTISING[]  = {1000, 1000};          // slow pulse
const uint16_t PAT_SCROLLING[]    = {100,  100};            // fast blink
const uint16_t PAT_DWELL[]        = {100,  100, 100, 500};  // double blink
const uint16_t PAT_CALIBRATING[]  = {50,   50};             // rapid flicker

struct LedPattern { const uint16_t* phases; uint8_t count; };

const LedPattern PATTERNS[] = {
  {PAT_ADVERTISING, 2},  // LED_ADVERTISING
  {nullptr,         0},  // LED_CONNECTED  (solid — no pattern)
  {PAT_SCROLLING,   2},  // LED_SCROLLING
  {PAT_DWELL,       4},  // LED_DWELL
  {PAT_CALIBRATING, 2},  // LED_CALIBRATING
};

LedState          ledState      = LED_ADVERTISING;
uint8_t           ledPhase      = 0;
unsigned long     ledLastChange = 0;

void setLedState(LedState state) {
  ledState      = state;
  ledPhase      = 0;
  ledLastChange = millis();
  // all patterns start ON; solid connected also starts ON
  digitalWrite(LED_PIN, HIGH);
}

void updateLED() {
  const LedPattern& pat = PATTERNS[ledState];
  if (pat.count == 0) return;  // solid — nothing to do

  if (millis() - ledLastChange >= pat.phases[ledPhase]) {
    ledPhase      = (ledPhase + 1) % pat.count;
    ledLastChange = millis();
    digitalWrite(LED_PIN, (ledPhase % 2 == 0) ? HIGH : LOW);
  }
}

// =============================================================
// Buzzer helpers
// =============================================================
void buzzerOn()   { ledcWrite(BUZZER_CHANNEL, BUZZER_DUTY); }
void buzzerOff()  { ledcWrite(BUZZER_CHANNEL, 0); }

void beepShort()  { buzzerOn(); delay(50);  buzzerOff(); }
void beepLong()   { buzzerOn(); delay(300); buzzerOff(); }
void beepDouble() { beepShort(); delay(60); beepShort(); }
void beepTriple() { beepShort(); delay(60); beepShort(); delay(60); beepShort(); }

// =============================================================
// BLE command characteristic — Python writes commands here
// =============================================================
class CommandCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pChar) {
    std::string val = pChar->getValue();
    if (val.length() == 0) return;
    switch (val[0]) {
      case '1': beepShort();                              break; // click
      case '2': beepDouble();                             break; // profile switch
      case '3': setLedState(LED_SCROLLING);               break; // scroll on
      case '4': setLedState(LED_CONNECTED);               break; // scroll off
      case '5': setLedState(LED_DWELL);                   break; // dwell countdown
      case '6': setLedState(LED_CONNECTED);               break; // dwell reset
    }
  }
};

// =============================================================
// BLE connection callbacks
// =============================================================
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    deviceConnected = true;
    Serial.println("Client connected!");
    setLedState(LED_CONNECTED);
    beepLong();
  }
  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    Serial.println("Client disconnected — restarting advertising...");
    setLedState(LED_ADVERTISING);
    beepTriple();
    pServer->startAdvertising();
  }
};

// =============================================================
// Calibration
// =============================================================
void calibrate() {
  setLedState(LED_CALIBRATING);
  Serial.println("=== CALIBRATION MODE ===");
  Serial.println("Step 1: Leave joystick centered...");

  for (int i = 3; i > 0; i--) {
    Serial.printf("%d...\n", i);
    delay(1000);
  }

  long xSum = 0, ySum = 0;
  for (int i = 0; i < 100; i++) {
    xSum += analogRead(VRX_PIN);
    ySum += analogRead(VRY_PIN);
    delay(10);
  }
  xCenter   = xSum / 100;
  yCenter   = ySum / 100;
  xFiltered = xCenter;
  yFiltered = yCenter;
  Serial.printf("Center captured! X=%d Y=%d\n", xCenter, yCenter);

  Serial.println("Step 2: Move joystick to ALL extremes (up, down, left, right, corners)...");
  for (int i = 6; i > 0; i--) {
    Serial.printf("%d seconds remaining...\n", i);
    delay(1000);
  }

  xMin = xCenter; xMax = xCenter;
  yMin = yCenter; yMax = yCenter;

  unsigned long start = millis();
  while (millis() - start < 6000) {
    int x = analogRead(VRX_PIN);
    int y = analogRead(VRY_PIN);
    if (x < xMin) xMin = x;
    if (x > xMax) xMax = x;
    if (y < yMin) yMin = y;
    if (y > yMax) yMax = y;
    delay(10);
  }

  prefs.begin("driftguard", false);
  prefs.putInt("xCenter", xCenter);
  prefs.putInt("yCenter", yCenter);
  prefs.putInt("xMin",    xMin);
  prefs.putInt("xMax",    xMax);
  prefs.putInt("yMin",    yMin);
  prefs.putInt("yMax",    yMax);
  prefs.putBool("calibrated", true);
  prefs.end();

  Serial.println("=== CALIBRATION SAVED ===");
  Serial.printf("X: center=%d min=%d max=%d\n", xCenter, xMin, xMax);
  Serial.printf("Y: center=%d min=%d max=%d\n", yCenter, yMin, yMax);
  Serial.println("=== STARTING NORMAL OPERATION ===");

  setLedState(LED_ADVERTISING);
}

void loadCalibration() {
  prefs.begin("driftguard", true);
  xCenter = prefs.getInt("xCenter", 2048);
  yCenter = prefs.getInt("yCenter", 2048);
  xMin    = prefs.getInt("xMin",    0);
  xMax    = prefs.getInt("xMax",    4095);
  yMin    = prefs.getInt("yMin",    0);
  yMax    = prefs.getInt("yMax",    4095);
  prefs.end();

  xFiltered = xCenter;
  yFiltered = yCenter;

  Serial.println("=== CALIBRATION LOADED ===");
  Serial.printf("X: center=%d min=%d max=%d\n", xCenter, xMin, xMax);
  Serial.printf("Y: center=%d min=%d max=%d\n", yCenter, yMin, yMax);
}

// =============================================================
// BLE setup
// =============================================================
void setupBLE() {
  BLEDevice::init("DriftGuard");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService* pService = pServer->createService(SERVICE_UUID);

  pCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pCharacteristic->addDescriptor(new BLE2902());

  BLECharacteristic* pCommand = pService->createCharacteristic(
    COMMAND_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );
  pCommand->setCallbacks(new CommandCallbacks());

  pService->start();

  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->start();
  Serial.println("BLE advertising as 'DriftGuard' — waiting for connection...");
}

// =============================================================
// Setup
// =============================================================
void setup() {
  Serial.begin(115200);
  delay(2000);

  pinMode(SW_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  analogSetAttenuation(ADC_11db);

  ledcSetup(BUZZER_CHANNEL, BUZZER_FREQ, BUZZER_RES);
  ledcAttachPin(BUZZER_PIN, BUZZER_CHANNEL);

  setLedState(LED_ADVERTISING);

  prefs.begin("driftguard", true);
  bool calibrated = prefs.getBool("calibrated", false);
  prefs.end();

  if (!calibrated) {
    Serial.println("No calibration found — running calibration...");
    calibrate();
  } else if (digitalRead(SW_PIN) == LOW) {
    Serial.println("Button held — entering calibration mode...");
    calibrate();
  } else {
    loadCalibration();
  }

  Serial.println("Setting up BLE...");
  setupBLE();
}

// =============================================================
// Loop
// =============================================================
void loop() {
  updateLED();

  int rawX = analogRead(VRX_PIN);
  int rawY = analogRead(VRY_PIN);
  int btn  = digitalRead(SW_PIN);
  int sensitivity = map(analogRead(POT_PIN), 0, 4095, 1, 50);

  xFiltered = ALPHA * rawX + (1 - ALPHA) * xFiltered;
  yFiltered = ALPHA * rawY + (1 - ALPHA) * yFiltered;

  int xMapped = constrain(map((int)xFiltered, xMin, xMax, -100, 100), -100, 100);
  int yMapped = constrain(map((int)yFiltered, yMin, yMax, -100, 100), -100, 100);

  if (abs(xMapped) < 20) xMapped = 0;
  if (abs(yMapped) < 25) yMapped = 0;

  if (deviceConnected) {
    char payload[20];
    snprintf(payload, sizeof(payload), "%d,%d,%d,%d", xMapped, yMapped, btn, sensitivity);
    pCharacteristic->setValue((uint8_t*)payload, strlen(payload));
    pCharacteristic->notify();
  }

  Serial.print("X: "); Serial.print(xMapped);
  Serial.print(" | Y: "); Serial.print(yMapped);
  Serial.print(" | BTN: "); Serial.print(btn);
  Serial.print(" | SENS: "); Serial.println(sensitivity);

  delay(10);
}
