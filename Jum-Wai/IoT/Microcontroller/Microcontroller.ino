#include <WiFiS3.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <ArduinoJson.h>

// === OLED (U8g2) ===
#include <Wire.h>
#include <U8g2lib.h>

// ====== CONFIG ======
const char WIFI_SSID[]     = "Luck";
const char WIFI_PASSWORD[] = "6969696969";
const char HOST[] = "todolist-phycom-default-rtdb.asia-southeast1.firebasedatabase.app";
const char USER_UID[] = "PV8IlHrRCMd8sSeInr3Y5HVknwp2";

String tasksPath = String("/tasklist/") + USER_UID + ".json";

const int OFFSETS_MIN[] = {10, 5, 0};
const size_t OFFSETS_COUNT = sizeof(OFFSETS_MIN)/sizeof(OFFSETS_MIN[0]);

const int LED_PINS[OFFSETS_COUNT] = {5, 6, 7};
const int TONES_HZ[OFFSETS_COUNT] = {1200, 1800, 2400};

const int BUZZER_PIN = 9;
const unsigned long FETCH_INTERVAL_MS = 1000;

bool uiMuted = true;
unsigned long toastUntil = 0;

void oledToast(const char* line1, const char* line2 = "");

// ปุ่ม (INPUT_PULLUP)
const int BTN_ACK  = 2;
const int BTN_DONE = 3;

// ====== TIME (UTC) ======
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60 * 1000);
WiFiSSLClient https;

// ====== STATE ======
struct TriggerState {
  char taskId[33];
  bool fired[OFFSETS_COUNT];
};
const size_t MAX_TRACK = 50;
TriggerState track[MAX_TRACK];
unsigned long lastFetch = 0;

// ====== แจ้งเตือน ======
bool alertActive = false;
unsigned long alertStart = 0;
int alertIdx = -1;
char alertTaskId[64] = {0};
uint64_t alertTargetMs = 0ULL;

// ====== OLED (U8g2) CONFIG ======
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

char nextTaskName[32] = "-";
long nextTaskMinsLeft = -1;
long nextTaskRemainMs = -1; 

// ====== UTIL ======
int findOrCreateTrackSlot(const char* id) {
  for (size_t i = 0; i < MAX_TRACK; i++) {
    if (track[i].taskId[0] != '\0' && strcmp(track[i].taskId, id) == 0) return (int)i;
  }
  for (size_t i = 0; i < MAX_TRACK; i++) {
    if (track[i].taskId[0] == '\0') {
      strncpy(track[i].taskId, id, sizeof(track[i].taskId)-1);
      track[i].taskId[sizeof(track[i].taskId)-1] = '\0';
      for (size_t j = 0; j < OFFSETS_COUNT; j++) track[i].fired[j] = false;
      return (int)i;
    }
  }
  return -1;
}

String cutToFitUTF8(const char* src, uint8_t maxLen) {
  
  String s(src ? src : "");
  if ((int)s.length() <= maxLen) return s;
  
  int end = maxLen - 1;
  while (end > 0 && ((s[end] & 0xC0) == 0x80)) end--;
  return s.substring(0, end) + "…";
}

void fmtMMSS(long sec, char* out, size_t outlen) {
  if (sec < 0) sec = 0;
  long mm = sec / 60;
  long ss = sec % 60;
  snprintf(out, outlen, "%02ld:%02ld", mm, ss);
}

// ====== HTTP ======
String httpGET(const char* host, const String& path) {
  String payload = "";
  if (!https.connect(host, 443)) {
    Serial.println("HTTPS connect failed");
    return payload;
  }
  https.print(String("GET ") + path + " HTTP/1.1\r\n" +
              "Host: " + host + "\r\nConnection: close\r\n\r\n");
  unsigned long t0 = millis();
  while (https.connected() && !https.available()) {
    if (millis() - t0 > 8000) break;
    delay(10);
  }
  String resp;
  while (https.available()) resp += (char)https.read();
  https.stop();
  int idx = resp.indexOf("\r\n\r\n");
  if (idx >= 0) payload = resp.substring(idx + 4);
  else payload = resp;
  return payload;
}

