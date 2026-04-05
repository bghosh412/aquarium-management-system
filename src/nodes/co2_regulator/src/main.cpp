#include <Arduino.h>
#ifdef ESP8266
    #include <ESP8266WiFi.h>
#else
    #include <WiFi.h>
#endif
#include <LittleFS.h>
#include "protocol/messages.h"
#include "ESPNowManager.h"
#include "node_base.h"

// ============================================================================
// CO2 REGULATOR NODE - pH-based CO2 injection control
// ============================================================================
// Platform: ESP8266 D1 Mini (Wemos)
// Sensor:   DFRobot Gravity Analog pH Sensor (BNC probe -> sensor board -> A0)
// Actuator: Relay module (HIGH = CO2 ON, LOW = CO2 OFF)
// Fail-safe: CO2 OFF (CRITICAL - prevent CO2 overdose which is lethal to fish)
//
// Architecture:
//   - Node reads pH periodically and reports to hub
//   - Hub analyzes pH TREND (not absolute) over configurable time window
//   - Hub sends ON/OFF commands to control CO2 solenoid via relay
//   - Node has local safety cutoff if pH drops below emergency threshold
//
// Pin Configuration (configurable via /node_config.txt):
//   A0    = pH sensor analog input (ESP8266 only has A0 for ADC)
//   D0/16 = CO2 relay output (HIGH = ON, LOW = OFF)
//   D5/14 = Status LED
//
// Command Format from Hub:
//   data[0] = command type:
//     0  = Emergency stop (CO2 OFF immediately)
//     1  = CO2 ON (relay HIGH)
//     2  = CO2 OFF (relay LOW)
//     40 = Status request (read pH and report)
//     41 = Calibrate: data[1..4] = actual pH (float); node computes voltage
//          trim and persists PH_CALIB_VOLT_TRIM to /node_config.txt
//     0xFF = Force fail-safe (test)
//
// Status Data Format (sent to hub):
//   statusData[0]   = relay state (0=OFF, 1=ON)
//   statusData[1-4] = pH value (float, little-endian)
//   statusData[5-6] = raw ADC value (uint16_t, little-endian)
//   statusData[7]   = error/flags (0=OK, 1=sensor_error, 2=safety_cutoff)
// ============================================================================

// ============================================================================
// CO2-SPECIFIC CONFIGURATION (loaded from /node_config.txt)
// ============================================================================
// These settings are parsed locally (not in NodeBase) to keep shared code clean.

struct CO2Config {
    uint8_t  phSensorPin;          // Analog pin for pH probe (A0 on ESP8266)
    uint8_t  relayPin;             // Digital pin for CO2 relay
    uint32_t phReadIntervalMs;     // How often to do burst-read pH (command 40 fallback)
    uint8_t  phSampleCount;        // Number of ADC samples per burst reading
    uint8_t  phDiscardCount;       // Samples to discard from each end (outliers)
    float    phCalibOffset;        // pH calibration offset (pH = slope * V + offset)
    float    phCalibSlope;         // pH calibration slope
    float    phCalibVoltTrim;      // Voltage trim: applied as V_corr = V_raw + trim
                                   // Set by hub cmd 41 (calibrate with known actual pH)
    float    phSafetyLow;          // Emergency low pH limit (force CO2 OFF below this)
    float    phSafetyHigh;         // Warning high pH limit (informational)
    float    adcVoltageRef;        // ADC reference voltage (3.3V for D1 Mini)
    uint16_t adcMaxValue;          // ADC max reading (1024 for ESP8266)

    // Pipeline config: continuous median-filtered pH reading
    uint32_t phADCReadIntervalMs;  // How often to read A0 (default 200ms)
    uint8_t  phSamplesPerMedian;   // Readings per short-term median (default 20)
    uint32_t phTrendWindowSec;     // Trend window in seconds (default 300s)
};

static CO2Config co2Cfg = {
    .phSensorPin      = A0,       // Only ADC pin on ESP8266
    .relayPin         = 16,       // D0 on D1 Mini (GPIO16)
    .phReadIntervalMs = 300000,   // 5 minutes (burst-read fallback for cmd 40)
    .phSampleCount    = 10,       // 10 samples per burst reading
    .phDiscardCount   = 2,        // Discard 2 highest + 2 lowest
    .phCalibOffset    = -5.0f,    // Regulator board: pH = 6.0 * V - 5.0
    .phCalibSlope     = 6.0f,     // Regulator board: pH4=1.5V, pH7=2.0V, pH9=2.5V
    .phCalibVoltTrim  = 0.0f,     // Voltage trim (updated by hub cmd 41 / node_config.txt)
    .phSafetyLow      = 5.0f,     // Emergency: force CO2 OFF below pH 5.0
    .phSafetyHigh     = 8.5f,     // Warning above pH 8.5
    .adcVoltageRef    = 3.3f,     // D1 Mini voltage divider gives 0-3.3V range
    .adcMaxValue      = 1024,     // ESP8266 10-bit ADC

    // Pipeline defaults
    .phADCReadIntervalMs = 200,   // Read A0 every 200ms
    .phSamplesPerMedian  = 20,    // 20 readings per short-term median
    .phTrendWindowSec    = 300    // Trend window: 300 seconds (5 min)
};

// ============================================================================
// CO2 STATE
// ============================================================================

enum class CO2State : uint8_t {
    IDLE,               // Relay OFF, waiting for commands
    CO2_ON,             // Relay ON, CO2 injecting
    SAFETY_CUTOFF,      // Local safety triggered - CO2 forced OFF
    FAILSAFE            // Hub communication lost - CO2 forced OFF
};

