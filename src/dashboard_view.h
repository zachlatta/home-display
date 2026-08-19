// Shared dashboard payload parsing and e-paper layout.
//
// This header is compiled unchanged by two targets:
//   * the firmware (src/main.cpp) against GxEPD2_7C and the real GDEP073E01
//   * the host simulator (sim/) against a PanelCanvas that reproduces
//     GxEPD2_7C's paging and colour quantisation, then writes a PNG
//
// Keeping one copy is the point: a simulator that re-implements the layout
// would not catch clipping, truncation, or page-boundary bugs in the code the
// device actually runs. Everything here must therefore stay free of networking,
// deep sleep, and other device-only concerns.

#ifndef DASHBOARD_VIEW_H
#define DASHBOARD_VIEW_H

#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <mbedtls/base64.h>

#include <stdlib.h>
#include <string.h>

#if defined(ESP32)
#include <esp_heap_caps.h>
#endif

// The GxEPD_* constants come from GxEPD2 on the device and from sim/panel.h on
// the host; either way they must be defined before this header is included.
#ifndef GxEPD_BLACK
#error "include GxEPD2_7C.h (firmware) or sim/panel.h (simulator) before dashboard_view.h"
#endif

namespace dashboard {

constexpr int16_t PANEL_WIDTH = 800;
constexpr int16_t PANEL_HEIGHT = 480;
constexpr uint32_t MAX_DISPLAY_BUFFER_SIZE = 16000;
constexpr size_t MAX_TIMELINE_ENTRIES = 6;
constexpr int16_t PHOTO_X = 508;
constexpr int16_t PHOTO_Y = 96;
constexpr int16_t PHOTO_WIDTH = 270;
constexpr int16_t PHOTO_HEIGHT = 250;
constexpr size_t PHOTO_BYTES = PHOTO_WIDTH * PHOTO_HEIGHT / 2;
// 5 split the timeline's merged "ACTOR: summary" string into separate actor and
// summary fields. The device refuses anything else, so gateway and firmware
// must be deployed together.
constexpr int PROTOCOL_VERSION = 5;

struct CalendarEvent {
  char day[12] = "";
  char time[12] = "";
  char title[52] = "";
  char location[34] = "";
  char relative[16] = "UP NEXT";
  bool urgent = false;
};

// Actor and summary stay separate so the layout can weight them differently:
// who it was in bold, what happened in regular. Merging them into one string
// also meant truncation ate the actor's name before the message.
struct TimelineEntry {
  char time[10] = "";
  char source[14] = "";
  char actor[26] = "";
  char summary[72] = "";
};

struct DashboardData {
  char dateLabel[36] = "PERSONAL DASHBOARD";
  char updatedLabel[28] = "";
  CalendarEvent nextEvent;
  bool hasNextEvent = false;
  TimelineEntry timeline[MAX_TIMELINE_ENTRIES];
  size_t timelineCount = 0;
  char photoLabel[28] = "";
  uint8_t* photoPixels = nullptr;
  bool photoAvailable = false;
  bool stale = false;
};

// The device prefers PSRAM for the 33 KB photo buffer; the host just uses the
// heap. Both fall back to malloc so the failure path is identical.
inline uint8_t* allocatePhotoBuffer() {
#if defined(ESP32)
  void* buffer = heap_caps_malloc(PHOTO_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buffer) buffer = malloc(PHOTO_BYTES);
#else
  void* buffer = malloc(PHOTO_BYTES);
#endif
  return static_cast<uint8_t*>(buffer);
}

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------

template <class Display>
void setFont(Display& display, const GFXfont* font, uint16_t color) {
  display.setFont(font);
  display.setTextSize(1);
  display.setTextColor(color);
}

template <class Display>
int16_t textWidth(Display& display, const char* text) {
  int16_t x1, y1;
  uint16_t width, height;
  display.getTextBounds(text, 0, 0, &x1, &y1, &width, &height);
  return width;
}

template <class Display>
void drawRight(Display& display, const char* text, int16_t right, int16_t baseline) {
  display.setCursor(right - textWidth(display, text), baseline);
  display.print(text);
}

template <class Display>
void drawCentered(Display& display, const char* text, int16_t centerX, int16_t baseline) {
  display.setCursor(centerX - textWidth(display, text) / 2, baseline);
  display.print(text);
}

// Shortens `text` to fit `maxWidth` at the display's current font, ending with
// ".." when anything was removed. Returns true if the text was shortened.
// Split out from drawClipped so the simulator can report exactly which fields
// the device will truncate, using the same glyph metrics rather than a guess.
template <class Display>
bool clipText(Display& display, const char* text, int16_t maxWidth, char* out,
              size_t outSize) {
  strlcpy(out, text, outSize);
  if (textWidth(display, out) <= maxWidth) return false;
  size_t length = strlen(out);
  while (length > 2 && textWidth(display, out) > maxWidth) {
    out[--length] = '\0';
  }
  if (length > 2) {
    out[length - 2] = '.';
    out[length - 1] = '.';
  }
  return true;
}

template <class Display>
void drawClipped(Display& display, const char* text, int16_t x, int16_t baseline,
                 int16_t maxWidth) {
  char clipped[64];
  clipText(display, text, maxWidth, clipped, sizeof(clipped));
  display.setCursor(x, baseline);
  display.print(clipped);
}

// ---------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------

template <class Display>
void drawSetupScreen(Display& display, const char* setupPassword) {
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.fillRect(0, 0, 800, 58, GxEPD_BLACK);
    setFont(display, &FreeSansBold18pt7b, GxEPD_WHITE);
    display.setCursor(22, 39);
    display.print("WI-FI SETUP");
    display.fillRect(0, 58, 267, 4, GxEPD_RED);
    display.fillRect(267, 58, 266, 4, GxEPD_GREEN);
    display.fillRect(533, 58, 267, 4, GxEPD_BLUE);
    setFont(display, &FreeSansBold24pt7b, GxEPD_BLACK);
    display.setCursor(45, 165);
    display.print("Zach-Display-Setup");
    setFont(display, &FreeSansBold12pt7b, GxEPD_BLACK);
    display.setCursor(47, 220);
    display.print("Password: ");
    display.print(setupPassword);
    setFont(display, &FreeSansBold12pt7b, GxEPD_BLACK);
    display.setCursor(47, 285);
    display.print("Connect, then open 192.168.4.1");
    setFont(display, &FreeSans9pt7b, GxEPD_BLACK);
    display.setCursor(47, 340);
    display.print("This screen will update automatically when connected.");
  } while (display.nextPage());
  display.hibernate();
}

