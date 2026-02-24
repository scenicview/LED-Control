/*
 * RGB LED Controller "Rosie" for ESP32-C3 Super Mini
 * Controls WS2812B LEDs via BLE + WiFi Web Server
 *
 * Wiring:
 *   5V  -> 5V
 *   GND -> GND
 *   DI  -> GPIO3
 */

#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <WebServer.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// LED Configuration
#define LED_PIN 3
#define NUM_LEDS 28

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
  MODE_STROBE = 2
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
uint8_t currentBrightness = 128;
LedMode currentMode = MODE_NORMAL;
bool powerOn = true;

// Animation state
unsigned long lastUpdate = 0;
int pulseValue = 0;
int pulseDirection = 1;
bool strobeState = false;

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
      currentMode = (LedMode)((uint8_t)value[0] % 3);
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
  <title>RGB LED Control</title>
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
    currentMode = (LedMode)(server.arg("m").toInt() % 3);
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
  }
  strip.show();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n=== Rosie LED Controller ===");

  // Initialize LEDs
  strip.begin();
  strip.clear();
  strip.show();
  Serial.println("LEDs initialized on GPIO3");

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

  updateLEDs();
  delay(10);
}
