// Host renderer for the reTerminal E1002 dashboard.
//
//   sim/build/simulate sim/fixtures/typical-day.json --out sim/out/typical
//
// Parses a gateway payload with the firmware's own parser, draws it with the
// firmware's own layout code into a GxEPD2-equivalent paged framebuffer, and
// writes both a flat PNG and a simulated-panel PNG. Also reports what the
// device would put on the glass: colour usage, unsupported states, truncated
// strings, and legibility of each palette colour against the background.

#include "panel.h"

#include "../src/dashboard_view.h"

#include "appearance.h"
#include "png.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Options {
  std::string payloadPath;
  std::string outPrefix = "sim/out/dashboard";
  std::string screen = "dashboard";
  std::string errorDetail = "Dashboard HTTP 503";
  std::string setupPassword = "simulator-password";
  sim::PanelOptics optics;
  bool idealOnly = false;
  bool json = false;
};

void usage() {
  std::cerr <<
      "usage: simulate [payload.json] [options]\n"
      "\n"
      "  --out PREFIX          output path prefix (default sim/out/dashboard)\n"
      "  --screen NAME         dashboard | setup | error (default dashboard)\n"
      "  --error TEXT          detail line for --screen error\n"
      "  --palette NAME        measured | adapted | blend (default blend)\n"
      "  --ambient F           room brightness, 1.0 = well lit\n"
      "  --blur F              pigment spread sigma in pixels (default 0.62)\n"
      "  --grain F             surface noise, 0-255 units (default 3.2)\n"
      "  --mottle F            slow reflectance variation (default 2.4)\n"
      "  --scale N             integer upscale of the output PNGs\n"
      "  --flag-unsupported    paint unsupported colours magenta\n"
      "  --ideal-only          skip the simulated-panel render\n"
      "  --json                emit a machine-readable report instead of text\n";
}

bool readFile(const std::string& path, std::string& contents) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  std::ostringstream buffer;
  buffer << file.rdbuf();
  contents = buffer.str();
  return true;
}

bool parseOptions(int argc, char** argv, Options& options) {
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    auto next = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "simulate: " << name << " needs a value\n";
        exit(2);
      }
      return argv[++i];
    };
    if (argument == "--help" || argument == "-h") {
      usage();
      exit(0);
    } else if (argument == "--out") {
      options.outPrefix = next("--out");
    } else if (argument == "--screen") {
      options.screen = next("--screen");
    } else if (argument == "--error") {
      options.errorDetail = next("--error");
    } else if (argument == "--password") {
      options.setupPassword = next("--password");
    } else if (argument == "--palette") {
      const std::string mode = next("--palette");
      if (mode == "measured") options.optics.palette = sim::PaletteMode::kMeasured;
      else if (mode == "adapted") options.optics.palette = sim::PaletteMode::kAdapted;
      else if (mode == "blend") options.optics.palette = sim::PaletteMode::kBlend;
      else {
        std::cerr << "simulate: unknown palette " << mode << "\n";
        return false;
      }
    } else if (argument == "--ambient") {
      options.optics.ambient = std::stod(next("--ambient"));
    } else if (argument == "--blur") {
      options.optics.blurSigma = std::stod(next("--blur"));
    } else if (argument == "--grain") {
      options.optics.grain = std::stod(next("--grain"));
    } else if (argument == "--mottle") {
      options.optics.mottle = std::stod(next("--mottle"));
    } else if (argument == "--scale") {
      options.optics.scale = std::stoi(next("--scale"));
    } else if (argument == "--flag-unsupported") {
      options.optics.flagUnsupported = true;
    } else if (argument == "--ideal-only") {
      options.idealOnly = true;
    } else if (argument == "--json") {
      options.json = true;
    } else if (argument.rfind("--", 0) == 0) {
      std::cerr << "simulate: unknown option " << argument << "\n";
      return false;
    } else if (options.payloadPath.empty()) {
      options.payloadPath = argument;
    } else {
      std::cerr << "simulate: unexpected argument " << argument << "\n";
      return false;
    }
  }
  return true;
}

double relativeLuminance(uint8_t red, uint8_t green, uint8_t blue) {
  auto channel = [](uint8_t value) {
    const double normalized = value / 255.0;
    return normalized <= 0.04045 ? normalized / 12.92
                                 : std::pow((normalized + 0.055) / 1.055, 2.4);
  };
  return 0.2126 * channel(red) + 0.7152 * channel(green) + 0.0722 * channel(blue);
}

