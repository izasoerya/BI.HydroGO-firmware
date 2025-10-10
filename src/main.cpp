#include <Arduino.h>
#include <LiquidCrystal_I2C.h>
#include "AnalogSensorService.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// =================================================================================
// --- Struktur Konfigurasi Terpusat ---
// Semua setelan sekarang ada di sini, diambil dari Supabase.
// =================================================================================
struct AppConfig {
    // Kolom Wajib (dulu di Preferences)
    float ph_offset = 0.0;
    float tds_offset = 0.0;
    float ph_target_min = 5.8;
    float ph_target_max = 6.2;
    float tds_target_min = 700.0;

    // Kolom Tambahan (dulu konstanta di kode)
    unsigned long automation_start_delay_ms = 180000;
    unsigned long pump_ph_on_duration_ms = 2000;
    unsigned long pump_ph_cooldown_duration_ms = 15000;
    unsigned long fertilizer_mix_duration_ms = 5000;
    unsigned long fertilizer_push_duration_ms = 3000;
    unsigned long fertilizer_cooldown_duration_ms = 45000;
    unsigned long supabase_send_interval_ms = 10000;
    float ph_cal_m_value = -8.491;
    float ph_cal_c_value = 35.597;
    bool is_automation_active = true;

    // BARU: Kolom untuk fitur Realtime
    unsigned long config_fetch_interval_ms = 300000; // Default 5 menit
};


// =================================================================================
// --- KONFIGURASI JARINGAN & HARDWARE (TETAP) ---
// =================================================================================
const char* DEFAULT_WIFI_SSID = "hydrogoo"; // Ganti dengan SSID Anda
const char* DEFAULT_WIFI_PASSWORD = "hydrogoo"; // Ganti dengan Password Anda

const char* SUPABASE_URL_SENSORS = "https://ntudiforfsotyqdufhxu.supabase.co/rest/v1/sensor_logs";
const char* SUPABASE_URL_CONFIG = "https://ntudiforfsotyqdufhxu.supabase.co/rest/v1/config";
const char* SUPABASE_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Im50dWRpZm9yZnNvdHlxZHVmaHh1Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3NTkxMjUzMTksImV4cCI6MjA3NDcwMTMxOX0.nSlLo-F6fUs-5hnqq2lt3zk8OU1wRnIjjCvBEMsqe1Y";

// --- PIN & ALAMAT (TETAP) ---
const int LCD_ADDRESS = 0x27;
const int LCD_COLS = 16;
const int LCD_ROWS = 2;
const int ADS1115_ADDRESS = 0x48;

// Pin Relay Pompa
const int relayPins[] = {2, 4, 16, 17, 5, 18, 19}; // MIX, A, B, pH-, pH+, SENSOR_5V, SENSOR_GND
const int PUMP_MIX_RELAY_INDEX = 0;
const int PUMP_A_RELAY_INDEX = 1;
const int PUMP_B_RELAY_INDEX = 2;
const int PUMP_PH_DOWN_RELAY_INDEX = 3;
const int PUMP_PH_UP_RELAY_INDEX = 4;
const int SENSOR_POWER_5V_INDEX = 5;
const int SENSOR_POWER_GND_INDEX = 6;

// =================================================================================
// --- OBJEK & VARIABEL GLOBAL ---
// =================================================================================
AppConfig config; // Objek global untuk menyimpan semua konfigurasi
Preferences preferences; // Untuk menyimpan backup konfigurasi

LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_COLS, LCD_ROWS);
AnalogSensorService analogSensorService(relayPins[SENSOR_POWER_5V_INDEX], relayPins[SENSOR_POWER_GND_INDEX], ADS1115_ADDRESS);

// Variabel status & waktu
unsigned long lastSupabaseSend = 0;
unsigned long lastPhDoseTime = 0;
unsigned long lastFertilizerDoseTime = 0;
unsigned long appStartTime = 0;
bool isAutomationReady = false;

// BARU: Timer untuk pengecekan konfigurasi realtime
unsigned long lastConfigFetch = 0;

// Status Dosing pH
enum PhDoseState { PH_IDLE, PH_DOSING, PH_COOLDOWN };
PhDoseState phDoseState = PH_IDLE;
unsigned long phDoseStartTime = 0;
bool isPhUpDosing = false;

