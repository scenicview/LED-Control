/*
 * Rosie - RGB LED Controller
 * Board: ESP32-C3 Mini
 *
 * Wiring:
 *   5V  -> 5V
 *   GND -> GND
 *   LED DI     -> GPIO10
 *   Color Btn  -> GPIO4 (click to cycle colors)
 *   Mode Btn   -> GPIO5 (short press: cycle modes, long press 2s: power on/off)
 */

#include <Adafruit_NeoPixel.h>
#include <math.h>
#include <WiFi.h>
#include <WebServer.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// LED Configuration
#define LED_PIN 10
#define NUM_LEDS 28

// Button Configuration
#define BTN_COLOR_PIN 4    // GPIO4 - Color change button
#define BTN_MODE_PIN 5     // GPIO5 - Mode/Power button
#define LONG_PRESS_TIME 2000  // 2 seconds for long press
#define DEBOUNCE_TIME 50      // 50ms debounce

// WiFi AP Settings
const char* ap_ssid = "RGB-LED";
const char* ap_password = "12345678";
WebServer server(80);

// BLE UUIDs
#define SERVICE_UUID        "19B10000-E8F2-537E-4F6C-D104768A1214"
#define COLOR_CHAR_UUID     "19B10001-E8F2-537E-4F6C-D104768A1214"
#define MODE_CHAR_UUID      "19B10002-E8F2-537E-4F6C-D104768A1214"
#define BRIGHTNESS_CHAR_UUID "19B10003-E8F2-537E-4F6C-D104768A1214"
#define POWER_CHAR_UUID     "19B10004-E8F2-537E-4F6C-D104768A1214"

// Modes
enum LedMode {
  MODE_NORMAL = 0,
  MODE_PULSE = 1,
  MODE_STROBE = 2,
  MODE_PRINCESS = 3
};
#define NUM_MODES 4

// Princess colors (light purple, pink, light blue, white)
const uint8_t princessColors[][3] = {
  {200, 150, 255},  // Light purple
  {255, 150, 200},  // Pink
  {150, 200, 255},  // Light blue
  {255, 255, 255}   // White sparkle
};

// LED strip
Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

// BLE
BLEServer* pServer = nullptr;
BLECharacteristic* pColorChar = nullptr;
BLECharacteristic* pModeChar = nullptr;
BLECharacteristic* pBrightnessChar = nullptr;
BLECharacteristic* pPowerChar = nullptr;

bool deviceConnected = false;
bool oldDeviceConnected = false;

// State
uint8_t currentR = 255;
uint8_t currentG = 0;
uint8_t currentB = 0;
uint8_t currentBrightness = 255;  // Full brightness by default
LedMode currentMode = MODE_PRINCESS;
bool powerOn = true;

// Animation state
unsigned long lastUpdate = 0;
int pulseValue = 0;
int pulseDirection = 1;
bool strobeState = false;

// Button state
bool btn1LastState = HIGH;
bool btn2LastState = HIGH;
unsigned long btn1PressTime = 0;
unsigned long btn2PressTime = 0;
bool btn2LongPressHandled = false;

// Preset colors for cycling (R, G, B)
const uint8_t presetColors[][3] = {
  {255, 0, 0},     // Red
  {255, 69, 0},    // Orange Red
  {255, 165, 0},   // Orange
  {255, 255, 0},   // Yellow
  {0, 255, 0},     // Green
  {0, 255, 255},   // Cyan
  {0, 0, 255},     // Blue
  {139, 0, 255},   // Purple
  {255, 0, 255},   // Magenta
  {255, 255, 255}  // White
};
const int NUM_COLORS = 10;
int currentColorIndex = 0;


// BLE Callbacks
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    deviceConnected = true;
    Serial.println("BLE: Client connected");
  }

  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    Serial.println("BLE: Client disconnected");
  }
};

class ColorCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) {
    String value = pCharacteristic->getValue();
    if (value.length() >= 3) {
      currentR = (uint8_t)value[0];
      currentG = (uint8_t)value[1];
      currentB = (uint8_t)value[2];
      Serial.printf("BLE: Color R=%d G=%d B=%d\n", currentR, currentG, currentB);
    }
  }
};

class ModeCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) {
    String value = pCharacteristic->getValue();
    if (value.length() >= 1) {
      currentMode = (LedMode)((uint8_t)value[0] % NUM_MODES);
      pulseValue = 0;
      pulseDirection = 1;
      Serial.printf("BLE: Mode %d\n", currentMode);
    }
  }
};

class BrightnessCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) {
    String value = pCharacteristic->getValue();
    if (value.length() >= 1) {
      currentBrightness = (uint8_t)value[0];
      Serial.printf("BLE: Brightness %d\n", currentBrightness);
    }
  }
};

class PowerCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) {
    String value = pCharacteristic->getValue();
    if (value.length() >= 1) {
      powerOn = (uint8_t)value[0] > 0;
      Serial.printf("BLE: Power %s\n", powerOn ? "ON" : "OFF");
    }
  }
};

// Web page
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Rosie LED Control</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body { font-family: sans-serif; background: #1a1a2e; color: white; padding: 20px; }
    h1 { text-align: center; margin-bottom: 20px; }
    .section { background: #16213e; border-radius: 12px; padding: 15px; margin-bottom: 15px; }
    .color-grid { display: grid; grid-template-columns: repeat(5, 1fr); gap: 8px; }
    .color-btn { width: 100%; aspect-ratio: 1; border-radius: 50%; border: 3px solid transparent; cursor: pointer; }
    .color-btn.active { border-color: white; }
    .mode-btns { display: flex; gap: 10px; }
    .mode-btn { flex: 1; padding: 12px; border: none; border-radius: 8px; background: #0f3460; color: white; cursor: pointer; }
    .mode-btn.active { background: #e94560; }
    input[type="range"] { width: 100%; margin-top: 10px; }
    .preview { height: 50px; border-radius: 8px; margin-bottom: 15px; background: red; }
  </style>
</head>
<body>
  <h1>Rosie LED Controller</h1>
  <div class="preview" id="preview"></div>
  <div class="section">
    <div class="color-grid" id="colors"></div>
  </div>
  <div class="section">
    <div class="mode-btns">
      <button class="mode-btn active" onclick="setMode(0,this)">Normal</button>
      <button class="mode-btn" onclick="setMode(1,this)">Pulse</button>
      <button class="mode-btn" onclick="setMode(2,this)">Strobe</button>
      <button class="mode-btn" onclick="setMode(3,this)">Princess</button>
    </div>
  </div>
  <div class="section">
    <input type="range" min="5" max="255" value="128" onchange="fetch('/brightness?b='+this.value)">
  </div>
  <script>
    const colors=['#FF0000','#FF4500','#FFA500','#FFFF00','#00FF00','#00FFFF','#0000FF','#8B00FF','#FF00FF','#FFFFFF'];
    const grid=document.getElementById('colors');
    colors.forEach((c,i)=>{
      const btn=document.createElement('button');
      btn.className='color-btn'+(i===0?' active':'');
      btn.style.background=c;
      btn.onclick=()=>{
        document.querySelectorAll('.color-btn').forEach(b=>b.classList.remove('active'));
        btn.classList.add('active');
        document.getElementById('preview').style.background=c;
        const r=parseInt(c.slice(1,3),16),g=parseInt(c.slice(3,5),16),b=parseInt(c.slice(5,7),16);
        fetch('/color?r='+r+'&g='+g+'&b='+b);
      };
      grid.appendChild(btn);
    });
    function setMode(m,btn){
      document.querySelectorAll('.mode-btn').forEach(b=>b.classList.remove('active'));
      btn.classList.add('active');
      fetch('/mode?m='+m);
    }
  </script>
</body>
</html>
)rawliteral";

void handleRoot() { server.send(200, "text/html", index_html); }

void handleColor() {
  if (server.hasArg("r") && server.hasArg("g") && server.hasArg("b")) {
    currentR = server.arg("r").toInt();
    currentG = server.arg("g").toInt();
    currentB = server.arg("b").toInt();
    Serial.printf("Web: Color R=%d G=%d B=%d\n", currentR, currentG, currentB);
  }
  server.send(200, "text/plain", "OK");
}

void handleMode() {
  if (server.hasArg("m")) {
    currentMode = (LedMode)(server.arg("m").toInt() % NUM_MODES);
    pulseValue = 0;
    pulseDirection = 1;
  }
  server.send(200, "text/plain", "OK");
}

void handleBrightness() {
  if (server.hasArg("b")) {
    currentBrightness = server.arg("b").toInt();
  }
  server.send(200, "text/plain", "OK");
}

void setupWiFi() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid, ap_password);
  Serial.print("WiFi AP: ");
  Serial.println(ap_ssid);
  Serial.print("IP: ");
  Serial.println(WiFi.softAPIP());

  server.on("/", handleRoot);
  server.on("/color", handleColor);
  server.on("/mode", handleMode);
  server.on("/brightness", handleBrightness);
  server.begin();
}

void setupBLE() {
  BLEDevice::init("RGB-LED-Rosie");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService* pService = pServer->createService(SERVICE_UUID);

  pColorChar = pService->createCharacteristic(COLOR_CHAR_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  pColorChar->setCallbacks(new ColorCallbacks());
  uint8_t initColor[3] = {currentR, currentG, currentB};
  pColorChar->setValue(initColor, 3);

  pModeChar = pService->createCharacteristic(MODE_CHAR_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  pModeChar->setCallbacks(new ModeCallbacks());
  uint8_t initMode = currentMode;
  pModeChar->setValue(&initMode, 1);

  pBrightnessChar = pService->createCharacteristic(BRIGHTNESS_CHAR_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  pBrightnessChar->setCallbacks(new BrightnessCallbacks());
  pBrightnessChar->setValue(&currentBrightness, 1);

  pPowerChar = pService->createCharacteristic(POWER_CHAR_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  pPowerChar->setCallbacks(new PowerCallbacks());
  uint8_t initPower = powerOn ? 1 : 0;
  pPowerChar->setValue(&initPower, 1);

  pService->start();

  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  BLEDevice::startAdvertising();

  Serial.println("BLE: Advertising as RGB-LED-Rosie");
}

void updateLEDs() {
  if (!powerOn) {
    strip.clear();
    strip.show();
    return;
  }

  unsigned long now = millis();
  uint8_t r, g, b;

  switch (currentMode) {
    case MODE_NORMAL:
      r = (currentR * currentBrightness) / 255;
      g = (currentG * currentBrightness) / 255;
      b = (currentB * currentBrightness) / 255;
      for (int i = 0; i < NUM_LEDS; i++) {
        strip.setPixelColor(i, strip.Color(r, g, b));
      }
      break;

    case MODE_PULSE:
      if (now - lastUpdate > 15) {
        lastUpdate = now;
        pulseValue += pulseDirection * 5;
        if (pulseValue >= 255) { pulseValue = 255; pulseDirection = -1; }
        else if (pulseValue <= 0) { pulseValue = 0; pulseDirection = 1; }
      }
      r = (currentR * pulseValue * currentBrightness) / 65025;
      g = (currentG * pulseValue * currentBrightness) / 65025;
      b = (currentB * pulseValue * currentBrightness) / 65025;
      for (int i = 0; i < NUM_LEDS; i++) {
        strip.setPixelColor(i, strip.Color(r, g, b));
      }
      break;

    case MODE_STROBE:
      if (now - lastUpdate > 80) {
        lastUpdate = now;
        strobeState = !strobeState;
      }
      if (strobeState) {
        r = (currentR * currentBrightness) / 255;
        g = (currentG * currentBrightness) / 255;
        b = (currentB * currentBrightness) / 255;
        for (int i = 0; i < NUM_LEDS; i++) {
          strip.setPixelColor(i, strip.Color(r, g, b));
        }
      } else {
        strip.clear();
      }
      break;

    case MODE_PRINCESS:
      if (now - lastUpdate > 30) {
        lastUpdate = now;

        // Wave position that flows along the strip
        float wavePos = (now % 3000) / 3000.0 * NUM_LEDS;

        // Global pulse for breathing effect
        int breathe = 200 + 55 * sin(now / 800.0);

        for (int i = 0; i < NUM_LEDS; i++) {
          // Flowing color wave - each LED smoothly transitions through colors
          float colorPhase = fmod((float)i / 4.0 + now / 150.0, 4.0);
          int colorIndex1 = (int)colorPhase % 4;
          int colorIndex2 = (colorIndex1 + 1) % 4;
          float blend = colorPhase - (int)colorPhase;

          // Blend between two adjacent colors for smooth transitions
          uint8_t baseR = princessColors[colorIndex1][0] * (1 - blend) + princessColors[colorIndex2][0] * blend;
          uint8_t baseG = princessColors[colorIndex1][1] * (1 - blend) + princessColors[colorIndex2][1] * blend;
          uint8_t baseB = princessColors[colorIndex1][2] * (1 - blend) + princessColors[colorIndex2][2] * blend;

          // Add wave shimmer based on position
          float distFromWave = fabs(i - wavePos);
          if (distFromWave > NUM_LEDS / 2) distFromWave = NUM_LEDS - distFromWave;
          int waveBoost = max(0, (int)(80 * (1 - distFromWave / 8.0)));

          // Individual LED twinkle
          int twinkle = 150 + random(106);

          // Combine all effects
          int finalBright = min(255, (breathe + waveBoost) * twinkle / 255);

          // Random sparkle - bright white twinkle
          if (random(100) < 12) {
            r = (255 * currentBrightness) / 255;
            g = (255 * currentBrightness) / 255;
            b = (255 * currentBrightness) / 255;
          } else {
            r = (baseR * finalBright * currentBrightness) / 65025;
            g = (baseG * finalBright * currentBrightness) / 65025;
            b = (baseB * finalBright * currentBrightness) / 65025;
          }
          strip.setPixelColor(i, strip.Color(r, g, b));
        }
      }
      break;
  }
  strip.show();
}

void setupButtons() {
  pinMode(BTN_COLOR_PIN, INPUT_PULLUP);
  pinMode(BTN_MODE_PIN, INPUT_PULLUP);
  Serial.println("Buttons initialized on GPIO4 and GPIO5");
}

void handleButtons() {
  unsigned long now = millis();

  // Read button states (LOW = pressed with INPUT_PULLUP)
  bool btn1State = digitalRead(BTN_COLOR_PIN);
  bool btn2State = digitalRead(BTN_MODE_PIN);

  // Button 1 (GPIO4) - Color change on click
  if (btn1State == LOW && btn1LastState == HIGH) {
    // Button just pressed
    btn1PressTime = now;
  }
  if (btn1State == HIGH && btn1LastState == LOW) {
    // Button just released - check if it was a valid press
    if (now - btn1PressTime > DEBOUNCE_TIME) {
      // Cycle to next color
      currentColorIndex = (currentColorIndex + 1) % NUM_COLORS;
      currentR = presetColors[currentColorIndex][0];
      currentG = presetColors[currentColorIndex][1];
      currentB = presetColors[currentColorIndex][2];
      Serial.printf("Button: Color %d (R=%d G=%d B=%d)\n", currentColorIndex, currentR, currentG, currentB);
    }
  }
  btn1LastState = btn1State;

  // Button 2 (GPIO5) - Long press: power, Short press: mode
  if (btn2State == LOW && btn2LastState == HIGH) {
    // Button just pressed
    btn2PressTime = now;
    btn2LongPressHandled = false;
  }
  if (btn2State == LOW && !btn2LongPressHandled) {
    // Button held - check for long press
    if (now - btn2PressTime >= LONG_PRESS_TIME) {
      // Long press detected - toggle power
      powerOn = !powerOn;
      Serial.printf("Button: Power %s\n", powerOn ? "ON" : "OFF");
      btn2LongPressHandled = true;
    }
  }
  if (btn2State == HIGH && btn2LastState == LOW) {
    // Button just released
    if (!btn2LongPressHandled && (now - btn2PressTime > DEBOUNCE_TIME)) {
      // Short press - cycle mode
      currentMode = (LedMode)((currentMode + 1) % NUM_MODES);
      pulseValue = 0;
      pulseDirection = 1;
      Serial.printf("Button: Mode %d\n", currentMode);
    }
  }
  btn2LastState = btn2State;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n=== Rosie LED Controller ===");
  Serial.println("Board: ESP32-C3 Mini");

  // Initialize LEDs
  strip.begin();
  strip.clear();
  strip.show();
  Serial.println("LEDs initialized on GPIO10");

  // Startup animation - green chase
  for (int i = 0; i < NUM_LEDS; i++) {
    strip.setPixelColor(i, strip.Color(0, 50, 0));
    strip.show();
    delay(50);
  }
  delay(300);

  // Flash all red
  for (int i = 0; i < NUM_LEDS; i++) {
    strip.setPixelColor(i, strip.Color(50, 0, 0));
  }
  strip.show();
  delay(300);

  strip.clear();
  strip.show();

  // Setup WiFi
  setupWiFi();

  // Setup BLE
  setupBLE();

  // Setup Buttons
  setupButtons();

  Serial.println("Ready!");
}

void loop() {
  server.handleClient();

  // Handle BLE reconnection
  if (!deviceConnected && oldDeviceConnected) {
    delay(500);
    pServer->startAdvertising();
    oldDeviceConnected = deviceConnected;
  }
  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = deviceConnected;
  }

  // Handle button presses
  handleButtons();

  updateLEDs();
  delay(10);
}
