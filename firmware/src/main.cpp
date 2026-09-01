#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <heltec-eink-modules.h>
#include <time.h>
#include <math.h>

#include "secrets.h"

EInkDisplay_WirelessPaperV1_2 display;

namespace {

const unsigned long POLL_INTERVAL_MS = 60000;
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;
const unsigned long HTTP_TIMEOUT_MS = 5000;

// POSIX TZ rule, DST-aware. Change this if the display isn't in Germany.
const char *LOCAL_TIMEZONE = "CET-1CEST,M3.5.0,M10.5.0/3";

struct AppUsage {
  int h5_pct = -1;
  int week_pct = -1;
  long h5_reset = 0;    // unix epoch seconds, 0 = unknown
  long week_reset = 0;
};

struct DashboardState {
  bool valid = false;  // true once at least one successful fetch happened
  AppUsage claude;
  AppUsage codex;
};

DashboardState lastGood;
String lastSignature = "";
int waveVariant = 0;  // rotates through 4 separator wave shapes on each redraw

bool configIsPlaceholder() {
  return strcmp(WIFI_SSID, "CHANGE_ME_WIFI_SSID") == 0 ||
         strcmp(WIFI_PASSWORD, "CHANGE_ME_WIFI_PASSWORD") == 0 ||
         strcmp(DASHBOARD_HOST, "CHANGE_ME_DASHBOARD_IP") == 0;
}

String buildUrl() {
  String url = "http://";
  url += DASHBOARD_HOST;
  url += ":";
  url += String(DASHBOARD_PORT);
  url += "/state.json";
  return url;
}

bool ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  Serial.print("WiFi: connecting to \"");
  Serial.print(WIFI_SSID);
  Serial.println("\"...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi: connected, IP=");
    Serial.println(WiFi.localIP());
    configTime(0, 0, "pool.ntp.org", "time.google.com");  // sync UTC via NTP
    setenv("TZ", LOCAL_TIMEZONE, 1);  // then interpret it as local time (DST-aware)
    tzset();
    return true;
  }

  Serial.println("WiFi: connection failed");
  return false;
}

// True once NTP has set the clock (vs. the ESP32's boot-time default).
bool timeIsSynced() {
  return time(nullptr) > 1700000000L;
}

// Extract "ok" + used_pct from one app's JSON object; false if missing,
// not-ok, or the percentages are out of range.
bool parseAppUsage(JsonVariantConst obj, AppUsage &out) {
  if (obj.isNull()) {
    return false;
  }

  bool ok = obj["ok"] | false;
  // Server may send used_pct/reset as floats (e.g. 21.0) - ArduinoJson's
  // `| int_default` silently falls back to the default for those, so read
  // as float/double and round instead of reading straight into an int.
  float h5f = obj["h5"]["used_pct"] | -1.0f;
  float weekf = obj["week"]["used_pct"] | -1.0f;
  int h5 = (h5f < 0.0f) ? -1 : (int)(h5f + 0.5f);
  int week = (weekf < 0.0f) ? -1 : (int)(weekf + 0.5f);

  if (!ok || h5 < 0 || h5 > 100 || week < 0 || week > 100) {
    return false;
  }

  double h5resetF = obj["h5"]["reset"] | 0.0;
  double weekresetF = obj["week"]["reset"] | 0.0;

  out.h5_pct = h5;
  out.week_pct = week;
  out.h5_reset = (long)h5resetF;
  out.week_reset = (long)weekresetF;
  return true;
}

