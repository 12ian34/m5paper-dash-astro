/*
 * M5Paper Astronomy Dashboard
 * Connects to WiFi, fetches astronomy data JSON from Pi,
 * displays sun/moon/planets/aurora/ISS on e-ink.
 *
 * Layout (960x540, 3x2 grid, each tile 320x270):
 *   [UPDATED AT] [   SUN    ] [   MOON   ]
 *   [ PLANETS  ] [  AURORA  ] [ ISS PASS ]
 *
 * Uses M5.shutdown() for timed wake (RTC alarm).
 */

#include <M5EPD.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <cmath>
#include <esp_task_wdt.h>
#include <esp_wifi.h>
#include "time.h"

// ---- CONFIG (injected from .env at build time) ----
#ifndef WIFI_SSID
#error "WIFI_SSID not set — check .env file in project root"
#endif
#ifndef WIFI_PASS
#error "WIFI_PASS not set — check .env file in project root"
#endif
#ifndef DASHBOARD_URL
#error "DASHBOARD_URL not set — check .env file in project root"
#endif

const char* wifi_ssid     = WIFI_SSID;
const char* wifi_pass     = WIFI_PASS;
const char* dashboard_url = DASHBOARD_URL;
const int   REFRESH_MINS  = 30;

// Color map: 0=white, 15=black (inverted from what you'd expect)
#define C_WHITE 0
#define C_BLACK 15
#define C_DARK  12
#define C_MID   8
#define C_LIGHT 3

M5EPD_Canvas canvas(&M5.EPD);

// Forward declarations
void syncTime();
void drawDashboard(JsonObject& widgets, int battPct);
void drawError(const char* msg);
void drawNoWifi();

// ---- Power ----

void goToSleep() {
    Serial.printf("goToSleep: M5.shutdown for %d min\n", REFRESH_MINS);
    Serial.flush();
    M5.shutdown(REFRESH_MINS * 60);
}

// ---- Setup ----

void setup() {
    M5.begin();
    M5.EPD.SetRotation(0);
    M5.RTC.begin();

    esp_task_wdt_init(30, true);
    esp_task_wdt_add(NULL);

    uint32_t battMv = M5.getBatteryVoltage();
    int battPct = constrain(map(battMv, 3300, 4200, 0, 100), 0, 100);

    // Boot screen
    canvas.createCanvas(960, 540);
    canvas.fillCanvas(C_WHITE);
    canvas.setTextSize(4);
    canvas.setTextColor(C_BLACK);
    canvas.setTextDatum(MC_DATUM);
    canvas.drawString("BOOTING...", 480, 270);
    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);

    // Connect WiFi
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(wifi_ssid, wifi_pass);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
        delay(250);
        attempts++;
    }

    if (WiFi.status() != WL_CONNECTED) {
        drawNoWifi();
        goToSleep();
        return;
    }

    auto showStatus = [&](const char* msg, const char* sub = nullptr) {
        canvas.fillCanvas(C_WHITE);
        canvas.setTextSize(5);
        canvas.setTextColor(C_BLACK);
        canvas.setTextDatum(MC_DATUM);
        canvas.drawString(msg, 480, sub ? 240 : 270);
        if (sub) {
            canvas.setTextSize(2);
            canvas.setTextColor(C_MID);
            canvas.drawString(sub, 480, 300);
        }
        canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
    };

    showStatus("WiFi connected", WiFi.localIP().toString().c_str());
    esp_task_wdt_reset();
    delay(1500);

    esp_wifi_set_max_tx_power(8);

    showStatus("Preparing fetch...");
    esp_task_wdt_reset();
    delay(5000);
    esp_task_wdt_reset();

    showStatus("Fetching dashboard...");
    esp_task_wdt_reset();

    HTTPClient http;
    http.begin(dashboard_url);
    http.setConnectTimeout(5000);
    http.setTimeout(5000);
    int httpCode = http.GET();
    String payload;
    if (httpCode == 200) {
        payload = http.getString();
    }
    http.end();

    if (httpCode != 200) {
        char errBuf[64];
        snprintf(errBuf, sizeof(errBuf), "HTTP %d", httpCode);
        drawError(errBuf);
        goToSleep();
        return;
    }

    payload.trim();
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        char errBuf[80];
        snprintf(errBuf, sizeof(errBuf), "Parse: %s (%d)", err.c_str(), payload.length());
        drawError(errBuf);
        goToSleep();
        return;
    }

    JsonObject widgets = doc["widgets"];
    drawDashboard(widgets, battPct);

    syncTime();  // after draw — NTP can hang on battery
    WiFi.disconnect(true);
    goToSleep();
}

