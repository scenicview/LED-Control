/*
 * RGB LED Controller for Waveshare ESP32-C6
 * Controls WS2812B LEDs via BLE + WiFi Web Server + Physical Buttons
 *
 * Waveshare ESP32-C6 Pinout:
 *   LED Strip:
 *     5V  -> 5V
 *     GND -> GND
 *     DI  -> GPIO1
 *
 *   Button 1 (On/Off + Mode):
 *     COM -> GPIO4
 *     NO  -> GND
 *
 *   Button 2 (Color Cycle):
 *     COM -> GPIO5
 *     NO  -> GND
 *
 * Control Methods:
 *   1. Buttons - Button 1: Long press=On/Off, Short press=Mode cycle
 *                Button 2: Short press=Color cycle
 *   2. BLE - Use the Android app (search for "Games Room LED")
 *   3. WiFi - Connect to "Games Room LED" network, open http://192.168.4.1
 */

#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <WebServer.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// WiFi AP Settings
const char* ap_ssid = "Games Room LED";
const char* ap_password = "12345678";
WebServer server(80);

// LED Configuration - GPIO1 for Waveshare ESP32-C6
#define LED_PIN 1
#define NUM_LEDS 60

// Button Configuration - GPIO4 and GPIO5 are general purpose on C6
#define BTN1_PIN 4  // On/Off (long press) + Mode (short press)
#define BTN2_PIN 5  // Color cycle

#define DEBOUNCE_MS 50
#define LONG_PRESS_MS 800

// BLE UUIDs
#define SERVICE_UUID        "19B10000-E8F2-537E-4F6C-D104768A1214"
#define COLOR_CHAR_UUID     "19B10001-E8F2-537E-4F6C-D104768A1214"
#define MODE_CHAR_UUID      "19B10002-E8F2-537E-4F6C-D104768A1214"
#define BRIGHTNESS_CHAR_UUID "19B10003-E8F2-537E-4F6C-D104768A1214"

// Modes
enum LedMode {
  MODE_NORMAL = 0,
  MODE_PULSE = 1,
  MODE_STROBE = 2,
  MODE_RAINBOW = 3,
  MODE_COUNT = 4
};

// Preset colors (RGB)
const uint8_t presetColors[][3] = {
  {255, 0, 0},     // Red
  {255, 69, 0},    // Orange Red
  {255, 165, 0},   // Orange
  {255, 255, 0},   // Yellow
  {173, 255, 47},  // Green Yellow
  {0, 255, 0},     // Green
  {0, 250, 154},   // Medium Spring Green
  {0, 255, 255},   // Cyan
  {30, 144, 255},  // Dodger Blue
  {0, 0, 255},     // Blue
  {138, 43, 226},  // Blue Violet
  {255, 0, 255},   // Magenta
  {255, 20, 147},  // Deep Pink
  {255, 255, 255}, // White
  {255, 215, 0}    // Gold
};
const int numPresetColors = sizeof(presetColors) / sizeof(presetColors[0]);

// Global state
Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);
BLEServer* pServer = nullptr;
BLECharacteristic* pColorChar = nullptr;
BLECharacteristic* pModeChar = nullptr;
BLECharacteristic* pBrightnessChar = nullptr;

bool deviceConnected = false;
bool oldDeviceConnected = false;

uint8_t currentR = 255;
uint8_t currentG = 0;
uint8_t currentB = 0;
uint8_t currentBrightness = 128;
LedMode currentMode = MODE_NORMAL;
bool ledsOn = true;
int currentColorIndex = 0;

// Animation state
unsigned long lastUpdate = 0;
int pulseValue = 0;
int pulseDirection = 1;
bool strobeState = false;
uint16_t rainbowHue = 0;

// Button state
struct ButtonState {
  bool lastState;
  bool currentState;
  unsigned long pressedTime;
  unsigned long releasedTime;
  bool isPressed;
  bool longPressHandled;
};

ButtonState btn1 = {HIGH, HIGH, 0, 0, false, false};
ButtonState btn2 = {HIGH, HIGH, 0, 0, false, false};

// BLE Server Callbacks
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    deviceConnected = true;
    Serial.println("BLE Client connected");
  }

  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    Serial.println("BLE Client disconnected");
  }
};