// Fetch and parse /state.json. Only overwrites `out` on full success;
// on any failure `out` is left untouched so the caller can keep showing
// the last known-good values.
bool fetchState(DashboardState &out) {
  HTTPClient http;
  String url = buildUrl();

  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);

  if (!http.begin(url)) {
    Serial.println("HTTP: begin() failed");
    return false;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.print("HTTP: error, code=");
    Serial.println(code);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  Serial.print("HTTP: code=");
  Serial.print(code);
  Serial.print(" length=");
  Serial.println(payload.length());

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.print("JSON: parse error: ");
    Serial.println(err.c_str());
    Serial.print("JSON: Payload(200)=");
    Serial.println(payload.substring(0, 200));
    return false;
  }

  AppUsage claude, codex;
  bool claudeOk = parseAppUsage(doc["claude"], claude);
  bool codexOk = parseAppUsage(doc["codex"], codex);

  if (!claudeOk || !codexOk) {
    Serial.printf("JSON: claude(ok=%d h5=%.1f week=%.1f) codex(ok=%d h5=%.1f week=%.1f)\n",
                   (bool)(doc["claude"]["ok"] | false),
                   (float)(doc["claude"]["h5"]["used_pct"] | -1.0),
                   (float)(doc["claude"]["week"]["used_pct"] | -1.0),
                   (bool)(doc["codex"]["ok"] | false),
                   (float)(doc["codex"]["h5"]["used_pct"] | -1.0),
                   (float)(doc["codex"]["week"]["used_pct"] | -1.0));
    return false;
  }

  out.claude = claude;
  out.codex = codex;
  out.valid = true;
  return true;
}

// Layout constants for the usage rows (250 x 122 landscape screen).
const int MARGIN = 4;
const int LABEL_X = MARGIN;
const int BAR_X = 32;
const int BAR_W = 160;
const int BAR_H = 16;
const int PCT_X = BAR_X + BAR_W + 6;

void formatPctText(char *buf, size_t len, int pct) {
  if (pct < 0) {
    snprintf(buf, len, " --%%");
  } else {
    snprintf(buf, len, "%3d%%", pct);
  }
}

// Time remaining until `resetEpoch` as "HH:MM", or "DHH:MM" (day count
// then hours:minutes) when withDays is set. Dashes if unknown/not synced.
void formatCountdown(char *buf, size_t len, long resetEpoch, bool withDays) {
  if (resetEpoch <= 0 || !timeIsSynced()) {
    snprintf(buf, len, withDays ? "-D--:--" : "--:--");
    return;
  }

  long remaining = resetEpoch - (long)time(nullptr);
  if (remaining < 0) remaining = 0;
  long totalMin = remaining / 60;

  if (withDays) {
    long days = totalMin / (24 * 60);
    long hours = (totalMin / 60) % 24;
    long mins = totalMin % 60;
    snprintf(buf, len, "%ldD%02ld:%02ld", days, hours, mins);
  } else {
    long hours = totalMin / 60;
    long mins = totalMin % 60;
    snprintf(buf, len, "%02ld:%02ld", hours, mins);
  }
}

// Local clock time a window resets at, e.g. "Reset 14:32" - stamped on the
// bar from RESET_STAMP_PCT on (see drawUsageRow()). Falls back to "LIMIT"
// if the reset time is unknown or the clock isn't synced yet.
void formatResetClock(char *buf, size_t len, long resetEpoch) {
  if (resetEpoch <= 0 || !timeIsSynced()) {
    snprintf(buf, len, "LIMIT");
    return;
  }
  time_t t = (time_t)resetEpoch;
  struct tm tmResult;
  localtime_r(&t, &tmResult);
  strftime(buf, len, "Reset %H:%M", &tmResult);
}

// Same, but with a weekday prefix - for the 7-day window, whose reset can
// be days away, e.g. "Reset Thu 22:00".
void formatResetClockWithDay(char *buf, size_t len, long resetEpoch) {
  if (resetEpoch <= 0 || !timeIsSynced()) {
    snprintf(buf, len, "LIMIT");
    return;
  }
  time_t t = (time_t)resetEpoch;
  struct tm tmResult;
  localtime_r(&t, &tmResult);
  strftime(buf, len, "Reset %a %H:%M", &tmResult);
}

// The default fixed-width font leaves ':' looking too spaced-out and 'D'
// too cramped. printTightTime() manually kerns both (text size 2 only).
const int COLON_TRIM_PX = 4;  // shaved off total width, split both sides
const int D_GAP_PX = 6;       // extra space after 'D', ~half a char

int countChar(const char *s, char c) {
  int n = 0;
  for (; *s; ++s) {
    if (*s == c) n++;
  }
  return n;
}

