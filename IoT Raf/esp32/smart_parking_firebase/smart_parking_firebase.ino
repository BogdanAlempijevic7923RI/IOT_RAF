#include <ESP32Servo.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

// ===================== WIFI / FIREBASE PODESAVANJA =====================
const char* WIFI_SSID     = "";
const char* WIFI_PASSWORD = "";

const char* FIREBASE_API_KEY = "";

const char* FIREBASE_DATABASE_URL = "";
const char* FIREBASE_DEVICE_EMAIL = "";
const char* FIREBASE_DEVICE_PASSWORD = "";

const unsigned long FIREBASE_HEARTBEAT_MS = 10000; // osveži signal uređaja na 10 s
const unsigned long WIFI_RETRY_MS = 10000;

// ===================== PINOVI =====================
const int IR_PIN        = 27;
const int TRIG_PIN      = 23;
const int ECHO_PIN      = 18;
const int SERVO_PIN     = 13;
const int LED_RED_PIN   = 25;
const int LED_GREEN_PIN = 26;
const int BUZZER_PIN    = 14;

// ===================== PODEŠAVANJA =====================
const int IR_ACTIVE_LEVEL = LOW;
const int BUZZER_ON_LEVEL  = HIGH;
const int BUZZER_OFF_LEVEL = (BUZZER_ON_LEVEL == HIGH) ? LOW : HIGH;

const int GATE_CLOSED_ANGLE = 0;
const int GATE_OPEN_ANGLE   = 90;
const unsigned long SERVO_STEP_MS = 12;

const float PARK_DISTANCE_CM  = 15.0;
const float LEAVE_DISTANCE_CM = 20.0;
const unsigned long PARK_CONFIRM_MS  = 2000;
const unsigned long LEAVE_CONFIRM_MS = 1500;
const unsigned long US_READ_INTERVAL_MS = 100;

const unsigned long IR_CONFIRM_MS        = 300;
const unsigned long GATE_CLEAR_MS        = 2000;
const unsigned long GATE_OPEN_TIMEOUT_MS = 15000;
const unsigned long ENTRY_TIMEOUT_MS     = 60000;

const unsigned long BILLING_INTERVAL_MS = 60000;
const int PRICE_PER_INTERVAL = 10; // RSD po punom minutu

// ===================== STANJA =====================
enum State { ST_FREE, ST_ENTERING, ST_OCCUPIED, ST_LEAVING };
const char* STATE_NAMES[] = { "SLOBODNO", "ULAZI", "ZAUZETO", "IZLAZI" };
State state = ST_FREE;
unsigned long stateSince = 0;

// ===================== GLOBALNE PROMENLJIVE =====================
Servo gateServo;
int servoAngle  = GATE_CLOSED_ANGLE;
int servoTarget = GATE_CLOSED_ANGLE;
unsigned long lastServoStep = 0;

bool gateOpen = false;
bool gatePassSeen = false;
unsigned long gateOpenedAt = 0;

bool irRaw = false;
bool irConfirmed = false;
unsigned long irSince = 0;
unsigned long irClearSince = 0;

float distanceCm = 999.0;
unsigned long lastUsRead = 0;
unsigned long nearSince = 0;
unsigned long farSince = 0;

unsigned long buzzerOffAt = 0;

unsigned long parkStart = 0;       // millis() za preciznu lokalnu naplatu
uint32_t parkStartEpoch = 0;       // Unix vreme za prikaz dolaska u web aplikaciji
int billedIntervals = 0;
int totalCost = 0;

uint32_t lastSessionSec = 0;
int lastSessionCost = 0;
bool hasLastReceipt = false;

// ===================== FIREBASE GLOBALNE PROMENLJIVE =====================
String firebaseIdToken;
String firebaseRefreshToken;
unsigned long firebaseTokenExpiresAt = 0;
unsigned long lastFirebaseHeartbeat = 0;
unsigned long lastWifiAttempt = 0;
bool firebaseDirty = true;
bool ntpConfigured = false;

