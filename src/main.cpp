#include <Arduino.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "AnalogSensorService.h"

// --- KONFIGURASI JARINGAN & SUPABASE ---
const char* DEFAULT_WIFI_SSID = "hydrogoo";
const char* DEFAULT_WIFI_PASSWORD = "hydrogoo";
// URL untuk mengambil konfigurasi dari tabel 'config'
const char* SUPABASE_URL_CONFIG = "https://ntudiforfsotyqdufhxu.supabase.co/rest/v1/config";
// URL untuk mengirim data sensor ke tabel 'sensor_logs'
const char* SUPABASE_URL_SENSORS = "https://ntudiforfsotyqdufhxu.supabase.co/rest/v1/sensor_logs";
const char* SUPABASE_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Im50dWRpZm9yZnNvdHlxZHVmaHh1Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3NTkxMjUzMTksImV4cCI6MjA3NDcwMTMxOX0.nSlLo-F6fUs-5hnqq2lt3zk8OU1wRnIjjCvBEMsqe1Y";

// --- KONFIGURASI PIN ---
const int LCD_ADDRESS = 0x27;
const int LCD_COLS = 16;
const int LCD_ROWS = 2;
const int SENSOR_5V_RELAY_PIN  = 16;
const int SENSOR_GND_RELAY_PIN = 19;
// Pin Relay: R1, R2, R3, R4, R5
const int relayPins[] = {13, 27, 26, 25, 33};
const int NUM_RELAYS = 5;
// Pin Tombol: B1, B2, B3, B4, B5
const int buttonPins[] = {32, 35, 34, 39, 36};
const int NUM_BUTTONS = 5;

// Indeks Relay untuk Otomasi
const int PUMP_MIX_RELAY_INDEX = 0;      // Pompa 1: Pendorong Mix
const int PUMP_A_RELAY_INDEX = 1;        // Pompa 2: Pupuk A
const int PUMP_B_RELAY_INDEX = 2;        // Pompa 3: Pupuk B
const int PUMP_PH_MINUS_RELAY_INDEX = 3; // Pompa 4: pH Down
const int PUMP_PH_PLUS_RELAY_INDEX = 4;  // Pompa 5: pH Up

// --- STRUKTUR UNTUK MENYIMPAN KONFIGURASI DARI SUPABASE ---
struct Config {
    float ph_target_min = 6.0;
    float ph_target_max = 6.5;
    float tds_target_min = 800;
    float ph_offset = 0.0;
    float tds_offset = 0.0;
    bool is_automation_active = false;
    double ph_cal_m_value = -8.491;
    double ph_cal_c_value = 35.597;
    unsigned long automation_start_delay_ms = 180000;
    unsigned long pump_ph_on_duration_ms = 2000;
    unsigned long pump_ph_cooldown_duration_ms = 30000;
    unsigned long fertilizer_mix_duration_ms = 5000;
    unsigned long fertilizer_push_duration_ms = 3000;
    unsigned long fertilizer_cooldown_duration_ms = 600000;
    unsigned long supabase_send_interval_ms = 10000;
    unsigned long config_fetch_interval_ms = 60000;
};

// --- ENUM & STATE UNTUK OTOMASI ---
enum FertilizerDoseState { FERTILIZER_IDLE, FERTILIZER_MIXING, FERTILIZER_PUSHING };
FertilizerDoseState fertilizerDoseState = FERTILIZER_IDLE;

// --- OBJEK & VARIABEL GLOBAL ---
LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_COLS, LCD_ROWS);
AnalogSensorService sensorService(SENSOR_5V_RELAY_PIN, SENSOR_GND_RELAY_PIN);
Config config; // Variabel global untuk menyimpan semua konfigurasi

// Variabel state
unsigned long lastSupabaseSend = 0;
unsigned long lastConfigFetch = 0;
unsigned long lastPhDoseTime = 0;
unsigned long lastFertilizerDoseTime = 0;
unsigned long lastLcdUpdate = 0;
float lastKnownPh = -1.0;
float lastKnownTds = -1.0;
bool isPhDosing = false;
unsigned long phDoseStartTime = 0;
int phDosingRelayIndex = -1;
unsigned long fertilizerDoseStartTime = 0;
bool relayStates[NUM_RELAYS] = {false};