double contrastRatio(uint8_t r1, uint8_t g1, uint8_t b1, uint8_t r2, uint8_t g2,
                     uint8_t b2) {
  const double a = relativeLuminance(r1, g1, b1);
  const double b = relativeLuminance(r2, g2, b2);
  const double lighter = std::max(a, b);
  const double darker = std::min(a, b);
  return (lighter + 0.05) / (darker + 0.05);
}

// Draws the requested screen into a canvas with the given page height.
void drawScreen(sim::PanelCanvas& canvas, const Options& options,
                const dashboard::DashboardData& data) {
  canvas.init(0);
  canvas.setRotation(0);
  canvas.setFullWindow();
  if (options.screen == "setup") {
    dashboard::drawSetupScreen(canvas, options.setupPassword.c_str());
  } else if (options.screen == "error") {
    dashboard::drawErrorScreen(canvas, options.errorDetail.c_str());
  } else {
    dashboard::drawDashboard(canvas, data);
  }
}

struct Clipped {
  std::string field;
  std::string original;
  std::string rendered;
};

// Runs the firmware's own clipText against the same fonts and widths the layout
// uses, so this reports what the device will really show rather than guessing
// from string lengths. The column arithmetic mirrors drawDashboard exactly; if
// the two drift apart, the reported widths stop matching the render.
std::vector<Clipped> findClipped(const dashboard::DashboardData& data,
                                 std::vector<std::string>& layoutProblems) {
  sim::PanelCanvas scratch(sim::PanelCanvas::kHeight);
  std::vector<Clipped> clipped;
  char rendered[64];

  auto check = [&](const std::string& label, const char* value, const GFXfont* font,
                   int16_t maxWidth) {
    if (!value[0]) return;
    if (maxWidth <= 0) {
      layoutProblems.push_back(label + " has no room left (" +
                               std::to_string(maxWidth) + "px)");
      return;
    }
    dashboard::setFont(scratch, font, GxEPD_BLACK);
    if (dashboard::clipText(scratch, value, maxWidth, rendered, sizeof(rendered))) {
      clipped.push_back({label, value, rendered});
    }
  };

  for (size_t i = 0; i < data.timelineCount; ++i) {
    const dashboard::TimelineEntry& entry = data.timeline[i];
    const std::string label = "timeline[" + std::to_string(i) + "]";
    dashboard::setFont(scratch, &FreeSansBold12pt7b, GxEPD_BLACK);
    char actorText[sizeof(entry.actor)];
    if (dashboard::clipText(scratch, entry.actor, 463 * 2 / 5, actorText,
                            sizeof(actorText))) {
      clipped.push_back({label + ".actor", entry.actor, actorText});
    }
    const int16_t summaryLeft =
        22 + dashboard::textWidth(scratch, actorText) + (actorText[0] ? 10 : 0);
    check(label + ".summary", entry.summary, &FreeSans12pt7b, 22 + 463 - summaryLeft);
  }

  if (data.hasNextEvent) {
    const dashboard::CalendarEvent& event = data.nextEvent;
    dashboard::setFont(scratch, &FreeSansBold18pt7b, GxEPD_BLACK);
    const int16_t relativeWidth = dashboard::textWidth(scratch, event.relative);
    int16_t columnX = 22 + relativeWidth + 30;
    if (columnX < 220) columnX = 220;
    // The relative label prints unclipped at a fixed origin, so it is the one
    // string that can silently run into the column beside it.
    if (778 - columnX < 200) {
      layoutProblems.push_back(std::string("footer detail column is only ") +
                               std::to_string(778 - columnX) + "px wide after the \"" +
                               event.relative + "\" label");
    }
    check("calendar.title", event.title, &FreeSansBold18pt7b, 778 - columnX);
    dashboard::setFont(scratch, &FreeSans9pt7b, GxEPD_BLACK);
    const int16_t locationX = columnX + dashboard::textWidth(scratch, event.time) + 18;
    check("calendar.location", event.location, &FreeSans9pt7b, 778 - locationX);
  }
  return clipped;
}