void printTightTime(int x, int y, const char *text, int textSize) {
  const int charW = 6 * textSize;
  int cx = x;
  display.setTextSize(textSize);
  for (const char *p = text; *p; ++p) {
    int drawX = (*p == ':') ? cx - COLON_TRIM_PX / 2 : cx;
    display.setCursor(drawX, y);
    display.print(*p);
    cx += (*p == ':') ? charW - COLON_TRIM_PX : charW;
    if (*p == 'D') cx += D_GAP_PX;
  }
}

// One section's header: app name left-aligned, "<5H>-<7D>" countdown
// right-aligned, e.g. "CLAUDE" ... "02:47-3D14:07". Either countdown may
// be null - a window at 100% has nothing left to count down, so it's
// dropped from the header entirely (its reset time is shown in the bar
// instead; see formatResetClock()).
void drawSectionHeader(int y, const char *label, const char *h5cd, const char *weekcd) {
  display.setTextSize(2);
  display.setTextColor(BLACK);

  display.setCursor(MARGIN, y);
  display.print(label);

  char countdown[24];
  if (h5cd && weekcd) {
    snprintf(countdown, sizeof(countdown), "%s-%s", h5cd, weekcd);
  } else if (h5cd || weekcd) {
    snprintf(countdown, sizeof(countdown), "%s", h5cd ? h5cd : weekcd);
  } else {
    return;  // both windows maxed - nothing to show up here
  }

  int rawWidth = display.getTextWidth(countdown);
  int trimmedWidth = rawWidth - COLON_TRIM_PX * countChar(countdown, ':') +
                      D_GAP_PX * countChar(countdown, 'D');
  int x = display.width() - trimmedWidth - MARGIN;
  printTightTime(x, y, countdown, 2);
}

// Separator between the two sections: a small pixel wave (variant 0-3,
// rotates on each redraw) plus the OFFLINE/STALE badge, right-aligned on
// the same row - there's no room for the badge next to the edge-to-edge
// header lines above, so it lives here instead.
void drawWaveSeparator(int yCenter, int variant, const char *statusMarker) {
  const int amplitude = 3;
  const float wavelengths[4] = {36.0f, 22.0f, 54.0f, 16.0f};
  const float phases[4] = {0.0f, 1.1f, 2.3f, 0.5f};
  const float wavelength = wavelengths[variant % 4];
  const float phase = phases[variant % 4];

  const int startX = MARGIN;
  int endX = display.width() - MARGIN;

  display.setTextSize(1);
  display.setTextColor(BLACK);
  if (statusMarker) {
    uint16_t w = display.getTextWidth(statusMarker);
    endX = display.width() - MARGIN - w - 6;
  }

  for (int x = startX; x < endX; x += 3) {
    float t = (x - startX) / wavelength;
    int yOff = (int)roundf(amplitude * sinf(2.0f * PI * t + phase));
    display.fillRect(x, yCenter + yOff - 1, 2, 2, BLACK);
  }

  if (statusMarker) {
    uint16_t w = display.getTextWidth(statusMarker);
    display.setCursor(display.width() - w - MARGIN, yCenter - 4);
    display.print(statusMarker);
  }
}

// Earliest percentage the bar stamp may appear at. Below 100% it's only
// actually drawn once the black-filled part is wide enough to hold it
// (checked against fillW below) - at low percentages/long strings (e.g.
// the 7-day "Reset Thu 22:00") it just stays hidden a bit longer.
const int RESET_STAMP_PCT = 66;