// --- FUNGSI PROTOTIPE ---
void setupWifi();
void fetchConfigurationFromSupabase();
void sendToSupabase();
void handleManualPumpControl();
void handleAutomation();
void handlePhAutomation();
void handleFertilizerAutomation();
void startPhDose(int relayIndex);
void handlePhDosing();
void startFertilizerDose();
void handleFertilizerDosing();
void updateDisplay();
bool isRelayControlledByAutomation(int relayIndex);


void setup() {
    Serial.begin(115200);
    Serial.println("\n--- Sistem Hidroponik v12.1 (Supabase Debug) ---");
    lcd.init();
    lcd.backlight();
    lcd.setCursor(0, 0); lcd.print("Inisialisasi...");

    for (int i = 0; i < NUM_BUTTONS; i++) pinMode(buttonPins[i], INPUT_PULLUP);
    for (int i = 0; i < NUM_RELAYS; i++) {
        pinMode(relayPins[i], OUTPUT);
        digitalWrite(relayPins[i], HIGH); // Matikan semua relay
    }

    setupWifi();
    
    // Ambil konfigurasi dari Supabase saat pertama kali dinyalakan
    fetchConfigurationFromSupabase(); 

    // Mulai layanan sensor
    sensorService.begin();
  
    Serial.println("Sistem berjalan.");
    delay(1000);
    lcd.clear();
}

void loop() {
    sensorService.update();

    // Cek apakah sudah waktunya mengambil konfigurasi baru dari Supabase
    if (millis() - lastConfigFetch > config.config_fetch_interval_ms) {
        fetchConfigurationFromSupabase();
    }

    handleManualPumpControl();
    handleAutomation();

    // Cek apakah sudah waktunya mengirim data sensor ke Supabase
    if (WiFi.status() == WL_CONNECTED && millis() - lastSupabaseSend > config.supabase_send_interval_ms) {
        lastSupabaseSend = millis();
        sendToSupabase();
    }
    
    // Perbarui tampilan LCD secara berkala
    if (millis() - lastLcdUpdate > 500) {
        lastLcdUpdate = millis();
        updateDisplay();
    }
}

