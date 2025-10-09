#include <Arduino.h>
#include <LiquidCrystal_I2C.h>
#include "AnalogSensorService.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// --- KONFIGURASI JARINGAN ---
const char* DEFAULT_WIFI_SSID = "hydrogoo";
const char* DEFAULT_WIFI_PASSWORD = "hydrogoo";
const char* SUPABASE_URL_SENSORS = "https://ntudiforfsotyqdufhxu.supabase.co/rest/v1/sensor_logs";
const char* SUPABASE_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Im50dWRpZm9yZnNvdHlxZHVmaHh1Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3NTkxMjUzMTksImV4cCI6MjA3NDcwMTMxOX0.nSlLo-F6fUs-5hnqq2lt3zk8OU1wRnIjjCvBEMsqe1Y";
const unsigned long SUPABASE_SEND_INTERVAL = 10000;

// --- KONFIGURASI UMUM ---
const int LCD_ADDRESS = 0x27;
const int LCD_COLS = 16;
const int LCD_ROWS = 2;
const int SENSOR_5V_RELAY_PIN  = 16;
const int SENSOR_GND_RELAY_PIN = 19;
const int relayPins[] = {13, 27, 26, 25, 33};
const int NUM_RELAYS = 5;
const int buttonPins[] = {32, 35, 34, 39, 36};
const int NUM_BUTTONS = 5;
const int PH_MINUS_RELAY_INDEX = 3;
const int PH_PLUS_RELAY_INDEX = 4;
const unsigned long AUTOMATION_START_DELAY = 180000;
const unsigned long PUMP_ON_DURATION = 2000;
const unsigned long PUMP_COOLDOWN_DURATION = 30000;
const unsigned long LCD_UPDATE_INTERVAL = 500;

// --- ENUM & STATE UNTUK MODE OPERASI ---
enum OperatingMode { NORMAL, CALIBRATION };
OperatingMode currentMode = NORMAL;

enum CalibrationMode { NONE, PH_CAL, TDS_CAL, PH_THRESH_MIN_CAL, PH_THRESH_MAX_CAL };
CalibrationMode calMode = NONE;

// --- OBJEK & VARIABEL GLOBAL ---
LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_COLS, LCD_ROWS);
AnalogSensorService sensorService(SENSOR_5V_RELAY_PIN, SENSOR_GND_RELAY_PIN);
Preferences preferences;

// Variabel state & konfigurasi
int selectedPumpIndex = 0;
unsigned long buttonPressStartTime = 0;
bool buttonHeld = false;
float phCalibrationOffset = 0.0;
float tdsCalibrationOffset = 0.0;
float phChangeStep = 0.01;
int tdsChangeStep = 1;
unsigned long lastSupabaseSend = 0;
unsigned long lastDoseTime = 0;
unsigned int autoDoseCount = 0;
unsigned long lastLcdUpdate = 0;
float lastKnownPh = -1.0;
float lastKnownTds = -1.0;
bool isDosing = false;
unsigned long doseStartTime = 0;
int dosingRelayIndex = -1;
bool relayStates[NUM_RELAYS] = {false};
float phTargetMin = 5.8; // Sekarang menjadi variabel, bukan konstanta
float phTargetMax = 6.2; // Sekarang menjadi variabel, bukan konstanta


// --- FUNGSI PROTOTIPE ---
void loadConfiguration();
void handleNormalMode();
void handleCalibrationMode();
void updateNormalDisplay();
void updateCalibrationDisplay();
void setupWifi();
void sendToSupabase();
void handleAutomation();
void startDose(int relayIndex);
void handleDosing();


void setup() {
  Serial.begin(115200);
  Serial.println("\n--- Sistem Monitoring v10.0 (Configurable Thresholds) ---");
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0); lcd.print("Inisialisasi...");

  for (int i = 0; i < NUM_BUTTONS; i++) pinMode(buttonPins[i], INPUT_PULLUP);
  for (int i = 0; i < NUM_RELAYS; i++) {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], HIGH);
  }

  setupWifi();
  sensorService.setCompensationPH(-8.491, 35.597); 
  loadConfiguration();
  sensorService.begin();
  
  Serial.println("Sistem berjalan dalam mode NORMAL.");
  delay(1000);
  lcd.clear();
}

