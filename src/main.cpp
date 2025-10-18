#include "HEADER.h"

void setup()
{
	Serial.begin(115200);
	Serial.println("\n--- Sistem Monitoring v11.1 (Mode Pameran) ---");
	lcd.init();
	lcd.backlight();
	lcd.setCursor(0, 0);
	lcd.print("Inisialisasi...");
	configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
	timeinfo = updateLocalTime(); // Call it once to populate timeinfo
	lastDay = timeinfo.tm_yday;	  // Set the *initial* boot day

	for (int i = 0; i < NUM_BUTTONS; i++)
		pinMode(buttonPins[i], INPUT_PULLUP);
	for (int i = 0; i < NUM_RELAYS; i++)
	{
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

void loop()
{
	sensorService.update();

	if (currentMode == NORMAL)
	{
		handleNormalMode();
	}
	else if (currentMode == CALIBRATION)
	{
		handleCalibrationMode();
	}

	if (millis() - lastTimeUpdate >= TIME_UPDATE_INTERVAL)
	{
		timeinfo = updateLocalTime();
		// lastDay = timeinfo.tm_yday;
	}

	if (millis() - lastLcdUpdate >= LCD_UPDATE_INTERVAL)
	{
		lastLcdUpdate = millis();
		if (currentMode == NORMAL)
			updateNormalDisplay();
		else
			updateCalibrationDisplay();
	}
}

void loadConfiguration()
{
	preferences.begin("sensor-cfg", true);
	phCalibrationOffset = preferences.getFloat("ph_offset", 0.0);
	tdsCalibrationOffset = preferences.getFloat("tds_offset", 0.0);
	phTargetMin = preferences.getFloat("ph_min", 5.8);
	phTargetMax = preferences.getFloat("ph_max", 6.2);
	tdsTargetMin = preferences.getFloat("tds_min", 700);
	preferences.end();
	Serial.printf("Konfigurasi dimuat: pH Offset=%.2f, TDS Offset=%.0f, pH Target=%.2f-%.2f, TDS Min=%.0f\n",
				  phCalibrationOffset, tdsCalibrationOffset, phTargetMin, phTargetMax, tdsTargetMin);
}

void handleNormalMode()
{
	// Tombol 1 & 2 untuk Pompa Manual
	if (digitalRead(buttonPins[0]) == LOW)
	{
		selectedPumpIndex = (selectedPumpIndex + 1) % NUM_RELAYS;
		delay(200);
	}
	bool pump_on = (digitalRead(buttonPins[1]) == LOW);
	digitalWrite(relayPins[selectedPumpIndex], pump_on ? LOW : HIGH);
	if (!isPhDosing && fertilizerDoseState == FERTILIZER_IDLE)
		relayStates[selectedPumpIndex] = pump_on;

	// Tombol 3: Tahan untuk masuk mode kalibrasi
	if (digitalRead(buttonPins[2]) == LOW)
	{
		if (!buttonHeld)
		{
			buttonPressStartTime = millis();
			buttonHeld = true;
		}
		else if (millis() - buttonPressStartTime > 3000)
		{
			currentMode = CALIBRATION;
			calMode = PH_CAL;
			loadConfiguration();
			Serial.println("Masuk ke Mode Kalibrasi.");
			lcd.clear();
			lcd.print("Mode Konfigurasi");
			delay(1000);
			buttonHeld = false;
			return;
		}
	}
	else
	{
		buttonHeld = false;
	}

	// Proses Latar Belakang
	handlePhDosing();
	handleFertilizerDosing();
	handleAutomationTask();
	if (WiFi.status() == WL_CONNECTED && millis() - lastSupabaseSend >= SUPABASE_SEND_INTERVAL)
	{
		lastSupabaseSend = millis();
		sendToSupabase();
	}
}

void handleCalibrationMode()
{
	// Tombol B4 (Turun) dan B5 (Naik)
	if (digitalRead(buttonPins[3]) == LOW)
	{ // TURUN
		if (calMode == PH_CAL)
			phCalibrationOffset -= phChangeStep;
		else if (calMode == TDS_CAL)
			tdsCalibrationOffset -= tdsChangeStep;
		else if (calMode == PH_THRESH_MIN_CAL)
			phTargetMin -= phChangeStep;
		else if (calMode == PH_THRESH_MAX_CAL)
			phTargetMax -= phChangeStep;
		else if (calMode == TDS_THRESH_MIN_CAL)
			tdsTargetMin -= tdsChangeStep;
		delay(100);
	}
	if (digitalRead(buttonPins[4]) == LOW)
	{ // NAIK
		if (calMode == PH_CAL)
			phCalibrationOffset += phChangeStep;
		else if (calMode == TDS_CAL)
			tdsCalibrationOffset += tdsChangeStep;
		else if (calMode == PH_THRESH_MIN_CAL)
			phTargetMin += phChangeStep;
		else if (calMode == PH_THRESH_MAX_CAL)
			phTargetMax += phChangeStep;
		else if (calMode == TDS_THRESH_MIN_CAL)
			tdsTargetMin += tdsChangeStep;
		delay(100);
	}

	// Tombol B3 (Kontekstual: Klik -> ubah step, Tahan -> simpan & lanjut)
	if (digitalRead(buttonPins[2]) == LOW)
	{
		if (!buttonHeld)
		{
			buttonPressStartTime = millis();
			buttonHeld = true;
		}
		else if (millis() - buttonPressStartTime > 3000)
		{
			// Aksi TAHAN LAMA (SIMPAN & LANJUT)
			if (calMode == PH_CAL)
			{
				calMode = TDS_CAL;
				lcd.clear();
				lcd.print("Lanjut ke TDS..");
				delay(1000);
			}
			else if (calMode == TDS_CAL)
			{
				calMode = PH_THRESH_MIN_CAL;
				lcd.clear();
				lcd.print("Set Batas Min pH");
				delay(1000);
			}
			else if (calMode == PH_THRESH_MIN_CAL)
			{
				calMode = PH_THRESH_MAX_CAL;
				lcd.clear();
				lcd.print("Set Batas Max pH");
				delay(1000);
			}
			else if (calMode == PH_THRESH_MAX_CAL)
			{
				calMode = TDS_THRESH_MIN_CAL;
				lcd.clear();
				lcd.print("Set Batas Min TDS");
				delay(1000);
			}
			else if (calMode == TDS_THRESH_MIN_CAL)
			{
				preferences.begin("sensor-cfg", false);
				preferences.putFloat("ph_offset", phCalibrationOffset);
				preferences.putFloat("tds_offset", tdsCalibrationOffset);
				preferences.putFloat("ph_min", phTargetMin);
				preferences.putFloat("ph_max", phTargetMax);
				preferences.putFloat("tds_min", tdsTargetMin);
				preferences.end();
				Serial.println("Semua konfigurasi disimpan permanen.");
				lcd.clear();
				lcd.print("Tersimpan!");
				delay(1000);
				ESP.restart();
			}
			buttonHeld = false;
		}
	}
	else
	{
		if (buttonHeld)
		{ // Aksi KLIK BIASA (UBAH STEP)
			if (calMode == PH_CAL || calMode == PH_THRESH_MIN_CAL || calMode == PH_THRESH_MAX_CAL)
			{
				phChangeStep = (phChangeStep == 0.01) ? 0.1 : 0.01;
			}
			else if (calMode == TDS_CAL || calMode == TDS_THRESH_MIN_CAL)
			{
				if (tdsChangeStep == 1)
					tdsChangeStep = 10;
				else if (tdsChangeStep == 10)
					tdsChangeStep = 100;
				else
					tdsChangeStep = 1;
			}
		}
		buttonHeld = false;
	}
}

void updateNormalDisplay()
{
	char line1[17], line2[17];

	lastKnownPh = sensorService.getCalibratedPHValue() + phCalibrationOffset;
	lastKnownTds = sensorService.getCalibratedTDSValue(25.0) + tdsCalibrationOffset;

	snprintf(line1, sizeof(line1), "pH:%.2f TDS:%-4.0f",
			 lastKnownPh > 0 ? lastKnownPh : 0.0,
			 lastKnownTds > 0 ? lastKnownTds : 0.0);

	char relayStatusStr[17] = "R:";
	for (int i = 0; i < NUM_RELAYS; i++)
	{
		if (i == selectedPumpIndex && !relayStates[i])
		{
			strcat(relayStatusStr, "[");
			char pumpNum[2] = {(char)('1' + i), '\0'};
			strcat(relayStatusStr, pumpNum);
			strcat(relayStatusStr, "]");
		}
		else
		{
			char status[2] = {relayStates[i] ? (char)('1' + i) : '-', '\0'};
			strcat(relayStatusStr, status);
		}
	}

	char autoStatus[10];
	snprintf(autoStatus, sizeof(autoStatus), " A:%s", (millis() > AUTOMATION_START_DELAY) ? "ON" : "OFF");
	strcat(relayStatusStr, autoStatus);
	snprintf(line2, sizeof(line2), "%s", relayStatusStr);

	lcd.setCursor(0, 0);
	lcd.print(line1);
	lcd.setCursor(0, 1);
	lcd.print(line2);
}

void updateCalibrationDisplay()
{
	char line1[17], line2[17];

	switch (calMode)
	{
	case PH_CAL:
		snprintf(line1, sizeof(line1), "Kalibrasi pH");
		snprintf(line2, sizeof(line2), "Trgt:%.2f(%.2f)", sensorService.getCalibratedPHValue() + phCalibrationOffset, phChangeStep);
		break;
	case TDS_CAL:
		snprintf(line1, sizeof(line1), "Kalibrasi TDS");
		snprintf(line2, sizeof(line2), "Trgt:%.0f(%d)", sensorService.getCalibratedTDSValue(25.0) + tdsCalibrationOffset, tdsChangeStep);
		break;
	case PH_THRESH_MIN_CAL:
		snprintf(line1, sizeof(line1), "Set Batas Min pH");
		snprintf(line2, sizeof(line2), "Nilai: %.2f(%.2f)", phTargetMin, phChangeStep);
		break;
	case PH_THRESH_MAX_CAL:
		snprintf(line1, sizeof(line1), "Set Batas Max pH");
		snprintf(line2, sizeof(line2), "Nilai: %.2f(%.2f)", phTargetMax, phChangeStep);
		break;
	case TDS_THRESH_MIN_CAL:
		snprintf(line1, sizeof(line1), "Set Batas Min TDS");
		snprintf(line2, sizeof(line2), "Nilai: %.0f(%d)", tdsTargetMin, tdsChangeStep);
		break;
	default:
		snprintf(line1, sizeof(line1), "Mode Tidak Dikenal");
		break;
	}

	lcd.setCursor(0, 0);
	lcd.print(line1);
	lcd.setCursor(0, 1);
	lcd.print(line2);
}

void handleAutomationTask()
{
	// Every 2 day, at 9 AM, do watering PH and TDS buffer
	if (timeinfo.tm_hour == 9 && timeinfo.tm_yday - 2 == lastDay && !isWatered)
	{
		if (digitalRead(buttonPins[1]) == LOW) // Right Now Pump is ON by MANUAL OVERRIDE
			return;
		float currentPh = sensorService.getCalibratedPHValue() + phCalibrationOffset;
		float currentTds = sensorService.getCalibratedTDSValue(25.0) + tdsCalibrationOffset;
		if (currentPh <= 0)
			return;
		if (currentPh < phTargetMin)
		{
			startPhDose(PUMP_PH_PLUS_RELAY_INDEX);
		}
		else if (currentPh > phTargetMax)
		{
			startPhDose(PUMP_PH_MINUS_RELAY_INDEX);
		}
		if (currentTds < tdsTargetMin)
		{
			if (fertilizerDoseState == FERTILIZER_IDLE)
				return;
			startFertilizerDose();
		}
		isWatered = true;
		lastDay = timeinfo.tm_yday;
	}
	if (timeinfo.tm_yday == lastDay + 1)
	{
		isWatered = false;
	}
}

// --- FUNGSI-FUNGSI UTILITAS ---

void setupWifi()
{
	String ssid = DEFAULT_WIFI_SSID;
	String password = DEFAULT_WIFI_PASSWORD;
	WiFi.begin(ssid.c_str(), password.c_str());
	Serial.printf("\nMencoba koneksi ke %s...", ssid.c_str());
	lcd.clear();
	lcd.setCursor(0, 0);
	lcd.print("Connect to");
	lcd.setCursor(0, 1);
	lcd.print(ssid);
	for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++)
	{
		delay(500);
		Serial.print(".");
	}
	if (WiFi.status() == WL_CONNECTED)
	{
		Serial.println("\nTerhubung!");
		Serial.print("Alamat IP: ");
		Serial.println(WiFi.localIP());
		lcd.clear();
		lcd.print("WiFi Terhubung!");
		lcd.setCursor(0, 1);
		lcd.print(WiFi.localIP());
		delay(2000);
	}
	else
	{
		Serial.println("\nWiFi Gagal!");
		lcd.clear();
		lcd.print("WiFi Gagal!");
	}
}

void sendToSupabase()
{
	if (lastKnownPh < 0 || lastKnownTds < 0)
		return;
	JsonDocument doc;
	doc["ph"] = lastKnownPh;
	doc["tds"] = (int)lastKnownTds;
	doc["ph_auto"] = autoDoseCount;
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
	if (httpResponseCode == 201)
		Serial.println("Data berhasil dikirim ke Supabase!");
	else
	{
		Serial.print("Gagal mengirim data. Kode HTTP: ");
		Serial.println(httpResponseCode);
	}
	http.end();
}

void startPhDose(int relayIndex)
{
	if (isPhDosing)
		return;
	isPhDosing = true;
	phDosingRelayIndex = relayIndex;
	phDoseStartTime = millis();
	digitalWrite(relayPins[phDosingRelayIndex], LOW);
	relayStates[phDosingRelayIndex] = true;
	Serial.printf("Mulai dosing pH untuk relay %d\n", phDosingRelayIndex + 1);
	autoDoseCount++;
}

void handlePhDosing()
{
	if (!isPhDosing)
		return;
	if (millis() - phDoseStartTime >= PUMP_PH_ON_DURATION)
	{
		digitalWrite(relayPins[phDosingRelayIndex], HIGH);
		relayStates[phDosingRelayIndex] = false;
		Serial.printf("Dosing pH selesai untuk relay %d\n", phDosingRelayIndex + 1);
		isPhDosing = false;
		phDosingRelayIndex = -1;
		lastPhDoseTime = millis();
	}
}

void startFertilizerDose()
{
	if (fertilizerDoseState != FERTILIZER_IDLE)
		return;
	Serial.println("Memulai sekuens dosing nutrisi...");
	fertilizerDoseState = FERTILIZER_MIXING;
	fertilizerDoseStartTime = millis();
	digitalWrite(relayPins[PUMP_A_RELAY_INDEX], LOW);
	digitalWrite(relayPins[PUMP_B_RELAY_INDEX], LOW);
	relayStates[PUMP_A_RELAY_INDEX] = true;
	relayStates[PUMP_B_RELAY_INDEX] = true;
}

void handleFertilizerDosing()
{
	if (fertilizerDoseState == FERTILIZER_IDLE)
		return;

	if (fertilizerDoseState == FERTILIZER_MIXING)
	{
		if (millis() - fertilizerDoseStartTime >= FERTILIZER_MIX_DURATION)
		{
			Serial.println("Tahap mixing selesai, lanjut ke tahap push.");
			digitalWrite(relayPins[PUMP_A_RELAY_INDEX], HIGH);
			digitalWrite(relayPins[PUMP_B_RELAY_INDEX], HIGH);
			relayStates[PUMP_A_RELAY_INDEX] = false;
			relayStates[PUMP_B_RELAY_INDEX] = false;

			fertilizerDoseState = FERTILIZER_PUSHING;
			fertilizerDoseStartTime = millis();
			digitalWrite(relayPins[PUMP_MIX_RELAY_INDEX], LOW);
			relayStates[PUMP_MIX_RELAY_INDEX] = true;
		}
	}
	else if (fertilizerDoseState == FERTILIZER_PUSHING)
	{
		if (millis() - fertilizerDoseStartTime >= FERTILIZER_PUSH_DURATION)
		{
			Serial.println("Sekuens dosing nutrisi selesai.");
			digitalWrite(relayPins[PUMP_MIX_RELAY_INDEX], HIGH);
			relayStates[PUMP_MIX_RELAY_INDEX] = false;

			fertilizerDoseState = FERTILIZER_IDLE;
			lastFertilizerDoseTime = millis();
		}
	}
}
