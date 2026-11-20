#pragma once
// Decodificador IMA ADPCM mono (WAVE_FORMAT 0x11, blocos com cabeçalho de 4 bytes), usado por Audio::loadWav.
// Os .wav de assets/sfx são gerados por tools/compress_wav.py (4 bits/amostra).
#include <cstdint>
#include <cstddef>
#include <vector>

namespace e0 {

inline void decodeImaAdpcm(const uint8_t* d, size_t len, int blockAlign, size_t totalSamples, std::vector<int16_t>& out) {
    static const int16_t kStep[89] = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88,
        97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060,
        1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
        9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
    static const int8_t kIdx[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
    out.clear();
    if (blockAlign <= 4) return;
    out.reserve(totalSamples ? totalSamples : len * 2);
    for (size_t o = 0; o + 4 <= len; o += (size_t)blockAlign) {
        const size_t end = o + (size_t)blockAlign < len ? o + (size_t)blockAlign : len;
        int pred = (int16_t)(d[o] | (d[o + 1] << 8));
        int idx = d[o + 2] > 88 ? 88 : d[o + 2];
        out.push_back((int16_t)pred);
        for (size_t i = o + 4; i < end; ++i) {
            for (int h = 0; h < 2; ++h) {
                const int code = h ? (d[i] >> 4) : (d[i] & 15);
                const int step = kStep[idx];
                int vd = step >> 3;
                if (code & 4) vd += step;
                if (code & 2) vd += step >> 1;
                if (code & 1) vd += step >> 2;
                pred = (code & 8) ? pred - vd : pred + vd;
                pred = pred < -32768 ? -32768 : (pred > 32767 ? 32767 : pred);
                idx += kIdx[code & 7];
                idx = idx < 0 ? 0 : (idx > 88 ? 88 : idx);
                out.push_back((int16_t)pred);
            }
        }
    }
    if (totalSamples && out.size() > totalSamples) out.resize(totalSamples);
}

}  // namespace e0
