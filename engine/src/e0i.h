#pragma once
// Upload de imagens .e0i para o GL. Aceita:
//   E0I1: 'E0I1' + w u16 + h u16 + RGBA8 (linhas de cima para baixo)
//   E0I2: 'E0I2' + w u16 + h u16 + níveis u8 + 3 bytes, e por nível: tamanho u32 + blocos ETC1/ETC2 RGB8 (tools/compress_textures.py)
// Cria a textura já ligada em GL_TEXTURE_2D (CLAMP_TO_EDGE, filtro linear; com mipmaps se `mips` e houver níveis).
#include <GLES3/gl3.h>
#include <cstdint>
#include <cstring>
#include <cstddef>

namespace e0 {

inline unsigned uploadE0I(const uint8_t* b, size_t len, int* outW, int* outH, bool mips) {
    if (!b || len < 8) return 0;
    const int w = b[4] | (b[5] << 8), h = b[6] | (b[7] << 8);
    if (w <= 0 || h <= 0) return 0;
    unsigned tex = 0;
    if (std::memcmp(b, "E0I1", 4) == 0) {
        if (len < 8 + (size_t)w * h * 4) return 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, b + 8);
        if (mips) glGenerateMipmap(GL_TEXTURE_2D);
    } else if (std::memcmp(b, "E0I2", 4) == 0 && len >= 12) {
        const int nl = b[8];
        size_t off = 12;
        int lw = w, lh = h, used = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        for (int l = 0; l < nl && (l == 0 || mips); ++l) {
            if (off + 4 > len) break;
            const uint32_t sz = (uint32_t)b[off] | ((uint32_t)b[off + 1] << 8) | ((uint32_t)b[off + 2] << 16) | ((uint32_t)b[off + 3] << 24);
            off += 4;
            if (off + sz > len || sz != (uint32_t)(((lw + 3) / 4) * ((lh + 3) / 4) * 8)) break;
            glCompressedTexImage2D(GL_TEXTURE_2D, l, GL_COMPRESSED_RGB8_ETC2, lw, lh, 0, (GLsizei)sz, b + off);
            off += sz; ++used;
            lw = lw > 1 ? lw / 2 : 1; lh = lh > 1 ? lh / 2 : 1;
        }
        if (!used) { glDeleteTextures(1, &tex); return 0; }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, used - 1);
        if (used == 1) mips = false;
    } else {
        return 0;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (outW) *outW = w;
    if (outH) *outH = h;
    return tex;
}

}  // namespace e0
