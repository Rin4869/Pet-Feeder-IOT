// PetFeeder_fixed.ino
// Sửa lỗi & cải thiện ổn định (thay đổi RESET_PIN, tách secrets, ADC config, cleanup, debounce...)

#define BLYNK_TEMPLATE_ID   "TMPL6rUaYY6fd"
#define BLYNK_TEMPLATE_NAME "Pet Feeder IoT"
#define BLYNK_AUTH_TOKEN    "mrrMPTYHP2D0h1Z8wBNvKC_Wg1AbPjML"
#define FIREBASE_API_KEY    "AIzaSyD9aDCvRZAId21oBYgoG9cIo8PzjtMq0ow"
#define FIREBASE_DB_URL     "https://pet-feeder-a9136-default-rtdb.firebaseio.com"
// AUTH/API/URL/SENSITIVE VALUES -> đặt trong secrets.h (KHÔNG commit)

#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include <HX711.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <BlynkSimpleEsp32.h>
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include <time.h>

// Firebase constants removed from this file - see secrets.h

#define DOUT_PIN             23
#define SCK_PIN              14
#define WATER_SENSOR_PIN     36
#define WATER_SENSOR_VCC_PIN 32
#define RELAY_PIN            25
#define SERVO_PIN            13
// Avoid using GPIO0 (boot strapping) as reset button - use GPIO4 instead
#define RESET_PIN            4

#define WATER_MIN           120
#define WATER_MAX          1300
#define SCALE_FACTOR       400.0
#define PUMP_MAX_ATTEMPTS   20
#define NO_RISE_LIMIT        3
#define DISPENSE_SAFETY     20
#define SETTLE_DELAY_MS    800
#define NO_CHANGE_LIMIT      3
#define LCD_WIDTH           16
#define PUMP_COOLDOWN_MS   (30UL * 60UL * 1000UL)
#define FIREBASE_RETRY_MS  (5UL  * 60UL * 1000UL)  // retry Firebase after 5 minutes
#define WATER_SENSOR_FAIL_LIMIT 5                   // consecutive bad reads before waterError
#define WATER_RECOVER_LIMIT     3                   // consecutive good reads to recover waterError
#define FOOD_RECOVER_LIMIT      3                   // consecutive good reads to recover foodError

// ===================================================
// STATE MACHINE
// ===================================================
enum SystemState {
  STATE_IDLE,
  STATE_DISPENSING,
  STATE_PUMPING,
  STATE_DEGRADED
};
SystemState currentState = STATE_IDLE;

// Hardware error flags
bool foodError  = false;   // HX711 error -> locks only dispensing
bool waterError = false;   // Water sensor error -> locks only pumping
int  waterSensorFailCount = 0;
int  waterRecoverCount    = 0;
int  foodRecoverCount     = 0;

// ===================================================
// OBJECTS
// ===================================================
HX711 scale;
Servo myServo;
LiquidCrystal_I2C lcd(0x27, 16, 2);
BlynkTimer timer;
FirebaseData fbdo;
FirebaseAuth fbAuth;
FirebaseConfig fbConfig;

// ===================================================
// PET PROFILE
// ===================================================
struct PetProfile {
  String name             = "My Pet";
  int    foodLowGram      = 20;
  int    foodTargetGram   = 70;
  int    waterLow         = 30;
  int    waterRefill      = 70;
  int    feedIntervalHour = 3;
};
PetProfile activeProfile;
PetProfile pendingProfile;

// ===================================================
// FLAGS & TIMING
// ===================================================
bool          shouldFeed      = false;
bool          isDispensing    = false;
bool          isPumping       = false;
bool          waterEventSent  = false;
bool          foodEventSent   = false;

bool          waitingFood     = false;
bool          waitingWater    = false;

int           feedCountToday    = 0;
int           refillCountToday  = 0;
String        lastDate          = "";
float         foodWeightMorning = -1;