struct CO2StateData {
    CO2State state;
    bool     relayOn;            // Current relay state
    float    lastPH;             // Last pH reading
    uint16_t lastRawADC;         // Last raw ADC value
    uint32_t lastPHReadTime;     // millis() of last pH reading
    uint8_t  errorFlags;         // 0=OK, 1=sensor_error, 2=safety_cutoff
    bool     sensorError;        // pH reading out of plausible range
    uint8_t  lastCommandId;      // Last processed command ID
};

static CO2StateData co2State = {
    .state           = CO2State::IDLE,
    .relayOn         = false,
    .lastPH          = 0.0f,
    .lastRawADC      = 0,
    .lastPHReadTime  = 0,
    .errorFlags      = 0,
    .sensorError     = false,
    .lastCommandId   = 0
};

// ============================================================================
// pH PIPELINE - Continuous median-filtered reading
// ============================================================================
// Pipeline: A0 read every 200ms → buffer 20 → short-term median → buffer
//           medians over 300s → trend median (for hub reporting)
//
// Max medians in 300s window: 300s / (20 * 0.2s) = 75 medians. Buffer = 80.
// ============================================================================

#define PH_PIPELINE_MAX_SAMPLES   20   // Max samples per median batch
#define PH_PIPELINE_MAX_MEDIANS   80   // Max medians in trend window

struct PHPipeline {
    // --- Raw reading buffer (filled every phADCReadIntervalMs) ---
    float    rawReadings[PH_PIPELINE_MAX_SAMPLES];
    uint8_t  rawCount;              // Readings collected in current batch (0..samplesPerMedian)
    uint32_t lastADCReadTime;       // millis() of last ADC read

    // --- Short-term median buffer (accumulates over trend window) ---
    float    medianValues[PH_PIPELINE_MAX_MEDIANS];
    uint8_t  medianCount;           // Medians collected in current trend window
    uint32_t trendStartTime;        // millis() when current trend window started

    // --- Results ---
    float    lastShortMedian;       // Latest short-term median (20 readings)
    float    lastTrendMedian;       // Latest trend median (300s of medians)
    bool     hasTrendMedian;        // True when at least one trend median computed
};

static PHPipeline phPipeline = {
    .rawReadings     = {0},
    .rawCount        = 0,
    .lastADCReadTime = 0,
    .medianValues    = {0},
    .medianCount     = 0,
    .trendStartTime  = 0,
    .lastShortMedian = 0.0f,
    .lastTrendMedian = 0.0f,
    .hasTrendMedian  = false
};

// ============================================================================
// CO2-SPECIFIC CONFIG LOADING
// ============================================================================
// Parses CO2-specific keys from /node_config.txt (called AFTER loadNodeConfiguration)

static void loadCO2Config() {
    if (!LittleFS.exists("/node_config.txt")) return;

    File file = LittleFS.open("/node_config.txt", "r");
    if (!file) return;

    while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.startsWith("#") || line.length() == 0) continue;

        int sep = line.indexOf('=');
        if (sep == -1) continue;

        String key   = line.substring(0, sep);
        String value = line.substring(sep + 1);
        key.trim();
        value.trim();

        if (key == "PH_SENSOR_PIN") {
            co2Cfg.phSensorPin = (uint8_t)value.toInt();
        } else if (key == "RELAY_PIN") {
            co2Cfg.relayPin = (uint8_t)value.toInt();
        } else if (key == "PH_READ_INTERVAL_MS") {
            co2Cfg.phReadIntervalMs = (uint32_t)value.toInt();
        } else if (key == "PH_SAMPLE_COUNT") {
            uint8_t val = (uint8_t)value.toInt();
            if (val >= 3 && val <= 50) co2Cfg.phSampleCount = val;
        } else if (key == "PH_DISCARD_COUNT") {
            co2Cfg.phDiscardCount = (uint8_t)value.toInt();
        } else if (key == "PH_CALIB_OFFSET") {
            co2Cfg.phCalibOffset = value.toFloat();
        } else if (key == "PH_CALIB_SLOPE") {
            co2Cfg.phCalibSlope = value.toFloat();
        } else if (key == "PH_CALIB_VOLT_TRIM") {
            co2Cfg.phCalibVoltTrim = value.toFloat();
        } else if (key == "PH_SAFETY_LOW") {
            co2Cfg.phSafetyLow = value.toFloat();
        } else if (key == "PH_SAFETY_HIGH") {
            co2Cfg.phSafetyHigh = value.toFloat();
        } else if (key == "ADC_VOLTAGE_REF") {
            co2Cfg.adcVoltageRef = value.toFloat();
        } else if (key == "ADC_MAX_VALUE") {
            co2Cfg.adcMaxValue = (uint16_t)value.toInt();
        } else if (key == "PH_ADC_READ_INTERVAL_MS") {
            uint32_t val = (uint32_t)value.toInt();
            if (val >= 50 && val <= 10000) co2Cfg.phADCReadIntervalMs = val;
        } else if (key == "PH_SAMPLES_PER_MEDIAN") {
            uint8_t val = (uint8_t)value.toInt();
            if (val >= 3 && val <= PH_PIPELINE_MAX_SAMPLES) co2Cfg.phSamplesPerMedian = val;
        } else if (key == "PH_TREND_WINDOW_SEC") {
            uint32_t val = (uint32_t)value.toInt();
            if (val >= 10 && val <= 3600) co2Cfg.phTrendWindowSec = val;
        }
    }
    file.close();

    // Validate discard count vs sample count
    if (co2Cfg.phDiscardCount * 2 >= co2Cfg.phSampleCount) {
        co2Cfg.phDiscardCount = 0;  // Disable discarding if invalid
    }

    if (nodeConfig.debugSerial) {
        Serial.println("[CO2] Configuration loaded:");
        Serial.printf("   pH Sensor Pin: %d (A0=%d)\n", co2Cfg.phSensorPin, A0);
        Serial.printf("   Relay Pin: GPIO%d\n", co2Cfg.relayPin);
        Serial.printf("   Read Interval: %u ms (%u min)\n",
                      co2Cfg.phReadIntervalMs, co2Cfg.phReadIntervalMs / 60000);
        Serial.printf("   Samples: %d (discard %d from each end)\n",
                      co2Cfg.phSampleCount, co2Cfg.phDiscardCount);
        Serial.printf("   Calibration: pH = %.2f * (V + %.4f) + (%.2f)\n",
                      co2Cfg.phCalibSlope, co2Cfg.phCalibVoltTrim, co2Cfg.phCalibOffset);
        Serial.printf("   Safety: LOW=%.1f  HIGH=%.1f\n",
                      co2Cfg.phSafetyLow, co2Cfg.phSafetyHigh);
        Serial.printf("   Pipeline: read every %ums, %d samples/median, %us trend window\n",
                      co2Cfg.phADCReadIntervalMs, co2Cfg.phSamplesPerMedian,
                      co2Cfg.phTrendWindowSec);
    }
}