void loop() {
  sensorService.update();

  if (currentMode == NORMAL) {
    handleNormalMode();
  } else if (currentMode == CALIBRATION) {
    handleCalibrationMode();
  }

  if (millis() - lastLcdUpdate >= LCD_UPDATE_INTERVAL) {
    lastLcdUpdate = millis();
    if (currentMode == NORMAL) updateNormalDisplay();
    else updateCalibrationDisplay();
  }
}

void loadConfiguration() {
  preferences.begin("sensor-cal", true);
  phCalibrationOffset = preferences.getFloat("ph_offset", 0.0);
  tdsCalibrationOffset = preferences.getFloat("tds_offset", 0.0);
  phTargetMin = preferences.getFloat("ph_min", 5.8);
  phTargetMax = preferences.getFloat("ph_max", 6.2);
  preferences.end();
  Serial.printf("Konfigurasi dimuat: pH Offset=%.2f, TDS Offset=%.0f, pH Target=%.2f-%.2f\n",
                phCalibrationOffset, tdsCalibrationOffset, phTargetMin, phTargetMax);
}

void handleNormalMode() {
  // Tombol 1: Selector Pompa
  if (digitalRead(buttonPins[0]) == LOW) {
    selectedPumpIndex = (selectedPumpIndex + 1) % NUM_RELAYS;
    delay(200);
  }
  // Tombol 2: Aktifkan pompa terpilih (momentary)
  bool pump_on = (digitalRead(buttonPins[1]) == LOW);
  digitalWrite(relayPins[selectedPumpIndex], pump_on ? LOW : HIGH);
  if(!isDosing) relayStates[selectedPumpIndex] = pump_on;

  // Tombol 3: Tahan untuk masuk mode kalibrasi
  if (digitalRead(buttonPins[2]) == LOW) {
    if (!buttonHeld) {
      buttonPressStartTime = millis();
      buttonHeld = true;
    } else if (millis() - buttonPressStartTime > 3000) {
      currentMode = CALIBRATION;
      calMode = PH_CAL;
      loadConfiguration(); // Muat nilai TERSIMPAN saat ini ke variabel kerja
      Serial.println("Masuk ke Mode Kalibrasi.");
      lcd.clear(); lcd.print("Mode Kalibrasi"); delay(1000);
      buttonHeld = false;
      return;
    }
  } else {
    buttonHeld = false;
  }

  // Proses Latar Belakang
  handleDosing(); 
  handleAutomation();
  if (WiFi.status() == WL_CONNECTED && millis() - lastSupabaseSend >= SUPABASE_SEND_INTERVAL) {
    lastSupabaseSend = millis();
    sendToSupabase();
  }
}

