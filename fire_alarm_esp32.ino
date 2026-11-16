/*
 * ============================================================
 *  HE THONG CANH BAO CHAY IOT - ESP32
 * ============================================================
 *  Giu nguyen logic goc cua ban, bo sung:
 *   - Gui du lieu len Firebase (nhanh /esp32/sensors) bang tac vu rieng,
 *     nen viec gui mang KHONG lam cham viec phat hien lua/khoi
 *   - Nhan lenh tu web (nhanh /esp32/control): tu dong / thu cong,
 *     bat tat quat, bom, coi, tat tieng coi, chinh nguong nhiet do
 *   - Ghi nhat ky canh bao (nhanh /esp32/events)
 *   - Chong nhieu cam bien khoi/lua, tranh nhay relay
 *   - An toan: khi co lua/khoi, he thong LUON tu bat thiet bi,
 *     ke ca dang o che do thu cong
 *
 *  THU VIEN: DHT sensor library, Adafruit Unified Sensor,
 *            LiquidCrystal I2C, ArduinoJson (phien ban 7.x)
 * ============================================================
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>

// =====================================================
// 1. CHAN ESP32
// =====================================================
#define DHTPIN        4
#define DHTTYPE       DHT11

#define SMOKE_PIN     35
#define FLAME_PIN     19

#define FAN_RELAY     25
#define PUMP_RELAY    27
#define BUZZER_RELAY  26

// =====================================================
// 2. LOGIC RELAY / CAM BIEN
// =====================================================
#define RELAY_ON      HIGH
#define RELAY_OFF     LOW

#define SMOKE_ACTIVE  LOW
#define FLAME_ACTIVE  LOW

// =====================================================
// 3. WIFI VA FIREBASE
// =====================================================
const char* WIFI_NAME     = "TEN_WIFI";
const char* WIFI_PASSWORD = "MAT_KHAU_WIFI";

// Khong co dau / o cuoi
const String FIREBASE_BASE = "https://fire-94185-default-rtdb.asia-southeast1.firebasedatabase.app";

const char* PATH_SENSORS = "/esp32/sensors.json";   // ESP32 ghi, web doc
const char* PATH_CONTROL = "/esp32/control.json";   // web ghi, ESP32 doc
const char* PATH_EVENTS  = "/esp32/events.json";    // nhat ky canh bao

// =====================================================
// 4. THOI GIAN
// =====================================================
const unsigned long DHT_INTERVAL      = 2000;
const unsigned long LCD_INTERVAL      = 500;
const unsigned long SERIAL_INTERVAL   = 1000;
const unsigned long STATUS_INTERVAL   = 2000;   // gui trang thai len web
const unsigned long CONTROL_INTERVAL  = 2000;   // doc lenh tu web
const unsigned long WIFI_RETRY_MS     = 10000;

const int CONFIRM_COUNT = 4;      // cam bien phai bao lien tiep 4 lan (~200ms) moi tinh la co lua/khoi
const int DHT_FAIL_LIMIT = 3;     // doc loi 3 lan lien tiep moi bao LOI DHT11

// =====================================================
// 5. DOI TUONG
// =====================================================
LiquidCrystal_I2C lcd(0x27, 16, 2);
DHT dht(DHTPIN, DHTTYPE);

// =====================================================
// 6. DU LIEU CHIA SE GIUA LOOP VA TAC VU MANG
// =====================================================
// -- Cam bien --
volatile float temperature = NAN;
volatile float humidity = NAN;
volatile bool  dhtOK = false;
volatile int   smokeRaw = 1, flameRaw = 1;
volatile bool  smokeDetected = false, flameDetected = false;
volatile int   alarmLevel = 0;    // 0 binh thuong, 1 nhiet do cao, 2 khoi, 3 lua

// -- Trang thai dau ra --
volatile bool fanOut = false, pumpOut = false, buzzerOut = false;

// -- Lenh tu web --
volatile bool  ctlManual = false;
volatile bool  ctlFan = false, ctlPump = false, ctlBuzzer = false;
volatile bool  ctlMute = false;
volatile float ctlTempLimit = 33.0;

// -- Su kien canh bao can gui len web (-1 = khong co) --
volatile int eventLevel = -1;

// -- Noi bo cua loop --
int  dhtFailCount = 0;
bool hotLatched = false;
int  smokeCount = 0, flameCount = 0;
int  prevLevel = 0;
unsigned long dhtPrev = 0, lcdPrev = 0, serialPrev = 0, wifiPrev = 0;

// =====================================================
// 7. LCD
// =====================================================
void lcdLine(int row, const char* text, bool showWifi) {
  char buf[17];
  snprintf(buf, sizeof(buf), "%-16.16s", text);
  if (showWifi) buf[15] = (WiFi.status() == WL_CONNECTED) ? '*' : '!';
  lcd.setCursor(0, row);
  lcd.print(buf);
}

// =====================================================
// 8. WIFI
// =====================================================
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_NAME, WIFI_PASSWORD);

  Serial.println();
  Serial.print("Dang ket noi WiFi");
  lcd.clear();
  lcdLine(0, "KET NOI WIFI", false);

  int dem = 0;
  while (WiFi.status() != WL_CONNECTED && dem < 40) {
    delay(500);
    Serial.print(".");
    lcd.setCursor(dem % 16, 1);
    lcd.print(".");
    dem++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Da ket noi WiFi, IP: ");
    Serial.println(WiFi.localIP());
    lcd.clear();
    lcdLine(0, "WIFI DA KET NOI", false);
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP());
  } else {
    Serial.println("Khong ket noi duoc WiFi, van chay che do offline");
    lcd.clear();
    lcdLine(0, "LOI WIFI", false);
    lcdLine(1, "CHAY OFFLINE", false);
  }
}

void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - wifiPrev >= WIFI_RETRY_MS) {
    wifiPrev = millis();
    Serial.println("Mat WiFi, thu ket noi lai...");
    WiFi.disconnect();
    WiFi.begin(WIFI_NAME, WIFI_PASSWORD);
  }
}

// =====================================================
// 9. FIREBASE (chay trong tac vu rieng tren core 0)
// =====================================================
int firebaseRequest(const char* method, const char* path, const String& body, String* response) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(4000);
  http.setConnectTimeout(4000);

  if (!http.begin(client, FIREBASE_BASE + path)) return -1;
  http.addHeader("Content-Type", "application/json");

  int code;
  if (strcmp(method, "GET") == 0)        code = http.GET();
  else if (strcmp(method, "POST") == 0)  code = http.POST(body);
  else if (strcmp(method, "PATCH") == 0) code = http.PATCH(body);
  else                                   code = http.PUT(body);

  if (code > 0 && response) *response = http.getString();
  http.end();
  return code;
}

long nowEpoch() {
  time_t t = time(nullptr);
  return (t > 1700000000) ? (long)t : 0;
}

void sendStatus() {
  JsonDocument doc;

  if (dhtOK) {
    doc["temperature"] = round(temperature * 10) / 10.0;
    doc["humidity"]    = round(humidity * 10) / 10.0;
  } else {
    doc["temperature"] = nullptr;
    doc["humidity"]    = nullptr;
  }
  doc["dhtOK"]         = (bool)dhtOK;
  doc["smokeRaw"]      = (int)smokeRaw;
  doc["flameRaw"]      = (int)flameRaw;
  doc["smokeDetected"] = (bool)smokeDetected;
  doc["flameDetected"] = (bool)flameDetected;
  doc["fan"]           = (bool)fanOut;
  doc["pump"]          = (bool)pumpOut;
  doc["buzzer"]        = (bool)buzzerOut;
  doc["online"]        = true;
  doc["uptime"]        = millis();
  doc["mode"]          = ctlManual ? "manual" : "auto";
  doc["muteBuzzer"]    = (bool)ctlMute;
  doc["tempLimit"]     = (float)ctlTempLimit;
  doc["level"]         = (int)alarmLevel;
  doc["rssi"]          = WiFi.RSSI();
  doc["ip"]            = WiFi.localIP().toString();
  doc["ts"]            = nowEpoch();

  String body;
  serializeJson(doc, body);

  int code = firebaseRequest("PATCH", PATH_SENSORS, body, nullptr);
  if (code < 200 || code >= 300) {
    Serial.printf("[Firebase] Gui trang thai loi, ma: %d\n", code);
  }
}

void readControl() {
  String payload;
  int code = firebaseRequest("GET", PATH_CONTROL, "", &payload);
  if (code != 200) {
    Serial.printf("[Firebase] Doc lenh loi, ma: %d\n", code);
    return;
  }
  if (payload == "null" || payload.length() < 2) return;

  JsonDocument doc;
  if (deserializeJson(doc, payload)) return;

  const char* mode = doc["mode"] | "auto";
  ctlManual = (strcmp(mode, "manual") == 0);
  ctlFan    = doc["fan"]        | false;
  ctlPump   = doc["pump"]       | false;
  ctlBuzzer = doc["buzzer"]     | false;
  ctlMute   = doc["muteBuzzer"] | false;

  float lim = doc["tempLimit"] | 33.0f;
  if (lim >= 20.0f && lim <= 60.0f) ctlTempLimit = lim;
}

void sendEvent(int level) {
  const char* names[] = {"normal", "hot", "smoke", "fire"};
  if (level < 0 || level > 3) return;

  JsonDocument doc;
  doc["ts"]    = nowEpoch();
  doc["level"] = level;
  doc["type"]  = names[level];
  if (dhtOK) doc["temperature"] = round(temperature * 10) / 10.0;
  doc["uptime"] = millis() / 1000;

  String body;
  serializeJson(doc, body);
  firebaseRequest("POST", PATH_EVENTS, body, nullptr);
}

void cloudTask(void* param) {
  unsigned long lastStatus = 0, lastControl = 0;
  bool ntpStarted = false;

  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      if (!ntpStarted) {
        configTime(7 * 3600, 0, "pool.ntp.org", "time.google.com");   // GMT+7
        ntpStarted = true;
      }

      // Su kien canh bao co do uu tien cao nhat
      int ev = eventLevel;
      if (ev >= 0) {
        eventLevel = -1;
        sendEvent(ev);
      }

      unsigned long now = millis();
      if (now - lastControl >= CONTROL_INTERVAL) {
        lastControl = now;
        readControl();
      }
      if (now - lastStatus >= STATUS_INTERVAL) {
        lastStatus = now;
        sendStatus();
      }
    }
    vTaskDelay(200 / portTICK_PERIOD_MS);
  }
}

// =====================================================
// 10. DOC CAM BIEN
// =====================================================
void readDHT11() {
  if (millis() - dhtPrev < DHT_INTERVAL) return;
  dhtPrev = millis();

  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (!isnan(t) && !isnan(h)) {
    temperature = t;
    humidity = h;
    dhtFailCount = 0;
    dhtOK = true;
  } else {
    if (dhtFailCount < 100) dhtFailCount++;
    if (dhtFailCount >= DHT_FAIL_LIMIT) dhtOK = false;
    Serial.println("LOI DOC DHT11!");
  }
}

void readFireSensors() {
  smokeRaw = digitalRead(SMOKE_PIN);
  flameRaw = digitalRead(FLAME_PIN);

  // Chong nhieu: phai bao lien tiep nhieu lan
  if (smokeRaw == SMOKE_ACTIVE) { if (smokeCount < 100) smokeCount++; } else smokeCount = 0;
  if (flameRaw == FLAME_ACTIVE) { if (flameCount < 100) flameCount++; } else flameCount = 0;

  smokeDetected = (smokeCount >= CONFIRM_COUNT);
  flameDetected = (flameCount >= CONFIRM_COUNT);
}

// =====================================================
// 11. DIEU KHIEN RELAY
// =====================================================
void controlOutputs() {
  float limit = ctlTempLimit;

  // Nhiet do cao (co tre 1 do de tranh bat tat lien tuc)
  if (dhtOK) {
    if (!hotLatched && temperature > limit) hotLatched = true;
    else if (hotLatched && temperature < limit - 1.0) hotLatched = false;
  } else {
    hotLatched = false;
  }

  bool fan = false, pump = false, buzzer = false;

  if (ctlManual) {
    fan = ctlFan;
    pump = ctlPump;
    buzzer = ctlBuzzer;
  } else {
    // TRUONG HOP 3: nhiet do vuot nguong -> quat + bom (giong code goc)
    if (hotLatched) { fan = true; pump = true; }
  }

  // AN TOAN: co lua/khoi thi LUON bat, bat ke che do nao
  if (flameDetected) { pump = true; buzzer = true; fan = true; }   // TRUONG HOP 1
  if (smokeDetected) { fan = true; buzzer = true; }                // TRUONG HOP 2

  // Nguoi dung co the tat tieng coi tren web
  if (ctlMute) buzzer = false;

  fanOut = fan;
  pumpOut = pump;
  buzzerOut = buzzer;

  digitalWrite(FAN_RELAY,    fan ? RELAY_ON : RELAY_OFF);
  digitalWrite(PUMP_RELAY,   pump ? RELAY_ON : RELAY_OFF);
  digitalWrite(BUZZER_RELAY, buzzer ? RELAY_ON : RELAY_OFF);

  // Muc canh bao
  int level = flameDetected ? 3 : (smokeDetected ? 2 : (hotLatched ? 1 : 0));
  alarmLevel = level;
  if (level != prevLevel) {
    prevLevel = level;
    eventLevel = level;     // bao tac vu mang ghi nhat ky
    Serial.printf(">>> THAY DOI MUC CANH BAO: %d\n", level);
  }
}

// =====================================================
// 12. LCD
// =====================================================
void updateLCD() {
  if (millis() - lcdPrev < LCD_INTERVAL) return;
  lcdPrev = millis();

  char l1[20], l2[20];

  if (smokeDetected && flameDetected) {
    snprintf(l1, sizeof(l1), "CANH BAO CHAY!");
    snprintf(l2, sizeof(l2), "KHOI + LUA");
  } else if (flameDetected) {
    snprintf(l1, sizeof(l1), "PHAT HIEN LUA!");
    snprintf(l2, sizeof(l2), "BOM + COI BAT");
  } else if (smokeDetected) {
    snprintf(l1, sizeof(l1), "PHAT HIEN KHOI");
    snprintf(l2, sizeof(l2), "QUAT + COI BAT");
  } else if (dhtOK) {
    snprintf(l1, sizeof(l1), "T:%.1fC H:%.0f%%", (float)temperature, (float)humidity);
    if (hotLatched && !ctlManual)  snprintf(l2, sizeof(l2), "NONG! QUAT+BOM");
    else if (ctlManual)            snprintf(l2, sizeof(l2), "CHE DO THU CONG");
    else                           snprintf(l2, sizeof(l2), "HE THONG OK");
  } else {
    snprintf(l1, sizeof(l1), "LOI DHT11");
    snprintf(l2, sizeof(l2), "KIEM TRA SENSOR");
  }

  lcdLine(0, l1, false);
  lcdLine(1, l2, true);     // ky tu cuoi dong 2: * = co WiFi, ! = mat WiFi
}

// =====================================================
// 13. SETUP
// =====================================================
void setup() {
  Serial.begin(115200);

  pinMode(SMOKE_PIN, INPUT);
  pinMode(FLAME_PIN, INPUT);

  pinMode(FAN_RELAY, OUTPUT);
  pinMode(PUMP_RELAY, OUTPUT);
  pinMode(BUZZER_RELAY, OUTPUT);
  digitalWrite(FAN_RELAY, RELAY_OFF);
  digitalWrite(PUMP_RELAY, RELAY_OFF);
  digitalWrite(BUZZER_RELAY, RELAY_OFF);

  dht.begin();

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcdLine(0, "HE THONG IOT", false);
  lcdLine(1, "KHOI DONG...", false);
  delay(2000);

  connectWiFi();
  delay(1500);
  lcd.clear();

  // Tac vu gui/nhan du lieu chay rieng tren core 0 (loop chay tren core 1)
  xTaskCreatePinnedToCore(cloudTask, "cloud", 12288, NULL, 1, NULL, 0);
}

// =====================================================
// 14. LOOP
// =====================================================
void loop() {
  maintainWiFi();
  readDHT11();
  readFireSensors();
  controlOutputs();
  updateLCD();

  if (millis() - serialPrev >= SERIAL_INTERVAL) {
    serialPrev = millis();
    Serial.println("======================");
    Serial.print("Nhiet do: "); if (dhtOK) Serial.println((float)temperature); else Serial.println("LOI");
    Serial.print("Do am: ");    if (dhtOK) Serial.println((float)humidity);    else Serial.println("LOI");
    Serial.print("Khoi: ");     Serial.println(smokeDetected ? "CO" : "KHONG");
    Serial.print("Lua: ");      Serial.println(flameDetected ? "CO" : "KHONG");
    Serial.printf("Quat=%d Bom=%d Coi=%d | %s | Nguong=%.1fC\n",
                  fanOut, pumpOut, buzzerOut, ctlManual ? "THU CONG" : "TU DONG", (float)ctlTempLimit);
    Serial.println("======================");
  }

  delay(50);
}
