#pragma once
#include <cstdint>

// Diagnostic face identity follows the VIF producer into the ordered MTVU kick.
extern int g_g788Mode;
void dc2_g788_watchRef(uint32_t frame, uint32_t address, uint64_t tag, uint32_t seq);
uint32_t dc2_g788_takeRef(uint32_t address, uint64_t tag);
void dc2_g788_vifMarker(uint64_t tagHi);
uint32_t dc2_g788_vuFace();
void dc2_g788_captureVu(uint32_t seq, const uint8_t *data, const float (*vf)[4], uint32_t pc, bool resume);