void requestFirebaseUpdate() {
  firebaseDirty = true;
}

// ===================== POMOĆNE FUNKCIJE ZA VREME =====================
uint32_t currentUnixSeconds() {
  time_t now = time(nullptr);
  if (now < 1700000000) return 0; // NTP još nije sinhronizovan
  return static_cast<uint32_t>(now);
}

uint64_t currentUnixMillis() {
  uint32_t seconds = currentUnixSeconds();
  return seconds ? static_cast<uint64_t>(seconds) * 1000ULL : 0ULL;
}

void configureNtpIfNeeded() {
  if (WiFi.status() != WL_CONNECTED || ntpConfigured) return;
  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  ntpConfigured = true;
}

void connectWiFiAtStartup() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Wi-Fi povezivanje");

  unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 10000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi povezan. IP: ");
    Serial.println(WiFi.localIP());
    configureNtpIfNeeded();

    unsigned long ntpStarted = millis();
    while (currentUnixSeconds() == 0 && millis() - ntpStarted < 7000) {
      delay(250);
    }
    Serial.println(currentUnixSeconds() ? "NTP vreme sinhronizovano." : "NTP nije dostupan; vreme dolaska neće biti prikazano dok se ne sinhronizuje.");
  } else {
    Serial.println("Wi-Fi nije dostupan. Pokušaću ponovo u radu.");
  }
}