bool httpPATCH(const String& path, const String& body) {
  if (!https.connect(HOST, 443)) return false;
  https.print(String("PATCH ") + path + " HTTP/1.1\r\n"
              "Host: " + HOST + "\r\n"
              "Content-Type: application/json\r\n"
              "Content-Length: " + String(body.length()) + "\r\n"
              "Connection: close\r\n\r\n" + body);
  delay(100);
  https.stop();
  return true;
}

// ====== OLED HELPERS (U8g2 + ไทย) ======
void oledShowBoot(const char* msg) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_unifont_t_extended);
  u8g2.drawUTF8(0, 14, "ToDo IoT");
  u8g2.drawUTF8(0, 30, "Starting...");
  u8g2.drawUTF8(0, 50, msg);
  u8g2.sendBuffer();
}

void oledShowIdle() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_unifont_t_extended);


  u8g2.drawUTF8(0, 30, "Next Task: ");
  u8g2.drawUTF8(72, 30, nextTaskName);

  // เหลือเวลา (นาที)
  char buf[24];
  if (nextTaskMinsLeft >= 0) {
    snprintf(buf, sizeof(buf), "%ld minutes left", nextTaskMinsLeft);
  } else {
    snprintf(buf, sizeof(buf), "minutes left: -");
  }
  u8g2.drawUTF8(0, 46, buf);

  // Progress bar 0–10 นาทีสุดท้าย
  // แสดง bar
  if (nextTaskRemainMs >= 0 && nextTaskRemainMs <= 600000L) {
    // กว้าง 120px, สูง 8px, เริ่มที่ x=4,y=56
    u8g2.drawFrame(4, 56, 120, 8);
    long filled = (600000L - nextTaskRemainMs) * 120L / 600000L; // 0..120
    if (filled < 0) filled = 0;
    if (filled > 120) filled = 120;
    if (filled > 0) u8g2.drawBox(4, 56, (uint8_t)filled, 8);
  }

  u8g2.sendBuffer();
}

void oledShowAlert(int idx) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_unifont_t_extended);

  // แถบหัวบน
  u8g2.drawBox(0, 0, 128, 14);
  u8g2.setDrawColor(0);
  u8g2.drawUTF8(2, 12, "Alert");
  u8g2.setDrawColor(1);

  char head[32];
  snprintf(head, sizeof(head), "%d minutes left", OFFSETS_MIN[idx]);
  u8g2.drawUTF8(0, 30, head);

  // นับถอยหลังจริงเป็น mm:ss
  uint64_t nowMs = (uint64_t)timeClient.getEpochTime() * 1000ULL;
  long remainSec = (long)((alertTargetMs > nowMs) ? ((alertTargetMs - nowMs) / 1000ULL) : 0);
  char mmss[8];
  fmtMMSS(remainSec, mmss, sizeof(mmss));
  char line2[40];
  snprintf(line2, sizeof(line2), "Countdown: %s", mmss);
  u8g2.drawUTF8(0, 46, line2);

  // ปุ่ม
  u8g2.drawUTF8(0, 62, "SW2:ACK SW3:DONE");

  u8g2.sendBuffer();
}

// ====== แจ้งเตือน ======
void startAlert(const char* id, int idx, uint64_t targetTsMs) { // <<< รับ tsMs เข้ามา
  alertActive = true;
  alertStart = millis();
  alertIdx = idx;
  alertTargetMs = targetTsMs;
  strncpy(alertTaskId, id, sizeof(alertTaskId)-1);
  alertTaskId[sizeof(alertTaskId)-1] = '\0';

  Serial.print("[ALERT START] TaskID: "); Serial.println(id);
  // oledShowAlert(idx);
}

