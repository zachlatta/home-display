// PanelCanvas — a host stand-in for GxEPD2_7C<GxEPD2_730c_GDEP073E01, 40>.
//
// It reproduces the parts of the driver stack that change what ends up on the
// glass:
//   * the GxEPD_* RGB565 constants
//   * GxEPD2_7C::color7()          — RGB565 -> 3-bit palette index
//   * GDEP073E01::_convert_to_native() — palette index -> panel wire code
//   * the paged framebuffer        — 12 bands of 40 rows for a 480-row panel
//
// Everything is transcribed from Seeed_GxEPD2 (src/GxEPD2.h, src/GxEPD2_7C.h,
// src/epd7c/GxEPD2_730c_GDEP073E01.cpp). Keeping the paging real matters: the
// firmware draws the whole layout 12 times and each pass only keeps the rows
// inside its band, so a page-boundary bug is invisible to a full-frame mock.

#ifndef SIM_PANEL_H
#define SIM_PANEL_H

#include <Adafruit_GFX.h>

#include <cstdint>
#include <vector>

// --- GxEPD2.h colour constants (RGB565) ------------------------------------
#define GxEPD_BLACK 0x0000
#define GxEPD_WHITE 0xFFFF
#define GxEPD_RED 0xF800
#define GxEPD_YELLOW 0xFFE0
#define GxEPD_BLUE 0x001F
#define GxEPD_GREEN 0x07E0
#define GxEPD_ORANGE 0xFC00

namespace sim {

// Palette indices as GxEPD2_7C numbers them.
enum PaletteIndex : uint8_t {
  kBlack = 0,
  kWhite = 1,
  kGreen = 2,
  kBlue = 3,
  kRed = 4,
  kYellow = 5,
  kOrange = 6,
  kPaletteSize = 7,
};

constexpr const char* kPaletteNames[kPaletteSize] = {
    "black", "white", "green", "blue", "red", "yellow", "orange",
};

// GxEPD2_7C::color7 — verbatim, including the RGB565 fallback for colours that
// are not one of the seven named constants.
inline uint8_t color7(uint16_t color) {
  switch (color) {
    case GxEPD_BLACK: return 0x00;
    case GxEPD_WHITE: return 0x01;
    case GxEPD_GREEN: return 0x02;
    case GxEPD_BLUE: return 0x03;
    case GxEPD_RED: return 0x04;
    case GxEPD_YELLOW: return 0x05;
    case GxEPD_ORANGE: return 0x06;
    default: {
      const uint16_t red = color & 0xF800;
      const uint16_t green = static_cast<uint16_t>((color & 0x07E0) << 5);
      const uint16_t blue = static_cast<uint16_t>((color & 0x001F) << 11);
      if ((red < 0x8000) && (green < 0x8000) && (blue < 0x8000)) return 0x00;
      if ((red >= 0x8000) && (green >= 0x8000) && (blue >= 0x8000)) return 0x01;
      if ((red >= 0x8000) && (blue >= 0x8000)) return red > blue ? 0x04 : 0x03;
      if ((green >= 0x8000) && (blue >= 0x8000)) return green > blue ? 0x02 : 0x03;
      if ((red >= 0x8000) && (green >= 0xC000)) return 0x05;
      if ((red >= 0x8000) && (green >= 0x4000)) return 0x06;
      if (red >= 0x8000) return 0x04;
      if (green >= 0x8000) return 0x02;
      return 0x03;
    }
  }
}

// GxEPD2_730c_GDEP073E01::_convert_to_native, per nibble. The panel's wire
// codes are not the same numbers the library uses internally.
inline uint8_t nativeCode(uint8_t paletteIndex) {
  switch (paletteIndex & 0x07) {
    case 0x00: return 0x00;  // black
    case 0x01: return 0x01;  // white
    case 0x02: return 0x06;  // green
    case 0x03: return 0x05;  // blue
    case 0x04: return 0x03;  // red
    case 0x05: return 0x02;  // yellow
    case 0x06: return 0x04;  // orange
    default: return 0x07;    // white
  }
}

class PanelCanvas : public Adafruit_GFX {
 public:
  static constexpr int16_t kWidth = 800;
  static constexpr int16_t kHeight = 480;

