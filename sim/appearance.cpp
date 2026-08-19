#include "appearance.h"

#include "panel.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sim {
namespace {

struct Rgb {
  uint8_t r, g, b;
};

// ---------------------------------------------------------------------------
// Palettes
// ---------------------------------------------------------------------------
//
// GDEP073E01 is an E Ink Spectra 6 panel: black, white, yellow, red, green,
// blue. The Good Display datasheet publishes no colorimetry at all (its optical
// characteristics table is a stub), so these are the median of five independent
// calibrated measurements of this exact panel from open-source projects, which
// agree to within a few percent on everything except black.
//
// Indices follow GxEPD2_7C::color7 ordering, not the panel's wire codes.

constexpr Rgb kMeasured[kPaletteSize] = {
    {0x1A, 0x0D, 0x23},  // black  — sources disagree here; some see a purple cast
    {0xB9, 0xC7, 0xC8},  // white  — cool paper-grey, roughly half the luminance of paper
    {0x28, 0x54, 0x3A},  // green  — very dark forest, the dimmest primary
    {0x05, 0x40, 0x8B},  // blue   — deep navy
    {0x75, 0x13, 0x00},  // red    — dark brick, not scarlet
    {0xC9, 0xB8, 0x00},  // yellow — olive-gold, no blue component at all
    {0x8E, 0x4A, 0x00},  // orange — NOT A REAL STATE; see kOrangeNote
};

constexpr Rgb kAdapted[kPaletteSize] = {
    {0x28, 0x14, 0x30}, {0xFF, 0xFF, 0xFF}, {0x3B, 0x6E, 0x4D}, {0x0A, 0x55, 0xB2},
    {0xA3, 0x1C, 0x00}, {0xFF, 0xEC, 0x00}, {0xC4, 0x63, 0x00},
};

constexpr Rgb kBlend[kPaletteSize] = {
    {0x22, 0x11, 0x2A}, {0xE0, 0xE5, 0xE6}, {0x32, 0x62, 0x44}, {0x08, 0x4B, 0xA0},
    {0x8E, 0x18, 0x00}, {0xF3, 0xD4, 0x00}, {0xA9, 0x56, 0x00},
};

constexpr Rgb kIdeal[kPaletteSize] = {
    {0x00, 0x00, 0x00}, {0xFF, 0xFF, 0xFF}, {0x00, 0xA8, 0x46}, {0x14, 0x46, 0xBE},
    {0xDC, 0x23, 0x23}, {0xF5, 0xD7, 0x19}, {0xF0, 0x7D, 0x19},
};

const Rgb* paletteFor(PaletteMode mode) {
  switch (mode) {
    case PaletteMode::kMeasured: return kMeasured;
    case PaletteMode::kAdapted: return kAdapted;
    case PaletteMode::kBlend: default: return kBlend;
  }
}

constexpr const char* kOrangeNote =
    "no ink on Spectra 6; the driver emits reserved wire code 0x4 and the "
    "displayed result is undefined (in practice a muddy red/yellow)";

// ---------------------------------------------------------------------------
// sRGB transfer
// ---------------------------------------------------------------------------

double toLinear(double channel) {
  channel /= 255.0;
  return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
}

uint8_t toSrgb(double linear) {
  linear = std::clamp(linear, 0.0, 1.0);
  const double encoded =
      linear <= 0.0031308 ? linear * 12.92 : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
  return static_cast<uint8_t>(std::lround(std::clamp(encoded, 0.0, 1.0) * 255.0));
}

// Deterministic value noise so repeated runs diff cleanly.
double hashNoise(int x, int y, uint32_t salt) {
  uint32_t h = static_cast<uint32_t>(x) * 374761393u +
               static_cast<uint32_t>(y) * 668265263u + salt * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u;
  h ^= h >> 16;
  return static_cast<double>(h) / 4294967295.0 * 2.0 - 1.0;  // -1..1
}

std::vector<uint8_t> upscale(const std::vector<uint8_t>& pixels, int width, int height,
                             int scale) {
  if (scale <= 1) return pixels;
  std::vector<uint8_t> out(static_cast<size_t>(width) * scale * height * scale * 3);
  for (int y = 0; y < height * scale; ++y) {
    for (int x = 0; x < width * scale; ++x) {
      const size_t source = (static_cast<size_t>(y / scale) * width + x / scale) * 3;
      const size_t target = (static_cast<size_t>(y) * width * scale + x) * 3;
      out[target] = pixels[source];
      out[target + 1] = pixels[source + 1];
      out[target + 2] = pixels[source + 2];
    }
  }
  return out;
}

}  // namespace

