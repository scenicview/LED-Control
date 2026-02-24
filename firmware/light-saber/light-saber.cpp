/*
 * Light-Saber LED Controller
 *
 * Hardware:
 *   - ESP32-S3 Super Mini
 *   - 2A power regulator/charger module
 *   - Single 18650 Li-Ion battery (3.7V nominal)
 *   - WS2812B LED strip
 *   - MAX98357A I2S DAC/Amplifier
 *   - MPU6050 6-axis IMU (accelerometer + gyroscope)
 *
 * Features:
 *   - WiFi AP with web interface
 *   - BLE control
 *   - Battery voltage monitoring
 *   - Low battery warning & auto-shutoff
 *   - Lightsaber ignition/retraction animations
 *   - Color modes: solid, pulse, rainbow, clash flash
 *   - Sound effects: ignite, retract, hum, swing, clash (WAV from LittleFS)
 *   - Procedural hum fallback when WAV files are missing
 *   - Motion detection: swing (gyroscope) and clash (accelerometer)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <NeoPixelBus.h>
#include <Preferences.h>
#include <driver/i2s.h>
#include <LittleFS.h>
#include <Wire.h>
#include <math.h>
#include <esp_sleep.h>

#ifndef PI
#define PI M_PI
#endif

// ============================================================
// PIN DEFINITIONS - ESP32-S3 Super Mini
// ============================================================
#define LED_DATA_PIN      2     // GPIO2 - NeoPixel data out
#define BATTERY_ADC_PIN   1     // GPIO1 - Battery voltage via divider
#define BUTTON_PIN        0     // GPIO0 - Boot button / mode select
#define ONBOARD_LED_PIN   48    // Onboard RGB LED (if present)

// I2S Audio - MAX98357A
#define I2S_BCLK_PIN      4     // GPIO4 - Bit clock
#define I2S_LRC_PIN       5     // GPIO5 - Left/Right clock (Word Select)
#define I2S_DOUT_PIN      6     // GPIO6 - Data out
#define AMP_SD_PIN        7     // GPIO7 - Amplifier shutdown/enable

// I2C - MPU6050
#define MPU_SDA_PIN       8     // GPIO8 - I2C data
#define MPU_SCL_PIN       9     // GPIO9 - I2C clock
#define MPU_INT_PIN       3     // GPIO3 - Motion interrupt (optional)
#define MPU_ADDR          0x68  // MPU6050 I2C address

// Deep Sleep
#define LONG_PRESS_MS     1500  // Hold 1.5s for deep sleep

// ============================================================
// LED CONFIGURATION
// ============================================================
#define NUM_LEDS          60    // Number of LEDs in the blade
#define LED_BRIGHTNESS    180   // Default brightness (0-255)
#define MIN_BRIGHTNESS    5
#define MAX_BRIGHTNESS    255

// ============================================================
// BATTERY CONFIGURATION
// ============================================================
// Voltage divider: Battery → [R1 100k] → ADC_PIN → [R2 100k] → GND
// This gives a 1:2 divider ratio, so 4.2V battery reads ~2.1V at ADC
#define VDIVIDER_RATIO    2.0   // Adjust based on your actual resistor values
#define ADC_RESOLUTION    4095.0
#define ADC_REF_VOLTAGE   3.3
#define BATT_FULL         4.20  // 18650 fully charged
#define BATT_NOMINAL      3.70  // 18650 nominal
#define BATT_LOW          3.30  // Low battery warning
#define BATT_CRITICAL     3.00  // Auto shutoff threshold
#define BATT_READ_INTERVAL 5000 // Read battery every 5 seconds

// ============================================================
// AUDIO CONFIGURATION
// ============================================================
#define AUDIO_SAMPLE_RATE   22050
#define AUDIO_BUFFER_SIZE   512   // Samples per DMA buffer
#define I2S_PORT            I2S_NUM_0
#define DEFAULT_VOLUME      180

// ============================================================
// MOTION CONFIGURATION
// ============================================================
#define SWING_THRESHOLD     150.0  // deg/s angular velocity
#define CLASH_THRESHOLD     2.5    // g-force spike
#define SWING_DEBOUNCE_MS   250
#define CLASH_DEBOUNCE_MS   300
#define MPU_SAMPLE_INTERVAL 10     // 100Hz (10ms)

// ============================================================
// WIFI CONFIGURATION
// ============================================================
const char* ap_ssid     = "Light-Saber";
const char* ap_password = "12345678";

// ============================================================
// BLE CONFIGURATION
// ============================================================
#define BLE_DEVICE_NAME    "Light-Saber"
#define SERVICE_UUID       "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHAR_COLOR_UUID    "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define CHAR_MODE_UUID     "1c95d5e3-d8f7-413a-bf3d-7a2e5d7be87e"
#define CHAR_BRIGHT_UUID   "a3c87500-8ed3-4bdf-8a39-a01bebede295"
#define CHAR_BATTERY_UUID  "00002a19-0000-1000-8000-00805f9b34fb"  // Standard battery level
#define CHAR_VOLUME_UUID   "d4e0e270-7e84-4e3b-a5b0-3e6e1a4b92c1"

// ============================================================
// ANIMATION MODES
// ============================================================
enum BladeMode {
    MODE_OFF = 0,
    MODE_SOLID,
    MODE_PULSE,
    MODE_RAINBOW,
    MODE_FIRE,
    MODE_CLASH,
    MODE_IGNITE,    // Ignition animation (one-shot)
    MODE_RETRACT,   // Retraction animation (one-shot)
    MODE_COUNT
};

const char* modeNames[] = {
    "Off", "Solid", "Pulse", "Rainbow", "Fire", "Clash", "Ignite", "Retract"
};

// ============================================================
// GLOBALS
// ============================================================
NeoPixelBus<NeoGrbFeature, NeoEsp32BitBangWs2812xMethod> strip(NUM_LEDS, LED_DATA_PIN);
WebServer server(80);
Preferences prefs;

// BLE
BLEServer* pServer = nullptr;
BLECharacteristic* pColorChar = nullptr;
BLECharacteristic* pModeChar = nullptr;
BLECharacteristic* pBrightChar = nullptr;
BLECharacteristic* pBatteryChar = nullptr;
BLECharacteristic* pVolumeChar = nullptr;
bool bleConnected = false;

// State
uint8_t currentR = 0, currentG = 255, currentB = 0;  // Default: green saber
uint8_t brightness = LED_BRIGHTNESS;
BladeMode currentMode = MODE_OFF;
BladeMode pendingMode = MODE_OFF;  // For post-animation transitions

// Volume
uint8_t volume = DEFAULT_VOLUME;

// Audio engine
File audioFile;
bool audioPlaying = false;
bool audioLooping = false;
uint32_t audioDataStart = 0;    // Offset past WAV header
uint32_t audioDataSize = 0;
uint32_t audioDataPos = 0;
bool littleFSReady = false;
bool humFileExists = false;
bool useProceduralHum = false;
float humPhase = 0.0;          // For procedural hum synthesis
int16_t audioBuf[AUDIO_BUFFER_SIZE];

// MPU6050
bool mpuReady = false;
float accelX, accelY, accelZ;   // In g
float gyroX, gyroY, gyroZ;      // In deg/s
unsigned long lastMPURead = 0;
unsigned long lastSwingTime = 0;
unsigned long lastClashTime = 0;

// Battery
float batteryVoltage = 4.2;
uint8_t batteryPercent = 100;
unsigned long lastBatteryRead = 0;
bool lowBatteryWarning = false;

// Animation
unsigned long lastAnimUpdate = 0;
int animStep = 0;
bool animComplete = false;

// Button
unsigned long lastButtonPress = 0;
bool buttonHandled = false;
unsigned long buttonPressStart = 0;
bool longPressHandled = false;

// ============================================================
// FORWARD DECLARATIONS
// ============================================================
void readBattery();
void clearStrip();
void setAllLeds(uint8_t r, uint8_t g, uint8_t b);
void setupAudio();
void playSound(const char* filename, bool loop);
void stopSound();
int16_t generateHumSample();
void startHum();
void feedAudio();
void mpuWriteReg(uint8_t reg, uint8_t val);
uint8_t mpuReadReg(uint8_t reg);
void setupMPU();
void readMPU();
void processMotion();
void animateSolid();
void animatePulse();
void animateRainbow();
void animateFire();
void animateClash();
void animateIgnite();
void animateRetract();
void updateAnimation();
void setMode(BladeMode newMode);
void enterDeepSleep();
void handleButton();
void saveSettings();
void loadSettings();
void setupBLE();
void setupWebServer();

// ============================================================
// BATTERY MONITORING
// ============================================================
void readBattery() {
    // Take multiple samples and average
    long total = 0;
    for (int i = 0; i < 16; i++) {
        total += analogRead(BATTERY_ADC_PIN);
    }
    float adcValue = total / 16.0;

    batteryVoltage = (adcValue / ADC_RESOLUTION) * ADC_REF_VOLTAGE * VDIVIDER_RATIO;

    // Clamp and calculate percentage (linear approximation)
    if (batteryVoltage >= BATT_FULL) {
        batteryPercent = 100;
    } else if (batteryVoltage <= BATT_CRITICAL) {
        batteryPercent = 0;
    } else {
        batteryPercent = (uint8_t)((batteryVoltage - BATT_CRITICAL) / (BATT_FULL - BATT_CRITICAL) * 100.0);
    }

    // Low battery warning
    if (batteryVoltage <= BATT_LOW && !lowBatteryWarning) {
        lowBatteryWarning = true;
        Serial.println("WARNING: Low battery!");
    }

    // Critical - turn off LEDs and sound to protect battery
    if (batteryVoltage <= BATT_CRITICAL) {
        Serial.println("CRITICAL: Battery too low, shutting down");
        currentMode = MODE_OFF;
        clearStrip();
        stopSound();
    }

    // Update BLE battery characteristic
    if (pBatteryChar != nullptr && bleConnected) {
        pBatteryChar->setValue(&batteryPercent, 1);
        pBatteryChar->notify();
    }

    Serial.printf("Battery: %.2fV (%d%%)\n", batteryVoltage, batteryPercent);
}

// ============================================================
// LED HELPERS
// ============================================================
void clearStrip() {
    for (int i = 0; i < NUM_LEDS; i++) {
        strip.SetPixelColor(i, RgbColor(0, 0, 0));
    }
    strip.Show();
}

void setAllLeds(uint8_t r, uint8_t g, uint8_t b) {
    float scale = brightness / 255.0;
    RgbColor color(r * scale, g * scale, b * scale);
    for (int i = 0; i < NUM_LEDS; i++) {
        strip.SetPixelColor(i, color);
    }
    strip.Show();
}

// ============================================================
// AUDIO ENGINE
// ============================================================
void setupAudio() {
    // Enable amplifier
    pinMode(AMP_SD_PIN, OUTPUT);
    digitalWrite(AMP_SD_PIN, HIGH);

    // Configure I2S
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = AUDIO_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,
        .dma_buf_len = AUDIO_BUFFER_SIZE,
        .use_apll = false,
        .tx_desc_auto_clear = true,
    };

    i2s_pin_config_t pin_config = {
        .mck_io_num = I2S_PIN_NO_CHANGE,  // MAX98357A doesn't use MCLK
        .bck_io_num = I2S_BCLK_PIN,
        .ws_io_num = I2S_LRC_PIN,
        .data_out_num = I2S_DOUT_PIN,
        .data_in_num = I2S_PIN_NO_CHANGE,
    };

    i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_PORT, &pin_config);
    i2s_zero_dma_buffer(I2S_PORT);
    Serial.println("I2S: OK (22050Hz, 16-bit, mono)");

    // Init LittleFS
    if (LittleFS.begin(true)) {
        littleFSReady = true;
        humFileExists = LittleFS.exists("/hum.wav");
        Serial.println("LittleFS: mounted");
        Serial.printf("  hum.wav: %s\n", humFileExists ? "found" : "MISSING (procedural fallback)");
        Serial.printf("  ignite.wav: %s\n", LittleFS.exists("/ignite.wav") ? "found" : "missing");
        Serial.printf("  retract.wav: %s\n", LittleFS.exists("/retract.wav") ? "found" : "missing");
        Serial.printf("  clash.wav: %s\n", LittleFS.exists("/clash.wav") ? "found" : "missing");
        Serial.printf("  swing.wav: %s\n", LittleFS.exists("/swing.wav") ? "found" : "missing");
    } else {
        Serial.println("LittleFS: FAILED to mount");
    }
}

void playSound(const char* filename, bool loop) {
    // Stop any current non-hum playback for one-shot sounds,
    // or stop everything for new loop request
    if (audioFile) {
        audioFile.close();
    }
    audioPlaying = false;

    if (!littleFSReady) return;

    digitalWrite(AMP_SD_PIN, HIGH);  // Enable amp

    // Check if file exists
    if (!LittleFS.exists(filename)) {
        Serial.printf("Audio: %s not found\n", filename);
        return;
    }

    audioFile = LittleFS.open(filename, "r");
    if (!audioFile) return;

    // Skip WAV header (44 bytes standard)
    uint8_t header[44];
    audioFile.read(header, 44);
    audioDataStart = 44;
    audioDataSize = audioFile.size() - 44;
    audioDataPos = 0;
    audioLooping = loop;
    audioPlaying = true;
    useProceduralHum = false;

    Serial.printf("Audio: playing %s (%s)\n", filename, loop ? "loop" : "once");
}

void stopSound() {
    if (audioFile) {
        audioFile.close();
    }
    audioPlaying = false;
    audioLooping = false;
    useProceduralHum = false;
    i2s_zero_dma_buffer(I2S_PORT);
    digitalWrite(AMP_SD_PIN, LOW);  // Disable amp to save power
}

int16_t generateHumSample() {
    // Additive synthesis: 120Hz fundamental + harmonics
    float sample = 0;
    sample += 0.5 * sin(humPhase * 2.0 * PI * 120.0);  // Fundamental
    sample += 0.3 * sin(humPhase * 2.0 * PI * 240.0);  // 2nd harmonic
    sample += 0.15 * sin(humPhase * 2.0 * PI * 360.0); // 3rd harmonic
    sample += 0.08 * sin(humPhase * 2.0 * PI * 480.0); // 4th harmonic

    humPhase += 1.0 / AUDIO_SAMPLE_RATE;
    if (humPhase >= 1.0) humPhase -= 1.0;

    float volScale = volume / 255.0;
    return (int16_t)(sample * 16000.0 * volScale);
}

void startHum() {
    if (humFileExists) {
        playSound("/hum.wav", true);
    } else {
        // Use procedural hum
        digitalWrite(AMP_SD_PIN, HIGH);  // Enable amp
        useProceduralHum = true;
        audioPlaying = false;  // Not file-based
        humPhase = 0.0;
        Serial.println("Audio: procedural hum started");
    }
}

void feedAudio() {
    size_t bytesWritten = 0;

    if (useProceduralHum) {
        // Generate procedural hum samples
        for (int i = 0; i < AUDIO_BUFFER_SIZE; i++) {
            audioBuf[i] = generateHumSample();
        }
        i2s_write(I2S_PORT, audioBuf, AUDIO_BUFFER_SIZE * sizeof(int16_t), &bytesWritten, 0);
        return;
    }

    if (!audioPlaying || !audioFile) return;

    // Read next chunk from WAV file
    int samplesToRead = AUDIO_BUFFER_SIZE;
    int bytesRead = audioFile.read((uint8_t*)audioBuf, samplesToRead * sizeof(int16_t));

    if (bytesRead <= 0) {
        if (audioLooping) {
            // Loop: seek back to start of audio data
            audioFile.seek(audioDataStart);
            audioDataPos = 0;
            bytesRead = audioFile.read((uint8_t*)audioBuf, samplesToRead * sizeof(int16_t));
            if (bytesRead <= 0) { stopSound(); return; }
        } else {
            // One-shot done - if blade is on, resume hum
            audioFile.close();
            audioPlaying = false;
            if (currentMode != MODE_OFF && currentMode != MODE_RETRACT) {
                startHum();
            }
            return;
        }
    }

    int samplesRead = bytesRead / sizeof(int16_t);
    audioDataPos += bytesRead;

    // Apply volume
    float volScale = volume / 255.0;
    for (int i = 0; i < samplesRead; i++) {
        audioBuf[i] = (int16_t)(audioBuf[i] * volScale);
    }

    i2s_write(I2S_PORT, audioBuf, samplesRead * sizeof(int16_t), &bytesWritten, 0);
}

// ============================================================
// MPU6050 - Direct Register Access
// ============================================================
void mpuWriteReg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(MPU_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

uint8_t mpuReadReg(uint8_t reg) {
    Wire.beginTransmission(MPU_ADDR);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)MPU_ADDR, (uint8_t)1);
    return Wire.read();
}

void setupMPU() {
    Wire.begin(MPU_SDA_PIN, MPU_SCL_PIN);
    Wire.setClock(400000);  // 400kHz I2C

    // Check WHO_AM_I
    uint8_t whoami = mpuReadReg(0x75);
    if (whoami != 0x68) {
        Serial.printf("MPU6050 not found (WHO_AM_I=0x%02X)\n", whoami);
        mpuReady = false;
        return;
    }

    mpuWriteReg(0x6B, 0x00);  // Wake up (PWR_MGMT_1)
    delay(10);
    mpuWriteReg(0x6B, 0x01);  // Clock source: PLL with X-axis gyro
    mpuWriteReg(0x1A, 0x03);  // DLPF config 3 (~44Hz bandwidth)
    mpuWriteReg(0x1B, 0x08);  // Gyro: ±500 dps (FS_SEL=1)
    mpuWriteReg(0x1C, 0x08);  // Accel: ±4g (AFS_SEL=1)
    mpuWriteReg(0x19, 0x09);  // Sample rate divider: 100Hz

    mpuReady = true;
    Serial.println("MPU6050: OK (±4g, ±500 dps, 100Hz)");
}

void readMPU() {
    Wire.beginTransmission(MPU_ADDR);
    Wire.write(0x3B);  // Start at ACCEL_XOUT_H
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)MPU_ADDR, (uint8_t)14);

    int16_t rawAx = (Wire.read() << 8) | Wire.read();
    int16_t rawAy = (Wire.read() << 8) | Wire.read();
    int16_t rawAz = (Wire.read() << 8) | Wire.read();
    Wire.read(); Wire.read();  // Skip temperature
    int16_t rawGx = (Wire.read() << 8) | Wire.read();
    int16_t rawGy = (Wire.read() << 8) | Wire.read();
    int16_t rawGz = (Wire.read() << 8) | Wire.read();

    // Convert: ±4g = 8192 LSB/g, ±500 dps = 65.5 LSB/(deg/s)
    accelX = rawAx / 8192.0;
    accelY = rawAy / 8192.0;
    accelZ = rawAz / 8192.0;
    gyroX  = rawGx / 65.5;
    gyroY  = rawGy / 65.5;
    gyroZ  = rawGz / 65.5;
}

void processMotion() {
    if (!mpuReady) return;

    unsigned long now = millis();
    if (now - lastMPURead < MPU_SAMPLE_INTERVAL) return;
    lastMPURead = now;

    readMPU();

    // Only detect motion when blade is on
    if (currentMode == MODE_OFF || currentMode == MODE_IGNITE || currentMode == MODE_RETRACT) return;

    // Clash detection: sudden acceleration spike
    float accelMag = sqrt(accelX * accelX + accelY * accelY + accelZ * accelZ);
    if (accelMag > CLASH_THRESHOLD && (now - lastClashTime > CLASH_DEBOUNCE_MS)) {
        lastClashTime = now;
        Serial.printf("CLASH! (%.1fg)\n", accelMag);
        // Trigger clash: white flash + sound
        animStep = 0;
        currentMode = MODE_CLASH;
        playSound("/clash.wav", false);
        return;
    }

    // Swing detection: angular velocity
    float gyroMag = sqrt(gyroX * gyroX + gyroY * gyroY + gyroZ * gyroZ);
    if (gyroMag > SWING_THRESHOLD && (now - lastSwingTime > SWING_DEBOUNCE_MS)) {
        lastSwingTime = now;
        Serial.printf("SWING! (%.0f deg/s)\n", gyroMag);
        playSound("/swing.wav", false);
    }
}

// ============================================================
// ANIMATIONS
// ============================================================
void animateSolid() {
    setAllLeds(currentR, currentG, currentB);
}

void animatePulse() {
    unsigned long now = millis();
    if (now - lastAnimUpdate < 20) return;
    lastAnimUpdate = now;

    float breath = (sin(animStep * 0.02) + 1.0) / 2.0;  // 0.0 to 1.0
    float minScale = 0.15;
    float scale = (minScale + (1.0 - minScale) * breath) * (brightness / 255.0);

    RgbColor color(currentR * scale, currentG * scale, currentB * scale);
    for (int i = 0; i < NUM_LEDS; i++) {
        strip.SetPixelColor(i, color);
    }
    strip.Show();
    animStep++;
}

void animateRainbow() {
    unsigned long now = millis();
    if (now - lastAnimUpdate < 30) return;
    lastAnimUpdate = now;

    float scale = brightness / 255.0;
    for (int i = 0; i < NUM_LEDS; i++) {
        float hue = fmod((float)(i * 360 / NUM_LEDS + animStep), 360.0);
        HslColor hsl(hue / 360.0, 1.0, 0.5 * scale);
        strip.SetPixelColor(i, RgbColor(hsl));
    }
    strip.Show();
    animStep = (animStep + 2) % 360;
}

void animateFire() {
    unsigned long now = millis();
    if (now - lastAnimUpdate < 40) return;
    lastAnimUpdate = now;

    float scale = brightness / 255.0;
    for (int i = 0; i < NUM_LEDS; i++) {
        uint8_t flicker = random(100, 255);
        float f = flicker / 255.0 * scale;
        // Tint fire toward the saber color
        strip.SetPixelColor(i, RgbColor(
            (uint8_t)(currentR * f),
            (uint8_t)(currentG * f * 0.3),
            (uint8_t)(currentB * f * 0.1)
        ));
    }
    strip.Show();
}

void animateClash() {
    // Brief white flash then return to solid
    unsigned long now = millis();
    if (animStep == 0) {
        float scale = brightness / 255.0;
        RgbColor white(255 * scale, 255 * scale, 255 * scale);
        for (int i = 0; i < NUM_LEDS; i++) {
            strip.SetPixelColor(i, white);
        }
        strip.Show();
        animStep = 1;
        lastAnimUpdate = now;
    } else if (now - lastAnimUpdate > 100) {
        // Flash done, return to solid
        currentMode = MODE_SOLID;
        animStep = 0;
    }
}

void animateIgnite() {
    unsigned long now = millis();
    if (now - lastAnimUpdate < 8) return;  // ~3ms per LED = fast ignition
    lastAnimUpdate = now;

    if (animStep < NUM_LEDS) {
        float scale = brightness / 255.0;
        // Light up LEDs from base to tip
        strip.SetPixelColor(animStep, RgbColor(
            currentR * scale, currentG * scale, currentB * scale
        ));
        // White-hot leading edge
        if (animStep > 0) {
            strip.SetPixelColor(animStep - 1, RgbColor(
                currentR * scale, currentG * scale, currentB * scale
            ));
        }
        strip.SetPixelColor(animStep, RgbColor(
            255 * scale, 255 * scale, 255 * scale
        ));
        strip.Show();
        animStep++;
    } else {
        // Ignition complete, switch to target mode
        currentMode = (pendingMode != MODE_OFF && pendingMode != MODE_IGNITE)
                      ? pendingMode : MODE_SOLID;
        animStep = 0;
        animComplete = true;
        // Start hum loop (only if ignite sound is done or wasn't playing)
        if (!audioPlaying) {
            startHum();
        }
    }
}

void animateRetract() {
    unsigned long now = millis();
    if (now - lastAnimUpdate < 8) return;
    lastAnimUpdate = now;

    int pos = NUM_LEDS - 1 - animStep;
    if (pos >= 0) {
        strip.SetPixelColor(pos, RgbColor(0, 0, 0));
        // Dim trailing edge
        if (pos + 1 < NUM_LEDS) {
            strip.SetPixelColor(pos + 1, RgbColor(0, 0, 0));
        }
        strip.Show();
        animStep++;
    } else {
        currentMode = MODE_OFF;
        animStep = 0;
        animComplete = true;
        clearStrip();
        stopSound();
    }
}

void updateAnimation() {
    switch (currentMode) {
        case MODE_OFF:     clearStrip(); break;
        case MODE_SOLID:   animateSolid(); break;
        case MODE_PULSE:   animatePulse(); break;
        case MODE_RAINBOW: animateRainbow(); break;
        case MODE_FIRE:    animateFire(); break;
        case MODE_CLASH:   animateClash(); break;
        case MODE_IGNITE:  animateIgnite(); break;
        case MODE_RETRACT: animateRetract(); break;
        default: break;
    }
}

// ============================================================
// MODE SWITCHING WITH ANIMATIONS
// ============================================================
void setMode(BladeMode newMode) {
    if (newMode == currentMode) return;

    animStep = 0;
    animComplete = false;

    // Turning on from off → play ignition
    if (currentMode == MODE_OFF && newMode != MODE_OFF && newMode != MODE_RETRACT) {
        pendingMode = newMode;
        currentMode = MODE_IGNITE;
        playSound("/ignite.wav", false);
        return;
    }

    // Turning off → play retraction
    if (newMode == MODE_OFF && currentMode != MODE_OFF) {
        currentMode = MODE_RETRACT;
        playSound("/retract.wav", false);
        return;
    }

    currentMode = newMode;
}

// ============================================================
// DEEP SLEEP
// ============================================================
void enterDeepSleep() {
    Serial.println("Entering deep sleep...");
    stopSound();
    clearStrip();
    digitalWrite(AMP_SD_PIN, LOW);

    // Disable WiFi and BLE
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    BLEDevice::deinit(false);

    // Configure GPIO0 as RTC wakeup source (LOW = button pressed)
    esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_PIN, 0);

    Serial.println("Good night!");
    Serial.flush();
    esp_deep_sleep_start();
}

// ============================================================
// BUTTON HANDLING
// ============================================================
void handleButton() {
    bool pressed = (digitalRead(BUTTON_PIN) == LOW);
    unsigned long now = millis();

    if (pressed && buttonPressStart == 0) {
        // Button just pressed — record start time
        buttonPressStart = now;
        longPressHandled = false;
    }

    if (pressed && !longPressHandled && buttonPressStart > 0) {
        // Check for long press while held
        if (now - buttonPressStart >= LONG_PRESS_MS) {
            longPressHandled = true;

            if (currentMode != MODE_OFF) {
                // Blade is on: retract then sleep
                Serial.println("Long press: retract → deep sleep");
                animStep = 0;
                animComplete = false;
                currentMode = MODE_RETRACT;
                playSound("/retract.wav", false);
                // Wait for retract animation to finish
                while (!animComplete) {
                    updateAnimation();
                    feedAudio();
                    delay(1);
                }
                enterDeepSleep();
            } else {
                // Blade is off: sleep immediately
                Serial.println("Long press: deep sleep");
                enterDeepSleep();
            }
        }
    }

    if (!pressed && buttonPressStart > 0) {
        // Button released
        if (!longPressHandled && (now - lastButtonPress > 300)) {
            // Short press: cycle modes
            lastButtonPress = now;

            switch (currentMode) {
                case MODE_OFF:     setMode(MODE_SOLID); break;
                case MODE_SOLID:   setMode(MODE_PULSE); break;
                case MODE_PULSE:   setMode(MODE_RAINBOW); break;
                case MODE_RAINBOW: setMode(MODE_FIRE); break;
                case MODE_FIRE:    setMode(MODE_OFF); break;
                case MODE_IGNITE:
                case MODE_RETRACT:
                    currentMode = MODE_OFF;
                    clearStrip();
                    stopSound();
                    break;
                default:           setMode(MODE_OFF); break;
            }

            Serial.printf("Mode: %s\n", modeNames[currentMode]);
        }
        buttonPressStart = 0;
        longPressHandled = false;
    }
}

// ============================================================
// SAVE / LOAD SETTINGS
// ============================================================
void saveSettings() {
    prefs.begin("saber", false);
    prefs.putUChar("r", currentR);
    prefs.putUChar("g", currentG);
    prefs.putUChar("b", currentB);
    prefs.putUChar("bright", brightness);
    prefs.putUChar("vol", volume);
    prefs.end();
}

void loadSettings() {
    prefs.begin("saber", true);
    currentR = prefs.getUChar("r", 0);
    currentG = prefs.getUChar("g", 255);
    currentB = prefs.getUChar("b", 0);
    brightness = prefs.getUChar("bright", LED_BRIGHTNESS);
    volume = prefs.getUChar("vol", DEFAULT_VOLUME);
    prefs.end();
}

// ============================================================
// BLE CALLBACKS
// ============================================================
class ServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* s) override {
        bleConnected = true;
        Serial.println("BLE: Client connected");
    }
    void onDisconnect(BLEServer* s) override {
        bleConnected = false;
        Serial.println("BLE: Client disconnected");
        BLEDevice::startAdvertising();
    }
};

class ColorCallback : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* pChar) override {
        String val = pChar->getValue();
        if (val.length() >= 3) {
            currentR = val[0];
            currentG = val[1];
            currentB = val[2];
            saveSettings();
            Serial.printf("BLE Color: R=%d G=%d B=%d\n", currentR, currentG, currentB);
        }
    }
};

class ModeCallback : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* pChar) override {
        String val = pChar->getValue();
        if (val.length() >= 1) {
            uint8_t mode = val[0];
            if (mode < MODE_COUNT) {
                setMode((BladeMode)mode);
                Serial.printf("BLE Mode: %s\n", modeNames[mode]);
            }
        }
    }
};

class BrightnessCallback : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* pChar) override {
        String val = pChar->getValue();
        if (val.length() >= 1) {
            brightness = constrain(val[0], MIN_BRIGHTNESS, MAX_BRIGHTNESS);
            saveSettings();
            Serial.printf("BLE Brightness: %d\n", brightness);
        }
    }
};

class VolumeCallback : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* pChar) override {
        String val = pChar->getValue();
        if (val.length() >= 1) {
            volume = val[0];
            saveSettings();
            Serial.printf("BLE Volume: %d\n", volume);
        }
    }
};

void setupBLE() {
    BLEDevice::init(BLE_DEVICE_NAME);
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new ServerCallbacks());

    BLEService* pService = pServer->createService(SERVICE_UUID);

    // Color characteristic (write: 3 bytes RGB)
    pColorChar = pService->createCharacteristic(
        CHAR_COLOR_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE
    );
    pColorChar->setCallbacks(new ColorCallback());

    // Mode characteristic (write: 1 byte)
    pModeChar = pService->createCharacteristic(
        CHAR_MODE_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE
    );
    pModeChar->setCallbacks(new ModeCallback());

    // Brightness characteristic (write: 1 byte)
    pBrightChar = pService->createCharacteristic(
        CHAR_BRIGHT_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE
    );
    pBrightChar->setCallbacks(new BrightnessCallback());

    // Volume characteristic (write: 1 byte, 0-255)
    pVolumeChar = pService->createCharacteristic(
        CHAR_VOLUME_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE
    );
    pVolumeChar->setCallbacks(new VolumeCallback());

    // Battery level characteristic (read/notify)
    pBatteryChar = pService->createCharacteristic(
        CHAR_BATTERY_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );
    pBatteryChar->addDescriptor(new BLE2902());

    pService->start();

    BLEAdvertising* pAdv = BLEDevice::getAdvertising();
    pAdv->addServiceUUID(SERVICE_UUID);
    pAdv->setScanResponse(true);
    pAdv->setMinPreferred(0x06);
    BLEDevice::startAdvertising();

    Serial.println("BLE: Advertising as '" BLE_DEVICE_NAME "'");
}

// ============================================================
// WEB SERVER
// ============================================================
const char HTML_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Light-Saber</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    font-family: -apple-system, sans-serif;
    background: #0a0a0a;
    color: #e0e0e0;
    padding: 16px;
    max-width: 420px;
    margin: 0 auto;
  }
  h1 {
    text-align: center;
    font-size: 22px;
    margin-bottom: 16px;
    color: #fff;
    text-shadow: 0 0 20px rgba(0,255,0,0.5);
  }
  .battery {
    text-align: center;
    padding: 8px;
    margin-bottom: 12px;
    border-radius: 8px;
    background: #1a1a1a;
    font-size: 14px;
  }
  .battery.low { color: #ff4444; }
  .battery.mid { color: #ffaa00; }
  .battery.ok  { color: #44ff44; }
  .section {
    background: #1a1a1a;
    border-radius: 10px;
    padding: 14px;
    margin-bottom: 12px;
  }
  .section h2 { font-size: 14px; color: #888; margin-bottom: 10px; text-transform: uppercase; }
  .color-grid {
    display: grid;
    grid-template-columns: repeat(5, 1fr);
    gap: 8px;
  }
  .color-btn {
    width: 100%; aspect-ratio: 1;
    border: 2px solid transparent;
    border-radius: 50%;
    cursor: pointer;
  }
  .color-btn.active { border-color: #fff; box-shadow: 0 0 10px rgba(255,255,255,0.4); }
  input[type=color] {
    width: 100%; height: 44px;
    border: none; border-radius: 8px;
    cursor: pointer; margin-top: 8px;
    background: #333;
  }
  .mode-grid {
    display: grid;
    grid-template-columns: repeat(3, 1fr);
    gap: 8px;
  }
  .mode-btn {
    padding: 10px 4px;
    border: 1px solid #333;
    border-radius: 8px;
    background: #222;
    color: #ccc;
    font-size: 13px;
    cursor: pointer;
    text-align: center;
  }
  .mode-btn.active { background: #2a4a2a; border-color: #4a8a4a; color: #fff; }
  .slider-row { display: flex; align-items: center; gap: 10px; }
  .slider-row input[type=range] { flex: 1; accent-color: #4a8a4a; }
  .slider-row span { min-width: 36px; text-align: right; font-size: 14px; }
  .power-btn {
    display: block;
    width: 100%;
    padding: 14px;
    margin-top: 12px;
    border: none;
    border-radius: 10px;
    font-size: 18px;
    font-weight: bold;
    cursor: pointer;
    transition: background 0.2s;
  }
  .power-btn.on  { background: #2a4a2a; color: #4f4; }
  .power-btn.off { background: #4a2a2a; color: #f44; }
</style>
</head>
<body>
<h1>Light-Saber</h1>

<div id="batt" class="battery ok">Battery: --</div>

<div class="section">
  <h2>Color</h2>
  <div class="color-grid" id="colors"></div>
  <input type="color" id="customColor" value="#00ff00">
</div>

<div class="section">
  <h2>Mode</h2>
  <div class="mode-grid" id="modes"></div>
</div>

<div class="section">
  <h2>Brightness</h2>
  <div class="slider-row">
    <input type="range" id="bright" min="5" max="255" value="180">
    <span id="brightVal">180</span>
  </div>
</div>

<div class="section">
  <h2>Volume</h2>
  <div class="slider-row">
    <input type="range" id="vol" min="0" max="255" value="180">
    <span id="volVal">180</span>
  </div>
</div>

<button id="powerBtn" class="power-btn off" onclick="togglePower()">IGNITE</button>

<script>
const presets = [
  '#00ff00','#ff0000','#0000ff','#ff8800','#ffff00',
  '#00ffff','#ff00ff','#ffffff','#8800ff','#ff0044',
  '#00ff88','#4488ff','#ff4400','#88ff00','#ff0088'
];
const modeNames = ['Solid','Pulse','Rainbow','Fire','Clash'];
const modeIds   = [1, 2, 3, 4, 5];

let activeColor = '#00ff00';
let activeMode = 1;
let isOn = false;

// Build color buttons
const cg = document.getElementById('colors');
presets.forEach(c => {
  const b = document.createElement('div');
  b.className = 'color-btn' + (c === activeColor ? ' active' : '');
  b.style.background = c;
  b.onclick = () => pickColor(c, b);
  cg.appendChild(b);
});

// Build mode buttons
const mg = document.getElementById('modes');
modeNames.forEach((name, i) => {
  const b = document.createElement('button');
  b.className = 'mode-btn' + (modeIds[i] === activeMode ? ' active' : '');
  b.textContent = name;
  b.onclick = () => pickMode(modeIds[i], b);
  mg.appendChild(b);
});

document.getElementById('customColor').addEventListener('input', e => {
  pickColor(e.target.value, null);
});

document.getElementById('bright').addEventListener('input', e => {
  document.getElementById('brightVal').textContent = e.target.value;
});
document.getElementById('bright').addEventListener('change', e => {
  fetch('/bright?v=' + e.target.value);
});

document.getElementById('vol').addEventListener('input', e => {
  document.getElementById('volVal').textContent = e.target.value;
});
document.getElementById('vol').addEventListener('change', e => {
  fetch('/volume?v=' + e.target.value);
});

function pickColor(hex, btn) {
  activeColor = hex;
  document.querySelectorAll('.color-btn').forEach(b => b.classList.remove('active'));
  if (btn) btn.classList.add('active');
  document.getElementById('customColor').value = hex;
  const r = parseInt(hex.slice(1,3),16);
  const g = parseInt(hex.slice(3,5),16);
  const b2 = parseInt(hex.slice(5,7),16);
  fetch('/color?r='+r+'&g='+g+'&b='+b2);
}

function pickMode(id, btn) {
  activeMode = id;
  document.querySelectorAll('.mode-btn').forEach(b => b.classList.remove('active'));
  btn.classList.add('active');
  if (isOn) fetch('/mode?m=' + id);
}

function togglePower() {
  isOn = !isOn;
  const btn = document.getElementById('powerBtn');
  if (isOn) {
    btn.className = 'power-btn on';
    btn.textContent = 'RETRACT';
    fetch('/mode?m=' + activeMode);
  } else {
    btn.className = 'power-btn off';
    btn.textContent = 'IGNITE';
    fetch('/mode?m=0');
  }
}

// Poll battery
function updateBattery() {
  fetch('/battery').then(r => r.json()).then(d => {
    const el = document.getElementById('batt');
    el.textContent = 'Battery: ' + d.voltage.toFixed(2) + 'V (' + d.percent + '%)';
    el.className = 'battery ' + (d.percent < 15 ? 'low' : d.percent < 40 ? 'mid' : 'ok');
  }).catch(() => {});
}
updateBattery();
setInterval(updateBattery, 10000);
</script>
</body>
</html>
)rawliteral";

void setupWebServer() {
    WiFi.softAP(ap_ssid, ap_password);
    Serial.print("WiFi AP: ");
    Serial.println(ap_ssid);
    Serial.print("IP: ");
    Serial.println(WiFi.softAPIP());

    server.on("/", []() {
        server.send(200, "text/html", HTML_PAGE);
    });

    server.on("/color", []() {
        if (server.hasArg("r") && server.hasArg("g") && server.hasArg("b")) {
            currentR = server.arg("r").toInt();
            currentG = server.arg("g").toInt();
            currentB = server.arg("b").toInt();
            saveSettings();
        }
        server.send(200, "text/plain", "OK");
    });

    server.on("/mode", []() {
        if (server.hasArg("m")) {
            uint8_t m = server.arg("m").toInt();
            if (m < MODE_COUNT) {
                setMode((BladeMode)m);
            }
        }
        server.send(200, "text/plain", "OK");
    });

    server.on("/bright", []() {
        if (server.hasArg("v")) {
            brightness = constrain(server.arg("v").toInt(), MIN_BRIGHTNESS, MAX_BRIGHTNESS);
            saveSettings();
        }
        server.send(200, "text/plain", "OK");
    });

    server.on("/volume", []() {
        if (server.hasArg("v")) {
            volume = constrain(server.arg("v").toInt(), 0, 255);
            saveSettings();
        }
        server.send(200, "text/plain", "OK");
    });

    server.on("/battery", []() {
        String json = "{\"voltage\":" + String(batteryVoltage, 2) +
                      ",\"percent\":" + String(batteryPercent) + "}";
        server.send(200, "application/json", json);
    });

    server.on("/status", []() {
        String json = "{\"mode\":" + String(currentMode) +
                      ",\"r\":" + String(currentR) +
                      ",\"g\":" + String(currentG) +
                      ",\"b\":" + String(currentB) +
                      ",\"brightness\":" + String(brightness) +
                      ",\"volume\":" + String(volume) +
                      ",\"battery\":" + String(batteryPercent) + "}";
        server.send(200, "application/json", json);
    });

    server.begin();
    Serial.println("Web server started on port 80");
}

// ============================================================
// SETUP
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=========================");
    Serial.println("  Light-Saber Controller");
    Serial.println("  ESP32-S3 Super Mini");
    Serial.println("=========================");

    // Print wake reason
    esp_sleep_wakeup_cause_t wakeReason = esp_sleep_get_wakeup_cause();
    if (wakeReason == ESP_SLEEP_WAKEUP_EXT0) {
        Serial.println("Wake from deep sleep (button press)");
    } else {
        Serial.println("Normal boot");
    }

    // Pin setup
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);

    // Load saved settings
    loadSettings();

    // Init LED strip
    strip.Begin();
    clearStrip();
    Serial.printf("LEDs: %d on GPIO%d\n", NUM_LEDS, LED_DATA_PIN);

    // Init audio (I2S + LittleFS)
    setupAudio();

    // Init MPU6050
    setupMPU();

    // Read initial battery level
    readBattery();

    // Start BLE
    setupBLE();

    // Start WiFi + Web Server
    setupWebServer();

    Serial.println("Ready!");
}

// ============================================================
// MAIN LOOP
// ============================================================
void loop() {
    server.handleClient();
    handleButton();
    updateAnimation();
    processMotion();
    feedAudio();

    // Periodic battery check
    if (millis() - lastBatteryRead >= BATT_READ_INTERVAL) {
        lastBatteryRead = millis();
        readBattery();
    }

    delay(1);  // Yield to RTOS
}
