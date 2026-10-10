/* *******************************************************************
- TOWER CHRONOMETER v59.9 (Auto-Location Edition)
- *******************************************************************
- CHANGELOG v59.9:
- Weather fix: the API returns temperature_2m/weather_code twice --
-   first as text labels in current_units, then as real numbers in
-   current. The parser now reads from the "current" block, so it
-   no longer chokes on the labels (this was the WEATHER UNAVAILABLE
-   cause).
- DATE/TIME made more prominent in the status card.
- CHANGELOG v59.8:
- ROOT-CAUSE FIX: removed every literal % from the HTML template
-   (the template engine was deleting page sections between stray %s).
- Location via JSON API with key-based parsing (order-independent).
- Logs served as styled HTML (large type); delete requires typing DELETE.
- CHANGELOG v59.7:
- Two-column layout: status + timing | manual override + logs.
- CHANGELOG v59.6:
- Pump hour meter with 500-hr maintenance banner + MARK AS CLEANED.
- CHANGELOG v59.5:
- Weather display via Open-Meteo (free, no API key), every 15 min;
-   freeze warning banner at 35F or below.
- PINOUT: Relay=17 | SD: CS=5, SCK=18, MISO=19, MOSI=23
- *******************************************************************/

#include <WiFi.h>
#include <WiFiManager.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h> // Added for Auto-Location
#include "FS.h"
#include "SD.h"
#include "SPI.h"
#include "time.h"

// --- Global Configuration ---
#define MDNS_HOSTNAME "timer"

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = -21600;
const int   daylightOffset_sec = 3600;

const int relayPin = 17;
const int chipSelect = 5;

// Freeze warning threshold (F). Banner appears at or below this temp.
const float FREEZE_WARN_F = 35.0;

// Pump maintenance interval (hours of runtime between cleanings)
const float MAINT_HOURS = 500.0;

// How often to persist the hour meter (ms). State changes save immediately;
// this is the backstop so a power cut loses at most this much runtime.
const unsigned long PUMP_SAVE_INTERVAL = 60000UL; // 60 seconds

// Weather refresh interval (15 minutes; weather changes slowly)
const unsigned long WX_INTERVAL = 15UL * 60UL * 1000UL;

AsyncWebServer server(80);
Preferences preferences;

unsigned long onTime, offTime;
bool pumpIsActive = false;
unsigned long previousMillis = 0;
bool sdActive = false;
String stationName = "LOCATING..."; // Will automatically update via WiFi

// --- Pump hour meter state ---
unsigned long pumpSecsTotal = 0;   // lifetime pump runtime, seconds
unsigned long pumpSecsMaint = 0;   // seconds since last cleaning
unsigned long pumpMsAccum = 0;     // sub-second accumulator
unsigned long lastAccumMillis = 0;
unsigned long lastPumpSaveMillis = 0;

// --- Weather state ---
float wxLat = 0, wxLon = 0;      // from IP geolocation
float wxTempF = 0;
String wxDesc = "--";
bool wxOk = false;
unsigned long lastWxMillis = 0;

// --- SD Log Rotation ---
// When tower_log.txt exceeds this size, it is renamed to tower_log.bak
// (overwriting the old backup) and a fresh log starts. Total log storage
// never exceeds 2 x this value.
const size_t LOG_MAX_BYTES = 100 * 1024; // 100 KB ~= 30 days at 15/15-min cycles

void savePumpHours() {
  preferences.putULong("pumpSecs", pumpSecsTotal);
  preferences.putULong("maintSecs", pumpSecsMaint);
}

// --- Tiny JSON extractors (no JSON library needed) ---
bool extractJsonString(const String& json, const char* key, String &out) {
  String k = String("\"") + key + "\"";
  int i = json.indexOf(k);
  if (i < 0) return false;
  int s = i + k.length();
  while (s < (int)json.length() && (json[s] == ' ' || json[s] == ':')) s++;
  if (s >= (int)json.length() || json[s] != '"') return false;
  s++;
  int e = json.indexOf('"', s);
  if (e < 0) return false;
  out = json.substring(s, e);
  return true;
}