// ---- Loop (USB fallback) ----

void loop() {
    static unsigned long loopStart = millis();
    if (millis() - loopStart > (unsigned long)REFRESH_MINS * 60 * 1000) {
        ESP.restart();
    }
    M5.shutdown(REFRESH_MINS * 60);
    delay(30000);
}

// ---- NTP ----

void syncTime() {
    configTzTime("GMT0BST,M3.5.0/1,M10.5.0", "pool.ntp.org");
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 5000)) {
        rtc_time_t rtcTime;
        rtcTime.hour = timeinfo.tm_hour;
        rtcTime.min  = timeinfo.tm_min;
        rtcTime.sec  = timeinfo.tm_sec;
        M5.RTC.setTime(&rtcTime);

        rtc_date_t rtcDate;
        rtcDate.year = timeinfo.tm_year + 1900;
        rtcDate.mon  = timeinfo.tm_mon + 1;
        rtcDate.day  = timeinfo.tm_mday;
        rtcDate.week = timeinfo.tm_wday;
        M5.RTC.setDate(&rtcDate);
    }
}

// ==================================================================
// Drawing
// ==================================================================

// Tile grid: 3 columns x 2 rows
#define TW   320   // tile width  (960 / 3)
#define TH   270   // tile height (540 / 2)

void drawGrid() {
    int g = 10;
    canvas.fillRect(TW - g / 2,      0, g, 540, C_LIGHT);
    canvas.fillRect(TW * 2 - g / 2,  0, g, 540, C_LIGHT);
    canvas.fillRect(0, TH - g / 2, 960, g, C_LIGHT);
}

void drawLabel(int cx, int y, const char* label) {
    canvas.setTextDatum(TC_DATUM);
    canvas.setTextSize(3);
    canvas.setTextColor(C_MID);
    canvas.drawString(label, cx, y);
}

void drawBigValue(int cx, int y, const char* value) {
    canvas.setTextDatum(TC_DATUM);
    canvas.setTextSize(7);
    canvas.setTextColor(C_BLACK);
    canvas.drawString(value, cx, y);
}

void drawSub(int cx, int y, const char* sub) {
    canvas.setTextDatum(TC_DATUM);
    canvas.setTextSize(3);
    canvas.setTextColor(C_DARK);
    canvas.drawString(sub, cx, y);
}

// Simple tile: label, big value, optional sub
void drawTile(int col, int row, const char* label, const char* value,
              const char* sub) {
    int x  = col * TW;
    int y  = row * TH;
    int cx = x + TW / 2;
    drawLabel(cx, y + 18, label);
    drawBigValue(cx, y + 80, value);
    if (sub && sub[0]) {
        drawSub(cx, y + 155, sub);
    }
}

// ---- Sun tile ----

void drawSunTile(int col, int row, const char* rise, const char* set) {
    int x  = col * TW;
    int y  = row * TH;
    int cx = x + TW / 2;
    drawLabel(cx, y + 10, "SUN");

    canvas.setTextDatum(TC_DATUM);
    canvas.setTextSize(3);
    canvas.setTextColor(C_MID);
    canvas.drawString("rise", cx, y + 55);
    canvas.setTextSize(5);
    canvas.setTextColor(C_BLACK);
    canvas.drawString(rise, cx, y + 85);

    canvas.setTextSize(3);
    canvas.setTextColor(C_MID);
    canvas.drawString("set", cx, y + 155);
    canvas.setTextSize(5);
    canvas.setTextColor(C_BLACK);
    canvas.drawString(set, cx, y + 185);
}