// One "5H [bar] xx%" row: label, proportional bar, exact percentage.
// pct < 0 (no data yet) draws an empty bar with "--%". From RESET_STAMP_PCT
// on, `limitText` (the local reset clock time - see formatResetClock()) is
// stamped in white on the filled part of the bar: left-aligned while
// there's still an unfilled part (so it's not next to a moving edge), or
// right-aligned once the bar is fully black at 100%. Only drawn if the
// filled part is actually wide enough to hold it, white-on-white would be
// invisible otherwise.
void drawUsageRow(int y, const char *label, int pct, const char *limitText) {
  display.setTextSize(2);
  display.setTextColor(BLACK);
  display.setCursor(LABEL_X, y);
  display.print(label);

  display.drawRect(BAR_X, y, BAR_W, BAR_H, BLACK);

  int innerW = BAR_W - 2;
  int fillW = (pct <= 0) ? 0 : (int)((long)innerW * pct / 100);
  if (fillW > innerW) fillW = innerW;
  if (fillW > 0) {
    display.fillRect(BAR_X + 1, y + 1, fillW, BAR_H - 2, BLACK);
  }

  if (pct >= RESET_STAMP_PCT) {
    display.setTextSize(1);
    display.setTextColor(WHITE);
    uint16_t lw = display.getTextWidth(limitText);
    bool rightAligned = (pct >= 100);
    int available = rightAligned ? BAR_W : fillW;  // right-aligned: whole bar is black anyway
    if (lw + 8 <= (uint16_t)available) {
      int stampX = rightAligned ? (BAR_X + BAR_W - lw - 4) : (BAR_X + 4);
      display.setCursor(stampX, y + (BAR_H - 8) / 2);
      display.print(limitText);
    }
    display.setTextColor(BLACK);
  }

  char pctText[6];
  formatPctText(pctText, sizeof(pctText), pct);
  display.setTextSize(2);
  display.setCursor(PCT_X, y);
  display.print(pctText);
}

// Redraws the display, but only if the content actually changed since the
// last draw - avoids unnecessary e-ink refreshes.
void renderScreen(const DashboardState &state, bool currentFetchOk, bool wifiConnected) {
  const char *statusMarker = nullptr;
  if (!wifiConnected) {
    statusMarker = "OFFLINE";
  } else if (!currentFetchOk) {
    statusMarker = "STALE";
  }

  int claude5h = state.valid ? state.claude.h5_pct : -1;
  int claude7d = state.valid ? state.claude.week_pct : -1;
  int codex5h = state.valid ? state.codex.h5_pct : -1;
  int codex7d = state.valid ? state.codex.week_pct : -1;

  // Header countdown: only computed (and shown) for windows below 100% -
  // a maxed-out window has nothing left to count down, and dropping it
  // stops the header from changing every minute for no reason.
  char claudeH5cdBuf[8], claudeWkcdBuf[10], codexH5cdBuf[8], codexWkcdBuf[10];
  const char *claudeH5cd = nullptr;
  const char *claudeWkcd = nullptr;
  const char *codexH5cd = nullptr;
  const char *codexWkcd = nullptr;

  if (claude5h < 100) {
    formatCountdown(claudeH5cdBuf, sizeof(claudeH5cdBuf), state.valid ? state.claude.h5_reset : 0, false);
    claudeH5cd = claudeH5cdBuf;
  }
  if (claude7d < 100) {
    formatCountdown(claudeWkcdBuf, sizeof(claudeWkcdBuf), state.valid ? state.claude.week_reset : 0, true);
    claudeWkcd = claudeWkcdBuf;
  }
  if (codex5h < 100) {
    formatCountdown(codexH5cdBuf, sizeof(codexH5cdBuf), state.valid ? state.codex.h5_reset : 0, false);
    codexH5cd = codexH5cdBuf;
  }
  if (codex7d < 100) {
    formatCountdown(codexWkcdBuf, sizeof(codexWkcdBuf), state.valid ? state.codex.week_reset : 0, true);
    codexWkcd = codexWkcdBuf;
  }

  // Bar stamp for a maxed-out window: the local reset clock time instead
  // of the countdown that moved out of the header.
  char claudeLimitH5[16], claudeLimitWk[20], codexLimitH5[16], codexLimitWk[20];
  formatResetClock(claudeLimitH5, sizeof(claudeLimitH5), state.valid ? state.claude.h5_reset : 0);
  formatResetClockWithDay(claudeLimitWk, sizeof(claudeLimitWk), state.valid ? state.claude.week_reset : 0);
  formatResetClock(codexLimitH5, sizeof(codexLimitH5), state.valid ? state.codex.h5_reset : 0);
  formatResetClockWithDay(codexLimitWk, sizeof(codexLimitWk), state.valid ? state.codex.week_reset : 0);

  String signature = String(statusMarker ? statusMarker : "OK") + "|" +
                      claude5h + "|" + claude7d + "|" + codex5h + "|" + codex7d + "|" +
                      (claudeH5cd ? claudeH5cd : "MAX") + "|" + (claudeWkcd ? claudeWkcd : "MAX") + "|" +
                      (codexH5cd ? codexH5cd : "MAX") + "|" + (codexWkcd ? codexWkcd : "MAX") + "|" +
                      claudeLimitH5 + "|" + claudeLimitWk + "|" + codexLimitH5 + "|" + codexLimitWk;
  if (signature == lastSignature) {
    return;  // Nothing changed on screen - skip the refresh.
  }
  lastSignature = signature;

  display.clear();

  // Claude block starts at the top edge; Codex is pushed down, leaving a
  // wider middle gap for the wave separator and the status badge.
  const int yClaudeHeader = 1;
  const int yClaude5h = 19;
  const int yClaude7d = 37;
  const int yClaudeBlockEnd = yClaude7d + BAR_H;
  const int yCodexHeader = 68;
  const int yWaveCenter = (yClaudeBlockEnd + yCodexHeader) / 2;
  const int yCodex5h = 86;
  const int yCodex7d = 104;

  drawSectionHeader(yClaudeHeader, "CLAUDE", claudeH5cd, claudeWkcd);
  drawUsageRow(yClaude5h, "5H", claude5h, claudeLimitH5);
  drawUsageRow(yClaude7d, "7D", claude7d, claudeLimitWk);

  drawWaveSeparator(yWaveCenter, waveVariant, statusMarker);
  waveVariant = (waveVariant + 1) % 4;

  drawSectionHeader(yCodexHeader, "CODEX", codexH5cd, codexWkcd);
  drawUsageRow(yCodex5h, "5H", codex5h, codexLimitH5);
  drawUsageRow(yCodex7d, "7D", codex7d, codexLimitWk);

  display.update();

  Serial.println("Display: updated");
}