// Status Dosing Pupuk
enum FertilizerDoseState { FERTILIZER_IDLE, FERTILIZER_MIXING, FERTILIZER_PUSHING, FERTILIZER_COOLDOWN };
FertilizerDoseState fertilizerDoseState = FERTILIZER_IDLE;
unsigned long fertilizerDoseStartTime = 0;

// =================================================================================
// --- FUNGSI BARU: Mengelola Konfigurasi ---
// =================================================================================

void saveConfigToPreferences(const AppConfig& cfg) {
    preferences.begin("hydro-config", false);
    preferences.putBytes("appConfig", &cfg, sizeof(AppConfig));
    preferences.end();
    Serial.println("[CONFIG] Konfigurasi berhasil disimpan ke Preferences sebagai backup.");
}

bool loadConfigFromPreferences(AppConfig& cfg) {
    preferences.begin("hydro-config", true);
    if (preferences.isKey("appConfig")) {
        preferences.getBytes("appConfig", &cfg, sizeof(AppConfig));
        preferences.end();
        Serial.println("[CONFIG] Konfigurasi berhasil dimuat dari Preferences.");
        return true;
    }
    preferences.end();
    Serial.println("[CONFIG] Tidak ada backup konfigurasi di Preferences.");
    return false;
}

// Direvisi untuk mengambil kolom baru
bool fetchConfigurationFromSupabase(AppConfig& cfg) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[SUPABASE] WiFi tidak terhubung, tidak bisa mengambil konfigurasi.");
        return false;
    }

    HTTPClient http;
    String url = String(SUPABASE_URL_CONFIG) + "?id=eq.1&select=*";
    http.begin(url);
    http.addHeader("apikey", SUPABASE_KEY);
    http.addHeader("Authorization", "Bearer " + String(SUPABASE_KEY));

    Serial.println("[SUPABASE] Mengambil konfigurasi dari server...");
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        // Serial.println("[SUPABASE] Response: " + payload); // Uncomment for debugging

        DynamicJsonDocument doc(2048); // Kapasitas ditambah untuk jaga-jaga
        DeserializationError error = deserializeJson(doc, payload);

        if (error) {
            Serial.print(F("[JSON] Deserialisasi gagal: "));
            Serial.println(error.c_str());
            http.end();
            return false;
        }

        if (doc.is<JsonArray>() && doc.as<JsonArray>().size() > 0) {
            JsonObject obj = doc.as<JsonArray>()[0];
            
            // Map JSON ke struct AppConfig
            cfg.ph_offset = obj["ph_offset"];
            cfg.tds_offset = obj["tds_offset"];
            cfg.ph_target_min = obj["ph_target_min"];
            cfg.ph_target_max = obj["ph_target_max"];
            cfg.tds_target_min = obj["tds_target_min"];
            cfg.automation_start_delay_ms = obj["automation_start_delay_ms"];
            cfg.pump_ph_on_duration_ms = obj["pump_ph_on_duration_ms"];
            cfg.pump_ph_cooldown_duration_ms = obj["pump_ph_cooldown_duration_ms"];
            cfg.fertilizer_mix_duration_ms = obj["fertilizer_mix_duration_ms"];
            cfg.fertilizer_push_duration_ms = obj["fertilizer_push_duration_ms"];
            cfg.fertilizer_cooldown_duration_ms = obj["fertilizer_cooldown_duration_ms"];
            cfg.supabase_send_interval_ms = obj["supabase_send_interval_ms"];
            cfg.ph_cal_m_value = obj["ph_cal_m_value"];
            cfg.ph_cal_c_value = obj["ph_cal_c_value"];
            cfg.is_automation_active = obj["is_automation_active"];
            // BARU: Ambil interval fetch
            cfg.config_fetch_interval_ms = obj["config_fetch_interval_ms"];


            Serial.println("[SUPABASE] Konfigurasi berhasil diambil dan di-parse.");
            http.end();
            return true;
        } else {
            Serial.println("[SUPABASE] Response JSON kosong atau format salah.");
        }
    } else {
        Serial.print("[SUPABASE] Gagal mengambil konfigurasi, HTTP code: ");
        Serial.println(httpCode);
    }

    http.end();
    return false;
}