// Finds "key":<number>. startFrom skips earlier same-named keys
// (e.g. the text labels inside "current_units").
bool extractNumber(const String& json, const char* key, float &out, int startFrom = 0) {
  if (startFrom < 0) startFrom = 0;
  String k = String("\"") + key + "\":";
  int i = json.indexOf(k, (unsigned int)startFrom);
  if (i < 0) {
    k = String("\"") + key + "\": "; // tolerate a space after the colon
    i = json.indexOf(k, (unsigned int)startFrom);
    if (i < 0) return false;
  }
  int s = i + k.length();
  int e = s;
  while (e < (int)json.length()) {
    char ch = json[e];
    if (isDigit(ch) || ch == '.' || ch == '-' || ch == '+' || ch == 'e' || ch == 'E') e++;
    else break;
  }
  if (e == s) return false;
  out = json.substring(s, e).toFloat();
  return true;
}

// --- Auto-Location Logic (JSON, key-based: immune to field order) ---
void updateLocation() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin("http://ip-api.com/json/?fields=status,city,region,lat,lon");
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    String body = http.getString();
    String status, city, region;
    float la = 0, lo = 0;
    bool ok = extractJsonString(body, "status", status)
           && extractJsonString(body, "city", city)
           && extractJsonString(body, "region", region)
           && extractNumber(body, "lat", la)
           && extractNumber(body, "lon", lo);
    if (ok && status == "success") {
      city.trim(); region.trim();
      city.toUpperCase(); region.toUpperCase();
      stationName = (region.length() > 0) ? (city + ", " + region) : city;
      if (stationName.length() > 28 || stationName.length() == 0) {
        stationName = "UNKNOWN"; // Safety net for malformed responses
      }
      wxLat = la;
      wxLon = lo;
      Serial.println("[OK] Auto-Location: " + stationName +
                     " lat=" + String(wxLat, 4) + " lon=" + String(wxLon, 4));
    } else {
      stationName = "UNKNOWN";
      Serial.println("[ERR] Location parse failed: " + body);
    }
  } else {
    stationName = "OFFLINE / UNKNOWN";
    Serial.println("[ERR] Auto-Location failed");
  }
  http.end();
}

// --- Weather Logic (Open-Meteo: free, no API key) ---
const char* wxCodeToText(int code) {
  switch (code) {
    case 0: return "CLEAR SKY";
    case 1: return "MAINLY CLEAR";
    case 2: return "PARTLY CLOUDY";
    case 3: return "OVERCAST";
    case 45: case 48: return "FOG";
    case 51: case 53: case 55: return "DRIZZLE";
    case 56: case 57: return "FREEZING DRIZZLE";
    case 61: return "LIGHT RAIN";
    case 63: return "RAIN";
    case 65: return "HEAVY RAIN";
    case 66: case 67: return "FREEZING RAIN";
    case 71: return "LIGHT SNOW";
    case 73: return "SNOW";
    case 75: return "HEAVY SNOW";
    case 77: return "SNOW GRAINS";
    case 80: case 81: case 82: return "RAIN SHOWERS";
    case 85: case 86: return "SNOW SHOWERS";
    case 95: return "THUNDERSTORM";
    case 96: case 99: return "T-STORM W/ HAIL";
    default: return "UNKNOWN";
  }
}