void stopAlert() {
  alertActive = false;
  alertIdx = -1;
  alertTaskId[0] = '\0';
  alertTargetMs = 0ULL;
  noTone(BUZZER_PIN);
  for (size_t i = 0; i < OFFSETS_COUNT; i++) digitalWrite(LED_PINS[i], LOW);
  Serial.println("[ALERT STOP]");

  oledShowIdle();
}

void serviceAlert() {
  if (!alertActive) return;
  if (!uiMuted) return; 

  unsigned long elapsed = millis() - alertStart;

  // เกิน 1 นาที (60000ms) ให้หยุด
  if (elapsed > 60000UL) {
    Serial.println("[AUTO STOP] Alert timeout 1 minute");
    stopAlert();
    return;
  }

  static unsigned long lastBeat = 0;
  if (millis() - lastBeat >= 500) {  // จอกระพริบทุก 0.5s
    lastBeat = millis();

    static bool toggle = false;
    toggle = !toggle;

    if (alertIdx >= 0 && alertIdx < (int)OFFSETS_COUNT) {
      digitalWrite(LED_PINS[alertIdx], toggle ? HIGH : LOW);
      if (toggle) tone(BUZZER_PIN, TONES_HZ[alertIdx], 250);
    }

    // อัปเดตตัวเลขนับถอยหลังบนจอแบบเรียลไทม์
    oledShowAlert(alertIdx);
  }
}

void checkButtons() {
  bool ack  = (digitalRead(BTN_ACK)  == LOW);
  bool done = (digitalRead(BTN_DONE) == LOW);

  if (!ack && !done) return;

  if (ack) {
    Serial.println("[BTN] ACK pressed");
    if (alertActive) stopAlert();
    oledToast("ACK received", "Alert stopped");
  }

  if (done) {
    Serial.println("[BTN] DONE pressed");
    if (alertActive && alertTaskId[0]) {
      String path = String("/tasklist/") + USER_UID + "/" + alertTaskId + ".json";
      String body = "{\"done\":true}";
      httpPATCH(path, body);
    }
    stopAlert();
    oledToast("Task marked DONE");        
  }
}


// ====== อ่านค่า ts ======
bool readTsMs(const JsonVariant& vts, uint64_t& outTsMs) {
  outTsMs = 0ULL;
  if (vts.is<uint64_t>()) outTsMs = vts.as<uint64_t>();
  else if (vts.is<long long>()) { long long x = vts.as<long long>(); if (x < 0) return false; outTsMs = (uint64_t)x; }
  else if (vts.is<double>()) outTsMs = (uint64_t)vts.as<double>();
  else if (vts.is<const char*>()) {
    const char* s = vts.as<const char*>(); if (!s) return false;
    outTsMs = strtoull(s, NULL, 10);
  } else return false;

  if (outTsMs < 100000000000ULL) outTsMs *= 1000ULL; // secs → ms
  return true;
}