void fetchConfigurationFromSupabase() {
    lastConfigFetch = millis();
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Tidak ada koneksi WiFi untuk mengambil konfigurasi.");
        return;
    }

    HTTPClient http;
    // Query untuk mengambil baris data dimana kolom 'id' bernilai 1
    String url = String(SUPABASE_URL_CONFIG) + "?select=*&id=eq.1";
    http.begin(url);
    http.addHeader("apikey", SUPABASE_KEY);
    http.addHeader("Authorization", "Bearer " + String(SUPABASE_KEY));
    http.addHeader("Accept", "application/vnd.pgrst.object+json"); // Meminta hasil sebagai objek tunggal

    Serial.println("Mengambil konfigurasi dari Supabase...");
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        
        if (payload.length() == 0 || payload == "null") {
            Serial.println("WARNING: Menerima response kosong dari Supabase.");
            Serial.println("--> PASTIKAN ada satu baris data di tabel 'config' dengan kolom 'id' bernilai 1.");
            return;
        }

        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, payload);

        if (error) {
            Serial.print(F("deserializeJson() gagal: "));
            Serial.println(error.c_str());
            Serial.println("--- Payload Mentah yang Diterima ---");
            Serial.println(payload);
            Serial.println("------------------------------------");
            return;
        }

        // Update semua variabel konfigurasi dari JSON yang diterima
        config.ph_target_min = doc["ph_target_min"] | config.ph_target_min;
        config.ph_target_max = doc["ph_target_max"] | config.ph_target_max;
        config.tds_target_min = doc["tds_target_min"] | config.tds_target_min;
        config.ph_offset = doc["ph_offset"] | config.ph_offset;
        config.tds_offset = doc["tds_offset"] | config.tds_offset;
        config.is_automation_active = doc["is_automation_active"] | config.is_automation_active;
        config.ph_cal_m_value = doc["ph_cal_m_value"] | config.ph_cal_m_value;
        config.ph_cal_c_value = doc["ph_cal_c_value"] | config.ph_cal_c_value;
        config.automation_start_delay_ms = doc["automation_start_delay_ms"] | config.automation_start_delay_ms;
        config.pump_ph_on_duration_ms = doc["pump_ph_on_duration_ms"] | config.pump_ph_on_duration_ms;
        config.pump_ph_cooldown_duration_ms = doc["pump_ph_cooldown_duration_ms"] | config.pump_ph_cooldown_duration_ms;
        config.fertilizer_mix_duration_ms = doc["fertilizer_mix_duration_ms"] | config.fertilizer_mix_duration_ms;
        config.fertilizer_push_duration_ms = doc["fertilizer_push_duration_ms"] | config.fertilizer_push_duration_ms;
        config.fertilizer_cooldown_duration_ms = doc["fertilizer_cooldown_duration_ms"] | config.fertilizer_cooldown_duration_ms;
        config.supabase_send_interval_ms = doc["supabase_send_interval_ms"] | config.supabase_send_interval_ms;
        config.config_fetch_interval_ms = doc["config_fetch_interval_ms"] | config.config_fetch_interval_ms;

        // Terapkan nilai kalibrasi pH yang baru ke sensor service
        sensorService.setCompensationPH(config.ph_cal_m_value, config.ph_cal_c_value);
        
        Serial.println("Konfigurasi berhasil diperbarui dari Supabase!");
        Serial.printf("Otomasi: %s\n", config.is_automation_active ? "AKTIF" : "NONAKTIF");

        // --- TAMBAHAN: Tampilkan semua data yang diterima di Serial Monitor ---
        Serial.println("--- Detail Konfigurasi Diterima ---");
        Serial.printf("  - Target pH: %.2f - %.2f\n", config.ph_target_min, config.ph_target_max);
        Serial.printf("  - Target TDS Min: %.0f ppm\n", config.tds_target_min);
        Serial.printf("  - Offset pH: %.2f | Offset TDS: %.0f\n", config.ph_offset, config.tds_offset);
        Serial.printf("  - Kalibrasi pH (m, c): %.3f, %.3f\n", config.ph_cal_m_value, config.ph_cal_c_value);
        Serial.printf("  - Jeda Awal Otomasi: %lu ms\n", config.automation_start_delay_ms);
        Serial.printf("  - Dosing pH (Durasi | Jeda): %lu ms | %lu ms\n", config.pump_ph_on_duration_ms, config.pump_ph_cooldown_duration_ms);
        Serial.printf("  - Dosing Nutrisi (Mix | Dorong | Jeda): %lu ms | %lu ms | %lu ms\n", config.fertilizer_mix_duration_ms, config.fertilizer_push_duration_ms, config.fertilizer_cooldown_duration_ms);
        Serial.printf("  - Interval (Kirim Sensor | Ambil Konfig): %lu ms | %lu ms\n", config.supabase_send_interval_ms, config.config_fetch_interval_ms);
        Serial.println("------------------------------------");


    } else {
        Serial.printf("Gagal mengambil konfigurasi. Kode HTTP: %d\n", httpCode);
        String errorPayload = http.getString();
        Serial.println("Pesan Error dari Server:");
        Serial.println(errorPayload);
    }
    http.end();
}

void handleManualPumpControl() {
    // Loop untuk setiap tombol, B1-B5 mengontrol R1-R5
    for (int i = 0; i < NUM_BUTTONS; i++) {
        // Cek apakah relay ini sedang dikontrol oleh automasi
        if (isRelayControlledByAutomation(i)) {
            continue; // Lewati jika sedang dipakai automasi
        }

        // Jika tombol ditekan (LOW), nyalakan relay (LOW). Jika tidak, matikan (HIGH).
        bool buttonPressed = (digitalRead(buttonPins[i]) == LOW);
        digitalWrite(relayPins[i], buttonPressed ? LOW : HIGH);
        relayStates[i] = buttonPressed;
    }
}