// ---- Moon tile ----

void drawMoonDisc(int cx, int cy, int r, float phaseFrac) {
    float k = cosf(2.0f * M_PI * phaseFrac);

    for (int dy = -r; dy <= r; dy++) {
        float w = sqrtf((float)(r * r - dy * dy));
        if (w < 1) continue;
        float tx = w * k;
        int darkL, darkR;
        if (phaseFrac <= 0.5f) {
            darkL = (int)(cx - w);
            darkR = (int)(cx + tx);
        } else {
            darkL = (int)(cx - tx);
            darkR = (int)(cx + w);
        }
        int len = darkR - darkL;
        if (len > 0) {
            canvas.drawFastHLine(darkL, cy + dy, len, C_BLACK);
        }
    }
    canvas.drawCircle(cx, cy, r, C_BLACK);
}

void drawMoonTile(int col, int row, const char* name, float illum, float age) {
    int x  = col * TW;
    int y  = row * TH;
    int cx = x + TW / 2;
    drawLabel(cx, y + 10, "MOON");

    float phaseFrac = fmodf(age, 29.53f) / 29.53f;
    drawMoonDisc(cx, y + 115, 55, phaseFrac);

    canvas.setTextDatum(TC_DATUM);
    canvas.setTextSize(3);
    canvas.setTextColor(C_BLACK);
    canvas.drawString(name, cx, y + 185);

    char infoBuf[32];
    snprintf(infoBuf, sizeof(infoBuf), "%.0f%%  day %.0f", illum, age);
    canvas.setTextSize(2);
    canvas.setTextColor(C_DARK);
    canvas.drawString(infoBuf, cx, y + 220);
}

// ---- Planets tile ----

void drawPlanetsTile(int col, int row, JsonObject& planets) {
    int x  = col * TW;
    int y  = row * TH;
    int cx = x + TW / 2;
    drawLabel(cx, y + 10, "PLANETS");

    if (!planets.containsKey("planets")) {
        drawSub(cx, y + 120, "no data");
        return;
    }

    JsonArray arr = planets["planets"];
    int count = arr.size();

    if (count == 0) {
        canvas.setTextDatum(TC_DATUM);
        canvas.setTextSize(4);
        canvas.setTextColor(C_BLACK);
        canvas.drawString("None", cx, y + 90);
        canvas.setTextSize(3);
        canvas.setTextColor(C_DARK);
        canvas.drawString("tonight", cx, y + 145);
        return;
    }

    // Dynamic sizing: bigger text when fewer planets
    int textSz = count <= 3 ? 4 : 3;
    int lineH  = count <= 3 ? 55 : 42;
    int startY = y + 45 + (210 - count * lineH) / 2;

    canvas.setTextDatum(TC_DATUM);
    for (int i = 0; i < count && i < 5; i++) {
        JsonObject p = arr[i];
        const char* pName = p["name"] | "?";
        const char* pDir  = p["dir"]  | "?";

        char buf[32];
        snprintf(buf, sizeof(buf), "%s %s", pName, pDir);

        canvas.setTextSize(textSz);
        canvas.setTextColor(C_BLACK);
        canvas.drawString(buf, cx, startY + i * lineH);
    }
}

// ---- Aurora tile ----

