#pragma once
#include <cstdint>
// Calls the existing CPU transfer implementation, including its width handling,
// masks, nibble operations, and format-specific behavior. GPU verification must
// not call the acceleration's address/transform helpers.
uint32_t ps2GsTransferReadReference(const uint8_t* vram, uint32_t size, uint32_t bp,
                                  uint8_t bw, uint8_t psm, uint32_t x, uint32_t y);
void ps2GsTransferWriteReference(uint8_t* vram, uint32_t size, uint32_t bp,
                               uint8_t bw, uint8_t psm, uint32_t x, uint32_t y, uint32_t value);
