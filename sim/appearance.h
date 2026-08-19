// Turning palette indices into something that looks like the physical panel.
//
// Two renderings are produced from the same frame:
//
//   ideal  — the saturated RGB the library constants nominally mean. Flat and
//            crisp, so pixel diffs between runs are meaningful and layout
//            problems are obvious.
//   panel  — an approximation of the GDEP073E01 under normal indoor light:
//            muted reflective pigments, ~30:1 contrast, optical spreading
//            between neighbouring pixels, and the matte antiglare texture.
//
// The panel rendering exists to answer questions the ideal one cannot: is this
// 9pt label still readable, does yellow-on-white disappear, does the Floyd-
// Steinberg dither in the photo read as a photo or as noise.

#ifndef SIM_APPEARANCE_H
#define SIM_APPEARANCE_H

#include <cstdint>
#include <string>
#include <vector>

namespace sim {

// Which set of measured values to use. All three describe the same panel; they
// differ in how the measurement was normalised.
enum class PaletteMode {
  // Median of five independent calibrated captures, as photographed. Dim on a
  // bright monitor, but it is what a camera sees.
  kMeasured,
  // The same data normalised so panel white maps to #FFFFFF, approximating
  // chromatic adaptation once your eyes have accepted the panel as "white".
  kAdapted,
  // Halfway between the two. Reads as e-paper on screen without being so dark
  // that legibility judgements become pessimistic. Default.
  kBlend,
};

struct PanelOptics {
  PaletteMode palette = PaletteMode::kBlend;
  // Ambient illumination relative to a well-lit room. Reflective displays get
  // dimmer with the room; they do not emit their own light.
  double ambient = 1.0;
  // Gaussian spread, in pixels, of a single pigment microcup. Softens fine
  // detail and blends dithering the way the real surface does.
  double blurSigma = 0.62;
  // Matte surface graininess, in 0-255 units.
  double grain = 3.2;
  // Low-frequency reflectance variation across the panel.
  double mottle = 2.4;
  // Paint the unsupported orange state magenta instead of its likely physical
  // appearance, to make accidental use obvious.
  bool flagUnsupported = false;
  // Nearest-neighbour upscale applied after optics, for inspecting detail.
  int scale = 1;
};

// Flat library colours, no optical simulation.
std::vector<uint8_t> renderIdeal(const std::vector<uint8_t>& frame, int width, int height,
                                 int scale);

// Simulated physical appearance.
std::vector<uint8_t> renderPanel(const std::vector<uint8_t>& frame, int width, int height,
                                 const PanelOptics& optics);

// sRGB the panel is estimated to actually show for a palette index, before
// optics. Exposed so the report can print the palette it used.
void panelColor(uint8_t paletteIndex, PaletteMode mode, uint8_t& red, uint8_t& green,
                uint8_t& blue);

// True when the panel has no ink for this palette index.
bool isSupported(uint8_t paletteIndex);

// Human-readable note about how a palette entry behaves on this panel, or an
// empty string when it behaves as its name suggests.
std::string paletteCaveat(uint8_t paletteIndex);

}  // namespace sim

#endif  // SIM_APPEARANCE_H