void updateWeather() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (wxLat == 0 && wxLon == 0) return; // no location yet

  HTTPClient http;
  String url = "http://api.open-meteo.com/v1/forecast?latitude=" + String(wxLat, 4)
             + "&longitude=" + String(wxLon, 4)
             + "&current=temperature_2m,weather_code&temperature_unit=fahrenheit";
  http.begin(url);
  http.setTimeout(8000);
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    String body = http.getString();
    // Read from the "current" block, skipping the text labels in "current_units"
    int cur = body.indexOf("\"current\":{");
    float t, c;
    if (cur >= 0 && extractNumber(body, "temperature_2m", t, cur)
                 && extractNumber(body, "weather_code", c, cur)) {
      wxTempF = t;
      wxDesc = wxCodeToText((int)c);
      wxOk = true;
      Serial.printf("[OK] Weather: %.1f F, %s\n", wxTempF, wxDesc.c_str());
      if (wxTempF <= FREEZE_WARN_F) {
        logToSD("Freeze_Warning");
      }
    } else {
      Serial.println("[ERR] Weather parse failed");
    }
  } else {
    Serial.printf("[ERR] Weather fetch failed: %d\n", httpCode);
  }
  http.end();
}

// --- Time & Logging Logic ---
String getTimestamp() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)) return "0000-00-00 00:00:00";
  char buffer[25];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buffer);
}

void logToSD(String message) {
  if(!sdActive) return;

  // Rotate the log if it has hit the size cap
  File check = SD.open("/tower_log.txt", FILE_READ);
  if (check) {
    if (check.size() >= LOG_MAX_BYTES) {
      check.close();
      SD.remove("/tower_log.bak");
      SD.rename("/tower_log.txt", "/tower_log.bak");
      Serial.println("[OK] Log rotated to tower_log.bak");
    } else {
      check.close();
    }
  }

  bool success = false;
  for (int i = 0; i < 3; i++) {
    File file = SD.open("/tower_log.txt", FILE_APPEND);
    if(file) {
      file.printf("[%s] %s\n", getTimestamp().c_str(), message.c_str());
      file.close();
      success = true;
      break;
    }
    delay(50);
  }

  if (!success) {
    Serial.println("[ERR] SD Write Failed after 3 attempts.");
  }
}

// Serve a log file as readable styled HTML (large type, dark theme)
void serveLog(AsyncWebServerRequest *request, const char* path, const char* title) {
  if (!(sdActive && SD.exists(path))) {
    request->send(200, "text/plain", "LOG FILE EMPTY OR SD CARD MISSING");
    return;
  }
  File f = SD.open(path, FILE_READ);
  String html;
  html.reserve(8192);
  html = "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
         "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
         "<title>";
  html += title;
  html += "</title></head>"
          "<body style=\"background:#1a120b; color:#e8e8e8; font-family:monospace; "
          "font-size:1.25rem; line-height:1.7; padding:20px;\">"
          "<div style=\"color:#cd7f32; font-family:sans-serif; font-weight:bold; "
          "font-size:1.6rem; margin-bottom:16px;\">";
  html += title;
  html += "</div><pre style=\"white-space:pre-wrap; margin:0;\">";
  uint8_t buf[256];
  while (f.available()) {
    size_t n = f.read(buf, sizeof(buf));
    for (size_t i = 0; i < n; i++) {
      char c = (char)buf[i];
      if (c == '&') html += "&amp;";
      else if (c == '<') html += "&lt;";
      else if (c == '>') html += "&gt;";
      else html += c;
    }
  }
  f.close();
  html += "</pre><div style=\"margin-top:24px;\">"
          "<a href=\"/\" style=\"color:#00FF00; font-family:sans-serif; "
          "font-size:1.2rem; font-weight:bold;\">&#11013; BACK TO TOWER COMMAND</a>"
          "</div></body></html>";
  request->send(200, "text/html", html);
}

// --- High-Contrast UI: two columns (status/timing | manual/logs) ---
// NOTE: this template must not contain any literal % character outside of
// %PLACEHOLDER% pairs -- the template engine pairs each % with the next %
// and deletes everything between them as a "placeholder".
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html>
<head>
  <meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1.0">
