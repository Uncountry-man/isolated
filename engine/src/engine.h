// Isolated - núcleo: ciclo de vida Android, EGL, loop principal
#pragma once
struct android_app;

namespace e0 {

// Interface que o jogo implementa.
class Game {
public:
    virtual ~Game() = default;
    virtual void onStart() {}
    virtual void onUpdate(float /*dt*/) {}
};

void run(android_app* app);  // loop principal (bloqueia até a activity fechar)

}  // namespace e0
