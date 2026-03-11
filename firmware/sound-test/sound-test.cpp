/*
 * MAX98357A Sound Test
 * Generates a sine wave tone to verify I2S wiring.
 *
 * Wiring (ESP32-S3 Super Mini → MAX98357A):
 *   GPIO4  → BCLK
 *   GPIO5  → LRC (Word Select)
 *   GPIO6  → DIN (Data In)
 *   GPIO7  → SD  (Shutdown - HIGH=enable)
 *   VIN    → VIN (5V)
 *   GND    → GND
 *
 * Expected: You should hear a 440Hz tone (A4 note) from the speaker.
 * The tone cycles through different frequencies every 2 seconds.
 */

#include <Arduino.h>
#include <driver/i2s.h>
#include <math.h>

#ifndef PI
#define PI M_PI
#endif

// Pin definitions - match your wiring
#define I2S_BCLK_PIN    4
#define I2S_LRC_PIN     5
#define I2S_DOUT_PIN    6
#define AMP_SD_PIN      7

#define SAMPLE_RATE     22050
#define BUFFER_SIZE     256
#define I2S_PORT        I2S_NUM_0

int16_t sampleBuf[BUFFER_SIZE];
float phase = 0.0;

// Test frequencies to cycle through
const float testFreqs[] = {440.0, 880.0, 262.0, 523.0, 330.0};
const char* freqNames[] = {"A4 (440Hz)", "A5 (880Hz)", "C4 (262Hz)", "C5 (523Hz)", "E4 (330Hz)"};
const int numFreqs = 5;
int currentFreq = 0;
unsigned long lastSwitch = 0;

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("================================");
    Serial.println("  MAX98357A Sound Test");
    Serial.println("================================");
    Serial.printf("BCLK: GPIO%d\n", I2S_BCLK_PIN);
    Serial.printf("LRC:  GPIO%d\n", I2S_LRC_PIN);
    Serial.printf("DIN:  GPIO%d\n", I2S_DOUT_PIN);
    Serial.printf("SD:   GPIO%d\n", AMP_SD_PIN);
    Serial.println();

    // Enable amplifier
    pinMode(AMP_SD_PIN, OUTPUT);
    digitalWrite(AMP_SD_PIN, HIGH);
    Serial.println("Amplifier enabled (SD pin HIGH)");

    // Configure I2S - use stereo so data goes on BOTH L+R channels
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,
        .dma_buf_len = BUFFER_SIZE,
        .use_apll = false,
        .tx_desc_auto_clear = true,
    };

    i2s_pin_config_t pin_config = {
        .mck_io_num = I2S_PIN_NO_CHANGE,
        .bck_io_num = I2S_BCLK_PIN,
        .ws_io_num = I2S_LRC_PIN,
        .data_out_num = I2S_DOUT_PIN,
        .data_in_num = I2S_PIN_NO_CHANGE,
    };

    esp_err_t err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("I2S driver install FAILED: %d\n", err);
        return;
    }

    err = i2s_set_pin(I2S_PORT, &pin_config);
    if (err != ESP_OK) {
        Serial.printf("I2S set pin FAILED: %d\n", err);
        return;
    }

    i2s_zero_dma_buffer(I2S_PORT);
    Serial.println("I2S initialized OK");
    Serial.println();
    Serial.println("You should hear tones from the speaker.");
    Serial.println("Cycling through frequencies every 2 seconds...");
    Serial.println();
    Serial.printf(">> Playing: %s\n", freqNames[0]);

    lastSwitch = millis();
}

void loop() {
    // Switch frequency every 2 seconds
    if (millis() - lastSwitch > 2000) {
        currentFreq = (currentFreq + 1) % numFreqs;
        Serial.printf(">> Playing: %s\n", freqNames[currentFreq]);
        lastSwitch = millis();
    }

    float freq = testFreqs[currentFreq];
    float phaseInc = freq / SAMPLE_RATE;

    // Generate sine wave
    for (int i = 0; i < BUFFER_SIZE; i++) {
        float sample = sin(phase * 2.0 * PI);
        sampleBuf[i] = (int16_t)(sample * 30000);  // ~92% volume
        phase += phaseInc;
        if (phase >= 1.0) phase -= 1.0;
    }

    // Write to I2S
    size_t bytesWritten = 0;
    i2s_write(I2S_PORT, sampleBuf, BUFFER_SIZE * sizeof(int16_t), &bytesWritten, portMAX_DELAY);
}
