#include "png.h"

#include <zlib.h>

#include <cstdio>
#include <cstring>

namespace sim {
namespace {

void appendBigEndian32(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value >> 24));
  out.push_back(static_cast<uint8_t>(value >> 16));
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value));
}

void appendChunk(std::vector<uint8_t>& out, const char* type,
                 const std::vector<uint8_t>& data) {
  appendBigEndian32(out, static_cast<uint32_t>(data.size()));
  const size_t crcStart = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), data.begin(), data.end());
  const uLong crc = crc32(crc32(0L, Z_NULL, 0), out.data() + crcStart,
                          static_cast<uInt>(out.size() - crcStart));
  appendBigEndian32(out, static_cast<uint32_t>(crc));
}

}  // namespace

bool writePng(const std::string& path, int width, int height,
              const std::vector<uint8_t>& pixels, std::string& error) {
  const size_t expected = static_cast<size_t>(width) * height * 3;
  if (pixels.size() != expected) {
    error = "pixel buffer size does not match dimensions";
    return false;
  }

  // Filter type 0 (None) on every scanline: the images are flat colour and
  // compress well enough without adaptive filtering.
  std::vector<uint8_t> raw;
  raw.reserve(static_cast<size_t>(height) * (1 + static_cast<size_t>(width) * 3));
  for (int y = 0; y < height; ++y) {
    raw.push_back(0);
    const size_t offset = static_cast<size_t>(y) * width * 3;
    raw.insert(raw.end(), pixels.begin() + offset,
               pixels.begin() + offset + static_cast<size_t>(width) * 3);
  }

  uLongf compressedSize = compressBound(static_cast<uLong>(raw.size()));
  std::vector<uint8_t> compressed(compressedSize);
  if (compress2(compressed.data(), &compressedSize, raw.data(),
                static_cast<uLong>(raw.size()), Z_BEST_COMPRESSION) != Z_OK) {
    error = "zlib compression failed";
    return false;
  }
  compressed.resize(compressedSize);

  std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

  std::vector<uint8_t> header;
  appendBigEndian32(header, static_cast<uint32_t>(width));
  appendBigEndian32(header, static_cast<uint32_t>(height));
  header.push_back(8);  // bit depth
  header.push_back(2);  // colour type: truecolour
  header.push_back(0);  // deflate
  header.push_back(0);  // adaptive filtering
  header.push_back(0);  // no interlace
  appendChunk(out, "IHDR", header);
  appendChunk(out, "IDAT", compressed);
  appendChunk(out, "IEND", {});

  FILE* file = fopen(path.c_str(), "wb");
  if (!file) {
    error = std::string("cannot open ") + path + ": " + strerror(errno);
    return false;
  }
  const size_t written = fwrite(out.data(), 1, out.size(), file);
  fclose(file);
  if (written != out.size()) {
    error = std::string("short write to ") + path;
    return false;
  }
  return true;
}

}  // namespace sim