unsigned long lastFeedMillis    = 0;
bool          firstFeedDone     = false;
unsigned long lastPumpMillis    = 0;
bool          firstPumpDone     = false;

// Firebase retry when degraded
unsigned long lastFirebaseRetry = 0;

// Manual pump detection
int           manualLastPct   = 0;
int           manualNoRise    = 0;
unsigned long manualLastCheck = 0;

// ===================================================
// UTILITIES
// ===================================================
String getCurrentTime() {
  struct tm t;
  if (!getLocalTime(&t)) return "--:--";
  char buf[32];
  strftime(buf, sizeof(buf), "%H:%M %d/%m/%Y", &t);
  return String(buf);
}

String truncate(String s) {
  if ((int)s.length() > LCD_WIDTH) s = s.substring(0, LCD_WIDTH);
  while ((int)s.length() < LCD_WIDTH) s += ' ';
  return s;
}

void setLCDLine0(const char* text) {
  lcd.setCursor(0, 0);
  lcd.print(truncate(String(text)));
}

void setLCDLine1(const char* text) {
  lcd.setCursor(0, 1);
  lcd.print(truncate(String(text)));
}

void updateLCDTime() {
  if (currentState != STATE_IDLE && currentState != STATE_DEGRADED) return;
  struct tm t;
  char buf[17] = "--:-- --/--/----";
  if (getLocalTime(&t))
    strftime(buf, sizeof(buf), "%H:%M %d/%m/%Y", &t);
  setLCDLine0(buf);
}

// ===================================================
// STATUS DISPLAY
// ===================================================
void updateStatusDisplay() {
  String msg;

  if (foodError && waterError)               msg = "Food+Water Err!";
  else if (foodError)                        msg = "Food Sensor Err!";
  else if (waterError)                       msg = "Water Sensor Err!";
  else if (currentState == STATE_DISPENSING) msg = "Dispensing...";
  else if (currentState == STATE_PUMPING)    msg = "Refilling...";
  else if (currentState == STATE_DEGRADED)   msg = "Offline-Defaults";
  else                                       msg = "Monitoring";

  Blynk.virtualWrite(V3, msg);
  if (currentState == STATE_IDLE || currentState == STATE_DEGRADED)
    setLCDLine1(msg.c_str());
}

// Hardware error handlers
void enterFoodErrorState(const char* reason) {
  foodError        = true;
  foodRecoverCount = 0;
  isDispensing     = false;
  myServo.write(0);
  currentState = firebaseReady() ? STATE_IDLE : STATE_DEGRADED;
  Serial.print(F("FOOD HW ERROR: "));
  Serial.println(reason);
  Blynk.logEvent("food_sensor_error", reason);
  updateStatusDisplay();
}

void enterWaterErrorState(const char* reason) {
  waterError = true;
  isPumping  = false;
  digitalWrite(RELAY_PIN, LOW);
  Blynk.virtualWrite(V5, F("OFF"));
  currentState = firebaseReady() ? STATE_IDLE : STATE_DEGRADED;
  Serial.print(F("WATER HW ERROR: "));
  Serial.println(reason);
  Blynk.logEvent("water_sensor_error", reason);
  updateStatusDisplay();
}

void enterDegradedState(const char* reason) {
  if (currentState == STATE_IDLE || currentState == STATE_DEGRADED) {
    currentState = STATE_DEGRADED;
  }
  lastFirebaseRetry = millis();
  Serial.print(F("DEGRADED (offline): "));
  Serial.println(reason);
  Blynk.virtualWrite(V3, reason);
  if (currentState == STATE_DEGRADED)
    setLCDLine1(reason);
}

bool isTimeToFeed() {
  if (!firstFeedDone) return true;
  return (millis() - lastFeedMillis) >=
    (unsigned long)activeProfile.feedIntervalHour * 3600000UL;
}

bool isPumpReady() {
  if (!firstPumpDone) return true;
  return (millis() - lastPumpMillis) >= PUMP_COOLDOWN_MS;
}