void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    configureNtpIfNeeded();
    return;
  }
  unsigned long now = millis();
  if (now - lastWifiAttempt < WIFI_RETRY_MS) return;
  lastWifiAttempt = now;
  Serial.println("Wi-Fi reconnect...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

// ===================== FIREBASE REST / AUTH =====================
String urlEncode(const String& value) {
  String encoded;
  const char hex[] = "0123456789ABCDEF";
  for (size_t i = 0; i < value.length(); i++) {
    uint8_t c = static_cast<uint8_t>(value[i]);
    bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
    if (safe) encoded += static_cast<char>(c);
    else {
      encoded += '%';
      encoded += hex[(c >> 4) & 0x0F];
      encoded += hex[c & 0x0F];
    }
  }
  return encoded;
}

bool postHttps(const String& url, const String& body, const char* contentType, String& response, int& statusCode) {
  WiFiClientSecure client;

  client.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(1800);
  http.setTimeout(1800);
  if (!http.begin(client, url)) {
    statusCode = -1;
    return false;
  }
  http.addHeader("Content-Type", contentType);
  statusCode = http.POST(body);
  response = statusCode > 0 ? http.getString() : "";
  http.end();
  return statusCode >= 200 && statusCode < 300;
}

bool parseAuthResponse(const String& response, bool fromRefresh) {
  StaticJsonDocument<1536> doc;
  DeserializationError err = deserializeJson(doc, response);
  if (err) {
    Serial.print("Firebase Auth JSON greška: ");
    Serial.println(err.c_str());
    return false;
  }

  String newIdToken = fromRefresh ? doc["id_token"].as<String>() : doc["idToken"].as<String>();
  String newRefreshToken = fromRefresh ? doc["refresh_token"].as<String>() : doc["refreshToken"].as<String>();
  String expiresText = fromRefresh ? doc["expires_in"].as<String>() : doc["expiresIn"].as<String>();
  unsigned long expiresSeconds = expiresText.toInt();
  if (newIdToken.length() == 0) return false;

  firebaseIdToken = newIdToken;
  if (newRefreshToken.length() > 0) firebaseRefreshToken = newRefreshToken;
  if (expiresSeconds < 120) expiresSeconds = 3600;
  firebaseTokenExpiresAt = millis() + (expiresSeconds - 60) * 1000UL;
  Serial.println(fromRefresh ? "Firebase token osvežen." : "ESP32 prijavljen na Firebase.");
  return true;
}

bool signInFirebaseDevice() {
  StaticJsonDocument<256> req;
  req["email"] = FIREBASE_DEVICE_EMAIL;
  req["password"] = FIREBASE_DEVICE_PASSWORD;
  req["returnSecureToken"] = true;
  String payload;
  serializeJson(req, payload);

  String response;
  int statusCode = 0;
  String url = String("https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=") + urlEncode(FIREBASE_API_KEY);
  bool ok = postHttps(url, payload, "application/json", response, statusCode);
  if (!ok) {
    Serial.printf("Firebase prijava nije uspela (HTTP %d): %s\n", statusCode, response.c_str());
    return false;
  }
  return parseAuthResponse(response, false);
}

bool refreshFirebaseToken() {
  if (firebaseRefreshToken.length() == 0) return false;
  String payload = "grant_type=refresh_token&refresh_token=" + urlEncode(firebaseRefreshToken);
  String response;
  int statusCode = 0;
  String url = String("https://securetoken.googleapis.com/v1/token?key=") + urlEncode(FIREBASE_API_KEY);
  bool ok = postHttps(url, payload, "application/x-www-form-urlencoded", response, statusCode);
  if (!ok) {
    Serial.printf("Firebase refresh nije uspeo (HTTP %d): %s\n", statusCode, response.c_str());
    return false;
  }
  return parseAuthResponse(response, true);
}

bool ensureFirebaseToken() {
  if (firebaseIdToken.length() > 0 && static_cast<int32_t>(millis() - firebaseTokenExpiresAt) < 0) return true;
  if (firebaseRefreshToken.length() > 0 && refreshFirebaseToken()) return true;
  firebaseIdToken = "";
  firebaseRefreshToken = "";
  firebaseTokenExpiresAt = 0;
  return signInFirebaseDevice();
}

bool firebaseConfigurationReady() {
  return WiFi.status() == WL_CONNECTED &&
         String(FIREBASE_API_KEY).indexOf("ZAMENI_") < 0 &&
         String(FIREBASE_DATABASE_URL).indexOf("ZAMENI_") < 0 &&
         String(WIFI_SSID).indexOf("TVOJ_") < 0 &&
         String(FIREBASE_DEVICE_PASSWORD).indexOf("OVDE_") < 0;
}

void publishParkingToFirebase() {
  if (!firebaseConfigurationReady()) return;
  if (!ensureFirebaseToken()) return;

  StaticJsonDocument<512> doc;
  doc["state"] = STATE_NAMES[state];
  doc["gate"] = gateOpen;
  doc["since"] = state == ST_OCCUPIED ? parkStartEpoch : 0;
  doc["cost"] = totalCost;
  doc["price"] = PRICE_PER_INTERVAL;

  JsonObject seen = doc.createNestedObject("seen");
  seen[".sv"] = "timestamp";

  if (hasLastReceipt) {
    JsonObject last = doc.createNestedObject("last");
    last["sec"] = lastSessionSec;
    last["cost"] = lastSessionCost;
  } else {
    doc["last"] = nullptr;
  }

  String payload;
  serializeJson(doc, payload);

  String databaseUrl = String(FIREBASE_DATABASE_URL);
  while (databaseUrl.endsWith("/")) databaseUrl.remove(databaseUrl.length() - 1);
  String url = databaseUrl + "/parking.json?auth=" + urlEncode(firebaseIdToken);

  WiFiClientSecure client;

  client.setInsecure();
  HTTPClient http;
  http.setConnectTimeout(1800);
  http.setTimeout(1800);

  if (!http.begin(client, url)) {
    Serial.println("Firebase: HTTP begin nije uspeo.");
    return;
  }
  http.addHeader("Content-Type", "application/json");
  int statusCode = http.PUT(payload);
  String response = statusCode > 0 ? http.getString() : "";
  http.end();

  if (statusCode >= 200 && statusCode < 300) {
    Serial.println("Firebase: parking podaci poslati.");
  } else {
    Serial.printf("Firebase upis nije uspeo (HTTP %d): %s\n", statusCode, response.c_str());
    if (statusCode == 401) {
      firebaseIdToken = "";
      firebaseTokenExpiresAt = 0;
    }
  }
}

void serviceFirebase() {

  if (servoAngle != servoTarget) return;
  unsigned long now = millis();
  if (!firebaseDirty && now - lastFirebaseHeartbeat < FIREBASE_HEARTBEAT_MS) return;
  firebaseDirty = false;
  lastFirebaseHeartbeat = now;
  publishParkingToFirebase();
}

// ===================== BUZZER =====================
void beep(unsigned long ms) {
  digitalWrite(BUZZER_PIN, BUZZER_ON_LEVEL);
  buzzerOffAt = millis() + ms;
}

void updateBuzzer() {
  if (buzzerOffAt != 0 && static_cast<long>(millis() - buzzerOffAt) >= 0) {
    digitalWrite(BUZZER_PIN, BUZZER_OFF_LEVEL);
    buzzerOffAt = 0;
  }
}

// ===================== SERVO / RAMPA =====================
void updateServo() {
  if (servoAngle == servoTarget) return;
  unsigned long now = millis();
  if (now - lastServoStep < SERVO_STEP_MS) return;
  lastServoStep = now;
  servoAngle += (servoTarget > servoAngle) ? 1 : -1;
  gateServo.write(servoAngle);
}

void openGate(bool carAtGate) {
  gatePassSeen = carAtGate;
  gateOpenedAt = millis();
  if (!gateOpen) {
    gateOpen = true;
    servoTarget = GATE_OPEN_ANGLE;
    beep(400);
    Serial.println("RAMPA: otvaranje");
  }
  requestFirebaseUpdate();
}

void closeGate() {
  gateOpen = false;
  servoTarget = GATE_CLOSED_ANGLE;
  Serial.println("RAMPA: zatvaranje");
  requestFirebaseUpdate();
}

void updateGate() {
  if (!gateOpen) return;
  unsigned long now = millis();
  if (irRaw) gatePassSeen = true;
  bool clearLongEnough = (irClearSince != 0) && (now - irClearSince >= GATE_CLEAR_MS);
  bool passed = gatePassSeen && clearLongEnough;
  bool timedOut = (now - gateOpenedAt >= GATE_OPEN_TIMEOUT_MS) && clearLongEnough;
  if (passed || timedOut) closeGate();
}

// ===================== IR SENZOR =====================
void readIR() {
  unsigned long now = millis();
  irRaw = (digitalRead(IR_PIN) == IR_ACTIVE_LEVEL);
  if (irRaw) {
    if (irSince == 0) irSince = now;
    irClearSince = 0;
    irConfirmed = (now - irSince >= IR_CONFIRM_MS);
  } else {
    irSince = 0;
    irConfirmed = false;
    if (irClearSince == 0) irClearSince = now;
  }
}

// ===================== ULTRAZVUČNI SENZOR =====================
float readDistanceOnce() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  unsigned long duration = pulseIn(ECHO_PIN, HIGH, 12000);
  if (duration == 0) return 999.0;
  return duration * 0.0343f / 2.0f;
}