std::string jsonEscape(const std::string& value) {
  std::string out;
  for (const char character : value) {
    switch (character) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      default: out += character;
    }
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parseOptions(argc, argv, options)) return 2;

  dashboard::DashboardData data;
  if (options.screen == "dashboard") {
    if (options.payloadPath.empty()) {
      std::cerr << "simulate: a payload path is required for --screen dashboard\n";
      usage();
      return 2;
    }
    std::string payload;
    if (!readFile(options.payloadPath, payload)) {
      std::cerr << "simulate: cannot read " << options.payloadPath << "\n";
      return 1;
    }
    char error[96] = "";
    if (!dashboard::parseDashboardPayload(payload.c_str(), payload.size(), data, error,
                                          sizeof(error))) {
      // This is exactly what the device would refuse, so report it the same way.
      std::cerr << "simulate: payload rejected by firmware parser: " << error << "\n";
      std::cerr << "          the device would show the offline screen instead\n";
      return 1;
    }
  }

  // Render the way the device does: 12 bands of 40 rows.
  sim::PanelCanvas paged(40);
  drawScreen(paged, options, data);

  // Render again as one 480-row page. GxEPD2 guarantees these are identical;
  // if they are not, the layout depends on page boundaries and the device will
  // show something this simulator would otherwise hide.
  sim::PanelCanvas single(sim::PanelCanvas::kHeight);
  drawScreen(single, options, data);

  size_t mismatches = 0;
  for (size_t i = 0; i < paged.frame().size(); ++i) {
    if (paged.frame()[i] != single.frame()[i]) ++mismatches;
  }

  const std::vector<uint8_t>& frame = paged.frame();
  const int width = sim::PanelCanvas::kWidth;
  const int height = sim::PanelCanvas::kHeight;

  std::map<uint8_t, size_t> histogram;
  for (uint8_t index : frame) ++histogram[index];

  std::string writeError;
  const std::string idealPath = options.outPrefix + "-ideal.png";
  if (!sim::writePng(idealPath, width * options.optics.scale,
                     height * options.optics.scale,
                     sim::renderIdeal(frame, width, height, options.optics.scale),
                     writeError)) {
    std::cerr << "simulate: " << writeError << "\n";
    return 1;
  }

  std::string panelPath;
  if (!options.idealOnly) {
    panelPath = options.outPrefix + "-panel.png";
    if (!sim::writePng(panelPath, width * options.optics.scale,
                       height * options.optics.scale,
                       sim::renderPanel(frame, width, height, options.optics),
                       writeError)) {
      std::cerr << "simulate: " << writeError << "\n";
      return 1;
    }
  }

  size_t unsupportedPixels = 0;
  for (const auto& [index, count] : histogram) {
    if (!sim::isSupported(index)) unsupportedPixels += count;
  }
  std::vector<std::string> layoutProblems;
  const std::vector<Clipped> clipped = options.screen == "dashboard"
      ? findClipped(data, layoutProblems)
      : std::vector<Clipped>{};

  uint8_t whiteRgb[3];
  sim::panelColor(sim::kWhite, options.optics.palette, whiteRgb[0], whiteRgb[1],
                  whiteRgb[2]);

  // ------------------------------------------------------------------- json
  if (options.json) {
    printf("{\n");
    printf("  \"screen\": \"%s\",\n", jsonEscape(options.screen).c_str());
    printf("  \"payload\": \"%s\",\n", jsonEscape(options.payloadPath).c_str());
    printf("  \"pages\": %d,\n", paged.pages());
    printf("  \"pageHeight\": %d,\n", paged.pageHeight());
    printf("  \"pagedMatchesSinglePage\": %s,\n", mismatches == 0 ? "true" : "false");
    printf("  \"pageMismatchPixels\": %zu,\n", mismatches);
    printf("  \"hibernated\": %s,\n", paged.hibernated() ? "true" : "false");
    printf("  \"unsupportedPixels\": %zu,\n", unsupportedPixels);
    printf("  \"idealPng\": \"%s\",\n", jsonEscape(idealPath).c_str());
    printf("  \"panelPng\": \"%s\",\n", jsonEscape(panelPath).c_str());
    printf("  \"colours\": [\n");
    bool first = true;
    for (const auto& [index, count] : histogram) {
      uint8_t rgb[3];
      sim::panelColor(index, options.optics.palette, rgb[0], rgb[1], rgb[2]);
      if (!first) printf(",\n");
      first = false;
      printf("    {\"name\": \"%s\", \"pixels\": %zu, \"share\": %.4f, "
             "\"hex\": \"#%02X%02X%02X\", \"nativeCode\": %u, \"supported\": %s, "
             "\"contrastVsWhite\": %.2f}",
             sim::kPaletteNames[index], count,
             static_cast<double>(count) / frame.size(), rgb[0], rgb[1], rgb[2],
             sim::nativeCode(index), sim::isSupported(index) ? "true" : "false",
             contrastRatio(rgb[0], rgb[1], rgb[2], whiteRgb[0], whiteRgb[1],
                           whiteRgb[2]));
    }
    printf("\n  ],\n");
    printf("  \"layoutProblems\": [");
    for (size_t i = 0; i < layoutProblems.size(); ++i) {
      printf("%s\n    \"%s\"", i ? "," : "", jsonEscape(layoutProblems[i]).c_str());
    }
    printf("%s],\n", layoutProblems.empty() ? "" : "\n  ");
    printf("  \"clipped\": [");
    for (size_t i = 0; i < clipped.size(); ++i) {
      printf("%s\n    {\"field\": \"%s\", \"rendered\": \"%s\"}", i ? "," : "",
             jsonEscape(clipped[i].field).c_str(), jsonEscape(clipped[i].rendered).c_str());
    }
    printf("%s],\n", clipped.empty() ? "" : "\n  ");
    printf("  \"content\": {\"timelineEntries\": %zu, \"hasNextEvent\": %s, "
           "\"photoDecoded\": %s, \"stale\": %s}\n",
           data.timelineCount, data.hasNextEvent ? "true" : "false",
           data.photoAvailable ? "true" : "false", data.stale ? "true" : "false");
    printf("}\n");
    if (data.photoPixels) free(data.photoPixels);
    return mismatches == 0 ? 0 : 1;
  }

  // ------------------------------------------------------------------ report
  std::cout << "screen        " << options.screen << "\n";
  if (!options.payloadPath.empty()) std::cout << "payload       " << options.payloadPath << "\n";
  std::cout << "panel         GDEP073E01, 800x480, E Ink Spectra 6\n";
  std::cout << "paging        " << paged.pages() << " passes of " << paged.pageHeight()
            << " rows (firmware draws the full layout once per pass)\n";
  std::cout << "page check    "
            << (mismatches == 0 ? "paged and single-page renders agree"
                                : "MISMATCH")
            << "\n";
  if (mismatches != 0) {
    std::cout << "  ! " << mismatches
              << " pixels differ between paged and single-page rendering\n";
  }
  std::cout << "hibernate     " << (paged.hibernated() ? "yes" : "NO — panel left powered")
            << "\n";
  std::cout << "output        " << idealPath << "\n";
  if (!panelPath.empty()) std::cout << "              " << panelPath << "\n";

  std::cout << "\ncolour usage\n";
  for (const auto& [index, count] : histogram) {
    uint8_t rgb[3];
    sim::panelColor(index, options.optics.palette, rgb[0], rgb[1], rgb[2]);
    char hex[8];
    snprintf(hex, sizeof(hex), "#%02X%02X%02X", rgb[0], rgb[1], rgb[2]);
    const double share = 100.0 * static_cast<double>(count) / frame.size();
    printf("  %-7s %8zu px  %5.1f%%  %s  native 0x%X", sim::kPaletteNames[index], count,
           share, hex, sim::nativeCode(index));
    if (index != sim::kWhite) {
      printf("  contrast vs white %4.1f:1",
             contrastRatio(rgb[0], rgb[1], rgb[2], whiteRgb[0], whiteRgb[1],
                           whiteRgb[2]));
    }
    printf("\n");
    const std::string caveat = sim::paletteCaveat(index);
    if (!caveat.empty()) std::cout << "          ! " << caveat << "\n";
  }

  if (unsupportedPixels > 0) {
    std::cout << "\n  ! " << unsupportedPixels
              << " pixels use a colour this panel cannot render\n";
  }

  if (options.screen == "dashboard") {
    std::cout << "\ncontent\n";
    std::cout << "  timeline entries  " << data.timelineCount << " of "
              << dashboard::MAX_TIMELINE_ENTRIES << "\n";
    std::cout << "  next event        "
              << (data.hasNextEvent ? data.nextEvent.title : "(none — RUNWAY CLEAR)")
              << "\n";
    std::cout << "  photo             "
              << (data.photoAvailable ? "decoded" : "unavailable — placeholder shown")
              << "\n";
    std::cout << "  header state      " << (data.stale ? "CACHED" : data.updatedLabel)
              << "\n";
    for (const std::string& problem : layoutProblems) {
      std::cout << "  !! " << problem << "\n";
    }
    for (const Clipped& entry : clipped) {
      std::cout << "  ! " << entry.field << " clipped to fit: \"" << entry.rendered
                << "\"\n";
    }
  }

  if (data.photoPixels) free(data.photoPixels);
  return mismatches == 0 ? 0 : 1;
}