void drawAuroraTile(int col, int row, JsonObject& aurora) {
    int x  = col * TW;
    int y  = row * TH;
    int cx = x + TW / 2;
    drawLabel(cx, y + 10, "AURORA");

    if (aurora.containsKey("error")) {
        canvas.setTextDatum(TC_DATUM);
        canvas.setTextSize(3);
        canvas.setTextColor(C_BLACK);
        canvas.drawString("Unavailable", cx, y + 110);
        return;
    }

    const char* level = aurora["level"] | "?";

    if (!aurora["nt"].isNull()) {
        int nt = aurora["nt"] | 0;
        char ntBuf[16];
        snprintf(ntBuf, sizeof(ntBuf), "%d nT", nt);
        canvas.setTextDatum(TC_DATUM);
        canvas.setTextSize(6);
        canvas.setTextColor(C_BLACK);
        canvas.drawString(ntBuf, cx, y + 65);
    } else {
        canvas.setTextDatum(TC_DATUM);
        canvas.setTextSize(3);
        canvas.setTextColor(C_DARK);
        canvas.drawString("no reading", cx, y + 85);
    }

    // Activity level
    canvas.setTextDatum(TC_DATUM);
    canvas.setTextSize(5);
    canvas.setTextColor(C_BLACK);
    canvas.drawString(level, cx, y + 165);

    // Status bar: filled rectangle under the level to indicate severity
    const char* color = aurora["status_color"] | "green";
    if (strcmp(color, "red") == 0 || strcmp(color, "amber") == 0) {
        // Draw attention bar for elevated activity
        canvas.fillRect(x + 40, y + 230, TW - 80, 8, C_BLACK);
    }
}

// ---- ISS Pass tile ----

void drawIssTile(int col, int row, JsonObject& iss) {
    int x  = col * TW;
    int y  = row * TH;
    int cx = x + TW / 2;
    drawLabel(cx, y + 10, "ISS PASS");

    if (iss.containsKey("error")) {
        canvas.setTextDatum(TC_DATUM);
        canvas.setTextSize(3);
        canvas.setTextColor(C_BLACK);
        canvas.drawString("No visible", cx, y + 90);
        canvas.drawString("pass soon", cx, y + 135);
        return;
    }

    const char* time   = iss["time"]     | "--:--";
    const char* date   = iss["date"]     | "?";
    int maxAlt         = iss["max_alt"]  | 0;
    const char* rDir   = iss["rise_dir"] | "?";
    const char* sDir   = iss["set_dir"]  | "?";
    int dur            = iss["duration_min"] | 0;

    // Pass time (big)
    canvas.setTextDatum(TC_DATUM);
    canvas.setTextSize(6);
    canvas.setTextColor(C_BLACK);
    canvas.drawString(time, cx, y + 50);

    // Date
    canvas.setTextSize(3);
    canvas.setTextColor(C_DARK);
    canvas.drawString(date, cx, y + 115);

    // Direction: rise > set, max altitude
    char dirBuf[32];
    snprintf(dirBuf, sizeof(dirBuf), "%s>%s max %d", rDir, sDir, maxAlt);
    canvas.setTextSize(3);
    canvas.setTextColor(C_BLACK);
    canvas.drawString(dirBuf, cx, y + 165);

    // Duration
    char durBuf[16];
    snprintf(durBuf, sizeof(durBuf), "%dm visible", dur);
    canvas.setTextSize(2);
    canvas.setTextColor(C_DARK);
    canvas.drawString(durBuf, cx, y + 210);
}

// ==================================================================
// Main dashboard
// ==================================================================

// ---- Date tile (vertical stack: weekday, day month, year) ----