  // pageHeight 40 matches the firmware's MAX_DISPLAY_BUFFER_SIZE of 16000.
  // Passing kHeight renders the frame in a single page, which is only useful
  // for proving the paged path produces identical output.
  explicit PanelCanvas(int16_t pageHeight = 40)
      : Adafruit_GFX(kWidth, kHeight),
        pageHeight_(pageHeight),
        pages_(kHeight / pageHeight + ((kHeight % pageHeight) > 0)),
        pageBuffer_(static_cast<size_t>(kWidth) / 2 * pageHeight, 0),
        frame_(static_cast<size_t>(kWidth) * kHeight, kWhite) {}

  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    if ((x < 0) || (x >= width()) || (y < 0) || (y >= height())) return;
    switch (getRotation()) {
      case 1:
        std::swap(x, y);
        x = kWidth - x - 1;
        break;
      case 2:
        x = kWidth - x - 1;
        y = kHeight - y - 1;
        break;
      case 3:
        std::swap(x, y);
        y = kHeight - y - 1;
        break;
      default:
        break;
    }
    y -= currentPage_ * pageHeight_;
    if ((y < 0) || (y >= pageHeight_)) return;
    const size_t index = static_cast<size_t>(x) / 2 + static_cast<size_t>(y) * (kWidth / 2);
    const uint8_t value = color7(color);
    if (x & 1) pageBuffer_[index] = (pageBuffer_[index] & 0xF0) | value;
    else pageBuffer_[index] = (pageBuffer_[index] & 0x0F) | static_cast<uint8_t>(value << 4);
  }

  void fillScreen(uint16_t color) override {
    const uint8_t value = color7(color);
    const uint8_t packed = static_cast<uint8_t>(value | (value << 4));
    std::fill(pageBuffer_.begin(), pageBuffer_.end(), packed);
  }

  void init(uint32_t = 0) {}
  void setFullWindow() {}
  void hibernate() { hibernated_ = true; }

  void firstPage() {
    fillScreen(GxEPD_WHITE);
    currentPage_ = 0;
    hibernated_ = false;
    drawCalls_ = 0;
  }

  // Commits the current band into the frame, then advances. Returns true while
  // more bands remain, exactly like GxEPD2_7C's full-update path.
  bool nextPage() {
    const int16_t pageStart = currentPage_ * pageHeight_;
    const int16_t rows = std::min<int16_t>(pageHeight_, kHeight - pageStart);
    for (int16_t y = 0; y < rows; ++y) {
      for (int16_t x = 0; x < kWidth; ++x) {
        const size_t index =
            static_cast<size_t>(x) / 2 + static_cast<size_t>(y) * (kWidth / 2);
        const uint8_t packed = pageBuffer_[index];
        const uint8_t value = (x & 1) ? (packed & 0x0F) : (packed >> 4);
        frame_[static_cast<size_t>(pageStart + y) * kWidth + x] = value;
      }
    }
    ++currentPage_;
    ++drawCalls_;
    if (currentPage_ == pages_) {
      currentPage_ = 0;
      return false;
    }
    fillScreen(GxEPD_WHITE);
    return true;
  }

  // Palette index (0..6) per pixel, row-major, 800x480.
  const std::vector<uint8_t>& frame() const { return frame_; }
  int16_t pages() const { return pages_; }
  int16_t pageHeight() const { return pageHeight_; }
  int drawCalls() const { return drawCalls_; }
  bool hibernated() const { return hibernated_; }

  // Native wire code the driver would transmit for a given pixel.
  uint8_t nativeAt(size_t index) const { return nativeCode(frame_[index]); }

 private:
  int16_t pageHeight_;
  int16_t pages_;
  int16_t currentPage_ = 0;
  int drawCalls_ = 0;
  bool hibernated_ = false;
  std::vector<uint8_t> pageBuffer_;
  std::vector<uint8_t> frame_;
};

}  // namespace sim

#endif  // SIM_PANEL_H