template <class Display>
void drawErrorScreen(Display& display, const char* detail) {
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.fillRect(0, 0, 800, 58, GxEPD_BLACK);
    display.fillRect(0, 58, 800, 4, GxEPD_RED);
    setFont(display, &FreeSansBold18pt7b, GxEPD_WHITE);
    display.setCursor(22, 39);
    display.print("DASHBOARD OFFLINE");
    setFont(display, &FreeSansBold18pt7b, GxEPD_BLACK);
    display.setCursor(45, 185);
    display.print("Keeping the last good view.");
    setFont(display, &FreeSansBold12pt7b, GxEPD_BLACK);
    display.setCursor(47, 250);
    display.print(detail);
    setFont(display, &FreeSans9pt7b, GxEPD_BLACK);
    display.setCursor(47, 315);
    display.print("Retrying automatically in five minutes.");
  } while (display.nextPage());
  display.hibernate();
}

template <class Display>
void drawDashboard(Display& display, const DashboardData& data) {
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    // Compact masthead: high contrast without spending journal space on branding.
    display.fillRect(0, 0, 800, 58, GxEPD_BLACK);
    setFont(display, &FreeSansBold18pt7b, GxEPD_WHITE);
    display.setCursor(18, 39);
    display.print("ZACH / TODAY");
    setFont(display, &FreeSansBold9pt7b, GxEPD_WHITE);
    drawRight(display, data.dateLabel, 782, 26);
    setFont(display, &FreeSans9pt7b, GxEPD_WHITE);
    drawRight(display, data.stale ? "CACHED" : data.updatedLabel, 782, 49);
    display.fillRect(0, 58, 267, 4, GxEPD_RED);
    display.fillRect(267, 58, 266, 4, GxEPD_GREEN);
    display.fillRect(533, 58, 267, 4, GxEPD_BLUE);

    // The primary job: a truthful, glanceable journal of today.
    setFont(display, &FreeSansBold9pt7b, GxEPD_BLACK);
    display.setCursor(22, 82);
    display.print("THE DAY, SO FAR");
    display.drawFastHLine(22, 90, 463, GxEPD_BLACK);
    if (data.timelineCount == 0) {
      setFont(display, &FreeSansBold18pt7b, GxEPD_BLACK);
      display.setCursor(22, 145);
      display.print("The day is just beginning.");
    }
    for (size_t i = 0; i < data.timelineCount; ++i) {
      const int16_t top = 94 + static_cast<int16_t>(i) * 49;
      const TimelineEntry& entry = data.timeline[i];
      // Personal messaging shares green: the panel has only four chromatic
      // inks, and the source label beside the node carries the distinction.
      uint16_t sourceColor = GxEPD_YELLOW;
      if (strcmp(entry.source, "MESSAGES") == 0) sourceColor = GxEPD_GREEN;
      else if (strcmp(entry.source, "SLACK") == 0) sourceColor = GxEPD_BLUE;
      else if (strcmp(entry.source, "MAIL") == 0) sourceColor = GxEPD_RED;
      else if (strcmp(entry.source, "WHATSAPP") == 0) sourceColor = GxEPD_GREEN;

      setFont(display, &FreeSansBold9pt7b, GxEPD_BLACK);
      display.setCursor(22, top + 15);
      display.print(entry.time);
      display.fillCircle(99, top + 10, 5, sourceColor);
      display.drawCircle(99, top + 10, 5, GxEPD_BLACK);
      setFont(display, &FreeSansBold9pt7b, GxEPD_BLACK);
      display.setCursor(111, top + 15);
      display.print(entry.source);

      // Who, in bold, then what happened, in regular. The actor is capped at
      // two fifths of the row so a long name cannot crowd out the message.
      const int16_t rowLeft = 22;
      const int16_t rowWidth = 463;
      char actorText[sizeof(entry.actor)];
      setFont(display, &FreeSansBold12pt7b, GxEPD_BLACK);
      clipText(display, entry.actor, rowWidth * 2 / 5, actorText, sizeof(actorText));
      display.setCursor(rowLeft, top + 38);
      display.print(actorText);
      const int16_t summaryLeft =
          rowLeft + textWidth(display, actorText) + (actorText[0] ? 10 : 0);
      setFont(display, &FreeSans12pt7b, GxEPD_BLACK);
      drawClipped(display, entry.summary, summaryLeft, top + 38,
                  rowLeft + rowWidth - summaryLeft);
      display.drawFastHLine(22, top + 47, 463, GxEPD_BLACK);
    }

    // A recent photo, reduced to the panel's six native inks. The gateway
    // dithers into these same indices in this order. An index outside the range
    // would be an unsupported panel state, so it falls back to white rather
    // than letting the driver emit a reserved wire code.
    setFont(display, &FreeSansBold9pt7b, GxEPD_BLACK);
    display.setCursor(507, 82);
    display.print("A RECENT MOMENT");
    display.drawFastHLine(507, 90, 272, GxEPD_BLACK);
    display.drawRect(507, 95, 272, 252, GxEPD_BLACK);
    if (data.photoAvailable && data.photoPixels) {
      const uint16_t palette[] = {
        GxEPD_BLACK, GxEPD_WHITE, GxEPD_GREEN, GxEPD_BLUE,
        GxEPD_RED, GxEPD_YELLOW,
      };
      constexpr uint8_t paletteSize = sizeof(palette) / sizeof(palette[0]);
      for (int16_t y = 0; y < PHOTO_HEIGHT; ++y) {
        for (int16_t x = 0; x < PHOTO_WIDTH; ++x) {
          const uint8_t packed = data.photoPixels[y * (PHOTO_WIDTH / 2) + x / 2];
          const uint8_t color = x & 1 ? packed & 0x0f : packed >> 4;
          display.drawPixel(PHOTO_X + x, PHOTO_Y + y,
                            color < paletteSize ? palette[color] : GxEPD_WHITE);
        }
      }
    } else {
      setFont(display, &FreeSansBold12pt7b, GxEPD_BLACK);
      drawCentered(display, "PHOTO UNAVAILABLE", 643, 225);
    }
    setFont(display, &FreeSans9pt7b, GxEPD_BLACK);
    drawRight(display, data.photoLabel, 777, 370);

    // Supporting job: make the next transition obvious without becoming a schedule wall.
    display.fillRect(0, 397, 800, 83, GxEPD_BLACK);
    display.fillRect(0, 397, 7, 83,
                     data.hasNextEvent && data.nextEvent.urgent ? GxEPD_RED : GxEPD_BLUE);
    setFont(display, &FreeSansBold9pt7b, GxEPD_WHITE);
    display.setCursor(22, 420);
    display.print("NEXT");
    if (data.hasNextEvent) {
      const CalendarEvent& event = data.nextEvent;
      constexpr int16_t footerLeft = 22;
      constexpr int16_t footerRight = 778;
      constexpr int16_t columnGap = 30;

      setFont(display, &FreeSansBold18pt7b, GxEPD_WHITE);
      display.setCursor(footerLeft, 460);
      display.print(event.relative);

      // The relative label varies from "NOW" to "TOMORROW", a 130px spread at
      // 18pt. A fixed second column collided with the longest labels, so the
      // detail column starts after whatever this one actually measures.
      const int16_t relativeWidth = textWidth(display, event.relative);
      int16_t columnX = footerLeft + relativeWidth + columnGap;
      if (columnX < 220) columnX = 220;

      drawClipped(display, event.title, columnX, 438, footerRight - columnX);
      setFont(display, &FreeSans9pt7b, GxEPD_WHITE);
      display.setCursor(columnX, 466);
      display.print(event.time);
      if (event.location[0]) {
        const int16_t locationX = columnX + textWidth(display, event.time) + 18;
        drawClipped(display, event.location, locationX, 466, footerRight - locationX);
      }
    } else {
      setFont(display, &FreeSansBold18pt7b, GxEPD_WHITE);
      display.setCursor(22, 460);
      display.print("RUNWAY CLEAR");
    }
  } while (display.nextPage());
  display.hibernate();
}