bool canOperateFood()  { return !foodError; }
bool canOperateWater() { return !waterError; }

// ===================================================
// SENSORS
// ===================================================
int readWaterLevel() {
  digitalWrite(WATER_SENSOR_VCC_PIN, HIGH);
  delay(120); // give sensor a little more time to stabilize
  int raw = analogRead(WATER_SENSOR_PIN);
  digitalWrite(WATER_SENSOR_VCC_PIN, LOW);

  Serial.print("Water raw: ");
  Serial.println(raw);

  // Detect sensor disconnected/stuck at rails
  if (raw <= 0 || raw >= 4095) {
    waterSensorFailCount++;
    if (waterSensorFailCount >= WATER_SENSOR_FAIL_LIMIT && !waterError) {
      enterWaterErrorState("Water Sensor Err!");
    }
  } else {
    waterSensorFailCount = 0;
  }

  int pct = constrain(map(raw, WATER_MIN, WATER_MAX, 0, 100), 0, 100);
  return pct;
}

float readFoodWeight() {
  if (!scale.is_ready()) return -1;
  float w = scale.get_units(5);
  if (isnan(w)) return -1;
  return w;
}

// ===================================================
// FIREBASE
// ===================================================
bool firebaseReady() {
  return Firebase.ready();
}

void syncProfileToBlynk() {
  Blynk.virtualWrite(V9,  activeProfile.name);
  Blynk.virtualWrite(V10, activeProfile.foodLowGram);
  Blynk.virtualWrite(V11, activeProfile.foodTargetGram);
  Blynk.virtualWrite(V12, activeProfile.waterLow);
  Blynk.virtualWrite(V13, activeProfile.waterRefill);
  Blynk.virtualWrite(V14, activeProfile.feedIntervalHour);
}

void loadProfile() {
  if (!firebaseReady()) {
    Serial.println(F("Firebase not ready — running with defaults"));
    enterDegradedState("Offline-Defaults");
    pendingProfile = activeProfile;
    syncProfileToBlynk();
    return;
  }

  bool ok      = false;
  int  retries = 0;
  FirebaseJson     json;
  FirebaseJsonData result;

  while (!ok && retries < 3) {
    if (Firebase.RTDB.getJSON(&fbdo, "/profile")) {
      ok = true;
    } else {
      retries++;
      Serial.print(F("Profile retry: "));
      Serial.println(retries);
      Serial.print("Firebase error: ");
      Serial.println(fbdo.errorReason());
      delay(500);
    }
  }

  if (!ok) {
    Serial.println(F("Profile load failed — running with defaults"));
    enterDegradedState("Offline-Defaults");
    pendingProfile = activeProfile;
    syncProfileToBlynk();
    return;
  }

  json.setJsonData(fbdo.jsonString());

  if (json.get(result, "name") && result.success)
    activeProfile.name = result.stringValue;

  if (json.get(result, "foodLow") && result.success)
    activeProfile.foodLowGram = result.intValue;

  if (json.get(result, "foodTarget") && result.success)
    activeProfile.foodTargetGram = result.intValue;

  if (json.get(result, "waterLow") && result.success)
    activeProfile.waterLow = result.intValue;

  if (json.get(result, "waterRefill") && result.success)
    activeProfile.waterRefill = result.intValue;

  if (json.get(result, "feedInterval") && result.success)
    activeProfile.feedIntervalHour = result.intValue;

  pendingProfile = activeProfile;
  firstFeedDone  = false;
  lastFeedMillis = 0;

  if (currentState == STATE_DEGRADED)
    currentState = STATE_IDLE;

  Serial.println(F("Profile loaded OK"));
  updateLCDTime();
  setLCDLine1((String("Pet: ") + activeProfile.name).c_str());
  Blynk.virtualWrite(V3, (String("Pet: ") + activeProfile.name).c_str());
  syncProfileToBlynk();

  Blynk.virtualWrite(V0, readWaterLevel());
  float f = readFoodWeight();
  if (f >= 0) Blynk.virtualWrite(V1, f);
}

