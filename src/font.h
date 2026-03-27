#ifndef FONT_H
#define FONT_H

#include "common.h"

// A simple 8x16 bitmap font (ISO-8859-1 subset)
// Each character is 16 bytes, each byte is one row of 8 bits.
extern const uint8_t font8x16[256][16];

#endif