</head>
<body style="background-color:#1a120b; color:#ffffff; font-family:sans-serif; font-size:20px; text-align:center; padding:18px;">
  <h1 style="color:#cd7f32; font-size:2.6rem; margin:0 0 6px 0;">TOWER COMMAND v59.9</h1>
  <p style="font-size:1.05rem; color:#b08d57; letter-spacing:2px; margin:0 0 14px 0;">STATION: %STATION_NAME%</p>
  %FREEZE_BANNER%
  <div style="font-size:1.35rem; color:#e8c87a; font-weight:bold; margin:0 0 20px 0;">&#127777; %WX_LINE%</div>
  %MAINT_BANNER%

  <div style="display:flex; flex-wrap:wrap; gap:24px; justify-content:center; align-items:flex-start; max-width:1040px; margin:0 auto;">

    <!-- LEFT COLUMN: status + timing -->
    <div style="flex:1 1 320px; max-width:480px; min-width:280px;">
      <div style="background:#000; border:4px solid #cd7f32; padding:26px 20px; margin:0 0 20px 0; border-radius:15px;">
        <div style="font-size:4rem; font-weight:900; color:%COLOR%; line-height:1;">%STATE%</div>
        <div style="font-size:1.6rem; color:#cd7f32; font-weight:bold; margin-top:14px;">REMAINING: %REMAINING%</div>
        <div style="font-size:1.35rem; color:#e0e0e0; font-weight:bold; margin-top:12px;">DATE: %CTIME%</div>
        <div style="font-size:1.15rem; color:#b08d57; margin-top:12px;">%PUMP_HRS%</div>
      </div>

      <div style="font-size:1.05rem; color:#d6d6d6; text-align:left; padding:0 8px; line-height:1.6; margin:0 0 16px 0;">
        <b style="color:#cd7f32;">HOW TO CHANGE TIMING:</b><br>
        1) Type new FLOW / IDLE minutes below.<br>
        2) Press <b style="color:#00FF00;">ENGAGE SETTINGS</b> &mdash; the new cycle starts immediately.<br>
        3) If the screen refreshes before you can press the button, hit <b>PAUSE UPDATES</b> below first. Updates stay paused until the page reloads after you save.
      </div>

      <form action="/update" style="text-align:left; margin:0;">
        <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:16px; font-size:1.45rem;">
          <b style="color:#cd7f32;">FLOW MINUTES</b>
          <input type="number" name="on" value="%ON%" style="width:96px; padding:6px; font-size:2rem; font-weight:bold; border:2px solid #cd7f32; background:#000; color:#fff; text-align:center;">
        </div>
        <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:22px; font-size:1.45rem;">
          <b style="color:#cd7f32;">IDLE MINUTES</b>
          <input type="number" name="off" value="%OFF%" style="width:96px; padding:6px; font-size:2rem; font-weight:bold; border:2px solid #cd7f32; background:#000; color:#fff; text-align:center;">
        </div>
        <input type="submit" value="&#11014; ENGAGE SETTINGS &#11014;" style="display:block; padding:26px 10px; background:#00FF00; color:#000; font-weight:900; font-size:2rem; border:5px solid #ffffff; border-radius:14px; cursor:pointer; margin-bottom:20px; box-shadow:0 0 30px #00FF00; letter-spacing:1px;">
      </form>

      <div>
        <div id="pauseBtn" onclick="togglePause()" style="padding:15px; background:#222; color:#cd7f32; font-weight:bold; font-size:1.2rem; border:2px solid #cd7f32; border-radius:8px; cursor:pointer; margin-bottom:8px;">&#10074;&#10074; PAUSE UPDATES</div>
        <div id="pauseHint" style="font-size:1rem; color:#999;">Live updates running &mdash; refreshing in <span id="count">10</span>s.</div>
      </div>
    </div>

    <!-- RIGHT COLUMN: manual override + logs -->
    <div style="flex:1 1 320px; max-width:480px; min-width:280px;">
      <div style="font-size:1.1rem; color:#cd7f32; font-weight:bold; letter-spacing:2px; margin:0 0 14px 0;">MANUAL &amp; LOGS</div>
      <a href="/toggle" style="text-decoration:none;">
        <div style="padding:24px; background:%TOG_BG%; color:#000; font-weight:900; font-size:1.9rem; border:4px solid #000; border-radius:12px; margin:0 0 24px 0;">
          %TOG_TEXT%
        </div>
      </a>

      <div style="font-size:1rem; color:#ccc; margin-bottom:10px; text-align:left; padding:0 10px; line-height:1.5;">
        <i>Note: Logs record the date and time of all automatic cycles and manual overrides. Scroll to the bottom for the newest entries.</i>
      </div>
      <a href="/view-log" style="text-decoration:none; display:block;">
        <div style="padding:16px; background:#3d2b1f; color:#fff; font-weight:bold; font-size:1.3rem; border:2px solid #5c3d2e; border-radius:8px; margin-bottom:14px;">
          VIEW SYSTEM LOGS
        </div>
      </a>
      <a href="/view-log-prev" style="text-decoration:none; display:block;">
        <div style="padding:10px; background:#222; color:#999; font-size:1rem; border:1px solid #5c3d2e; border-radius:5px; margin-bottom:14px;">
          VIEW PREVIOUS LOG (ARCHIVE)
        </div>
      </a>
      <a href="/clear-log" onclick="return confirmDelete()" style="text-decoration:none; display:block;">
        <div style="padding:12px; background:#4d0000; color:#ff9999; font-weight:bold; font-size:1rem; border-radius:5px;">
          [ RESET SD LOG FILE ]
        </div>
      </a>
    </div>
  </div>