// ============================================================================
// CO2 CALIBRATION TRIM PERSISTENCE
// ============================================================================

/**
 * @brief Persist the current phCalibVoltTrim to /node_config.txt in LittleFS.
 *
 * Reads the entire config file line by line. Replaces the
 * PH_CALIB_VOLT_TRIM=... line in-place, or appends it if the key is not
 * yet present. Writes the modified content back to the same file.
 *
 * @return true on success, false on any file I/O error.
 */
static bool saveCO2CalibTrim() {
    const char* configPath = "/node_config.txt";

    if (!LittleFS.exists(configPath)) {
        Serial.println("[CALIB] Cannot save trim: /node_config.txt not found");
        return false;
    }

    File readFile = LittleFS.open(configPath, "r");
    if (!readFile) {
        Serial.println("[CALIB] Cannot open config for reading");
        return false;
    }

    String newContent;
    newContent.reserve(readFile.size() + 40);
    bool trimLineFound = false;

    while (readFile.available()) {
        String line = readFile.readStringUntil('\n');
        // Strip trailing CR (Windows line endings)
        if (line.endsWith("\r")) line.remove(line.length() - 1);

        if (line.startsWith("PH_CALIB_VOLT_TRIM=")) {
            char buf[40];
            snprintf(buf, sizeof(buf), "PH_CALIB_VOLT_TRIM=%.4f", co2Cfg.phCalibVoltTrim);
            newContent += buf;
            trimLineFound = true;
        } else {
            newContent += line;
        }
        newContent += '\n';
    }
    readFile.close();

    // Key not present in file yet - append it
    if (!trimLineFound) {
        char buf[40];
        snprintf(buf, sizeof(buf), "PH_CALIB_VOLT_TRIM=%.4f\n", co2Cfg.phCalibVoltTrim);
        newContent += buf;
    }

    File writeFile = LittleFS.open(configPath, "w");
    if (!writeFile) {
        Serial.println("[CALIB] Cannot open config for writing");
        return false;
    }

    size_t written = writeFile.print(newContent);
    writeFile.close();

    bool ok = (written == newContent.length());
    if (ok) {
        Serial.printf("[CALIB] Trim saved: PH_CALIB_VOLT_TRIM=%.4f\n",
                      co2Cfg.phCalibVoltTrim);
    } else {
        Serial.printf("[CALIB] Warning: partial write (%zu of %u bytes)\n",
                      written, newContent.length());
    }
    return ok;
}

// ============================================================================
// pH SENSOR READING (with noise filtering)
// ============================================================================

/**
 * @brief Simple bubble sort for small integer array (used for median filtering)
 */
static void sortArray(int* arr, uint8_t len) {
    for (uint8_t i = 0; i < len - 1; i++) {
        for (uint8_t j = 0; j < len - i - 1; j++) {
            if (arr[j] > arr[j + 1]) {
                int temp = arr[j];
                arr[j]   = arr[j + 1];
                arr[j + 1] = temp;
            }
        }
    }
}

/**
 * @brief Sort a float array in-place (bubble sort, fine for small arrays)
 */
static void sortFloatArray(float* arr, uint8_t len) {
    for (uint8_t i = 0; i < len - 1; i++) {
        for (uint8_t j = 0; j < len - i - 1; j++) {
            if (arr[j] > arr[j + 1]) {
                float temp = arr[j];
                arr[j]     = arr[j + 1];
                arr[j + 1] = temp;
            }
        }
    }
}

/**
 * @brief Compute median of a float array (non-destructive - copies first)
 * @param arr Source array
 * @param len Number of elements (must be > 0)
 * @return Median value
 */
