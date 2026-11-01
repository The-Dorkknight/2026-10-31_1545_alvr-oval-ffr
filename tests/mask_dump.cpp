// Prints a mask so tests/test_mask_parity.py can compare it with the Python mirror.
// usage: mask_dump h v shape band outer width height tile cx cy max_log2
#include "ovalffr_core.h"

#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc < 12) { std::fprintf(stderr, "need 11 arguments\n"); return 2; }
    ovalffr::Config c;
    c.h = (float)std::atof(argv[1]); c.v = (float)std::atof(argv[2]);
    c.shape = (float)std::atof(argv[3]); c.band = (float)std::atof(argv[4]);
    c.outer = std::atoi(argv[5]);
    int w = std::atoi(argv[6]), h = std::atoi(argv[7]), tile = std::atoi(argv[8]);
    float cx = (float)std::atof(argv[9]), cy = (float)std::atof(argv[10]);
    int tw = 0, th = 0;
    auto m = ovalffr::make_mask(c, w, h, tile, cx, cy, std::atoi(argv[11]), &tw, &th);
    std::printf("%d %d %.6f\n", tw, th, ovalffr::estimated_saving(m));
    for (int y = 0; y < th; ++y) {
        for (int x = 0; x < tw; ++x) std::printf("%d%c", m[(size_t)y * tw + x], x + 1 < tw ? ' ' : '\n');
    }
    return 0;
}