<script>
var secs = 10;
var paused = false;
var timer = setInterval(tick, 1000);
function tick() {
  if (paused) return;
  secs--;
  var el = document.getElementById('count');
  if (el) el.textContent = secs;
  if (secs <= 0) location.reload();
}
function togglePause() {
  paused = !paused;
  document.getElementById('pauseBtn').innerHTML = paused ? '&#9654; RESUME UPDATES' : '&#10074;&#10074; PAUSE UPDATES';
  document.getElementById('pauseHint').innerHTML = paused
    ? 'Updates <b style="color:#cd7f32;">paused</b> &mdash; take your time, then press ENGAGE SETTINGS.'
    : 'Live updates running &mdash; refreshing in <span id="count">' + secs + '</span>s.';
}
function confirmDelete() {
  var t = prompt('To erase the SD log history, type DELETE and press OK:');
  return (t === 'DELETE');
}
</script>
</body></html>)rawliteral";

String processor(const String& var) {
  unsigned long now = millis();
  unsigned long interval = (pumpIsActive ? onTime : offTime) * 60000ULL;
  unsigned long elapsed = now - previousMillis;

  unsigned long remainingMillis = (elapsed >= interval) ? 0 : (interval - elapsed);
  unsigned long totalSeconds = remainingMillis / 1000;
  int displayMins = totalSeconds / 60;
  int displaySecs = totalSeconds % 60;
  char timeBuf[10];
  sprintf(timeBuf, "%02d:%02d", displayMins, displaySecs);

  if (var == "STATION_NAME") return stationName;
  if (var == "WX_LINE") {
    if (!wxOk) return "WEATHER UNAVAILABLE";
    char buf[48];
    snprintf(buf, sizeof(buf), "%.0f&deg;F &middot; %s", wxTempF, wxDesc.c_str());
    return String(buf);
  }
  if (var == "FREEZE_BANNER") {
    if (wxOk && wxTempF <= FREEZE_WARN_F) {
      char buf[160];
      snprintf(buf, sizeof(buf),
        "<div style=\"background:#7a0000; border:3px solid #ff4444; color:#fff; font-weight:900;"
        "font-size:1.3rem; padding:14px; border-radius:10px; margin:0 auto 20px auto; max-width:480px;\">"
        "&#9888; FREEZE WARNING &mdash; %.0f&deg;F &mdash; protect the tower!</div>",
        wxTempF);
      return String(buf);
    }
    return String("");
  }
  if (var == "PUMP_HRS") {
    char buf[80];
    float totalHrs = pumpSecsTotal / 3600.0;
    float sinceHrs = pumpSecsMaint / 3600.0;
    if (sinceHrs < MAINT_HOURS) {
      snprintf(buf, sizeof(buf), "PUMP RUNTIME: %.1f HRS &middot; CLEAN IN %.0f HRS",
               totalHrs, MAINT_HOURS - sinceHrs);
    } else {
      snprintf(buf, sizeof(buf), "PUMP RUNTIME: %.1f HRS", totalHrs);
    }
    return String(buf);
  }
  if (var == "MAINT_BANNER") {
    float sinceHrs = pumpSecsMaint / 3600.0;
    if (sinceHrs >= MAINT_HOURS) {
      char buf[280];
      snprintf(buf, sizeof(buf),
        "<div style=\"background:#5c3d00; border:3px solid #ffaa00; color:#ffe9b0; font-weight:900;"
        "font-size:1.2rem; padding:14px; border-radius:10px; margin:0 auto 20px auto; max-width:480px;\">"
        "&#9888; PUMP MAINTENANCE DUE &mdash; %.1f HRS SINCE LAST CLEANING<br>"
        "<a href=\"/reset-maint\" onclick=\"return confirm('Mark pump as cleaned?')\" "
        "style=\"color:#00FF00; font-size:1rem;\">[ MARK AS CLEANED ]</a></div>",
        sinceHrs);
      return String(buf);
    }
    return String("");
  }
  if (var == "STATE") return pumpIsActive ? "ACTIVE" : "IDLE";
  if (var == "COLOR") return pumpIsActive ? "#00FF00" : "#FF4444";
  if (var == "CTIME") return getTimestamp();
  if (var == "ON") return String(onTime);
  if (var == "OFF") return String(offTime);
  if (var == "REMAINING") return String(timeBuf);
  if (var == "TOG_BG") return pumpIsActive ? "#00FF00" : "#FF4444";
  if (var == "TOG_TEXT") return pumpIsActive ? "MANUAL STOP" : "MANUAL START";
  return String("");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n--- TOWER BOOT v59.9 ---");

  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, HIGH);

 // --- WiFiManager (Captive Portal) ---
  WiFiManager wm;

   //wm.resetSettings(); // <--- ADD THIS LINE HERE

  if (!wm.autoConnect("Tower-Setup")) {
    Serial.println("Failed to connect. Restarting...");
    delay(3000);
    ESP.restart();
  }

  Serial.print("\n[OK] Connected! IP: "); Serial.println(WiFi.localIP());

  // --- Auto Geolocation (also provides lat/lon for weather) ---
  updateLocation();

  // --- First weather fetch ---
  updateWeather();
  lastWxMillis = millis();

  // --- OTA Setup ---
  ArduinoOTA.setHostname("Hydro-Tower");
  ArduinoOTA.onStart([]() { Serial.println("Start updating"); });
  ArduinoOTA.onEnd([]() { Serial.println("\nEnd"); });
  ArduinoOTA.begin();

  // --- NTP Time Setup ---
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  // --- mDNS Setup ---
  if (MDNS.begin(MDNS_HOSTNAME)) {
    Serial.println("[OK] mDNS Responder Started. Access at http://" + String(MDNS_HOSTNAME) + ".local");
  }

  // --- SD Card Setup ---
  SPI.begin(18, 19, 23, 5);
  if(SD.begin(chipSelect)) {
    sdActive = true;
    logToSD("REBOOT SEQUENCE v59.9");
    Serial.println("[OK] SD Active");
  } else {
    Serial.println("[ERR] SD Card Mount Failed");
  }

  // --- Preferences (timings + pump hour meter) ---
  preferences.begin("tower", false);
  onTime = preferences.getUInt("on", 15);
  offTime = preferences.getUInt("off", 15);
  pumpSecsTotal = preferences.getULong("pumpSecs", 0);
  pumpSecsMaint = preferences.getULong("maintSecs", 0);
  lastAccumMillis = millis();
  lastPumpSaveMillis = millis();
  Serial.printf("[OK] Pump hour meter: %.1f hrs total, %.1f hrs since cleaning\n",
                pumpSecsTotal / 3600.0, pumpSecsMaint / 3600.0);

  // --- Web Server Routes ---
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) { request->send_P(200, "text/html", index_html, processor); });

  server.on("/view-log", HTTP_GET, [](AsyncWebServerRequest *request){
    serveLog(request, "/tower_log.txt", "SYSTEM LOG");
  });

  server.on("/view-log-prev", HTTP_GET, [](AsyncWebServerRequest *request){
    serveLog(request, "/tower_log.bak", "SYSTEM LOG (ARCHIVE)");
  });

  server.on("/clear-log", HTTP_GET, [](AsyncWebServerRequest *request){
    if(sdActive) { SD.remove("/tower_log.txt"); SD.remove("/tower_log.bak"); logToSD("LOG_CLEARED"); }
    request->redirect("/");
  });

  server.on("/reset-maint", HTTP_GET, [](AsyncWebServerRequest *request){
    pumpSecsMaint = 0;
    savePumpHours();
    logToSD("Pump_Cleaned");
    request->redirect("/");
  });

  server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request) {
    if(request->hasParam("on")) onTime = request->getParam("on")->value().toInt();
    if(request->hasParam("off")) offTime = request->getParam("off")->value().toInt();
    preferences.putUInt("on", onTime); preferences.putUInt("off", offTime);
    previousMillis = millis();
    request->redirect("/");
  });

  server.on("/toggle", HTTP_GET, [](AsyncWebServerRequest *request) {
    pumpIsActive = !pumpIsActive; digitalWrite(relayPin, pumpIsActive ? LOW : HIGH);
    previousMillis = millis();
    savePumpHours();
    logToSD(pumpIsActive ? "Manual_Start" : "Manual_Stop");
    request->redirect("/");
  });

  server.begin();
  Serial.println("[OK] System Ready");
}