static float computeMedianFloat(const float* arr, uint8_t len) {
    if (len == 0) return 0.0f;
    if (len == 1) return arr[0];

    // Copy to temp buffer so we don't modify the original
    float temp[PH_PIPELINE_MAX_MEDIANS];  // Max size we ever need
    uint8_t copyLen = (len <= PH_PIPELINE_MAX_MEDIANS) ? len : PH_PIPELINE_MAX_MEDIANS;
    memcpy(temp, arr, copyLen * sizeof(float));

    sortFloatArray(temp, copyLen);

    if (copyLen % 2 == 1) {
        return temp[copyLen / 2];
    } else {
        return (temp[copyLen / 2 - 1] + temp[copyLen / 2]) / 2.0f;
    }
}

/**
 * @brief Convert raw ADC value to pH using calibration constants
 * @param adcValue Raw ADC reading (0-1024)
 * @return pH value, or -1.0 if implausible
 */
static float adcToPH(int adcValue) {
    float voltage = (float)adcValue * co2Cfg.adcVoltageRef / (float)co2Cfg.adcMaxValue;
    float correctedVoltage = voltage + co2Cfg.phCalibVoltTrim;  // Apply calibration trim
    float pH = co2Cfg.phCalibSlope * correctedVoltage + co2Cfg.phCalibOffset;

    // Plausibility check
    if (pH < 0.0f || pH > 14.0f) {
        return -1.0f;
    }
    return pH;
}

// ============================================================================
// pH PIPELINE TICK - Called every loop iteration (non-blocking)
// ============================================================================

// Forward declarations (defined later in file)
static void checkLocalSafety();
static void sendPeriodicStatus();

/**
 * @brief Continuous pH reading pipeline
 *
 * Called from updateHardware() every loop(). Uses millis() for non-blocking
 * timing. Reads A0 every phADCReadIntervalMs, computes short-term median
 * every phSamplesPerMedian readings, and trend median every phTrendWindowSec.
 */
static void phPipelineTick() {
    uint32_t now = millis();

    // --- Initialize trend window on first call ---
    if (phPipeline.trendStartTime == 0) {
        phPipeline.trendStartTime = now;
    }

    // --- Step 1: Read A0 at configured interval (default 200ms) ---
    if (now - phPipeline.lastADCReadTime < co2Cfg.phADCReadIntervalMs) {
        return;  // Not time yet
    }
    phPipeline.lastADCReadTime = now;

    // Read raw ADC and convert to pH
    int rawADC = analogRead(co2Cfg.phSensorPin);
    float pH = adcToPH(rawADC);

    if (pH < 0.0f) {
        // Implausible reading - skip this sample
        co2State.sensorError = true;
        co2State.errorFlags  = 1;
        // Always log implausible readings (throttled to every 5s to avoid spam)
        static uint32_t lastImplausibleLog = 0;
        if (now - lastImplausibleLog >= 5000) {
            lastImplausibleLog = now;
            float voltage = (float)rawADC * co2Cfg.adcVoltageRef / (float)co2Cfg.adcMaxValue;
            float rawPH = co2Cfg.phCalibSlope * (voltage + co2Cfg.phCalibVoltTrim) + co2Cfg.phCalibOffset;
            Serial.printf("[PH] WARN: implausible ADC=%d V=%.3f pH=%.2f (out of 0-14 range), skipping\n",
                          rawADC, voltage, rawPH);
        }
        return;
    }

    co2State.sensorError = false;
    if (co2State.errorFlags == 1) co2State.errorFlags = 0;

    // Store the latest raw ADC for status reporting
    co2State.lastRawADC = (uint16_t)rawADC;

    // --- Step 2: Store in raw buffer ---
    if (phPipeline.rawCount < co2Cfg.phSamplesPerMedian) {
        phPipeline.rawReadings[phPipeline.rawCount] = pH;
        phPipeline.rawCount++;
    }

    // --- Step 3: When batch complete, compute short-term median ---
    if (phPipeline.rawCount >= co2Cfg.phSamplesPerMedian) {
        float shortMedian = computeMedianFloat(phPipeline.rawReadings, phPipeline.rawCount);
        phPipeline.lastShortMedian = shortMedian;

        // Update co2State with latest short-term median
        co2State.lastPH         = shortMedian;
        co2State.lastPHReadTime = now;

        // Print short-term median (trim shows the active calibration voltage offset)
        float batchTimeSec = (float)(co2Cfg.phADCReadIntervalMs * co2Cfg.phSamplesPerMedian) / 1000.0f;
        Serial.printf("[PH] Short-term median: %.2f (trim=%.4f, %d samples over %.1fs)\n",
                      shortMedian, co2Cfg.phCalibVoltTrim, phPipeline.rawCount, batchTimeSec);

        // Check local safety limits against short-term median
        checkLocalSafety();

        // --- Step 4: Store median in trend buffer ---
        if (phPipeline.medianCount < PH_PIPELINE_MAX_MEDIANS) {
            phPipeline.medianValues[phPipeline.medianCount] = shortMedian;
            phPipeline.medianCount++;
        }

        // Reset raw buffer for next batch
        phPipeline.rawCount = 0;
    }

    // --- Step 5: When trend window elapsed, compute trend median ---
    uint32_t trendWindowMs = co2Cfg.phTrendWindowSec * 1000UL;
    if (now - phPipeline.trendStartTime >= trendWindowMs && phPipeline.medianCount > 0) {
        float trendMedian = computeMedianFloat(phPipeline.medianValues, phPipeline.medianCount);
        phPipeline.lastTrendMedian = trendMedian;
        phPipeline.hasTrendMedian  = true;

        // Update co2State with trend median (this is the value for hub reporting)
        co2State.lastPH = trendMedian;

        Serial.printf("[PH] Trend median: %.2f (%d medians over %us)\n",
                      trendMedian, phPipeline.medianCount, co2Cfg.phTrendWindowSec);

        // Send periodic status to hub if connected
        if (isConnectedToHub) {
            sendPeriodicStatus();
        }

        // Reset trend window
        phPipeline.medianCount     = 0;
        phPipeline.trendStartTime  = now;
    }
}