void handleAutomation() {
    // Jalankan semua proses di latar belakang
    handlePhDosing(); 
    handleFertilizerDosing();

    // Hanya jalankan logika automasi jika diaktifkan dari Supabase
    if (!config.is_automation_active) return;
    
    // Cek apakah ada tombol yang sedang ditekan
    for(int i = 0; i < NUM_BUTTONS; i++) {
        if(digitalRead(buttonPins[i]) == LOW) return; // Jika ada, hentikan automasi sementara
    }

    handlePhAutomation();
    handleFertilizerAutomation();
}


void handlePhAutomation() {
    if (millis() < config.automation_start_delay_ms) return;
    if (millis() - lastPhDoseTime < config.pump_ph_cooldown_duration_ms) return;
    if (!sensorService.isPhActiveNow()) return; // Hanya dosing saat sensor pH aktif
    
    float currentPh = sensorService.getCalibratedPHValue() + config.ph_offset;
    if (currentPh <= 0) return; // Nilai tidak valid

    if (currentPh < config.ph_target_min) {
        startPhDose(PUMP_PH_PLUS_RELAY_INDEX); // pH terlalu rendah, naikkan
    } else if (currentPh > config.ph_target_max) {
        startPhDose(PUMP_PH_MINUS_RELAY_INDEX); // pH terlalu tinggi, turunkan
    }
}

void handleFertilizerAutomation() {
    if (millis() < config.automation_start_delay_ms) return;
    if (millis() - lastFertilizerDoseTime < config.fertilizer_cooldown_duration_ms) return;
    if (sensorService.isPhActiveNow()) return; // Hanya dosing saat sensor TDS aktif
    if (fertilizerDoseState != FERTILIZER_IDLE) return;

    float currentTds = sensorService.getCalibratedTDSValue(25.0) + config.tds_offset;
    if (currentTds <= 0) return; // Nilai tidak valid

    if (currentTds < config.tds_target_min) {
        startFertilizerDose();
    }
}

void updateDisplay() {
    char line1[17], line2[17];
    
    // Ambil nilai sensor terakhir
    lastKnownPh = sensorService.getCalibratedPHValue() + config.ph_offset;
    lastKnownTds = sensorService.getCalibratedTDSValue(25.0) + config.tds_offset;

    snprintf(line1, sizeof(line1), "pH:%.2f TDS:%-4.0f", 
            lastKnownPh > 0 ? lastKnownPh : 0.0, 
            lastKnownTds > 0 ? lastKnownTds : 0.0);
            
    // Tampilkan status relay R1-R5 dan status automasi
    char relayStatusStr[17];
    snprintf(relayStatusStr, sizeof(relayStatusStr), "R:%c%c%c%c%c A:%s",
        relayStates[0] ? '1' : '-',
        relayStates[1] ? '2' : '-',
        relayStates[2] ? '3' : '-',
        relayStates[3] ? '4' : '-',
        relayStates[4] ? '5' : '-',
        config.is_automation_active ? "ON" : "OFF"
    );

    lcd.setCursor(0, 0); lcd.print(line1);
    lcd.setCursor(0, 1); lcd.print(relayStatusStr);
}


// --- FUNGSI-FUNGSI UTILITAS (TIDAK BANYAK BERUBAH) ---

void setupWifi() {
  WiFi.begin(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASSWORD);
  Serial.printf("\nMencoba koneksi ke %s...", DEFAULT_WIFI_SSID);
  lcd.clear(); lcd.setCursor(0,0); lcd.print("Hubungkan WiFi");
  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) { delay(500); Serial.print("."); }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nTerhubung!"); Serial.print("Alamat IP: "); Serial.println(WiFi.localIP());
    lcd.clear(); lcd.print("WiFi Terhubung!"); lcd.setCursor(0,1); lcd.print(WiFi.localIP());
    delay(2000);
  } else {
    Serial.println("\nWiFi Gagal!"); lcd.clear(); lcd.print("WiFi Gagal!");
  }
}