// =================================================================================
// --- FUNGSI SETUP ---
// =================================================================================
void setup() {
    Serial.begin(115200);
    appStartTime = millis();

    lcd.init();
    lcd.backlight();
    lcd.setCursor(0, 0);
    lcd.print("HydroGO v2.1");
    lcd.setCursor(0, 1);
    lcd.print("Initializing...");

    for (int pin : relayPins) {
        pinMode(pin, OUTPUT);
        digitalWrite(pin, HIGH);
    }

    Serial.print("Menghubungkan ke WiFi...");
    WiFi.begin(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi terhubung!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    lcd.clear();
    lcd.print("WiFi Connected!");

    delay(1000);

    lcd.clear();
    lcd.print("Get Config...");
    if (fetchConfigurationFromSupabase(config)) {
        lcd.setCursor(0, 1);
        lcd.print("OK from Server!");
        saveConfigToPreferences(config); 
    } else {
        lcd.setCursor(0, 1);
        lcd.print("FAIL. Use local.");
        if (!loadConfigFromPreferences(config)) {
            Serial.println("[CONFIG] Gagal memuat dari mana pun, menggunakan nilai default.");
        }
    }
    
    delay(2000);

    analogSensorService.setCompensationPH(config.ph_cal_m_value, config.ph_cal_c_value);
    analogSensorService.begin();

    lcd.clear();
    lcd.print("System Ready.");
    lastConfigFetch = millis(); // Mulai timer pengecekan realtime
}


// =================================================================================
// --- FUNGSI-FUNGSI LOGIKA (TIDAK BERUBAH BANYAK) ---
// =================================================================================

void startPhDosing(bool isUp) {
    if (phDoseState != PH_IDLE) return;
    isPhUpDosing = isUp;
    phDoseState = PH_DOSING;
    phDoseStartTime = millis();
    digitalWrite(relayPins[isUp ? PUMP_PH_UP_RELAY_INDEX : PUMP_PH_DOWN_RELAY_INDEX], LOW);
    Serial.println(isUp ? "Mulai dosing pH Up." : "Mulai dosing pH Down.");
}

void handlePhDosing() {
    if (phDoseState == PH_IDLE) return;
    if (phDoseState == PH_DOSING) {
        if (millis() - phDoseStartTime >= config.pump_ph_on_duration_ms) {
            digitalWrite(relayPins[isPhUpDosing ? PUMP_PH_UP_RELAY_INDEX : PUMP_PH_DOWN_RELAY_INDEX], HIGH);
            phDoseState = PH_COOLDOWN;
            phDoseStartTime = millis();
            Serial.println("Dosing pH selesai, masuk masa cooldown.");
        }
    } else if (phDoseState == PH_COOLDOWN) {
        if (millis() - phDoseStartTime >= config.pump_ph_cooldown_duration_ms) {
            phDoseState = PH_IDLE;
            lastPhDoseTime = millis();
            Serial.println("Cooldown pH selesai.");
        }
    }
}

void startFertilizerDosing() {
    if (fertilizerDoseState != FERTILIZER_IDLE) return;
    fertilizerDoseState = FERTILIZER_MIXING;
    fertilizerDoseStartTime = millis();
    digitalWrite(relayPins[PUMP_A_RELAY_INDEX], LOW);
    digitalWrite(relayPins[PUMP_B_RELAY_INDEX], LOW);
    Serial.println("Mulai dosing pupuk: Tahap Mixing.");
}

void handleFertilizerDosing() {
    if (fertilizerDoseState == FERTILIZER_IDLE) return;
    if (fertilizerDoseState == FERTILIZER_MIXING) {
        if (millis() - fertilizerDoseStartTime >= config.fertilizer_mix_duration_ms) {
            digitalWrite(relayPins[PUMP_A_RELAY_INDEX], HIGH);
            digitalWrite(relayPins[PUMP_B_RELAY_INDEX], HIGH);
            fertilizerDoseState = FERTILIZER_PUSHING;
            fertilizerDoseStartTime = millis();
            digitalWrite(relayPins[PUMP_MIX_RELAY_INDEX], LOW);
            Serial.println("Tahap mixing selesai, lanjut ke tahap push.");
        }
    } else if (fertilizerDoseState == FERTILIZER_PUSHING) {
        if (millis() - fertilizerDoseStartTime >= config.fertilizer_push_duration_ms) {
            digitalWrite(relayPins[PUMP_MIX_RELAY_INDEX], HIGH);
            fertilizerDoseState = FERTILIZER_COOLDOWN;
            fertilizerDoseStartTime = millis();
            Serial.println("Tahap push selesai, masuk masa cooldown.");
        }
    } else if (fertilizerDoseState == FERTILIZER_COOLDOWN) {
        if (millis() - fertilizerDoseStartTime >= config.fertilizer_cooldown_duration_ms) {
            fertilizerDoseState = FERTILIZER_IDLE;
            lastFertilizerDoseTime = millis();
            Serial.println("Cooldown pupuk selesai.");
        }
    }
}

void handleAutomation() {
    if (!isAutomationReady || !config.is_automation_active) return;
    double currentPH = analogSensorService.getCalibratedPHValue() + config.ph_offset;
    double currentTDS = analogSensorService.getCalibratedTDSValue(25.0) + config.tds_offset;
    if (phDoseState == PH_IDLE && millis() - lastPhDoseTime > config.pump_ph_cooldown_duration_ms) {
        if (currentPH < config.ph_target_min && currentPH > 0) {
            startPhDosing(true);
        } else if (currentPH > config.ph_target_max) {
            startPhDosing(false);
        }
    }
    if (fertilizerDoseState == FERTILIZER_IDLE && millis() - lastFertilizerDoseTime > config.fertilizer_cooldown_duration_ms) {
        if (currentTDS < config.tds_target_min && currentTDS > 0) {
            startFertilizerDosing();
        }
    }
}

void sendToSupabase() {
    if (WiFi.status() != WL_CONNECTED || millis() - lastSupabaseSend < config.supabase_send_interval_ms) {
        return;
    }
    lastSupabaseSend = millis();
    double currentPH = analogSensorService.getCalibratedPHValue() + config.ph_offset;
    double currentTDS = analogSensorService.getCalibratedTDSValue(25.0) + config.tds_offset;
    if (currentPH <= 0 || currentTDS < 0) return;

    HTTPClient http;
    http.begin(SUPABASE_URL_SENSORS);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("apikey", SUPABASE_KEY);
    http.addHeader("Authorization", "Bearer " + String(SUPABASE_KEY));
    DynamicJsonDocument doc(256);
    doc["ph_value"] = currentPH;
    doc["tds_value"] = currentTDS;
    doc["device_id"] = "ESP32_HYDGO_01";
    String jsonString;
    serializeJson(doc, jsonString);
    int httpResponseCode = http.POST(jsonString);
    if (httpResponseCode > 0) {
        Serial.print("Data sent to Supabase, response code: ");
        Serial.println(httpResponseCode);
    } else {
        Serial.print("Error sending data to Supabase: ");
        Serial.println(httpResponseCode);
    }
    http.end();
}

void updateDisplay() {
    static unsigned long lastDisplayUpdate = 0;
    if (millis() - lastDisplayUpdate < 500) return;
    lastDisplayUpdate = millis();
    double currentPH = analogSensorService.getCalibratedPHValue() + config.ph_offset;
    double currentTDS = analogSensorService.getCalibratedTDSValue(25.0) + config.tds_offset;
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("pH: ");
    lcd.print(currentPH, 2);
    lcd.setCursor(0, 1);
    lcd.print("TDS: ");
    lcd.print(currentTDS, 0);
    lcd.print(" ppm");
}


// =================================================================================
// --- LOOP UTAMA ---
// Direvisi dengan logika realtime
// =================================================================================
void loop() {
    if (!isAutomationReady && millis() - appStartTime > config.automation_start_delay_ms) {
        isAutomationReady = true;
        Serial.println("Jeda awal selesai. Otomasi sekarang aktif.");
    }
    
    // BARU: Logika pengecekan konfigurasi secara berkala (Realtime)
    if (millis() - lastConfigFetch > config.config_fetch_interval_ms) {
        Serial.println("[REALTIME] Waktunya cek konfigurasi baru dari server...");
        if (fetchConfigurationFromSupabase(config)) {
            Serial.println("[REALTIME] Konfigurasi baru berhasil diterapkan!");
            saveConfigToPreferences(config); // Simpan backup terbaru
            // Terapkan ulang nilai kalibrasi jika berubah
            analogSensorService.setCompensationPH(config.ph_cal_m_value, config.ph_cal_c_value);
        } else {
            Serial.println("[REALTIME] Gagal mengambil konfigurasi baru, lanjut pakai yang lama.");
        }
        lastConfigFetch = millis(); // Reset timer pengecekan
    }

    analogSensorService.update();
    handlePhDosing();
    handleFertilizerDosing();
    handleAutomation();
    sendToSupabase();
    updateDisplay();
}


