#include "mbedtls/base64.h"

#include <stdint.h>

namespace {

int decodeValue(unsigned char character) {
  if (character >= 'A' && character <= 'Z') return character - 'A';
  if (character >= 'a' && character <= 'z') return character - 'a' + 26;
  if (character >= '0' && character <= '9') return character - '0' + 52;
  if (character == '+') return 62;
  if (character == '/') return 63;
  return -1;
}

bool isSkippable(unsigned char character) {
  return character == ' ' || character == '\t' || character == '\r' || character == '\n';
}

}  // namespace

int mbedtls_base64_decode(unsigned char* dst, size_t dlen, size_t* olen,
                          const unsigned char* src, size_t slen) {
  size_t needed = 0;
  size_t significant = 0;
  size_t padding = 0;

  for (size_t i = 0; i < slen; ++i) {
    const unsigned char character = src[i];
    if (isSkippable(character)) continue;
    if (character == '=') {
      if (++padding > 2) return MBEDTLS_ERR_BASE64_INVALID_CHARACTER;
      continue;
    }
    if (padding != 0) return MBEDTLS_ERR_BASE64_INVALID_CHARACTER;
    if (decodeValue(character) < 0) return MBEDTLS_ERR_BASE64_INVALID_CHARACTER;
    ++significant;
  }

  if (significant % 4 == 1) return MBEDTLS_ERR_BASE64_INVALID_CHARACTER;
  needed = significant / 4 * 3;
  switch (significant % 4) {
    case 2: needed += 1; break;
    case 3: needed += 2; break;
    default: break;
  }

  *olen = needed;
  if (dst == nullptr || dlen < needed) return MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL;

  uint32_t accumulator = 0;
  int bits = 0;
  size_t written = 0;
  for (size_t i = 0; i < slen; ++i) {
    const unsigned char character = src[i];
    if (isSkippable(character) || character == '=') continue;
    accumulator = (accumulator << 6) | static_cast<uint32_t>(decodeValue(character));
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      dst[written++] = static_cast<unsigned char>((accumulator >> bits) & 0xFF);
    }
  }

  *olen = written;
  return 0;
}
