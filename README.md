# RGB LED Controller

Control WS2812B LEDs from your ESP32-C3 Super Mini via BLE or WiFi.

## Wiring

```
LED Strip     ESP32-C3 Super Mini
─────────     ───────────────────
5V        →   5V
GND       →   GND  
DI        →   GPIO2
```

## Firmware Setup

### Option 1: Arduino IDE
1. Install ESP32 board support (add `https://dl.espressif.com/dl/package_esp32_index.json` to preferences)
2. Install "Adafruit NeoPixel" library
3. Select board: "ESP32C3 Dev Module"
4. Enable USB CDC on boot in Tools menu
5. Upload `firmware/led_controller.ino`

### Option 2: PlatformIO
1. Open the `firmware` folder in VS Code with PlatformIO
2. Build and upload

## Control Methods

### WiFi (works immediately)
1. Connect your phone to WiFi network **"RGB-LED"** (password: `12345678`)
2. Open browser and go to **http://192.168.4.1**
3. Use the web interface to control colors and modes

### BLE App (Android)
1. Open the `android_app` folder in Android Studio
2. Build and install the APK on your phone
3. Open app, tap "Connect" to find "RGB-LED-Controller"
4. Control colors, modes, and brightness

## Features

- **15 preset colors** with custom color picker
- **3 modes:**
  - Normal - solid color
  - Pulse - smooth breathing effect  
  - Strobe - fast flash effect
- **Brightness control** (5-255)
- **Dual connectivity** - WiFi web server + BLE

## Customization

### Change number of LEDs
In `led_controller.ino`, change:
```cpp
#define NUM_LEDS 5
```

### Change WiFi credentials
```cpp
const char* ap_ssid = "RGB-LED";
const char* ap_password = "12345678";
```

### Change BLE device name
```cpp
BLEDevice::init("RGB-LED-Controller");
```

## Troubleshooting

**LEDs don't light up:**
- Check wiring (especially DI to GPIO2)
- Verify 5V power supply
- Check serial monitor for errors

**Can't connect via BLE:**
- Make sure Bluetooth and Location are enabled on phone
- Grant all permissions when app requests them
- Try power cycling the ESP32

**Web interface not loading:**
- Confirm you're connected to "RGB-LED" WiFi
- Try http://192.168.4.1 (not https)
- Check serial monitor for IP confirmation

## License

MIT - do whatever you want with it.