// ====== MAIN LOGIC ======
void processTasksJson(const String& json) {
  if (json.length() == 0 || json == "null") return;
  StaticJsonDocument<8192> doc;
  if (deserializeJson(doc, json)) return;

  uint64_t nowMs = (uint64_t)timeClient.getEpochTime() * 1000ULL;

  // หาที่ใกล้สุด เพื่อแสดงบนจอ
  long bestMins = -1;
  String bestName = "-";
  long bestRemainMs = -1;

  for (JsonPair kv : doc.as<JsonObject>()) {
    const char* id = kv.key().c_str();
    JsonObject v = kv.value().as<JsonObject>();
    if ((bool)(v["done"] | false)) continue;

    const char* task = v["task"] | "(no title)";
    uint64_t tsMs;
    if (!readTsMs(v["ts"], tsMs)) continue;
    long diffMs = (long)(tsMs - nowMs);
    long minsLeft = diffMs / 60000L;

    // เก็บงานที่เหลือเวลาบวกและน้อยสุด
    if (minsLeft >= 0 && (bestMins < 0 || minsLeft < bestMins)) {
      bestMins = minsLeft;
      bestName = task;
      bestRemainMs = diffMs;
    }

    // แจ้งตาม 10/5/0 ±2s
    for (size_t i = 0; i < OFFSETS_COUNT; i++) {
      long target = OFFSETS_MIN[i] * 60000L;
      if (labs(diffMs - target) <= 2000) {
        int slot = findOrCreateTrackSlot(id);
        if (slot >= 0 && !track[slot].fired[i]) {
          Serial.print("[ALERT] อีก "); Serial.print(OFFSETS_MIN[i]);
          Serial.print(" นาที → "); Serial.println(task);

          String clipped = cutToFitUTF8(task, 23);
          clipped.toCharArray(nextTaskName, sizeof(nextTaskName));
          nextTaskMinsLeft = minsLeft;
          nextTaskRemainMs = diffMs;

          startAlert(id, i, tsMs);
          track[slot].fired[i] = true;
        }
      }
    }
  }

  // อัปเดตหน้าจอโหมดปกติ + progress bar
  if (!alertActive) {
    String clipped = cutToFitUTF8(bestName.c_str(), 23);
    clipped.toCharArray(nextTaskName, sizeof(nextTaskName));
    nextTaskMinsLeft = bestMins;
    nextTaskRemainMs = bestRemainMs;  // ใช้คำนวณ progress (0–10 นาที)

    oledShowIdle();
  }
}

// ====== TOAST MODE (show only when pressed) ======


void oledToast(const char* line1, const char* line2) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_unifont_t_extended);
  u8g2.drawUTF8(0, 20, line1);
  if (line2 && line2[0]) u8g2.drawUTF8(0, 40, line2);
  u8g2.sendBuffer();
  uiMuted = false;
  toastUntil = millis() + 2000;      // โชว์2 วิ แล้วดับ
}


// ====== SETUP ======
void setup() {
  Serial.begin(9600);

  // OLED init (U8g2)
  u8g2.begin();
  oledShowBoot("Connecting WiFi…");

  for (size_t i = 0; i < OFFSETS_COUNT; i++) {
    pinMode(LED_PINS[i], OUTPUT);
    digitalWrite(LED_PINS[i], LOW);
  }
  pinMode(BUZZER_PIN, OUTPUT);

  pinMode(BTN_ACK,  INPUT_PULLUP);
  pinMode(BTN_DONE, INPUT_PULLUP);

  for (size_t i = 0; i < MAX_TRACK; i++) {
    track[i].taskId[0] = '\0';
    for (size_t j = 0; j < OFFSETS_COUNT; j++) track[i].fired[j] = false;
  }

  int st = WL_IDLE_STATUS;
  while (st != WL_CONNECTED) {
    oledShowBoot("Connecting WiFi…");
    Serial.print("WiFi connecting: "); Serial.println(WIFI_SSID);
    st = WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    delay(3000);
  }
  Serial.print("IP: "); Serial.println(WiFi.localIP());
  oledShowBoot("WiFi connecting success");

  timeClient.begin();
  while (!timeClient.update()) {
    timeClient.forceUpdate();
    delay(200);
  }
  Serial.print("Epoch(UTC): "); Serial.println(timeClient.getEpochTime());
  oledShowIdle();
}

// ====== LOOP ======
void loop() {
  timeClient.update();
  serviceAlert();
  checkButtons();

  if (millis() - lastFetch >= FETCH_INTERVAL_MS) {
    lastFetch = millis();
    String payload = httpGET(HOST, tasksPath);
    processTasksJson(payload);
  }

if (!uiMuted && toastUntil && millis() > toastUntil) {
  u8g2.clearDisplay();
  uiMuted = true;
  toastUntil = 0;
  oledShowIdle();
}


}