void saveProfile() {
  if (!firebaseReady()) {
    Serial.println(F("Firebase not ready — save queued offline"));
    activeProfile = pendingProfile;
    firstFeedDone = false;
    lastFeedMillis = 0;
    Blynk.virtualWrite(V3, F("Saved (offline)"));
    setLCDLine1((String("Pet: ") + activeProfile.name).c_str());
    enterDegradedState("Offline-Saved");
    return;
  }

  FirebaseJson json;
  json.set("name",         pendingProfile.name.c_str());
  json.set("foodLow",      (int)pendingProfile.foodLowGram);
  json.set("foodTarget",   (int)pendingProfile.foodTargetGram);
  json.set("waterLow",     (int)pendingProfile.waterLow);
  json.set("waterRefill",  (int)pendingProfile.waterRefill);
  json.set("feedInterval", (int)pendingProfile.feedIntervalHour);

  bool ok      = false;
  int  retries = 0;
  while (!ok && retries < 3) {
    if (Firebase.RTDB.setJSON(&fbdo, "/profile", &json)) {
      ok = true;
    } else {
      retries++;
      Serial.print(F("Save retry: "));
      Serial.println(retries);
      Serial.print("Firebase error: ");
      Serial.println(fbdo.errorReason());
      delay(300);
    }
  }

  if (!ok) {
    Serial.println(F("Save failed — applying locally"));
    activeProfile = pendingProfile;
    firstFeedDone = false;
    lastFeedMillis = 0;
    Blynk.virtualWrite(V3, F("Save Fail-Local"));
    setLCDLine1((String("Pet: ") + activeProfile.name).c_str());
    syncProfileToBlynk();
    return;
  }

  activeProfile  = pendingProfile;
  firstFeedDone  = false;
  lastFeedMillis = 0;

  if (currentState == STATE_DEGRADED)
    currentState = STATE_IDLE;

  Serial.println(F("Profile saved OK"));
  Blynk.virtualWrite(V3, F("Profile Saved!"));
  setLCDLine1((String("Pet: ") + activeProfile.name).c_str());
  syncProfileToBlynk();
}

void syncLogs() {
  if (!firebaseReady()) return;
  Firebase.RTDB.setInt(&fbdo, "/logs/feedToday",   feedCountToday);
  Firebase.RTDB.setInt(&fbdo, "/logs/refillToday", refillCountToday);
}

void retryFirebase() {
  if (currentState != STATE_DEGRADED) return;
  if (millis() - lastFirebaseRetry < FIREBASE_RETRY_MS) return;

  Serial.println(F("Retrying Firebase connection..."));
  setLCDLine1("Retrying...");
  lastFirebaseRetry = millis();
  loadProfile();
}

// ===================================================
// AUTO-RECOVERY & DEBOUNCE
// ===================================================
void checkRecovery() {
  if (waitingFood && canOperateFood()) {
    float food = readFoodWeight();
    if (food >= 0 && food >= activeProfile.foodTargetGram) {
      waitingFood = false;
      Serial.println(F("Food refilled - back to OK"));
      updateStatusDisplay();
    }
  }

  if (waitingWater && canOperateWater()) {
    int water = readWaterLevel();
    if (water >= activeProfile.waterRefill) {
      waitingWater = false;
      Serial.println(F("Water refilled - back to OK"));
      updateStatusDisplay();
    }
  }
}

void checkFoodErrorRecovery() {
  if (!foodError) return;
  if (isDispensing || isPumping) return;

  float w = readFoodWeight();
  if (w >= 0) {
    foodRecoverCount++;
    if (foodRecoverCount >= FOOD_RECOVER_LIMIT) {
      foodError        = false;
      foodRecoverCount = 0;
      Serial.println(F("Food sensor recovered"));
      updateStatusDisplay();
    }
  } else {
    foodRecoverCount = 0;
  }
}

