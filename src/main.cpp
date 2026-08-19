// reTerminal E1002 firmware: fetch the dashboard payload, render it, deep sleep.
//
// Layout and payload parsing live in dashboard_view.h so the host simulator in
// sim/ exercises the same code. Everything in this file is device-only:
// networking, transports, provisioning, and power management.

#include <Arduino.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <GxEPD2_7C.h>
#include "secrets.h"
#include "isrg-root-x1.h"
#include "dashboard_view.h"

namespace {
constexpr int EPD_SCK = 7;
constexpr int EPD_MOSI = 9;
constexpr int EPD_CS = 10;
constexpr int EPD_DC = 11;
constexpr int EPD_RST = 12;
constexpr int EPD_BUSY = 13;
constexpr uint64_t REFRESH_INTERVAL_US = 30ULL * 60ULL * 1000000ULL;
constexpr uint64_t RETRY_INTERVAL_US = 5ULL * 60ULL * 1000000ULL;
constexpr size_t ERROR_SIZE = 96;

SPIClass epaperSpi(HSPI);

#define MAX_HEIGHT(EPD)                                                      \
  (EPD::HEIGHT <= dashboard::MAX_DISPLAY_BUFFER_SIZE / (EPD::WIDTH / 2)      \
       ? EPD::HEIGHT                                                         \
       : dashboard::MAX_DISPLAY_BUFFER_SIZE / (EPD::WIDTH / 2))

GxEPD2_7C<GxEPD2_730c_GDEP073E01, MAX_HEIGHT(GxEPD2_730c_GDEP073E01)> display(
    GxEPD2_730c_GDEP073E01(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

void logLine(const String& message) {
  Serial1.println(message);
  Serial1.flush();
}

void initDisplay() {
  pinMode(EPD_RST, OUTPUT);
  pinMode(EPD_DC, OUTPUT);
  pinMode(EPD_CS, OUTPUT);
  epaperSpi.begin(EPD_SCK, -1, EPD_MOSI, -1);
  display.epd2.selectSPI(epaperSpi, SPISettings(2000000, MSBFIRST, SPI_MODE0));
  display.init(0);
  display.setRotation(0);
  display.setFullWindow();
}

bool fetchDashboard(dashboard::DashboardData& data, char* error) {
  NetworkClientSecure secureClient;
  secureClient.setCACert(ISRG_ROOT_X1);
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(12000);
  if (!http.begin(secureClient, secrets::DASHBOARD_URL)) {
    snprintf(error, ERROR_SIZE, "Could not open dashboard URL");
    return false;
  }
  http.addHeader("Authorization", String("Bearer ") + secrets::DASHBOARD_TOKEN);
  http.addHeader("Accept", "application/json");
  const int status = http.GET();
  if (status != HTTP_CODE_OK) {
    snprintf(error, ERROR_SIZE, "Dashboard HTTP %d", status);
    http.end();
    return false;
  }
  String payload = http.getString();
  http.end();
  return dashboard::parseDashboardPayload(payload.c_str(), payload.length(), data,
                                          error, ERROR_SIZE);
}

bool fetchDashboardFromSerial(dashboard::DashboardData& data, char* error) {
  constexpr char RESPONSE_PREFIX[] = "SERIAL_DASHBOARD_RESPONSE ";
  while (Serial1.available()) Serial1.read();
  Serial1.setTimeout(2000);
  for (int attempt = 0; attempt < 10; ++attempt) {
    logLine("SERIAL_DASHBOARD_REQUEST");
    const String response = Serial1.readStringUntil('\n');
    if (!response.startsWith(RESPONSE_PREFIX)) continue;
    const String payload = response.substring(strlen(RESPONSE_PREFIX));
    if (payload.length() > 65536) {
      snprintf(error, ERROR_SIZE, "USB dashboard payload too large");
      return false;
    }
    return dashboard::parseDashboardPayload(payload.c_str(), payload.length(), data,
                                            error, ERROR_SIZE);
  }
  snprintf(error, ERROR_SIZE, "USB bridge did not respond");
  return false;
}

bool connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  if (secrets::WIFI_SSID[0]) {
    const int networkCount = WiFi.scanNetworks(false, true);
    bool targetFound = false;
    for (int index = 0; index < networkCount; ++index) {
      if (WiFi.SSID(index) == secrets::WIFI_SSID) {
        targetFound = true;
        logLine(String("WIFI_TARGET_FOUND: rssi=") + WiFi.RSSI(index) +
                " channel=" + WiFi.channel(index) +
                " auth=" + static_cast<int>(WiFi.encryptionType(index)));
        break;
      }
    }
    if (!targetFound) logLine("WIFI_TARGET_NOT_FOUND");
    WiFi.scanDelete();
    WiFi.begin(secrets::WIFI_SSID, secrets::WIFI_PASSWORD);
  } else {
    logLine("WIFI_USING_SAVED_CREDENTIALS");
    WiFi.begin();
  }
  logLine("WIFI_CONNECT_START");
  for (int attempt = 0; attempt < 40 && WiFi.status() != WL_CONNECTED; ++attempt) {
    delay(500);
  }
  if (WiFi.status() == WL_CONNECTED) return true;

  logLine("WIFI_STORED_CREDENTIALS_UNAVAILABLE");
  return false;
}

bool provisionWifi() {
  dashboard::drawSetupScreen(display, secrets::SETUP_PASSWORD);
  WiFiManager manager;
  manager.setConfigPortalTimeout(300);
  manager.setConnectTimeout(20);
  return manager.autoConnect("Zach-Display-Setup", secrets::SETUP_PASSWORD);
}

[[noreturn]] void sleepFor(uint64_t microseconds) {
  logLine(String("SLEEP_SECONDS: ") + static_cast<unsigned long>(microseconds / 1000000ULL));
  esp_sleep_enable_timer_wakeup(microseconds);
  delay(100);
  esp_deep_sleep_start();
  while (true) delay(1000);
}
}  // namespace

void setup() {
  Serial1.begin(115200, SERIAL_8N1, 44, 43);
  delay(200);
  logLine("HOME_DISPLAY_BOOT");
  initDisplay();

  dashboard::DashboardData data;
  char error[ERROR_SIZE] = "";
  bool fetched = false;
  if (connectWifi()) {
    logLine(String("WIFI_CONNECTED: ") + WiFi.localIP().toString());
    fetched = fetchDashboard(data, error);
  } else {
    logLine("WIFI_CONNECT_FAILED");
  }

  if (!fetched) {
    char networkError[ERROR_SIZE];
    strlcpy(networkError, error, ERROR_SIZE);
    if (fetchDashboardFromSerial(data, error)) {
      fetched = true;
      logLine(String("SERIAL_DASHBOARD_FETCH_OK: timeline=") + data.timelineCount);
    } else {
      logLine(String("SERIAL_DASHBOARD_FETCH_FAILED: ") + error);
      if (networkError[0]) strlcpy(error, networkError, ERROR_SIZE);
    }
  }

  if (!fetched && provisionWifi()) {
    logLine(String("WIFI_CONNECTED: ") + WiFi.localIP().toString());
    fetched = fetchDashboard(data, error);
  }

  if (!fetched) {
    logLine(String("DASHBOARD_FETCH_FAILED: ") + error);
    dashboard::drawErrorScreen(display, error);
    sleepFor(RETRY_INTERVAL_US);
  }

  logLine(String("DASHBOARD_FETCH_OK: timeline=") + data.timelineCount);
  dashboard::drawDashboard(display, data);
  logLine("DASHBOARD_RENDER_COMPLETE");
  sleepFor(REFRESH_INTERVAL_US);
}

void loop() {
  delay(1000);
}
