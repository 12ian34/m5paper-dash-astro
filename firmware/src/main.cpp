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
#include "landmap.h"

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

// Day of week: 0=Sun, 1=Mon, ..., 6=Sat (Tomohiko Sakamoto)
static int dayOfWeek(int y, int m, int d) {
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y--;
    return (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
}

// RTC stores UK local time (GMT or BST). Return BST offset (0 or 1).
static int bstOffset(int year, int mon, int day, int hour) {
    if (mon >= 4 && mon <= 9) return 1;
    if (mon <= 2 || mon >= 11) return 0;
    int lastSun = 31 - dayOfWeek(year, mon, 31);
    if (mon == 3) return (day > lastSun || (day == lastSun && hour >= 1)) ? 1 : 0;
    // October: BST ends at 02:00 BST = 01:00 UTC
    return (day < lastSun || (day == lastSun && hour < 2)) ? 1 : 0;
}

static float solarElevation(float lat, float lon, float utcHourF, int doy) {
    float decl = 23.44f * sinf((360.0f / 365.0f) * (doy - 81) * M_PI / 180.0f);
    float hourAngle = (15.0f * (utcHourF - 12.0f) + lon) * M_PI / 180.0f;
    float latR = lat * M_PI / 180.0f;
    float declR = decl * M_PI / 180.0f;
    return sinf(latR) * sinf(declR) + cosf(latR) * cosf(declR) * cosf(hourAngle);
}

static bool isLand(int mapX, int mapY) {
    if (mapX < 0 || mapX >= LANDMAP_W || mapY < 0 || mapY >= LANDMAP_H) return false;
    int byteIdx = mapY * LANDMAP_ROW_BYTES + mapX / 8;
    int bitIdx  = 7 - (mapX % 8);
    return pgm_read_byte(&LANDMAP[byteIdx]) & (1 << bitIdx);
}

static int dayOfYear(int year, int month, int day) {
    static const int mdays[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    int doy = mdays[month - 1] + day;
    if (month > 2 && (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0)) doy++;
    return doy;
}

void drawSunTile(int col, int row, const char* rise, const char* set,
                 float moonAgeDays) {
    int x  = col * TW;
    int y  = row * TH;

    // Rise and set side by side, compact
    canvas.setTextDatum(TC_DATUM);
    canvas.setTextSize(2);
    canvas.setTextColor(C_MID);
    canvas.drawString("rise", x + TW / 4, y + 6);
    canvas.drawString("set", x + 3 * TW / 4, y + 6);

    canvas.setTextSize(4);
    canvas.setTextColor(C_BLACK);
    canvas.drawString(rise, x + TW / 4, y + 28);
    canvas.drawString(set, x + 3 * TW / 4, y + 28);

    // World map with day/night terminator
    const int mapW = 300;
    const int mapH = 150;
    const int mX   = x + (TW - mapW) / 2;
    const int mY   = y + 75;

    // Convert RTC local time (UK) to UTC
    rtc_time_t t;
    rtc_date_t d;
    M5.RTC.getTime(&t);
    M5.RTC.getDate(&d);
    int offset = bstOffset(d.year, d.mon, d.day, t.hour);
    int utcH = t.hour - offset;
    int utcDay = d.day;
    if (utcH < 0) { utcH += 24; utcDay--; }
    float utcHourF = utcH + t.min / 60.0f;
    int doy = dayOfYear(d.year, d.mon, utcDay);

    // Solar declination for zenith markers
    float decl = 23.44f * sinf((360.0f / 365.0f) * (doy - 81) * M_PI / 180.0f);

    const uint8_t DAY_OCEAN   = 0;
    const uint8_t DAY_LAND    = 3;
    const uint8_t NIGHT_OCEAN = 10;
    const uint8_t NIGHT_LAND  = 13;

    for (int py = 0; py < mapH; py++) {
        float lat = 90.0f - (py * 180.0f / mapH);
        int bmY = py * LANDMAP_H / mapH;

        for (int px = 0; px < mapW; px++) {
            float lon = -180.0f + (px * 360.0f / mapW);
            int bmX = px * LANDMAP_W / mapW;

            float elev = solarElevation(lat, lon, utcHourF, doy);
            bool land = isLand(bmX, bmY);
            bool day  = (elev > -0.1f);

            uint8_t shade;
            if (day && land)   shade = DAY_LAND;
            else if (day)      shade = DAY_OCEAN;
            else if (land)     shade = NIGHT_LAND;
            else               shade = NIGHT_OCEAN;

            canvas.drawPixel(mX + px, mY + py, shade);
        }
    }

    // Subsolar point (sun zenith)
    float sunLat = decl;
    float sunLon = fmodf((12.0f - utcHourF) * 15.0f + 360.0f, 360.0f);
    if (sunLon > 180.0f) sunLon -= 360.0f;
    int sunPx = (int)((sunLon + 180.0f) / 360.0f * mapW);
    int sunPy = (int)((90.0f - sunLat) / 180.0f * mapH);
    if (sunPx >= 0 && sunPx < mapW && sunPy >= 0 && sunPy < mapH) {
        int sx = mX + sunPx, sy = mY + sunPy;
        canvas.fillCircle(sx, sy, 5, C_WHITE);
        canvas.drawCircle(sx, sy, 5, C_BLACK);
        canvas.drawCircle(sx, sy, 4, C_BLACK);
        for (int a = 0; a < 8; a++) {
            float ang = a * M_PI / 4.0f;
            int rx = (int)(8 * cosf(ang));
            int ry = (int)(8 * sinf(ang));
            canvas.drawLine(sx + (int)(6 * cosf(ang)), sy + (int)(6 * sinf(ang)),
                            sx + rx, sy + ry, C_BLACK);
        }
    }

    // Sublunar point (moon zenith) — approximate from moon age
    // Moon is EAST of sun: transits later, so sublunar longitude > subsolar longitude
    if (moonAgeDays >= 0) {
        float moonLon = sunLon + (moonAgeDays * 360.0f / 29.53f);
        if (moonLon < -180.0f) moonLon += 360.0f;
        if (moonLon > 180.0f) moonLon -= 360.0f;
        float moonLat = 23.4f * sinf(moonAgeDays * 360.0f / 27.3f * M_PI / 180.0f);
        int moonPx = (int)((moonLon + 180.0f) / 360.0f * mapW);
        int moonPy = (int)((90.0f - moonLat) / 180.0f * mapH);
        if (moonPx >= 0 && moonPx < mapW && moonPy >= 0 && moonPy < mapH) {
            int mx2 = mX + moonPx, my2 = mY + moonPy;
            // Crescent: white circle with dark overlay offset to make crescent shape
            canvas.fillCircle(mx2, my2, 5, C_WHITE);
            canvas.drawCircle(mx2, my2, 5, C_BLACK);
            canvas.fillCircle(mx2 + 3, my2, 4, C_BLACK);
        }
    }

    canvas.drawRect(mX - 1, mY - 1, mapW + 2, mapH + 2, C_LIGHT);
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

void drawMoonTile(int col, int row, const char* name, float illum, float age,
                  const char* nextNew, const char* nextFull) {
    int x  = col * TW;
    int y  = row * TH;
    int cx = x + TW / 2;
    drawLabel(cx, y + 10, "MOON");

    float phaseFrac = fmodf(age, 29.53f) / 29.53f;
    drawMoonDisc(cx, y + 95, 50, phaseFrac);

    canvas.setTextDatum(TC_DATUM);
    canvas.setTextSize(3);
    canvas.setTextColor(C_BLACK);
    canvas.drawString(name, cx, y + 160);

    char infoBuf[32];
    snprintf(infoBuf, sizeof(infoBuf), "%.0f%%  day %.0f", illum, age);
    canvas.setTextSize(2);
    canvas.setTextColor(C_DARK);
    canvas.drawString(infoBuf, cx, y + 195);

    // Next full and new moon dates
    if (nextFull && nextFull[0]) {
        char buf[32];
        snprintf(buf, sizeof(buf), "full: %s", nextFull);
        canvas.setTextSize(2);
        canvas.setTextColor(C_MID);
        canvas.setTextDatum(TC_DATUM);
        canvas.drawString(buf, cx, y + 225);
    }
    if (nextNew && nextNew[0]) {
        char buf[32];
        snprintf(buf, sizeof(buf), "new: %s", nextNew);
        canvas.setTextSize(2);
        canvas.setTextColor(C_MID);
        canvas.setTextDatum(TC_DATUM);
        canvas.drawString(buf, cx, y + 248);
    }
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

    int lineH  = count <= 3 ? 55 : 42;
    int startY = y + 45 + (210 - count * lineH) / 2;

    for (int i = 0; i < count && i < 5; i++) {
        JsonObject p = arr[i];
        const char* pName = p["name"] | "?";
        const char* pDir  = p["dir"]  | "?";
        int pAlt = p["alt"] | -1;
        int ly = startY + i * lineH;

        // Planet name left-aligned
        canvas.setTextDatum(TL_DATUM);
        canvas.setTextSize(count <= 3 ? 4 : 3);
        canvas.setTextColor(C_BLACK);
        canvas.drawString(pName, x + 12, ly);

        // Direction + altitude right-aligned
        char detBuf[24];
        if (pAlt >= 0) {
            snprintf(detBuf, sizeof(detBuf), "%s %d\xF7", pDir, pAlt);  // 0xF7 = degree symbol
        } else {
            snprintf(detBuf, sizeof(detBuf), "%s", pDir);
        }
        canvas.setTextDatum(TR_DATUM);
        canvas.setTextSize(count <= 3 ? 3 : 2);
        canvas.setTextColor(C_DARK);
        canvas.drawString(detBuf, x + TW - 12, ly + (count <= 3 ? 6 : 4));
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

    // Current nT + level on one line
    if (!aurora["nt"].isNull()) {
        int nt = aurora["nt"] | 0;
        char summBuf[32];
        snprintf(summBuf, sizeof(summBuf), "%d nT  %s", nt, level);
        canvas.setTextDatum(TC_DATUM);
        canvas.setTextSize(4);
        canvas.setTextColor(C_BLACK);
        canvas.drawString(summBuf, cx, y + 50);
    } else {
        canvas.setTextDatum(TC_DATUM);
        canvas.setTextSize(3);
        canvas.setTextColor(C_DARK);
        canvas.drawString(level, cx, y + 55);
    }

    // 24-hour bar chart
    JsonArray history = aurora["history"];
    int count = history.size();
    if (count == 0) return;

    const int chartL  = x + 28;    // left edge (room for axis label)
    const int chartR  = x + TW - 10;
    const int chartW  = chartR - chartL;
    const int chartT  = y + 100;   // top of chart area
    const int chartB  = y + 245;   // bottom of chart area
    const int chartH  = chartB - chartT;

    // Find max value for scaling (at least 50 so chart isn't all full)
    int maxNt = 50;
    for (int i = 0; i < count; i++) {
        int v = history[i]["nt"] | 0;
        if (v > maxNt) maxNt = v;
    }
    // Round up to a nice ceiling
    if (maxNt <= 50) maxNt = 50;
    else if (maxNt <= 100) maxNt = 100;
    else if (maxNt <= 200) maxNt = 200;
    else maxNt = ((maxNt / 100) + 1) * 100;

    // Baseline
    canvas.drawLine(chartL, chartB, chartR, chartB, C_MID);

    // Threshold line at 50 nT (yellow threshold)
    int y50 = chartB - (50 * chartH / maxNt);
    for (int dx = chartL; dx < chartR; dx += 6) {
        canvas.drawPixel(dx, y50, C_LIGHT);
        canvas.drawPixel(dx + 1, y50, C_LIGHT);
    }

    // Draw bars
    int barW = chartW / count;
    if (barW < 2) barW = 2;
    int gap = barW > 6 ? 2 : 1;

    for (int i = 0; i < count; i++) {
        int v = history[i]["nt"] | 0;
        int barH = v * chartH / maxNt;
        if (barH < 1 && v > 0) barH = 1;
        int bx = chartL + i * barW;
        int by = chartB - barH;

        uint8_t shade = (v >= 100) ? C_BLACK : (v >= 50) ? C_DARK : C_MID;
        canvas.fillRect(bx, by, barW - gap, barH, shade);
    }

    // Hour labels: first and last bar
    canvas.setTextSize(1);
    canvas.setTextColor(C_MID);
    const char* firstH = history[0]["h"] | "";
    const char* lastH  = history[count - 1]["h"] | "";
    canvas.setTextDatum(TL_DATUM);
    canvas.drawString(firstH, chartL, chartB + 4);
    canvas.setTextDatum(TR_DATUM);
    canvas.drawString(lastH, chartR, chartB + 4);

    // Y-axis max label
    char maxBuf[8];
    snprintf(maxBuf, sizeof(maxBuf), "%d", maxNt);
    canvas.setTextDatum(TR_DATUM);
    canvas.setTextSize(1);
    canvas.setTextColor(C_MID);
    canvas.drawString(maxBuf, chartL - 3, chartT - 2);
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
    snprintf(timeBuf, sizeof(timeBuf), "updated: %02d:%02d", t.hour, t.min);
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

    float moonAge = -1;
    if (widgets.containsKey("moon")) {
        JsonObject moon = widgets["moon"];
        moonAge = moon["age_days"] | -1.0f;
    }

    if (widgets.containsKey("sun")) {
        JsonObject sun = widgets["sun"];
        drawSunTile(1, 0, sun["sunrise"] | "--:--", sun["sunset"] | "--:--", moonAge);
    } else {
        drawTile(1, 0, "SUN", "--:--", "no data");
    }

    if (widgets.containsKey("moon")) {
        JsonObject moon = widgets["moon"];
        drawMoonTile(2, 0,
                     moon["name"]              | "Unknown",
                     moon["illumination_pct"]  | 0.0f,
                     moon["age_days"]          | 0.0f,
                     moon["next_new"]          | "",
                     moon["next_full"]         | "");
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