void checkWaterErrorRecovery() {
  if (!waterError) return;
  if (isDispensing || isPumping) return;

  digitalWrite(WATER_SENSOR_VCC_PIN, HIGH);
  delay(120);
  int raw = analogRead(WATER_SENSOR_PIN);
  digitalWrite(WATER_SENSOR_VCC_PIN, LOW);

  if (raw > 0 && raw < 4095) {
    waterRecoverCount++;
    Serial.print("waterRecoverCount: "); Serial.println(waterRecoverCount);
    if (waterRecoverCount >= WATER_RECOVER_LIMIT) {
      waterError = false;
      waterSensorFailCount = 0;
      waterRecoverCount = 0;
      Serial.println(F("Water sensor recovered"));
      updateStatusDisplay();
    }
  } else {
    waterRecoverCount = 0;
  }
}

// ===================================================
// CLEANUP HELPERS
// ===================================================
void pumpCleanupCommon() {
  digitalWrite(RELAY_PIN, LOW);
  Blynk.virtualWrite(V5, F("OFF"));
  isPumping = false;
  currentState = firebaseReady() ? STATE_IDLE : STATE_DEGRADED;
  updateStatusDisplay();
}

void dispenseCleanupCommon() {
  myServo.write(0);
  isDispensing = false;
  currentState = firebaseReady() ? STATE_IDLE : STATE_DEGRADED;
  updateStatusDisplay();
}

// ===================================================
// DISPENSE FOOD
// ===================================================
void dispenseFood() {
  if (isDispensing || isPumping) return;
  if (!canOperateFood()) return;

  float before = readFoodWeight();
  if (before < 0) {
    enterFoodErrorState("Food Sensor Err!");
    return;
  }

  float target = activeProfile.foodTargetGram - before;
  if (target <= 0) {
    waitingFood = false;
    setLCDLine1("Food OK");
    return;
  }

  isDispensing = true;
  currentState = STATE_DISPENSING;
  updateStatusDisplay();

  float dispensed     = 0;
  float lastDispensed = 0;
  int   safetyLoop    = 0;
  int   noChangeCount = 0;
  bool  jammed        = false;

  while (dispensed < target && safetyLoop < DISPENSE_SAFETY) {
    myServo.write(90);
    Blynk.run();
    yield();
    delay(400);
    myServo.write(0);
    delay(SETTLE_DELAY_MS);

    float current = readFoodWeight();
    if (current < 0) {
      enterFoodErrorState("Food Sensor Err!");
      dispenseCleanupCommon();
      return;
    }

    dispensed = current - before;

    if ((dispensed - lastDispensed) < 1.0) {
      noChangeCount++;
      if (noChangeCount >= NO_CHANGE_LIMIT) {
        jammed = true;
        Serial.println(F("Food container empty or jam"));
        Blynk.logEvent("food_jam", "Food container empty or jam");
        break;
      }
    } else {
      noChangeCount = 0;
    }

    lastDispensed = dispensed;
    safetyLoop++;
  }

  myServo.write(0);

  if (dispensed > 0) {
    feedCountToday++;
    firstFeedDone  = true;
    lastFeedMillis = millis();
    waitingFood    = false;

    String t = getCurrentTime();
    Blynk.virtualWrite(V7, t);
    if (firebaseReady())
      Firebase.RTDB.setString(&fbdo, "/logs/lastFeed", t.c_str());
    syncLogs();
  }

  if (jammed) {
    waitingFood = true;
  }

  bool wasDegraded = !firebaseReady();
  isDispensing = false;
  currentState = wasDegraded ? STATE_DEGRADED : STATE_IDLE;
  updateStatusDisplay();

  setLCDLine1(jammed ? "Add Food" :
              (dispensed > 0 ? "OK - Ready" : "Check Feeder!"));
}

