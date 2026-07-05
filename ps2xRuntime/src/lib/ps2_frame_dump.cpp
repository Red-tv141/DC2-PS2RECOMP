#include "ps2_frame_dump.h"
#include "ps2_runtime.h"
#include "runtime/ps2_gs_gpu.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <cstdio>

namespace dc2 {

bool dumpFramePpm(uint64_t tick, bool forceBlank, PS2Runtime* rt)
{
    if (!rt) return false;

    std::vector<uint8_t> pixels;
    uint32_t width = 0;
    uint32_t height = 0;
    
    if (!rt->gs().copyLatchedHostPresentationFrame(pixels, width, height, nullptr, nullptr, nullptr)) {
        return false;
    }

    if (pixels.empty() || width == 0 || height == 0) {
        return false;
    }

    if (!forceBlank) {
        bool allZero = true;
        const uint64_t* ptr64 = reinterpret_cast<const uint64_t*>(pixels.data());
        size_t count64 = pixels.size() / 8;
        for (size_t i = 0; i < count64; ++i) {
            if (ptr64[i] != 0) {
                allZero = false;
                break;
            }
        }
        if (allZero) {
            for (size_t i = count64 * 8; i < pixels.size(); ++i) {
                if (pixels[i] != 0) {
                    allZero = false;
                    break;
                }
            }
        }
        if (allZero) {
            return false;
        }
    }

    std::filesystem::create_directories("D:/ps2r/dc2/captures");
    char filename[256];
    std::snprintf(filename, sizeof(filename), "D:/ps2r/dc2/captures/frame_%06llu.ppm", static_cast<unsigned long long>(tick));

    std::ofstream out(filename, std::ios::binary);
    if (!out) return false;

    out << "P6\n" << width << " " << height << "\n255\n";

    for (size_t i = 0; i < pixels.size(); i += 4) {
        out.put(static_cast<char>(pixels[i]));
        out.put(static_cast<char>(pixels[i+1]));
        out.put(static_cast<char>(pixels[i+2]));
    }

    return true;
}

} // namespace dc2
