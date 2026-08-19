// mbedtls base64 shim matching the ESP-IDF signature and return codes used by
// the shared payload parser.

#ifndef SIM_MBEDTLS_BASE64_H
#define SIM_MBEDTLS_BASE64_H

#include <stddef.h>

#define MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL -0x002A
#define MBEDTLS_ERR_BASE64_INVALID_CHARACTER -0x002C

// Decodes `slen` bytes of base64 from `src` into `dst`, writing the decoded
// length to `olen`. Returns 0 on success. Whitespace is skipped, matching
// mbedtls; any other invalid character is an error.
int mbedtls_base64_decode(unsigned char* dst, size_t dlen, size_t* olen,
                          const unsigned char* src, size_t slen);

#endif  // SIM_MBEDTLS_BASE64_H