void loop() {
  ArduinoOTA.handle();

  unsigned long nowMs = millis();

  // --- Pump hour meter: accumulate while the pump is on ---
  unsigned long elapsed = nowMs - lastAccumMillis; // rollover-safe
  lastAccumMillis = nowMs;
  if (pumpIsActive && elapsed > 0) {
    pumpMsAccum += elapsed;
    while (pumpMsAccum >= 1000) {
      pumpMsAccum -= 1000;
      pumpSecsTotal++;
      pumpSecsMaint++;
    }
  }
  // Persist periodically so a power cut loses at most ~60 seconds
  if (nowMs - lastPumpSaveMillis >= PUMP_SAVE_INTERVAL) {
    lastPumpSaveMillis = nowMs;
    savePumpHours();
  }

  // Refresh weather every 15 minutes (non-blocking)
  if (nowMs - lastWxMillis >= WX_INTERVAL) {
    lastWxMillis = nowMs;
    updateWeather();
  }

  unsigned long interval = (pumpIsActive ? onTime : offTime) * 60000ULL;

  if (nowMs - previousMillis >= interval) {
    previousMillis = nowMs;
    pumpIsActive = !pumpIsActive;
    digitalWrite(relayPin, pumpIsActive ? LOW : HIGH);
    savePumpHours();
    logToSD(pumpIsActive ? "Auto_Start" : "Auto_Stop");
  }
}
