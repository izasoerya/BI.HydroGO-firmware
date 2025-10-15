#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#include <LiquidCrystal_I2C.h>
#include <Preferences.h>
#include "AnalogSensorService.h"

// --- KONFIGURASI JARINGAN ---
const char *DEFAULT_WIFI_SSID = "hydrogoo";
const char *DEFAULT_WIFI_PASSWORD = "hydrogoo";
const char *SUPABASE_URL_SENSORS = "https://ntudiforfsotyqdufhxu.supabase.co/rest/v1/sensor_logs";
const char *SUPABASE_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Im50dWRpZm9yZnNvdHlxZHVmaHh1Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3NTkxMjUzMTksImV4cCI6MjA3NDcwMTMxOX0.nSlLo-F6fUs-5hnqq2lt3zk8OU1wRnIjjCvBEMsqe1Y";
const unsigned long SUPABASE_SEND_INTERVAL = 10000;

// --- KONFIGURASI UMUM & PIN ---
const int LCD_ADDRESS = 0x27;
const int LCD_COLS = 16;
const int LCD_ROWS = 2;
const int SENSOR_5V_RELAY_PIN = 16;
const int SENSOR_GND_RELAY_PIN = 19;
const int relayPins[] = {13, 27, 26, 25, 33}; // R1, R2, R3, R4, R5
const int NUM_RELAYS = 5;
const int buttonPins[] = {32, 35, 34, 39, 36}; // B1, B2, B3, B4, B5
const int NUM_BUTTONS = 5;

// --- KONFIGURASI OTOMASI ---
// Indeks Relay (0-4) -> Pompa (1-5)
const int PUMP_MIX_RELAY_INDEX = 0;      // Pompa 1: Pendorong Mix
const int PUMP_A_RELAY_INDEX = 1;        // Pompa 2: Pupuk A
const int PUMP_B_RELAY_INDEX = 2;        // Pompa 3: Pupuk B
const int PUMP_PH_MINUS_RELAY_INDEX = 3; // Pompa 4: pH Down
const int PUMP_PH_PLUS_RELAY_INDEX = 4;  // Pompa 5: pH Up

const unsigned long AUTOMATION_START_DELAY = 180000; // 3 menit
const unsigned long LCD_UPDATE_INTERVAL = 500;       // 0.5 second
const unsigned long TIME_UPDATE_INTERVAL = 5000;     // 5 second

// --- PENGATURAN UNTUK DEMO PAMERAN ---
// Pengaturan Dosing pH
const unsigned long PUMP_PH_ON_DURATION = 2000;        // 2 detik
const unsigned long PUMP_PH_COOLDOWN_DURATION = 15000; // DEMO: 15 detik (Normal: 30000)
const bool AUTOMATION_DO_WHILE = false;                // Immediately ON is disabled (false)
const unsigned long AUTOMATION_TRIGGER = 9;            // 9 AM
const unsigned long AUTOMATION_DELAY_DAY = 2;          // every 2 day
const unsigned long THRESHOLD_PH = 8;                  // Set threshold PH
const unsigned long THRESHOLD_TDS = 1200;              // Set threshold TDS

// Pengaturan Dosing Pupuk
const unsigned long FERTILIZER_MIX_DURATION = 5000;       // 5 detik Pompa A & B nyala
const unsigned long FERTILIZER_PUSH_DURATION = 3000;      // 3 detik Pompa Mix nyala
const unsigned long FERTILIZER_COOLDOWN_DURATION = 45000; // DEMO: 45 detik (Normal: 600000)

// --- ENUM & STATE UNTUK MODE OPERASI ---
enum OperatingMode
{
    NORMAL,
    CALIBRATION
};
OperatingMode currentMode = NORMAL;

enum CalibrationMode
{
    NONE,
    PH_CAL,
    TDS_CAL,
    PH_THRESH_MIN_CAL,
    PH_THRESH_MAX_CAL,
    TDS_THRESH_MIN_CAL
};
CalibrationMode calMode = NONE;

enum FertilizerDoseState
{
    FERTILIZER_IDLE,
    FERTILIZER_MIXING,
    FERTILIZER_PUSHING
};
FertilizerDoseState fertilizerDoseState = FERTILIZER_IDLE;

// Variabel state & konfigurasi
int selectedPumpIndex = 0;
unsigned long buttonPressStartTime = 0;
bool buttonHeld = false;
float phCalibrationOffset = 0.0;
float tdsCalibrationOffset = 0.0;
float phChangeStep = 0.01;
int tdsChangeStep = 1;
unsigned long lastSupabaseSend = 0;
unsigned long lastPhDoseTime = 0;
unsigned long lastFertilizerDoseTime = 0;
unsigned int autoDoseCount = 0;
unsigned long lastLcdUpdate = 0;
unsigned long lastTimeUpdate = 0;
float lastKnownPh = -1.0;
float lastKnownTds = -1.0;
bool isPhDosing = false;
unsigned long phDoseStartTime = 0;
int phDosingRelayIndex = -1;
unsigned long fertilizerDoseStartTime = 0;
bool relayStates[NUM_RELAYS] = {false};
float phTargetMin = 5.8;
float phTargetMax = 6.2;
float tdsTargetMin = 700;
const char *ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 7;
const int daylightOffset_sec = 3600;
struct tm timeinfo;
byte lastDay = 0;
byte isWatered = false;

// --- FUNGSI PROTOTIPE ---
void loadConfiguration();
void handleNormalMode();
void handleCalibrationMode();
void updateNormalDisplay();
void updateCalibrationDisplay();
void setupWifi();
void sendToSupabase();
void handleAutomationTask();
void startPhDose(int relayIndex);
void handlePhDosing();
void startFertilizerDose();
void handleFertilizerDosing();
tm updateLocalTime()
{
    tm timeinfo;
    if (!getLocalTime(&timeinfo))
    {
        Serial.println("Failed to obtain time");
    }
    Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");
    Serial.print("Day of week: ");
    Serial.println(&timeinfo, "%A");
    Serial.print("Month: ");
    Serial.println(&timeinfo, "%B");
    Serial.print("Day of Month: ");
    Serial.println(&timeinfo, "%d");
    Serial.print("Year: ");
    Serial.println(&timeinfo, "%Y");
    Serial.print("Hour: ");
    Serial.println(&timeinfo, "%H");
    Serial.print("Hour (12 hour format): ");
    Serial.println(&timeinfo, "%I");
    Serial.print("Minute: ");
    Serial.println(&timeinfo, "%M");
    Serial.print("Second: ");
    Serial.println(&timeinfo, "%S");
    return timeinfo;
}

// --- OBJEK & VARIABEL GLOBAL ---
LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_COLS, LCD_ROWS);
AnalogSensorService sensorService(SENSOR_5V_RELAY_PIN, SENSOR_GND_RELAY_PIN);
Preferences preferences;