// Color Characteristic Callback
class ColorCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) {
    String value = pCharacteristic->getValue();
    if (value.length() >= 3) {
      currentR = (uint8_t)value[0];
      currentG = (uint8_t)value[1];
      currentB = (uint8_t)value[2];
      Serial.printf("BLE: Color set R=%d G=%d B=%d\n", currentR, currentG, currentB);
    }
  }
};

// Mode Characteristic Callback
class ModeCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) {
    String value = pCharacteristic->getValue();
    if (value.length() >= 1) {
      currentMode = (LedMode)((uint8_t)value[0] % MODE_COUNT);
      pulseValue = 0;
      pulseDirection = 1;
      Serial.printf("BLE: Mode set %d\n", currentMode);
    }
  }
};

// Brightness Characteristic Callback
class BrightnessCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) {
    String value = pCharacteristic->getValue();
    if (value.length() >= 1) {
      currentBrightness = (uint8_t)value[0];
      strip.setBrightness(currentBrightness);
      Serial.printf("BLE: Brightness set %d\n", currentBrightness);
    }
  }
};

// Web page HTML
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Games Room LED</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, sans-serif;
      background: #1a1a2e; color: white; padding: 20px;
      min-height: 100vh;
    }
    h1 { text-align: center; margin-bottom: 30px; font-size: 24px; }
    .section { background: #16213e; border-radius: 12px; padding: 20px; margin-bottom: 20px; }
    .section h2 { font-size: 16px; margin-bottom: 15px; color: #888; }
    .color-grid { display: grid; grid-template-columns: repeat(5, 1fr); gap: 10px; }
    .color-btn {
      aspect-ratio: 1; border-radius: 50%; border: 3px solid transparent;
      cursor: pointer; transition: transform 0.2s, border-color 0.2s;
    }
    .color-btn:hover { transform: scale(1.1); }
    .color-btn.active { border-color: white; }
    .custom-color { display: flex; gap: 10px; margin-top: 15px; align-items: center; }
    .custom-color input[type="color"] {
      width: 60px; height: 40px; border: none; border-radius: 8px; cursor: pointer;
    }
    .custom-color button {
      flex: 1; padding: 12px; background: #0f3460; border: none; border-radius: 8px;
      color: white; font-size: 16px; cursor: pointer;
    }
    .mode-btns { display: grid; grid-template-columns: repeat(2, 1fr); gap: 10px; }
    .mode-btn {
      padding: 15px; border: none; border-radius: 8px;
      font-size: 14px; cursor: pointer; transition: background 0.2s;
      background: #0f3460; color: white;
    }
    .mode-btn.active { background: #e94560; }
    .slider-container { margin-top: 15px; }
    .slider-container input { width: 100%; height: 8px; -webkit-appearance: none;
      background: #0f3460; border-radius: 4px; outline: none; }
    .slider-container input::-webkit-slider-thumb {
      -webkit-appearance: none; width: 24px; height: 24px;
      background: #e94560; border-radius: 50%; cursor: pointer;
    }
    .preview {
      height: 60px; border-radius: 8px; margin-bottom: 20px;
      background: rgb(255, 0, 0); transition: background 0.3s;
    }
    .power-btn {
      width: 100%; padding: 20px; font-size: 18px; border: none; border-radius: 8px;
      cursor: pointer; transition: background 0.2s;
      background: #e94560; color: white;
    }
    .power-btn.off { background: #333; }
    .info { text-align: center; color: #666; font-size: 12px; margin-top: 20px; }
  </style>
</head>
<body>
  <h1>Games Room LED</h1>
  <div class="preview" id="preview"></div>

  <div class="section">
    <button class="power-btn" id="powerBtn" onclick="togglePower()">POWER ON</button>
  </div>

  <div class="section">
    <h2>COLOR</h2>
    <div class="color-grid" id="colors"></div>
    <div class="custom-color">
      <input type="color" id="customColor" value="#ff0000">
      <button onclick="applyCustom()">Apply Custom</button>
    </div>
  </div>

  <div class="section">
    <h2>MODE</h2>
    <div class="mode-btns">
      <button class="mode-btn active" onclick="setMode(0, this)">Normal</button>
      <button class="mode-btn" onclick="setMode(1, this)">Pulse</button>
      <button class="mode-btn" onclick="setMode(2, this)">Strobe</button>
      <button class="mode-btn" onclick="setMode(3, this)">Rainbow</button>
    </div>
  </div>

  <div class="section">
    <h2>BRIGHTNESS</h2>
    <div class="slider-container">
      <input type="range" min="5" max="255" value="128" id="brightness" onchange="setBrightness(this.value)">
    </div>
  </div>

  <p class="info">Games Room LED Controller</p>

  <script>
    const colors = [
      '#FF0000', '#FF4500', '#FFA500', '#FFFF00', '#ADFF2F',
      '#00FF00', '#00FA9A', '#00FFFF', '#1E90FF', '#0000FF',
      '#8A2BE2', '#FF00FF', '#FF1493', '#FFFFFF', '#FFD700'
    ];

    let powerOn = true;

    const grid = document.getElementById('colors');
    colors.forEach((c, i) => {
      const btn = document.createElement('button');
      btn.className = 'color-btn' + (i === 0 ? ' active' : '');
      btn.style.background = c;
      btn.onclick = () => setColor(c, btn);
      grid.appendChild(btn);
    });

    function setColor(hex, btn) {
      document.querySelectorAll('.color-btn').forEach(b => b.classList.remove('active'));
      if(btn) btn.classList.add('active');
      document.getElementById('preview').style.background = hex;
      document.getElementById('customColor').value = hex;
      const r = parseInt(hex.slice(1,3), 16);
      const g = parseInt(hex.slice(3,5), 16);
      const b = parseInt(hex.slice(5,7), 16);
      fetch('/color?r=' + r + '&g=' + g + '&b=' + b);
    }

    function applyCustom() {
      const hex = document.getElementById('customColor').value;
      document.querySelectorAll('.color-btn').forEach(b => b.classList.remove('active'));
      setColor(hex, null);
    }

    function setMode(m, btn) {
      document.querySelectorAll('.mode-btn').forEach(b => b.classList.remove('active'));
      btn.classList.add('active');
      fetch('/mode?m=' + m);
    }

    function setBrightness(v) {
      fetch('/brightness?b=' + v);
    }

    function togglePower() {
      powerOn = !powerOn;
      const btn = document.getElementById('powerBtn');
      btn.textContent = powerOn ? 'POWER ON' : 'POWER OFF';
      btn.classList.toggle('off', !powerOn);
      fetch('/power?on=' + (powerOn ? '1' : '0'));
    }
  </script>
</body>
</html>
)rawliteral";

void handleRoot() {
  server.send(200, "text/html", index_html);
}

void handleColor() {
  if (server.hasArg("r") && server.hasArg("g") && server.hasArg("b")) {
    currentR = server.arg("r").toInt();
    currentG = server.arg("g").toInt();
    currentB = server.arg("b").toInt();
    Serial.printf("Web: Color set R=%d G=%d B=%d\n", currentR, currentG, currentB);
  }
  server.send(200, "text/plain", "OK");
}

void handleMode() {
  if (server.hasArg("m")) {
    currentMode = (LedMode)(server.arg("m").toInt() % MODE_COUNT);
    pulseValue = 0;
    pulseDirection = 1;
    Serial.printf("Web: Mode set %d\n", currentMode);
  }
  server.send(200, "text/plain", "OK");
}

void handleBrightness() {
  if (server.hasArg("b")) {
    currentBrightness = server.arg("b").toInt();
    strip.setBrightness(currentBrightness);
    Serial.printf("Web: Brightness set %d\n", currentBrightness);
  }
  server.send(200, "text/plain", "OK");
}

void handlePower() {
  if (server.hasArg("on")) {
    ledsOn = server.arg("on").toInt() == 1;
    Serial.printf("Web: Power %s\n", ledsOn ? "ON" : "OFF");
  }
  server.send(200, "text/plain", "OK");
}

void setupWiFi() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid, ap_password);

  IPAddress IP = WiFi.softAPIP();
  Serial.print("WiFi AP started - SSID: ");
  Serial.println(ap_ssid);
  Serial.print("Connect and browse to: http://");
  Serial.println(IP);

  server.on("/", handleRoot);
  server.on("/color", handleColor);
  server.on("/mode", handleMode);
  server.on("/brightness", handleBrightness);
  server.on("/power", handlePower);
  server.begin();
}

void setupBLE() {
  BLEDevice::init("Games Room LED");

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService* pService = pServer->createService(SERVICE_UUID);

  // Color characteristic (RGB - 3 bytes)
  pColorChar = pService->createCharacteristic(
    COLOR_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE
  );
  pColorChar->setCallbacks(new ColorCallbacks());
  uint8_t initColor[3] = {currentR, currentG, currentB};
  pColorChar->setValue(initColor, 3);

  // Mode characteristic (1 byte: 0=normal, 1=pulse, 2=strobe, 3=rainbow)
  pModeChar = pService->createCharacteristic(
    MODE_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE
  );
  pModeChar->setCallbacks(new ModeCallbacks());
  uint8_t initMode = currentMode;
  pModeChar->setValue(&initMode, 1);

  // Brightness characteristic (1 byte: 0-255)
  pBrightnessChar = pService->createCharacteristic(
    BRIGHTNESS_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE
  );
  pBrightnessChar->setCallbacks(new BrightnessCallbacks());
  pBrightnessChar->setValue(&currentBrightness, 1);

  pService->start();

  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  BLEDevice::startAdvertising();

  Serial.println("BLE advertising started - look for 'Games Room LED'");
}

void setupButtons() {
  pinMode(BTN1_PIN, INPUT_PULLUP);
  pinMode(BTN2_PIN, INPUT_PULLUP);
  Serial.println("Buttons initialized on GPIO4 and GPIO5");
}

void handleButtons() {
  unsigned long currentMillis = millis();

  // Read button states (LOW = pressed due to INPUT_PULLUP)
  bool btn1Reading = digitalRead(BTN1_PIN);
  bool btn2Reading = digitalRead(BTN2_PIN);

  // Button 1: Long press = On/Off, Short press = Mode cycle
  if (btn1Reading != btn1.lastState) {
    btn1.lastState = btn1Reading;

    if (btn1Reading == LOW) {
      // Button pressed
      btn1.pressedTime = currentMillis;
      btn1.isPressed = true;
      btn1.longPressHandled = false;
    } else {
      // Button released
      btn1.releasedTime = currentMillis;
      btn1.isPressed = false;

      // Check for short press (only if long press wasn't handled)
      if (!btn1.longPressHandled && (btn1.releasedTime - btn1.pressedTime) >= DEBOUNCE_MS) {
        // Short press - cycle mode
        currentMode = (LedMode)((currentMode + 1) % MODE_COUNT);
        pulseValue = 0;
        pulseDirection = 1;
        Serial.printf("Button: Mode changed to %d\n", currentMode);
      }
    }
  }

  // Check for long press while button is held
  if (btn1.isPressed && !btn1.longPressHandled) {
    if ((currentMillis - btn1.pressedTime) >= LONG_PRESS_MS) {
      // Long press - toggle on/off
      ledsOn = !ledsOn;
      btn1.longPressHandled = true;
      Serial.printf("Button: LEDs %s\n", ledsOn ? "ON" : "OFF");
    }
  }

  // Button 2: Short press = Color cycle
  if (btn2Reading != btn2.lastState) {
    btn2.lastState = btn2Reading;

    if (btn2Reading == LOW) {
      // Button pressed
      btn2.pressedTime = currentMillis;
      btn2.isPressed = true;
    } else {
      // Button released
      btn2.releasedTime = currentMillis;
      btn2.isPressed = false;

      // Short press - cycle color
      if ((btn2.releasedTime - btn2.pressedTime) >= DEBOUNCE_MS) {
        currentColorIndex = (currentColorIndex + 1) % numPresetColors;
        currentR = presetColors[currentColorIndex][0];
        currentG = presetColors[currentColorIndex][1];
        currentB = presetColors[currentColorIndex][2];
        Serial.printf("Button: Color changed to R=%d G=%d B=%d\n", currentR, currentG, currentB);
      }
    }
  }
}

void updateLEDs() {
  // If LEDs are off, clear and return
  if (!ledsOn) {
    strip.clear();
    strip.show();
    return;
  }

  unsigned long currentMillis = millis();

  switch (currentMode) {
    case MODE_NORMAL:
      // Solid color
      for (int i = 0; i < NUM_LEDS; i++) {
        strip.setPixelColor(i, strip.Color(currentR, currentG, currentB));
      }
      break;

    case MODE_PULSE:
      // Pulsing effect - smooth fade in/out
      if (currentMillis - lastUpdate > 10) {
        lastUpdate = currentMillis;
        pulseValue += pulseDirection * 3;
        if (pulseValue >= 255) {
          pulseValue = 255;
          pulseDirection = -1;
        } else if (pulseValue <= 0) {
          pulseValue = 0;
          pulseDirection = 1;
        }

        uint8_t r = (currentR * pulseValue) / 255;
        uint8_t g = (currentG * pulseValue) / 255;
        uint8_t b = (currentB * pulseValue) / 255;

        for (int i = 0; i < NUM_LEDS; i++) {
          strip.setPixelColor(i, strip.Color(r, g, b));
        }
      }
      break;

    case MODE_STROBE:
      // Strobe effect - fast on/off
      if (currentMillis - lastUpdate > 50) {
        lastUpdate = currentMillis;
        strobeState = !strobeState;

        for (int i = 0; i < NUM_LEDS; i++) {
          if (strobeState) {
            strip.setPixelColor(i, strip.Color(currentR, currentG, currentB));
          } else {
            strip.setPixelColor(i, strip.Color(0, 0, 0));
          }
        }
      }
      break;

    case MODE_RAINBOW:
      // Rainbow cycle effect
      if (currentMillis - lastUpdate > 20) {
        lastUpdate = currentMillis;
        rainbowHue += 256;
        if (rainbowHue >= 65536) rainbowHue = 0;

        for (int i = 0; i < NUM_LEDS; i++) {
          uint16_t pixelHue = rainbowHue + (i * 65536L / NUM_LEDS);
          strip.setPixelColor(i, strip.gamma32(strip.ColorHSV(pixelHue)));
        }
      }
      break;

    default:
      break;
  }

  strip.show();
}

void setup() {
  Serial.begin(115200);
  delay(1000);  // Give USB CDC time to initialize
  Serial.println("\n========================================");
  Serial.println("Waveshare ESP32-C6 RGB LED Controller");
  Serial.println("========================================");

  // Initialize LED strip
  strip.begin();
  strip.setBrightness(currentBrightness);
  strip.show();

  // Initialize buttons
  setupButtons();

  // Quick startup animation - green sweep
  Serial.println("Running startup animation...");
  for (int i = 0; i < NUM_LEDS; i++) {
    strip.setPixelColor(i, strip.Color(0, 50, 0));
    strip.show();
    delay(30);
  }
  delay(200);
  strip.clear();
  strip.show();

  // Initialize WiFi AP and web server
  setupWiFi();

  // Initialize BLE
  setupBLE();

  Serial.println("\n--- Ready! ---");
  Serial.println("Control options:");
  Serial.println("  Button 1 (GPIO4): Long press=On/Off, Short press=Mode");
  Serial.println("  Button 2 (GPIO5): Short press=Color cycle");
  Serial.printf("  WiFi: Connect to '%s' (pass: %s)\n", ap_ssid, ap_password);
  Serial.println("        Open http://192.168.4.1");
  Serial.println("  BLE: Search for 'Games Room LED' in the app");
  Serial.println("========================================\n");
}

void loop() {
  // Handle button inputs
  handleButtons();

  // Handle web server requests
  server.handleClient();

  // Handle BLE reconnection
  if (!deviceConnected && oldDeviceConnected) {
    delay(500);
    pServer->startAdvertising();
    Serial.println("BLE: Restarting advertising...");
    oldDeviceConnected = deviceConnected;
  }

  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = deviceConnected;
  }

  // Update LED animation
  updateLEDs();

  delay(1);
}