float median3(float a, float b, float c) {
  if ((a >= b && a <= c) || (a <= b && a >= c)) return a;
  if ((b >= a && b <= c) || (b <= a && b >= c)) return b;
  return c;
}

void readUltrasonic() {
  unsigned long now = millis();
  if (now - lastUsRead < US_READ_INTERVAL_MS) return;
  lastUsRead = now;
  float a = readDistanceOnce(); delay(5);
  float b = readDistanceOnce(); delay(5);
  float c = readDistanceOnce();
  distanceCm = median3(a, b, c);

  if (distanceCm < PARK_DISTANCE_CM) {
    if (nearSince == 0) nearSince = now;
  } else {
    nearSince = 0;
  }
  if (distanceCm > LEAVE_DISTANCE_CM) {
    if (farSince == 0) farSince = now;
  } else {
    farSince = 0;
  }
}

// ===================== NAPLATA =====================
void updateBilling(unsigned long now) {
  int intervals = (now - parkStart) / BILLING_INTERVAL_MS;
  if (intervals > billedIntervals) {
    billedIntervals = intervals;
    totalCost = intervals * PRICE_PER_INTERVAL;
    Serial.printf("NAPLATA: %d min -> %d RSD\n", intervals, totalCost);
    requestFirebaseUpdate();
  }
}