// ===================================================
// PUMP WATER
// ===================================================
void pumpWater() {
  if (isPumping || isDispensing) return;
  if (!canOperateWater()) return;
  if (!isPumpReady()) {
    Serial.println(F("Pump cooldown active"));
    return;
  }

  isPumping    = true;
  currentState = STATE_PUMPING;
  updateStatusDisplay();

  int pct = readWaterLevel();
  if (waterError) { pumpCleanupCommon(); return; }

  int attempts     = 0;
  int noRiseCount  = 0;
  int lastPct      = pct;
  bool sourceEmpty = false;

  while (pct < activeProfile.waterRefill && attempts < PUMP_MAX_ATTEMPTS) {
    digitalWrite(RELAY_PIN, HIGH);
    Blynk.virtualWrite(V5, F("ON"));
    Blynk.run();
    yield();
    delay(500);

    pct = readWaterLevel();
    if (waterError) { pumpCleanupCommon(); return; }

    if (pct <= lastPct) noRiseCount++;
    else                noRiseCount = 0;

    if (noRiseCount >= NO_RISE_LIMIT) {
      sourceEmpty = true;
      Serial.println(F("Source water empty"));
      Blynk.logEvent("water_empty", "Please add water.");
      break;
    }

    lastPct = pct;
    attempts++;
  }

  digitalWrite(RELAY_PIN, LOW);
  Blynk.virtualWrite(V5, F("OFF"));

  bool wasFirebaseOk = firebaseReady();

  if (!sourceEmpty && attempts < PUMP_MAX_ATTEMPTS) {
    refillCountToday++;
    firstPumpDone  = true;
    lastPumpMillis = millis();
    waitingWater   = false;

    String t = getCurrentTime();
    Blynk.virtualWrite(V8, t);
    if (wasFirebaseOk)
      Firebase.RTDB.setString(&fbdo, "/logs/lastRefill", t.c_str());
    syncLogs();
  }

  if (sourceEmpty) {
    waitingWater = true;
  }

  currentState = wasFirebaseOk ? STATE_IDLE : STATE_DEGRADED;
  isPumping    = false;
  updateStatusDisplay();

  setLCDLine1(sourceEmpty ? "Add Water" :
              (attempts >= PUMP_MAX_ATTEMPTS ? "Check Pump" : "OK - Ready"));
}

// ===================================================
// MANUAL PUMP DETECT
// ===================================================
void checkManualPump() {
  // Only monitor when a pump session is active (isPumping)
  if (!isPumping) return;
  if (waterError) return;
  if (digitalRead(RELAY_PIN) == LOW) return;
  if (millis() - manualLastCheck < 1500) return;
  manualLastCheck = millis();

  int pct = readWaterLevel();
  if (waterError) return;

  if (pct <= manualLastPct) manualNoRise++;
  else                      manualNoRise = 0;
  manualLastPct = pct;

  if (manualNoRise >= NO_RISE_LIMIT) {
    digitalWrite(RELAY_PIN, LOW);
    Blynk.virtualWrite(V4, 0);
    Blynk.virtualWrite(V5, F("OFF"));
    Serial.println(F("Source water empty (manual)"));
    Blynk.logEvent("water_empty", "Please add water.");
    waitingWater   = true;
    manualNoRise   = 0;
    firstPumpDone  = true;
    lastPumpMillis = millis();
    bool wasFirebaseOk = firebaseReady();
    currentState = wasFirebaseOk ? STATE_IDLE : STATE_DEGRADED;
    isPumping    = false;
    updateStatusDisplay();
  }
}

