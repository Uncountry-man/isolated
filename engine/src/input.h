// Isolated - entrada por toque: metade esquerda = mover, metade direita = olhar
#pragma once

namespace e0 {

struct Input {
    // eixo de movimento (-1..1), x = direita, y = frente
    float moveX = 0, moveY = 0;
    // delta de olhar acumulado em pixels desde o último frame
    float lookDX = 0, lookDY = 0;
};

}  // namespace e0