/**
 * @brief Read pH value from analog sensor with noise filtering
 *
 * Takes multiple samples, sorts them, discards outliers (highest & lowest),
 * averages remaining samples, converts to voltage, then to pH using
 * calibration constants.
 *
 * @return pH value (float). Returns -1.0 on sensor error.
 */
static float readPH() {
    const uint8_t totalSamples = co2Cfg.phSampleCount;
    const uint8_t discardEach  = co2Cfg.phDiscardCount;

    // Stack-allocate sample buffer (max 50 samples = 100 bytes)
    int readings[50];
    uint8_t count = (totalSamples <= 50) ? totalSamples : 50;

    // Collect ADC samples with brief settling delay
    for (uint8_t i = 0; i < count; i++) {
        readings[i] = analogRead(co2Cfg.phSensorPin);
        delay(2);  // 2ms settling time between reads (~20ms total for 10 samples)
    }

    // Sort for outlier removal
    sortArray(readings, count);

    // Average the middle samples (discard extremes)
    uint8_t start = discardEach;
    uint8_t end   = count - discardEach;
    if (start >= end) {
        // Fallback: use all samples if discard config is invalid
        start = 0;
        end   = count;
    }

    float sum = 0;
    for (uint8_t i = start; i < end; i++) {
        sum += readings[i];
    }
    float avgADC = sum / (float)(end - start);

    // Store raw ADC value
    co2State.lastRawADC = (uint16_t)(avgADC + 0.5f);

    // Convert ADC to voltage and apply trim
    float voltage = avgADC * co2Cfg.adcVoltageRef / (float)co2Cfg.adcMaxValue;
    float correctedVoltage = voltage + co2Cfg.phCalibVoltTrim;  // Apply calibration trim

    // Convert corrected voltage to pH using calibration: pH = slope * (V + trim) + offset
    float pH = co2Cfg.phCalibSlope * correctedVoltage + co2Cfg.phCalibOffset;

    // Plausibility check (pH must be 0-14)
    if (pH < 0.0f || pH > 14.0f) {
        co2State.sensorError = true;
        co2State.errorFlags  = 1;  // Sensor error
        if (nodeConfig.debugHardware) {
            Serial.printf("[PH] ERROR: Implausible pH=%.2f (V=%.3f, ADC=%d)\n",
                          pH, voltage, co2State.lastRawADC);
        }
        return -1.0f;
    }

    co2State.sensorError = false;
    if (co2State.errorFlags == 1) co2State.errorFlags = 0;  // Clear sensor error

    if (nodeConfig.debugHardware) {
        Serial.printf("[PH] Reading: pH=%.2f (V=%.3fV, ADC=%d, samples=%d)\n",
                      pH, voltage, co2State.lastRawADC, end - start);
    }

    return pH;
}

// ============================================================================
// RELAY CONTROL
// ============================================================================

/**
 * @brief Set CO2 relay state
 * @param on true = relay ON (CO2 injecting), false = relay OFF
 */
static void setRelay(bool on) {
    digitalWrite(co2Cfg.relayPin, on ? HIGH : LOW);
    co2State.relayOn = on;

    if (nodeConfig.debugHardware || nodeConfig.debugSerial) {
        Serial.printf("[CO2] Relay %s (GPIO%d = %s)\n",
                      on ? "ON" : "OFF", co2Cfg.relayPin, on ? "HIGH" : "LOW");
    }
}

/**
 * @brief Emergency CO2 shutoff - forces relay OFF and updates state
 * @param reason Error flag value (2=safety_cutoff, 0=normal)
 */
static void emergencyCO2Off(uint8_t reason) {
    setRelay(false);
    co2State.errorFlags = reason;

    if (reason == 2) {
        co2State.state = CO2State::SAFETY_CUTOFF;
        Serial.println("[CO2] *** SAFETY CUTOFF - pH below emergency threshold ***");
    }
}

// ============================================================================
// STATUS REPORTING
// ============================================================================

/**
 * @brief Fill status data array with current CO2 state
 * @param statusData Array to populate (min 8 bytes)
 */
static void fillStatusData(uint8_t* statusData) {
    // Byte 0: Relay state
    statusData[0] = co2State.relayOn ? 1 : 0;

    // Bytes 1-4: pH value as float (little-endian)
    memcpy(&statusData[1], &co2State.lastPH, sizeof(float));

    // Bytes 5-6: Raw ADC value (uint16_t, little-endian)
    memcpy(&statusData[5], &co2State.lastRawADC, sizeof(uint16_t));

    // Byte 7: Error flags
    statusData[7] = co2State.errorFlags;
}

/**
 * @brief Send current status to hub (unsolicited pH report)
 */
static void sendPeriodicStatus() {
    uint8_t statusData[8] = {0};
    fillStatusData(statusData);

    // Send unsolicited status (commandId=0xFF)
    sendStatusAck(nodeConfig.hubReturnMac, 0xFF, 0x00, statusData, 8);

    if (nodeConfig.debugESPNOW) {
        Serial.printf("[STATUS] Periodic pH report: pH=%.2f, relay=%s, flags=0x%02X\n",
                      co2State.lastPH,
                      co2State.relayOn ? "ON" : "OFF",
                      co2State.errorFlags);
    }
}