void drawDateTile(int col, int row) {
    int x  = col * TW;
    int y  = row * TH;
    int cx = x + TW / 2;
    drawLabel(cx, y + 18, "DATE");

    rtc_date_t d;
    M5.RTC.getDate(&d);

    const char* weekdays[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    const char* months[] = {"","Jan","Feb","Mar","Apr","May","Jun",
                            "Jul","Aug","Sep","Oct","Nov","Dec"};
    int w = (d.week >= 0 && d.week <= 6) ? d.week : 0;

    canvas.setTextDatum(TC_DATUM);
    canvas.setTextSize(4);
    canvas.setTextColor(C_BLACK);
    canvas.drawString(weekdays[w], cx, y + 65);

    char dayMon[16];
    snprintf(dayMon, sizeof(dayMon), "%d %s", d.day,
             (d.mon >= 1 && d.mon <= 12) ? months[d.mon] : "???");
    canvas.drawString(dayMon, cx, y + 115);

    char yearBuf[8];
    snprintf(yearBuf, sizeof(yearBuf), "%04d", d.year);
    canvas.setTextSize(3);
    canvas.setTextColor(C_DARK);
    canvas.drawString(yearBuf, cx, y + 165);
}

// ---- Small corner inlays (updated-at time + battery) ----

void drawInlays(int battPct) {
    rtc_time_t t;
    M5.RTC.getTime(&t);

    // Bottom-left: updated time
    char timeBuf[24];
    snprintf(timeBuf, sizeof(timeBuf), "UPD %02d:%02d", t.hour, t.min);
    canvas.setTextDatum(BL_DATUM);
    canvas.setTextSize(2);
    canvas.setTextColor(C_MID);
    canvas.drawString(timeBuf, 8, 534);

    // Bottom-right: battery
    char battBuf[16];
    snprintf(battBuf, sizeof(battBuf), "%d%%", battPct);
    canvas.setTextDatum(BR_DATUM);
    canvas.setTextSize(2);
    canvas.setTextColor(C_MID);
    canvas.drawString(battBuf, 952, 534);
}

void drawDashboard(JsonObject& widgets, int battPct) {
    canvas.createCanvas(960, 540);
    canvas.fillCanvas(C_WHITE);
    drawGrid();

    // ---- Row 0: Date | Sun | Moon ----

    drawDateTile(0, 0);

    if (widgets.containsKey("sun")) {
        JsonObject sun = widgets["sun"];
        drawSunTile(1, 0, sun["sunrise"] | "--:--", sun["sunset"] | "--:--");
    } else {
        drawTile(1, 0, "SUN", "--:--", "no data");
    }

    if (widgets.containsKey("moon")) {
        JsonObject moon = widgets["moon"];
        drawMoonTile(2, 0,
                     moon["name"]              | "Unknown",
                     moon["illumination_pct"]  | 0.0f,
                     moon["age_days"]          | 0.0f);
    } else {
        drawTile(2, 0, "MOON", "?", "no data");
    }

    // ---- Row 1: Planets | Aurora | ISS ----

    if (widgets.containsKey("planets")) {
        JsonObject planets = widgets["planets"];
        drawPlanetsTile(0, 1, planets);
    } else {
        drawTile(0, 1, "PLANETS", "?", "no data");
    }

    if (widgets.containsKey("aurora")) {
        JsonObject aurora = widgets["aurora"];
        drawAuroraTile(1, 1, aurora);
    } else {
        drawTile(1, 1, "AURORA", "?", "no data");
    }

    if (widgets.containsKey("iss")) {
        JsonObject iss = widgets["iss"];
        drawIssTile(2, 1, iss);
    } else {
        drawTile(2, 1, "ISS PASS", "?", "no data");
    }

    // ---- Small inlays: updated time + battery ----
    drawInlays(battPct);

    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
}

// ---- Error screens ----

void drawError(const char* msg) {
    canvas.createCanvas(960, 540);
    canvas.fillCanvas(C_WHITE);
    canvas.setTextSize(3);
    canvas.setTextColor(C_BLACK);
    canvas.setTextDatum(MC_DATUM);
    canvas.drawString("Error", 480, 240);
    canvas.setTextSize(2);
    canvas.drawString(msg, 480, 290);
    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
}

void drawNoWifi() {
    canvas.createCanvas(960, 540);
    canvas.fillCanvas(C_WHITE);
    canvas.setTextSize(3);
    canvas.setTextColor(C_BLACK);
    canvas.setTextDatum(MC_DATUM);
    canvas.drawString("WiFi Failed", 480, 220);
    canvas.setTextSize(2);
    canvas.drawString("Check SSID/password", 480, 280);
    char buf[64];
    snprintf(buf, sizeof(buf), "Retrying in %d min...", REFRESH_MINS);
    canvas.drawString(buf, 480, 320);
    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
}
