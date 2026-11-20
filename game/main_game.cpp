// Jogo v0: cenário plano e vazio. Aqui nascerá o gameplay.
#include <android_native_app_glue.h>

#include "../engine/src/engine.h"

namespace e0 {

class EmptyWorld : public Game {
public:
    void onStart() override {}
    void onUpdate(float) override {}
};

Game* createGame() { return new EmptyWorld(); }

}  // namespace e0

extern "C" void android_main(android_app* app) { e0::run(app); }