void panelColor(uint8_t paletteIndex, PaletteMode mode, uint8_t& red, uint8_t& green,
                uint8_t& blue) {
  const Rgb* palette = paletteFor(mode);
  const Rgb color = palette[paletteIndex < kPaletteSize ? paletteIndex : kWhite];
  red = color.r;
  green = color.g;
  blue = color.b;
}

bool isSupported(uint8_t paletteIndex) { return paletteIndex != kOrange; }

std::string paletteCaveat(uint8_t paletteIndex) {
  if (paletteIndex == kOrange) return kOrangeNote;
  return "";
}

std::vector<uint8_t> renderIdeal(const std::vector<uint8_t>& frame, int width, int height,
                                 int scale) {
  std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 3);
  for (size_t i = 0; i < frame.size(); ++i) {
    const Rgb color = kIdeal[frame[i] < kPaletteSize ? frame[i] : kWhite];
    pixels[i * 3] = color.r;
    pixels[i * 3 + 1] = color.g;
    pixels[i * 3 + 2] = color.b;
  }
  return upscale(pixels, width, height, scale);
}

std::vector<uint8_t> renderPanel(const std::vector<uint8_t>& frame, int width, int height,
                                 const PanelOptics& optics) {
  const Rgb* palette = paletteFor(optics.palette);
  const size_t count = static_cast<size_t>(width) * height;

  // 1. Palette lookup into linear light, where blurring and illumination are
  //    physically meaningful.
  std::vector<double> linear(count * 3);
  for (size_t i = 0; i < count; ++i) {
    const uint8_t index = frame[i] < kPaletteSize ? frame[i] : kWhite;
    Rgb color = palette[index];
    if (index == kOrange && optics.flagUnsupported) color = Rgb{0xFF, 0x00, 0xFF};
    linear[i * 3] = toLinear(color.r);
    linear[i * 3 + 1] = toLinear(color.g);
    linear[i * 3 + 2] = toLinear(color.b);
  }

  // 2. Separable Gaussian: pigment in a microcup is not a hard-edged square,
  //    and the antiglare coating spreads it further. This is what makes dither
  //    patterns resolve into tone.
  if (optics.blurSigma > 0.01) {
    const int radius = std::max(1, static_cast<int>(std::ceil(optics.blurSigma * 3.0)));
    std::vector<double> kernel(radius * 2 + 1);
    double sum = 0.0;
    for (int i = -radius; i <= radius; ++i) {
      const double weight =
          std::exp(-(i * i) / (2.0 * optics.blurSigma * optics.blurSigma));
      kernel[i + radius] = weight;
      sum += weight;
    }
    for (double& weight : kernel) weight /= sum;

    std::vector<double> horizontal(linear.size());
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        double accumulator[3] = {0, 0, 0};
        for (int k = -radius; k <= radius; ++k) {
          const int sx = std::clamp(x + k, 0, width - 1);
          const size_t source = (static_cast<size_t>(y) * width + sx) * 3;
          const double weight = kernel[k + radius];
          accumulator[0] += linear[source] * weight;
          accumulator[1] += linear[source + 1] * weight;
          accumulator[2] += linear[source + 2] * weight;
        }
        const size_t target = (static_cast<size_t>(y) * width + x) * 3;
        horizontal[target] = accumulator[0];
        horizontal[target + 1] = accumulator[1];
        horizontal[target + 2] = accumulator[2];
      }
    }
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        double accumulator[3] = {0, 0, 0};
        for (int k = -radius; k <= radius; ++k) {
          const int sy = std::clamp(y + k, 0, height - 1);
          const size_t source = (static_cast<size_t>(sy) * width + x) * 3;
          const double weight = kernel[k + radius];
          accumulator[0] += horizontal[source] * weight;
          accumulator[1] += horizontal[source + 1] * weight;
          accumulator[2] += horizontal[source + 2] * weight;
        }
        const size_t target = (static_cast<size_t>(y) * width + x) * 3;
        linear[target] = accumulator[0];
        linear[target + 1] = accumulator[1];
        linear[target + 2] = accumulator[2];
      }
    }
  }

  // 3. Ambient light, surface grain, and slow reflectance mottle. All are
  //    multiplicative on a reflective surface rather than additive.
  std::vector<uint8_t> pixels(count * 3);
  const double grain = optics.grain / 255.0;
  const double mottle = optics.mottle / 255.0;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const size_t offset = (static_cast<size_t>(y) * width + x) * 3;
      const double speckle = 1.0 + hashNoise(x, y, 0x9E3779B9u) * grain;
      const double slow = 1.0 + hashNoise(x / 48, y / 48, 0x85EBCA6Bu) * mottle;
      for (int channel = 0; channel < 3; ++channel) {
        pixels[offset + channel] =
            toSrgb(linear[offset + channel] * optics.ambient * speckle * slow);
      }
    }
  }

  return upscale(pixels, width, height, optics.scale);
}

}  // namespace sim