// ===================== PROMENA STANJA =====================
void changeState(State s) {
  state = s;
  stateSince = millis();
  Serial.printf(">> STANJE: %s\n", STATE_NAMES[s]);
  requestFirebaseUpdate();
}

void startParking() {
  parkStart = millis();
  parkStartEpoch = currentUnixSeconds();
  billedIntervals = 0;
  totalCost = 0;
  Serial.println("Auto parkiran, naplata pocinje");
  changeState(ST_OCCUPIED);
}

void endParking() {
  unsigned long seconds = (millis() - parkStart) / 1000;
  lastSessionSec = seconds;
  lastSessionCost = totalCost;
  hasLastReceipt = true;
  Serial.printf("Auto napustio mesto. Trajanje: %lu min %lu s | Za naplatu: %d RSD\n",
                seconds / 60, seconds % 60, totalCost);
  openGate(false);
  changeState(ST_LEAVING);
}

void runStateMachine() {
  unsigned long now = millis();
  bool carParked = (nearSince != 0) && (now - nearSince >= PARK_CONFIRM_MS);
  bool carGone = (farSince != 0) && (now - farSince >= LEAVE_CONFIRM_MS);

  switch (state) {
    case ST_FREE:
      if (carParked) {
        startParking();
      } else if (irConfirmed) {
        Serial.println("IR: auto na ulazu");
        openGate(true);
        changeState(ST_ENTERING);
      }
      break;

    case ST_ENTERING:
      if (carParked) {
        startParking();
      } else if (!gateOpen && now - stateSince >= ENTRY_TIMEOUT_MS) {
        Serial.println("Auto nije parkirao, vracam u stanje SLOBODNO");
        changeState(ST_FREE);
      }
      break;

    case ST_OCCUPIED:
      updateBilling(now);
      if (carGone) endParking();
      break;

    case ST_LEAVING:
      if (!gateOpen) changeState(ST_FREE);
      break;
  }
}

// ===================== ISPIS STATUSA =====================
void printStatus() {
  static unsigned long last = 0;
  if (millis() - last < 2000) return;
  last = millis();
  Serial.printf("[%s] IR: %s | US: %.1f cm | rampa: %s | cena: %d RSD | Wi-Fi: %s\n",
                STATE_NAMES[state], irRaw ? "AUTO" : "slobodno", distanceCm,
                gateOpen ? "otvorena" : "zatvorena", totalCost,
                WiFi.status() == WL_CONNECTED ? "OK" : "nema veze");
}

// ===================== SETUP / LOOP =====================
void setup() {
  Serial.begin(115200);

  pinMode(IR_PIN, INPUT);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(LED_RED_PIN, OUTPUT);
  pinMode(LED_GREEN_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, BUZZER_OFF_LEVEL);

  ESP32PWM::allocateTimer(0);
  gateServo.setPeriodHertz(50);
  gateServo.attach(SERVO_PIN, 500, 2400);
  gateServo.write(GATE_CLOSED_ANGLE);

  Serial.println("=== SMART PARKING + FIREBASE ===");
  connectWiFiAtStartup();
  changeState(ST_FREE);
  Serial.println("Sistem spreman.");
}

void loop() {
  readIR();
  readUltrasonic();
  updateGate();
  updateServo();
  updateBuzzer();
  runStateMachine();

  digitalWrite(LED_RED_PIN, state == ST_OCCUPIED ? HIGH : LOW);
  digitalWrite(LED_GREEN_PIN, state != ST_OCCUPIED ? HIGH : LOW);

  printStatus();
  ensureWiFi();
  serviceFirebase();
}