// ---------------------------------------------------------------------------
// Payload parsing
// ---------------------------------------------------------------------------

template <size_t N>
void copyJson(char (&destination)[N], JsonVariantConst value) {
  const char* text = value | "";
  strlcpy(destination, text, N);
}

// `error` is filled with a short, display-safe reason on failure.
inline bool parseDashboardPayload(const char* payload, size_t payloadLength,
                                  DashboardData& data, char* error, size_t errorSize) {
  JsonDocument document;
  const DeserializationError parseError = deserializeJson(document, payload, payloadLength);
  if (parseError) {
    snprintf(error, errorSize, "Invalid dashboard JSON: %s", parseError.c_str());
    return false;
  }
  if ((document["protocolVersion"] | 0) != PROTOCOL_VERSION) {
    snprintf(error, errorSize, "Unsupported dashboard protocol");
    return false;
  }

  copyJson(data.dateLabel, document["display"]["dateLabel"]);
  copyJson(data.updatedLabel, document["display"]["updatedLabel"]);
  data.stale = document["stale"] | false;
  data.hasNextEvent = false;
  for (JsonObjectConst item : document["calendar"].as<JsonArrayConst>()) {
    CalendarEvent& event = data.nextEvent;
    copyJson(event.day, item["dayLabel"]);
    copyJson(event.time, item["timeLabel"]);
    copyJson(event.title, item["title"]);
    copyJson(event.location, item["location"]);
    copyJson(event.relative, item["relativeLabel"]);
    event.urgent = item["urgent"] | false;
    data.hasNextEvent = true;
    break;
  }
  data.timelineCount = 0;
  for (JsonObjectConst item : document["timeline"].as<JsonArrayConst>()) {
    if (data.timelineCount >= MAX_TIMELINE_ENTRIES) break;
    TimelineEntry& entry = data.timeline[data.timelineCount++];
    copyJson(entry.time, item["timeLabel"]);
    copyJson(entry.source, item["sourceLabel"]);
    copyJson(entry.actor, item["actor"]);
    copyJson(entry.summary, item["summary"]);
  }
  copyJson(data.photoLabel, document["photo"]["capturedLabel"]);
  const int photoWidth = document["photo"]["width"] | 0;
  const int photoHeight = document["photo"]["height"] | 0;
  const char* encoding = document["photo"]["encoding"] | "";
  const char* encodedPixels = document["photo"]["pixels"] | "";
  if (photoWidth == PHOTO_WIDTH && photoHeight == PHOTO_HEIGHT
      && strcmp(encoding, "gxepd7c-4bpp") == 0 && encodedPixels[0]) {
    data.photoPixels = allocatePhotoBuffer();
    if (!data.photoPixels) {
      snprintf(error, errorSize, "Not enough memory for photo");
      return false;
    }
    size_t decodedLength = 0;
    const int decodeResult = mbedtls_base64_decode(
        data.photoPixels, PHOTO_BYTES, &decodedLength,
        reinterpret_cast<const unsigned char*>(encodedPixels), strlen(encodedPixels));
    if (decodeResult != 0 || decodedLength != PHOTO_BYTES) {
      free(data.photoPixels);
      data.photoPixels = nullptr;
      snprintf(error, errorSize, "Invalid dashboard photo");
      return false;
    }
    data.photoAvailable = true;
  }
  return true;
}

}  // namespace dashboard

#endif  // DASHBOARD_VIEW_H