// ============================================================================
// LOCAL SAFETY CHECK
// ============================================================================

/**
 * @brief Check pH against local emergency safety limits
 *
 * If pH drops below phSafetyLow, CO2 is forced OFF immediately.
 * This is a LOCAL safety net independent of hub communication.
 * Hub handles trend-based analysis; this handles catastrophic pH drop.
 */
static void checkLocalSafety() {
    if (co2State.lastPH <= 0.0f || co2State.sensorError) {
        return;  // No valid reading to check
    }

    // Emergency LOW pH cutoff
    if (co2State.lastPH < co2Cfg.phSafetyLow && co2State.relayOn) {
        Serial.printf("[SAFETY] pH %.2f BELOW emergency limit %.1f - FORCING CO2 OFF!\n",
                      co2State.lastPH, co2Cfg.phSafetyLow);
        emergencyCO2Off(2);  // 2 = safety cutoff flag

        // Report safety event to hub
        uint8_t statusData[8] = {0};
        fillStatusData(statusData);
        sendStatusAck(nodeConfig.hubReturnMac, 0xFE, 0xFF, statusData, 8);
    }

    // High pH warning (informational only)
    if (co2State.lastPH > co2Cfg.phSafetyHigh) {
        if (nodeConfig.debugSerial) {
            Serial.printf("[SAFETY] pH %.2f above warning threshold %.1f\n",
                          co2State.lastPH, co2Cfg.phSafetyHigh);
        }
    }
}

// ============================================================================
// HARDWARE IMPLEMENTATION (Required by NodeBase)
// ============================================================================

void setupHardware() {
    // Configure relay pin - ALWAYS start OFF (safety-critical)
    pinMode(co2Cfg.relayPin, OUTPUT);
    digitalWrite(co2Cfg.relayPin, LOW);  // CO2 OFF at boot
    co2State.relayOn = false;

    // Configure pH sensor pin (A0 is input by default, but be explicit)
    pinMode(co2Cfg.phSensorPin, INPUT);

    // Initialize state
    co2State.state          = CO2State::IDLE;
    co2State.lastPH         = 0.0f;
    co2State.lastRawADC     = 0;
    co2State.lastPHReadTime = 0;
    co2State.errorFlags     = 0;
    co2State.sensorError    = false;

    if (nodeConfig.debugSerial) {
        Serial.printf("[OK] CO2 hardware initialized:\n");
        Serial.printf("     Relay: GPIO%d = OFF (CO2 OFF)\n", co2Cfg.relayPin);
        Serial.printf("     pH Sensor: pin %d (A0=%d)\n", co2Cfg.phSensorPin, A0);
        Serial.printf("     Read interval: %u ms\n", co2Cfg.phReadIntervalMs);
    }
}

void enterFailSafeMode() {
    Serial.println("[WARN] *** FAIL-SAFE: Hub communication lost ***");
    Serial.println("[WARN] *** CO2 RELAY FORCED OFF (safety-critical) ***");

    // CRITICAL: Force CO2 OFF immediately
    setRelay(false);
    co2State.state      = CO2State::FAILSAFE;
    co2State.errorFlags = 0;  // Clear error flags, failsafe is a known state

    if (nodeConfig.debugESPNOW) {
        Serial.println("[OK] Fail-safe applied: CO2 relay OFF, state=FAILSAFE");
    }
}