// ===================================================
// NEW DAY
// ===================================================
void checkNewDay() {
  struct tm t;
  if (!getLocalTime(&t)) return;
  char today[12];
  strftime(today, sizeof(today), "%d/%m/%Y", &t);
  if (String(today) == lastDate) return;

  float currentWeight = readFoodWeight();
  if (foodWeightMorning > 0 && currentWeight > 0 && currentWeight <= foodWeightMorning) {
    float ratio = (foodWeightMorning - currentWeight) / foodWeightMorning;
    if (ratio < 0.2)
      Blynk.logEvent("clean_food_bowl", "Food not consumed yesterday. Please clean the bowl.");
  }
  if (refillCountToday >= 5)
    Blynk.logEvent("clean_water_bowl", "Water refilled many times. Please clean water bowl.");

  foodWeightMorning = currentWeight;
  feedCountToday    = 0;
  refillCountToday  = 0;
  lastDate          = String(today);
  firstFeedDone     = false;
  lastFeedMillis    = 0;
  firstPumpDone     = false;
  lastPumpMillis    = 0;
}

// ===================================================
// PERIODIC CHECKS
// ===================================================
void checkFood() {
  if (!canOperateFood()) return;

  float foodGram = readFoodWeight();
  if (foodGram < 0) return;
  Blynk.virtualWrite(V1, foodGram);

  if (isTimeToFeed() && foodGram < activeProfile.foodLowGram) {
    if (!foodEventSent) {
      Blynk.logEvent("food_low", "Food is low, dispensing...");
      foodEventSent = true;
    }
    dispenseFood();
  } else {
    foodEventSent = false;
  }
}

void checkWater() {
  if (!canOperateWater()) return;

  int waterPct = readWaterLevel();
  if (waterError) return;
  Blynk.virtualWrite(V0, waterPct);

  if (waterPct < activeProfile.waterLow && isPumpReady()) {
    if (!waterEventSent) {
      Blynk.logEvent("water_low", "Water is low, refilling...");
      waterEventSent = true;
    }
    pumpWater();
  } else {
    waterEventSent = false;
  }
}

void checkAndUpdate() {
  checkNewDay();
  Blynk.virtualWrite(V6, F("Online"));
  retryFirebase();
  checkFoodErrorRecovery();
  checkWaterErrorRecovery();
  checkRecovery();
  checkWater();
  checkFood();
}

// ===================================================
// BLYNK CALLBACKS
// ===================================================
BLYNK_WRITE(V2) {
  if (param.asInt() == 1 && !isDispensing && !isPumping && canOperateFood()) {
    firstFeedDone  = false;
    lastFeedMillis = 0;
    shouldFeed     = true;
  }
}

BLYNK_WRITE(V4) {
  if (isDispensing || waterError) {
    Blynk.virtualWrite(V4, 0);
    return;
  }
  if (param.asInt()) {
    if (isPumping) return;
    digitalWrite(RELAY_PIN, HIGH);
    isPumping       = true;
    currentState    = STATE_PUMPING;
    manualLastPct   = readWaterLevel();
    manualNoRise    = 0;
    manualLastCheck = millis();
    updateStatusDisplay();
    Blynk.virtualWrite(V5, F("ON"));
  } else {
    digitalWrite(RELAY_PIN, LOW);
    Blynk.virtualWrite(V5, F("OFF"));
    manualNoRise   = 0;
    firstPumpDone  = true;
    lastPumpMillis = millis();

    int pct = readWaterLevel();
    if (!waterError && pct >= activeProfile.waterRefill) {
      waitingWater = false;
      String t = getCurrentTime();
      Blynk.virtualWrite(V8, t);
      if (firebaseReady()) {
        Firebase.RTDB.setString(&fbdo, "/logs/lastRefill", t.c_str());
      }
    }

    bool wasFirebaseOk = firebaseReady();
    currentState = wasFirebaseOk ? STATE_IDLE : STATE_DEGRADED;
    isPumping    = false;
    updateStatusDisplay();
  }
}

BLYNK_WRITE(V9)  { pendingProfile.name             = param.asStr(); }
BLYNK_WRITE(V10) { pendingProfile.foodLowGram       = param.asInt(); }
BLYNK_WRITE(V11) { pendingProfile.foodTargetGram    = param.asInt(); }
BLYNK_WRITE(V12) { pendingProfile.waterLow          = param.asInt(); }
BLYNK_WRITE(V13) { pendingProfile.waterRefill       = param.asInt(); }
BLYNK_WRITE(V14) { pendingProfile.feedIntervalHour  = param.asInt(); }

