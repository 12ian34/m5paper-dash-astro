---
name: m5paper
description: Develop firmware for M5Paper v1.1 (ESP32 e-ink device) using PlatformIO and the M5EPD library. Covers display API, colors, rotation, WiFi, sensors, power management, buttons, and flashing. Use when building M5Paper projects, working with e-ink displays, or troubleshooting M5Paper firmware.
---

# M5Paper Development

## Hardware summary
- **MCU**: ESP32-D0WDQ6-V3, 240MHz dual-core, 520KB SRAM, 8MB PSRAM, 16MB Flash
- **Display**: 4.7" e-ink, 960x540 native landscape, 16 grayscale, IT8951 controller
- **Touch**: GT911 capacitive, 2-point
- **Sensors**: SHT30 (temp/humidity), BM8563 (RTC)
- **Battery**: 1150mAh LiPo, ~25µA deep sleep
- **USB**: CP2104 USB-to-serial (Type-C)
- **WiFi**: 2.4 GHz only (ESP32 limitation)
- **Buttons**: rear reset button + side 3-way toggle (click/left/right)

## PlatformIO setup

```ini
[env:m5paper]
platform = espressif32
board = m5stack-fire
framework = arduino
upload_speed = 2000000
monitor_speed = 115200
board_build.partitions = default_16MB.csv
build_flags =
  -std=gnu++17
  -Ofast
  -DBOARD_HAS_PSRAM
  -mfix-esp32-psram-cache-issue
build_unflags =
  -std=gnu++11
lib_deps =
  m5stack/M5EPD
  bblanchon/ArduinoJson
```

- Use `board = m5stack-fire` (no official m5paper board definition)
- PlatformIO needs pip in venv. If using uv: `uv pip install pip`

## Display — critical gotchas

### Rotation (use 0-3 only, NOT degrees)
| Value | Orientation |
|-------|-------------|
| **`SetRotation(0)`** | **Landscape, USB/rocker on top (use this for standing)** |
| `SetRotation(2)` | Landscape, right-side up |
| `SetRotation(1)` | Portrait |
| `SetRotation(3)` | Portrait inverted |

**Never pass degree values** like 90 — they get interpreted as `90 % 4`.

### Colors (INVERTED from most docs/examples)
- **0 = white, 15 = black** (opposite of what most docs say)
- Confirmed by testing: `fillCanvas(0)` = white, `fillCanvas(15)` = black

```cpp
#define C_WHITE 0
#define C_BLACK 15
#define C_DARK  12
#define C_MID   8
#define C_LIGHT 3
```

### Canvas workflow
```cpp
M5EPD_Canvas canvas(&M5.EPD);
canvas.createCanvas(960, 540);
canvas.fillCanvas(C_WHITE);
canvas.setTextSize(5);
canvas.setTextColor(C_BLACK);
canvas.setTextDatum(TC_DATUM);
canvas.drawString("Hello", 480, 100);
canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
```

### Update modes
| Mode | Speed | Quality | Use for |
|------|-------|---------|---------|
| `UPDATE_MODE_GC16` | ~1s | Best grayscale | Full refresh |
| `UPDATE_MODE_DU4` | ~0.3s | 4-level gray | Partial updates |
| `UPDATE_MODE_DU` | Fastest | B&W only | Text-only UI |

### Text sizing
- `setTextSize(n)` scales the built-in bitmap font (1–7)
- Size 7 is large and readable on e-ink at 235 PPI
- For nicer fonts: `loadFont()` + `createRender(size)` for TTF support

## Sensors

```cpp
// Temperature & humidity
M5.SHT30.UpdateData();
float temp = M5.SHT30.GetTemperature();
float hum  = M5.SHT30.GetRelHumidity();  // NOT GetHumidity()

// Battery
uint32_t mv = M5.getBatteryVoltage();  // 3300=empty, 4200=full

// RTC
rtc_time_t time; rtc_date_t date;
M5.RTC.getTime(&time); M5.RTC.getDate(&date);
```

## WiFi + HTTP

**Marginal WiFi / task WDT**: A fixed-count loop with `delay(250)` and no `esp_task_wdt_reset()` can hit the default **30s task watchdog** while associating, so the screen never leaves BOOTING. Use a **millis() deadline** (e.g. 45s), call **`esp_task_wdt_reset()`** each iteration, and optionally log status every 5s on Serial.

**Clean STA after RTC wake**: Before `WiFi.mode(WIFI_STA)`, reset driver state without wiping stored credentials:

```cpp
WiFi.disconnect(true, false);  // second arg false: do NOT erase NVS WiFi storage
WiFi.mode(WIFI_OFF);
delay(100);
esp_task_wdt_reset();

WiFi.mode(WIFI_STA);
WiFi.setSleep(false);
WiFi.begin("SSID", "PASS");

const unsigned long wifiDeadlineMs = 45000;
const unsigned long wifiStarted = millis();
unsigned long lastWifiLog = wifiStarted;
while (WiFi.status() != WL_CONNECTED && (millis() - wifiStarted) < wifiDeadlineMs) {
    esp_task_wdt_reset();
    delay(250);
    if (millis() - lastWifiLog >= 5000) {
        lastWifiLog = millis();
        Serial.printf("WiFi status=%d elapsed=%lu ms\n", (int)WiFi.status(), millis() - wifiStarted);
    }
}
```

```cpp
HTTPClient http;
http.begin("http://host:port/path");
// log + esp_task_wdt_reset() before/after long GET as needed
if (http.GET() == 200) {
    String payload = http.getString();
    // parse with ArduinoJson
}
http.end();
WiFi.disconnect(true);
```

### Battery: NTP must run AFTER draw

**Critical for battery operation**: If you use NTP (`syncTime()`, `configTzTime()`, `getLocalTime()`), call it **after** drawing the display, not before. On battery, NTP to the internet can block indefinitely. If syncTime runs before parse/draw, the firmware never reaches the display update — screen stays on "fetching" or boot screen.

**Correct order**: fetch → parse → **draw** → syncTime → disconnect → sleep

**Wrong order** (causes apparent "fetch hang" on battery): fetch → syncTime → parse → draw

At the start of `syncTime()`, call `esp_task_wdt_reset()` before `configTzTime()` so slow NTP does not starve the task WDT before your loop runs again.

## Power management

### Use `M5.shutdown(seconds)` — NOT ESP32 deep sleep

```cpp
void goToSleep() {
    M5.shutdown(REFRESH_MINS * 60);  // sets RTC alarm, powers off
    // Won't return on battery (device is OFF).
    // On USB, returns because USB keeps ESP32 alive — handle in loop().
}

void setup() {
    M5.begin();
    M5.EPD.SetRotation(0);
    // ... WiFi, fetch, draw, then goToSleep()
}

void loop() {
    // Fallback for USB: M5.shutdown() can't fully power off on USB.
    static unsigned long start = millis();
    if (millis() - start > (unsigned long)REFRESH_MINS * 60 * 1000)
        ESP.restart();
    M5.shutdown(REFRESH_MINS * 60);
    delay(30000);
}
```

**On battery**: `M5.shutdown()` cuts GPIO2 (power rail off, device fully OFF, ~0µA). RTC alarm fires → cold boot from `setup()`. E-ink retains image while off.

**On USB**: `shutdown()` can't fully power off. `loop()` re-arms the alarm and calls `ESP.restart()` after the interval.

**Wake vs rear reset (battery):** From full off after shutdown, **wake** with **RTC alarm**, **USB**, or **long-press side PWR**. The **rear reset does not power the board on** when there is no battery/USB rail — it only resets an already-powered ESP32.

### Why NOT ESP32 deep sleep

We tested `esp_deep_sleep_start()` with `gpio_hold_en` on GPIO2. **Unreliable:**
- `gpio_hold_dis()` at boot releases GPIO2 → power rail drops → display dies → device appears bricked
- Timer wake was intermittent
- Button wake via `ext0_wakeup(GPIO39)` never worked
- Adding ext0 config sometimes broke timer wake too

**Do not use `esp_deep_sleep_start()` on M5Paper.** Use `M5.shutdown()` instead.

## Buttons

```cpp
M5.update();
if (M5.BtnL.wasPressed()) { /* side toggle left  (GPIO39) */ }
if (M5.BtnP.wasPressed()) { /* side toggle click  (GPIO38) */ }
if (M5.BtnR.wasPressed()) { /* side toggle right (GPIO37) */ }
```

- **Rear button (reset)**: hardware reset when the ESP32 **already has power** (USB or battery rail on). After `M5.shutdown()` on **battery alone**, the unit is off — rear reset does **not** apply power; use RTC / USB / long-press PWR to wake.
- **Side toggle**: only works while ESP32 is powered on. **Cannot** wake from full `M5.shutdown()` off state.

## USB serial
- macOS: `/dev/cu.usbserial-*` (port name changes on reconnect)
- Linux: `/dev/ttyUSB*`
- Windows: `COM*`

## Common mistakes
1. Using degree values for rotation (use 0-3)
2. Assuming 0=black, 15=white (it's inverted: 0=white, 15=black)
3. Calling `GetHumidity()` instead of `GetRelHumidity()`
4. Using `board = m5paper` in PlatformIO (use `m5stack-fire`)
5. Forgetting `fillCanvas()` before redraw (canvas retains content)
6. WiFi wait without feeding the task WDT (use millis deadline + `esp_task_wdt_reset()` each iteration)
7. Using `esp_deep_sleep_start()` instead of `M5.shutdown()`
8. Not handling USB case in `loop()` when using `M5.shutdown()`
9. Running syncTime/NTP before draw on battery