void handleCommand(const uint8_t* mac, const uint8_t* data, size_t len) {
    if (len == 0 || data == nullptr) return;

    uint8_t commandType = data[0];

    if (nodeConfig.debugESPNOW) {
        Serial.println("+========================================================+");
        Serial.printf("| [CMD] COMMAND received\n");
        Serial.printf("| Tank ID: %d\n", nodeConfig.tankId);
        Serial.printf("| Command Type: %d\n", commandType);
        Serial.printf("| Data length: %zu bytes\n", len);
    }

    bool success = true;

    switch (commandType) {
        case 0: {  // Emergency stop - CO2 OFF immediately
            setRelay(false);
            co2State.state      = CO2State::IDLE;
            co2State.errorFlags = 0;

            if (nodeConfig.debugESPNOW) {
                Serial.println("| [OK] Emergency stop - CO2 OFF");
            }
            break;
        }

        case 1: {  // CO2 ON (relay HIGH)
            // Safety: don't turn ON if in safety cutoff
            if (co2State.state == CO2State::SAFETY_CUTOFF) {
                success = false;
                if (nodeConfig.debugESPNOW) {
                    Serial.println("| [ERROR] Cannot enable CO2 - safety cutoff active");
                    Serial.println("| [INFO]  Send emergency stop (cmd 0) first to clear");
                }
            } else {
                setRelay(true);
                co2State.state      = CO2State::CO2_ON;
                co2State.errorFlags = 0;

                if (nodeConfig.debugESPNOW) {
                    Serial.println("| [OK] CO2 ON - relay activated");
                }
            }
            break;
        }

        case 2: {  // CO2 OFF (relay LOW)
            setRelay(false);
            co2State.state      = CO2State::IDLE;
            co2State.errorFlags = 0;

            if (nodeConfig.debugESPNOW) {
                Serial.println("| [OK] CO2 OFF - relay deactivated");
            }
            break;
        }

        case 40: {  // Status request - read pH and report
            if (nodeConfig.debugESPNOW) {
                Serial.println("| [OK] Status request - reading pH sensor");
            }
            // Force an immediate pH reading
            float pH = readPH();
            if (pH >= 0.0f) {
                co2State.lastPH         = pH;
                co2State.lastPHReadTime = millis();
                checkLocalSafety();
            }
            break;
        }

        case 41: {  // pH Calibration: hub sends known actual pH, node computes trim
            // Payload: data[1..4] = actual pH as IEEE-754 float (little-endian)
            if (len < 5) {
                success = false;
                if (nodeConfig.debugESPNOW) {
                    Serial.println("| [ERROR] Calibration cmd: need at least 5 bytes");
                }
                break;
            }

            float actualPH;
            memcpy(&actualPH, &data[1], sizeof(float));

            if (actualPH < 0.0f || actualPH > 14.0f) {
                success = false;
                if (nodeConfig.debugESPNOW) {
                    Serial.printf("| [ERROR] Calibration: invalid pH=%.2f (must be 0-14)\n",
                                  actualPH);
                }
                break;
            }

            // Sanity-check slope before dividing
            if (co2Cfg.phCalibSlope == 0.0f) {
                success = false;
                Serial.println("[CALIB] ERROR: phCalibSlope is 0 - cannot compute trim");
                break;
            }

            // Compute voltage trim:
            //   pH = slope * (rawV + trim) + offset
            //   => trim = (actualPH - offset) / slope - rawV
            //
            // Both ESP8266 and ESP32 are little-endian; float is transmitted as
            // native IEEE-754 LE bytes, which is correct between these platforms.
            float rawVoltage    = (float)co2State.lastRawADC * co2Cfg.adcVoltageRef
                                  / (float)co2Cfg.adcMaxValue;
            float targetVoltage = (actualPH - co2Cfg.phCalibOffset) / co2Cfg.phCalibSlope;
            co2Cfg.phCalibVoltTrim = targetVoltage - rawVoltage;

            Serial.printf("[CALIB] Hub calibration: actual pH=%.2f, raw V=%.4fV, "
                          "target V=%.4fV, new trim=%.4fV\n",
                          actualPH, rawVoltage, targetVoltage, co2Cfg.phCalibVoltTrim);

            if (!saveCO2CalibTrim()) {
                success = false;  // Config write failed; trim is updated in RAM only
                Serial.println("[CALIB] WARNING: trim not persisted to config");
            }
            break;
        }

        case 0xFF: {  // TEST: Force fail-safe
            if (nodeConfig.debugSerial) {
                Serial.println("[TEST] Force-failsafe command received");
            }
            enterFailSafeMode();
            break;
        }

        default: {
            success = false;
            if (nodeConfig.debugESPNOW) {
                Serial.printf("| [ERROR] Unknown command type: %d\n", commandType);
            }
            break;
        }
    }

    if (nodeConfig.debugESPNOW) {
        Serial.println("+========================================================+");
    }

    // Send acknowledgment with current status
    uint8_t statusCode = success ? 0x00 : 0xFF;
    uint8_t statusData[8] = {0};
    fillStatusData(statusData);

    if (nodeConfig.debugESPNOW) {
        Serial.printf("[STATUS] relay=%s, pH=%.2f, ADC=%d, flags=0x%02X\n",
                      co2State.relayOn ? "ON" : "OFF",
                      co2State.lastPH,
                      co2State.lastRawADC,
                      co2State.errorFlags);
    }

    sendStatusAck(mac, 0, statusCode, statusData, 8);
}

void updateHardware() {
    // Run continuous pH pipeline (reads A0 every 200ms, computes medians)
    phPipelineTick();

    // Enforce relay state matches software state
    // (safety guard against hardware glitches)
    digitalWrite(co2Cfg.relayPin, co2State.relayOn ? HIGH : LOW);
}

// ============================================================================
// ESPNOW CALLBACK WRAPPERS (Match ESPNowManager signatures)
// ============================================================================

void onAckReceivedWrapper(const uint8_t* mac, const AckMessage& msg) {
    onAckReceived(mac, msg);
}

void onConfigReceivedWrapper(const uint8_t* mac, const ConfigMessage& msg) {
    onConfigReceived(mac, msg);
}

void onUnmapReceivedWrapper(const uint8_t* mac, const UnmapMessage& msg) {
    onUnmapReceived(mac, msg);
}

void onCommandReceivedWrapper(const uint8_t* mac, const uint8_t* data, size_t len) {
    handleCommand(mac, data, len);
}

// ============================================================================
// Arduino Entry Points
// ============================================================================

