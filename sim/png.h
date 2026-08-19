#ifndef SIM_PNG_H
#define SIM_PNG_H

#include <cstdint>
#include <string>
#include <vector>

namespace sim {

// Writes an 8-bit RGB PNG. `pixels` is row-major RGB triplets, width*height*3.
// Returns false and fills `error` if the file cannot be written.
bool writePng(const std::string& path, int width, int height,
              const std::vector<uint8_t>& pixels, std::string& error);

}  // namespace sim

#endif  // SIM_PNG_H