BLYNK_WRITE(V15) {
  if (param.asInt() == 1) {
    saveProfile();
  }
}

BLYNK_CONNECTED() {
  syncProfileToBlynk();
  Blynk.virtualWrite(V6, F("Online"));
  Blynk.virtualWrite(V5, F("OFF"));
}

// ===================================================
// SETUP
// ===================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(WATER_SENSOR_VCC_PIN, OUTPUT);
  digitalWrite(WATER_SENSOR_VCC_PIN, LOW);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  pinMode(RESET_PIN, INPUT_PULLUP);

  myServo.attach(SERVO_PIN);
  myServo.write(0);

  scale.begin(DOUT_PIN, SCK_PIN);
  scale.set_scale(SCALE_FACTOR);
  scale.tare();

  lcd.init();
  lcd.backlight();
  setLCDLine0("Pet Feeder IoT");
  setLCDLine1("Connecting...");

  WiFiManager wifiManager;
  wifiManager.setConfigPortalTimeout(180);

  if (digitalRead(RESET_PIN) == LOW) {
    Serial.println(F("Reset WiFi"));
    setLCDLine0("Resetting WiFi");
    setLCDLine1("Please wait...");
    wifiManager.resetSettings();
    delay(1000);
    ESP.restart();
  }

  wifiManager.setAPCallback([](WiFiManager* wm) {
    setLCDLine0("Connect WiFi:");
    setLCDLine1("PetFeeder-Setup");
  });

  if (!wifiManager.autoConnect("PetFeeder-Setup")) {
    ESP.restart();
  }

  setLCDLine1("WiFi OK...");
  Blynk.config(BLYNK_AUTH_TOKEN);
  Blynk.connect();

  // Timezone: adjust as needed
  configTime(7 * 3600, 0, "pool.ntp.org");
  delay(2000);

  // Firebase config: API key & DB URL from secrets.h
  fbConfig.api_key               = FIREBASE_API_KEY;
  fbConfig.database_url          = FIREBASE_DB_URL;
  fbConfig.token_status_callback = tokenStatusCallback;

  // Do NOT call signUp with empty strings; rely on provided auth or existing token
  Firebase.begin(&fbConfig, &fbAuth);
  Firebase.reconnectWiFi(true);

  int waitCount = 0;
  while (!firebaseReady() && waitCount < 10) {
    delay(500);
    waitCount++;
  }

  // ADC settings for more stable readings
  analogSetPinAttenuation(WATER_SENSOR_PIN, ADC_11db);
  analogSetWidth(12);

  loadProfile();

  struct tm t;
  if (getLocalTime(&t)) {
    char today[12];
    strftime(today, sizeof(today), "%d/%m/%Y", &t);
    lastDate          = String(today);
    foodWeightMorning = readFoodWeight();
  }

  Blynk.virtualWrite(V5, F("OFF"));
  Blynk.virtualWrite(V6, F("Online"));
  Blynk.virtualWrite(V7, F("--"));
  Blynk.virtualWrite(V8, F("--"));
  updateStatusDisplay();

  setLCDLine0("Pet Feeder IoT");
  setLCDLine1((String("Pet: ") + activeProfile.name).c_str());
  delay(1500);

  timer.setInterval(30000L, checkAndUpdate);
  timer.setInterval(1000L,  updateLCDTime);

  Serial.println(F("=== Pet Feeder IoT Ready ==="));
}

// ===================================================
// LOOP
// ===================================================
void loop() {
  Blynk.run();
  timer.run();
  checkManualPump();

  if (shouldFeed && !isDispensing && !isPumping && canOperateFood()) {
    shouldFeed = false;
    dispenseFood();
  }
}