void setup() {
    Serial.begin(115200);
    delay(2000);

    Serial.println("\n\n\n");
    Serial.println("================================");
    Serial.println("ESP8266 BOOT - Serial Working!");
    Serial.println("================================");

    Serial.println("\n\n");
    Serial.println("+===========================================================+");
    Serial.println("|        CO2 REGULATOR NODE - Aquarium Management           |");
    Serial.println("|        Platform: ESP8266 D1 Mini / pH Sensor + Relay      |");
    Serial.println("+===========================================================+");

    // Load base configuration using NodeBase
    Serial.println("[1] Loading configuration...");
    loadNodeConfiguration(NodeType::CO2, "UnmappedCO2");
    Serial.println("[1] Base configuration loaded OK");

    // Load CO2-specific configuration (re-parses same file for CO2 keys)
    Serial.println("[2] Loading CO2-specific configuration...");
    loadCO2Config();
    Serial.println("[2] CO2 configuration loaded OK");

    Serial.printf("Tank ID: %d | Node: %s | FW: v%d\n\n",
                  nodeConfig.tankId, nodeConfig.nodeName.c_str(), nodeConfig.firmwareVersion);

    // Initialize status LED (starts blinking - waiting for ACK)
    // NOTE: STATUS_LED_PIN is set to D5/GPIO14 in node_config.txt to avoid
    //       conflict with relay on D7/GPIO13
    Serial.println("[3] Initializing status LED...");
    setupStatusLED();
    Serial.println("[3] Status LED initialized OK");

    // Initialize hardware (relay OFF, pH sensor ready)
    Serial.println("[4] Initializing hardware...");
    setupHardware();
    Serial.println("[4] Hardware initialized OK");

    // Initialize ESPNowManager
    Serial.println("[5] Starting ESP-NOW initialization...");
    bool success = ESPNowManager::getInstance().begin(nodeConfig.espnowChannel, false);
    Serial.printf("[5] ESP-NOW init returned: %s\n", success ? "SUCCESS" : "FAILED");

    if (!success) {
        Serial.println("[ERROR] ESPNowManager initialization failed!");
        Serial.println("[WARN]  Entering fail-safe mode (CO2 OFF)");
        enterFailSafeMode();
        while (1) delay(1000);
    }

    // Register callbacks
    ESPNowManager::getInstance().onAckReceived(onAckReceivedWrapper);
    ESPNowManager::getInstance().onCommandReceived(onCommandReceivedWrapper);
    ESPNowManager::getInstance().onConfigReceived(onConfigReceivedWrapper);
    ESPNowManager::getInstance().onUnmapReceived(onUnmapReceivedWrapper);

    // Register OTA handler (common to all nodes via NodeBase)
    ESPNowManager::getInstance().onRawCommandReceived(handleOtaCommand);

    // Register base node callbacks (hub heartbeat handler, etc.)
    setupNodeBaseCallbacks();

    Serial.println("[OK] ESPNowManager ready");
    Serial.printf("   - Channel: %d\n", nodeConfig.espnowChannel);
    Serial.printf("   - Mode: NODE (ESP8266)\n");
    Serial.printf("   - Debug ESP-NOW: %s\n", nodeConfig.debugESPNOW ? "ON" : "OFF");

    // Send initial ANNOUNCE using NodeBase
    Serial.println("[6] Sending initial ANNOUNCE...");
    sendAnnounce();

    // Initialize pH pipeline
    Serial.println("[7] Initializing pH pipeline...");
    phPipeline.rawCount        = 0;
    phPipeline.medianCount     = 0;
    phPipeline.lastADCReadTime = millis();
    phPipeline.trendStartTime  = millis();
    phPipeline.lastShortMedian = 0.0f;
    phPipeline.lastTrendMedian = 0.0f;
    phPipeline.hasTrendMedian  = false;
    Serial.printf("[7] Pipeline: read A0 every %ums, %d samples/median, %us trend window\n",
                  co2Cfg.phADCReadIntervalMs, co2Cfg.phSamplesPerMedian,
                  co2Cfg.phTrendWindowSec);
    Serial.printf("[7] Calibration: pH = %.2f * (V + %.4f) + (%.2f)\n",
                  co2Cfg.phCalibSlope, co2Cfg.phCalibVoltTrim, co2Cfg.phCalibOffset);
    Serial.println("[7] Pipeline will start reading in loop()");

    Serial.printf("\n[OK] CO2 regulator node ready (ESP8266 D1 Mini)\n");
    Serial.println("     SAFETY: CO2 relay starts OFF");
    Serial.printf("     SAFETY: Local cutoff if pH < %.1f\n", co2Cfg.phSafetyLow);
    Serial.println("     Hub controls ON/OFF based on pH trend analysis\n");
    lastHeartbeatSent = millis();
}

void loop() {
    // Delegate processing, heartbeats, and hub-liveness checks to NodeBase
    nodeLoop();

    // Update hardware state (periodic pH reading, relay enforcement)
    updateHardware();

    // Send periodic ANNOUNCE for discovery (only when unmapped)
    static unsigned long lastAnnounce = 0;
    if (!isConnectedToHub && (millis() - lastAnnounce >= nodeConfig.announceIntervalMs)) {
        lastAnnounce = millis();
        sendAnnounce();
    }

    // Print memory status every 60 seconds
    static unsigned long lastMemoryPrint = 0;
    if (millis() - lastMemoryPrint >= 60000) {
        lastMemoryPrint = millis();
        Serial.printf("[HEARTBEAT] Free heap: %u bytes | pH=%.2f | relay=%s\n",
                      ESP.getFreeHeap(),
                      co2State.lastPH,
                      co2State.relayOn ? "ON" : "OFF");
    }

    // Print ESP-NOW statistics periodically
    if (nodeConfig.debugESPNOW) {
        static unsigned long lastStatsTime = 0;
        if (millis() - lastStatsTime > 60000) {
            lastStatsTime = millis();
            auto stats = ESPNowManager::getInstance().getStatistics();
            Serial.println("\n-----------------------------------------");
            Serial.println("[STATS] ESP-NOW Statistics (Last 60s):");
            Serial.printf("   Messages: %u sent / %u received\n",
                          stats.messagesSent, stats.messagesReceived);
            Serial.printf("   Fragments: %u sent / %u received\n",
                          stats.fragmentsSent, stats.fragmentsReceived);
            Serial.printf("   Errors: %u send failures / %u reassembly timeouts\n",
                          stats.sendFailures, stats.reassemblyTimeouts);
            Serial.printf("   Duplicates ignored: %u\n", stats.duplicatesIgnored);
            Serial.println("-----------------------------------------\n");
        }
    }

    delay(10);
}