void renderConfigMissing() {
  display.clear();
  display.setTextSize(2);
  display.setTextColor(BLACK);
  display.setCursor(6, 40);
  display.print("CONFIG");
  display.setCursor(6, 65);
  display.print("MISSING");
  display.update();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("Starting E-Ink...");

  display.landscape();
  display.clear();

  if (configIsPlaceholder()) {
    Serial.println("Config: include/secrets.h still contains placeholders.");
    Serial.println("Please set WIFI_SSID / WIFI_PASSWORD / DASHBOARD_HOST and reflash.");
    renderConfigMissing();
    return;
  }

  Serial.println("E-Ink ready, starting WiFi + data fetch...");
}

void loop() {
  if (configIsPlaceholder()) {
    delay(POLL_INTERVAL_MS);
    return;
  }

  bool wifiConnected = ensureWifi();
  bool fetchOk = false;

  if (wifiConnected) {
    DashboardState fetched;
    fetchOk = fetchState(fetched);
    if (fetchOk) {
      lastGood = fetched;
    }
  }

  Serial.print("Cycle: wifi=");
  Serial.print(wifiConnected ? "up" : "down");
  Serial.print(" fetch=");
  Serial.print(fetchOk ? "ok" : "fail");
  Serial.print(" time=");
  Serial.print(timeIsSynced() ? "sync" : "nosync");
  if (lastGood.valid) {
    Serial.printf(" claude(5h=%d%%,7d=%d%%) codex(5h=%d%%,7d=%d%%)\n",
                   lastGood.claude.h5_pct, lastGood.claude.week_pct,
                   lastGood.codex.h5_pct, lastGood.codex.week_pct);
  } else {
    Serial.println(" (no data yet)");
  }

  renderScreen(lastGood, fetchOk, wifiConnected);

  delay(POLL_INTERVAL_MS);
}