void sendToSupabase() {
  if (lastKnownPh < 0 || lastKnownTds < 0) return;
  JsonDocument doc;
  doc["ph"] = lastKnownPh;
  doc["tds"] = (int)lastKnownTds;
  doc["ph_v"] = sensorService.getFilteredPHVoltage();
  doc["tds_v"] = sensorService.getFilteredTDSVoltage();
  
  String jsonPayload;
  serializeJson(doc, jsonPayload);
  HTTPClient http;
  http.begin(SUPABASE_URL_SENSORS);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", "Bearer " + String(SUPABASE_KEY));
  int httpResponseCode = http.POST(jsonPayload);
  if (httpResponseCode == 201) Serial.println("Data sensor dikirim ke Supabase.");
  else { Serial.printf("Gagal kirim data. Kode HTTP: %d\n", httpResponseCode); }
  http.end();
}

void startPhDose(int relayIndex) {
    if (isPhDosing) return; 
    isPhDosing = true;
    phDosingRelayIndex = relayIndex;
    phDoseStartTime = millis();
    digitalWrite(relayPins[phDosingRelayIndex], LOW);
    relayStates[phDosingRelayIndex] = true;
    Serial.printf("Mulai dosing pH (Relay %d)\n", phDosingRelayIndex + 1);
}

void handlePhDosing() {
    if (!isPhDosing) return;
    if (millis() - phDoseStartTime >= config.pump_ph_on_duration_ms) {
        digitalWrite(relayPins[phDosingRelayIndex], HIGH);
        relayStates[phDosingRelayIndex] = false;
        Serial.printf("Dosing pH selesai (Relay %d)\n", phDosingRelayIndex + 1);
        isPhDosing = false;
        phDosingRelayIndex = -1;
        lastPhDoseTime = millis();
    }
}

void startFertilizerDose() {
    if (fertilizerDoseState != FERTILIZER_IDLE) return;
    Serial.println("Mulai sekuens dosing nutrisi...");
    fertilizerDoseState = FERTILIZER_MIXING;
    fertilizerDoseStartTime = millis();
    digitalWrite(relayPins[PUMP_A_RELAY_INDEX], LOW);
    digitalWrite(relayPins[PUMP_B_RELAY_INDEX], LOW);
    relayStates[PUMP_A_RELAY_INDEX] = true;
    relayStates[PUMP_B_RELAY_INDEX] = true;
}

void handleFertilizerDosing() {
    if (fertilizerDoseState == FERTILIZER_IDLE) return;
    
    if (fertilizerDoseState == FERTILIZER_MIXING) {
        if (millis() - fertilizerDoseStartTime >= config.fertilizer_mix_duration_ms) {
            Serial.println("Mix nutrisi selesai, lanjut mendorong.");
            digitalWrite(relayPins[PUMP_A_RELAY_INDEX], HIGH);
            digitalWrite(relayPins[PUMP_B_RELAY_INDEX], HIGH);
            relayStates[PUMP_A_RELAY_INDEX] = false;
            relayStates[PUMP_B_RELAY_INDEX] = false;

            fertilizerDoseState = FERTILIZER_PUSHING;
            fertilizerDoseStartTime = millis();
            digitalWrite(relayPins[PUMP_MIX_RELAY_INDEX], LOW);
            relayStates[PUMP_MIX_RELAY_INDEX] = true;
        }
    } else if (fertilizerDoseState == FERTILIZER_PUSHING) {
        if (millis() - fertilizerDoseStartTime >= config.fertilizer_push_duration_ms) {
            Serial.println("Sekuens dosing nutrisi selesai.");
            digitalWrite(relayPins[PUMP_MIX_RELAY_INDEX], HIGH);
            relayStates[PUMP_MIX_RELAY_INDEX] = false;
            
            fertilizerDoseState = FERTILIZER_IDLE;
            lastFertilizerDoseTime = millis();
        }
    }
}

bool isRelayControlledByAutomation(int relayIndex) {
    if (isPhDosing && relayIndex == phDosingRelayIndex) {
        return true;
    }
    if (fertilizerDoseState == FERTILIZER_MIXING && (relayIndex == PUMP_A_RELAY_INDEX || relayIndex == PUMP_B_RELAY_INDEX)) {
        return true;
    }
    if (fertilizerDoseState == FERTILIZER_PUSHING && relayIndex == PUMP_MIX_RELAY_INDEX) {
        return true;
    }
    return false;
}