void handleCalibrationMode() {
  // Tombol B4 (Turun) dan B5 (Naik)
  if (digitalRead(buttonPins[3]) == LOW) { // TURUN
    if (calMode == PH_CAL) phCalibrationOffset -= phChangeStep;
    else if (calMode == TDS_CAL) tdsCalibrationOffset -= tdsChangeStep;
    else if (calMode == PH_THRESH_MIN_CAL) phTargetMin -= phChangeStep;
    else if (calMode == PH_THRESH_MAX_CAL) phTargetMax -= phChangeStep;
    delay(100);
  }
  if (digitalRead(buttonPins[4]) == LOW) { // NAIK
    if (calMode == PH_CAL) phCalibrationOffset += phChangeStep;
    else if (calMode == TDS_CAL) tdsCalibrationOffset += tdsChangeStep;
    else if (calMode == PH_THRESH_MIN_CAL) phTargetMin += phChangeStep;
    else if (calMode == PH_THRESH_MAX_CAL) phTargetMax += phChangeStep;
    delay(100);
  }

  // Tombol B3 (Kontekstual: Klik -> ubah step, Tahan -> simpan & lanjut)
  if (digitalRead(buttonPins[2]) == LOW) {
    if (!buttonHeld) {
      buttonPressStartTime = millis();
      buttonHeld = true;
    } else if (millis() - buttonPressStartTime > 3000) {
      // Aksi TAHAN LAMA (SIMPAN & LANJUT)
      if (calMode == PH_CAL) {
        calMode = TDS_CAL;
        lcd.clear(); lcd.print("Lanjut ke TDS.."); delay(1000);
      } else if (calMode == TDS_CAL) {
        calMode = PH_THRESH_MIN_CAL;
        lcd.clear(); lcd.print("Set Batas Min pH"); delay(1000);
      } else if (calMode == PH_THRESH_MIN_CAL) {
        calMode = PH_THRESH_MAX_CAL;
        lcd.clear(); lcd.print("Set Batas Max pH"); delay(1000);
      } else if (calMode == PH_THRESH_MAX_CAL) {
        // Tahap terakhir: simpan semua ke NVS
        preferences.begin("sensor-cal", false);
        preferences.putFloat("ph_offset", phCalibrationOffset);
        preferences.putFloat("tds_offset", tdsCalibrationOffset);
        preferences.putFloat("ph_min", phTargetMin);
        preferences.putFloat("ph_max", phTargetMax);
        preferences.end();
        Serial.println("Semua konfigurasi disimpan permanen.");
        lcd.clear(); lcd.print("Tersimpan!"); delay(1000);
        ESP.restart();
      }
      buttonHeld = false;
    }
  } else {
    if (buttonHeld) { // Aksi KLIK BIASA (UBAH STEP)
      if (calMode == PH_CAL || calMode == PH_THRESH_MIN_CAL || calMode == PH_THRESH_MAX_CAL) {
        phChangeStep = (phChangeStep == 0.01) ? 0.1 : 0.01;
      } else if (calMode == TDS_CAL) {
        if (tdsChangeStep == 1) tdsChangeStep = 10;
        else if (tdsChangeStep == 10) tdsChangeStep = 100;
        else tdsChangeStep = 1;
      }
    }
    buttonHeld = false;
  }
}

void updateNormalDisplay() {
    char line1[17], line2[17];
    
    lastKnownPh = sensorService.getCalibratedPHValue() + phCalibrationOffset;
    lastKnownTds = sensorService.getCalibratedTDSValue(25.0) + tdsCalibrationOffset;

    snprintf(line1, sizeof(line1), "pH:%.2f TDS:%-4.0f", 
            lastKnownPh > 0 ? lastKnownPh : 0.0, 
            lastKnownTds > 0 ? lastKnownTds : 0.0);
            
    char relayStatusStr[17] = "R:";
    for (int i = 0; i < NUM_RELAYS; i++) {
      if (i == selectedPumpIndex && !relayStates[i]) {
        strcat(relayStatusStr, "[");
        char pumpNum[2] = {(char)('1' + i), '\0'};
        strcat(relayStatusStr, pumpNum);
        strcat(relayStatusStr, "]");
      } else {
        char status[2] = {relayStates[i] ? (char)('1' + i) : '-', '\0'};
        strcat(relayStatusStr, status);
      }
    }
    
    char autoStatus[10];
    snprintf(autoStatus, sizeof(autoStatus), " A:%s", (millis() > AUTOMATION_START_DELAY) ? "ON" : "OFF");
    strcat(relayStatusStr, autoStatus);

    lcd.setCursor(0, 0); lcd.print(line1);
    lcd.setCursor(0, 1); lcd.print(line2);
}

void updateCalibrationDisplay() {
  char line1[17], line2[17];
  
  switch(calMode) {
    case PH_CAL:
      {
        float basePhValue = sensorService.getCalibratedPHValue();
        snprintf(line1, sizeof(line1), "Kalibrasi pH");
        snprintf(line2, sizeof(line2), "Trgt:%.2f(%.2f)", basePhValue + phCalibrationOffset, phChangeStep);
      }
      break;
    case TDS_CAL:
      {
        float baseTdsValue = sensorService.getCalibratedTDSValue(25.0);
        snprintf(line1, sizeof(line1), "Kalibrasi TDS");
        snprintf(line2, sizeof(line2), "Trgt:%.0f(%d)", baseTdsValue + tdsCalibrationOffset, tdsChangeStep);
      }
      break;
    case PH_THRESH_MIN_CAL:
      snprintf(line1, sizeof(line1), "Set Batas Min pH");
      snprintf(line2, sizeof(line2), "Nilai: %.2f(%.2f)", phTargetMin, phChangeStep);
      break;
    case PH_THRESH_MAX_CAL:
      snprintf(line1, sizeof(line1), "Set Batas Max pH");
      snprintf(line2, sizeof(line2), "Nilai: %.2f(%.2f)", phTargetMax, phChangeStep);
      break;
    default:
      snprintf(line1, sizeof(line1), "Mode Tidak Dikenal");
      break;
  }
  
  lcd.setCursor(0, 0); lcd.print(line1);
  lcd.setCursor(0, 1); lcd.print(line2);
}


void handleAutomation() {
    if (millis() < AUTOMATION_START_DELAY) return;
    if (digitalRead(buttonPins[1]) == LOW) return;
    if (millis() - lastDoseTime < PUMP_COOLDOWN_DURATION) return;
    if (!sensorService.isPhActiveNow()) return;
    
    float currentPh = sensorService.getCalibratedPHValue() + phCalibrationOffset;
    if (currentPh <= 0) return;

    if (currentPh < phTargetMin) {
        startDose(PH_PLUS_RELAY_INDEX);
    } else if (currentPh > phTargetMax) {
        startDose(PH_MINUS_RELAY_INDEX);
    }
}

// --- FUNGSI-FUNGSI UTILITAS ---

void setupWifi() {
  String ssid = DEFAULT_WIFI_SSID;
  String password = DEFAULT_WIFI_PASSWORD;

  Serial.printf("\nMencoba koneksi ke %s...", ssid.c_str());
  lcd.clear(); 
  lcd.setCursor(0,0); 
  lcd.print("Connect to"); 
  lcd.setCursor(0,1); 
  lcd.print(ssid);

  WiFi.begin(ssid.c_str(), password.c_str());
  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500); 
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nTerhubung!");
    Serial.print("Alamat IP: "); 
    Serial.println(WiFi.localIP());
    lcd.clear(); 
    lcd.print("WiFi Terhubung!"); 
    lcd.setCursor(0,1); 
    lcd.print(WiFi.localIP());
    delay(2000);
  } else {
    Serial.println("\nTidak dapat terhubung ke jaringan WiFi.");
    lcd.clear(); 
    lcd.print("WiFi Gagal!");
  }
}

void sendToSupabase() {
  if (lastKnownPh < 0 || lastKnownTds < 0) {
    return;
  }
  
  float currentPhVoltage = sensorService.getFilteredPHVoltage();
  float currentTdsVoltage = sensorService.getFilteredTDSVoltage();

  JsonDocument doc;
  doc["ph"] = lastKnownPh;
  doc["tds"] = (int)lastKnownTds;
  doc["ph_auto"] = autoDoseCount;
  doc["ph_v"] = currentPhVoltage;
  doc["tds_v"] = currentTdsVoltage;
  
  String jsonPayload;
  serializeJson(doc, jsonPayload);
  
  HTTPClient http;
  http.begin(SUPABASE_URL_SENSORS);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", "Bearer " + String(SUPABASE_KEY));
  
  int httpResponseCode = http.POST(jsonPayload);
  if (httpResponseCode == 201) {
    Serial.println("Data berhasil dikirim ke Supabase!");
  } else {
    Serial.print("Gagal mengirim data. Kode HTTP: ");
    Serial.println(httpResponseCode);
  }
  http.end();
}

void startDose(int relayIndex) {
    if (isDosing) return; 

    isDosing = true;
    dosingRelayIndex = relayIndex;
    doseStartTime = millis();

    digitalWrite(relayPins[dosingRelayIndex], LOW);
    relayStates[dosingRelayIndex] = true;

    Serial.printf("Mulai dosing untuk relay %d\n", dosingRelayIndex + 1);
    autoDoseCount++;
}

void handleDosing() {
    if (!isDosing) return;

    if (millis() - doseStartTime >= PUMP_ON_DURATION) {
        digitalWrite(relayPins[dosingRelayIndex], HIGH);
        relayStates[dosingRelayIndex] = false;
        
        Serial.printf("Dosing selesai untuk relay %d\n", dosingRelayIndex + 1);

        isDosing = false;
        dosingRelayIndex = -1;
        lastDoseTime = millis();
    }
}

