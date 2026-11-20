// Isolated - TELA DE LOADING: arte de fundo (assets/loading.e0i), etapa atual em texto no canto inferior direito e um progress circular
// INDETERMINADO. Totalmente autônoma (programa GL próprio, texto rasterizado na CPU com font5x7.h): funciona ANTES de Renderer::init().
#pragma once
#include <android/asset_manager.h>
#include <string>

namespace e0 {

class LoadingScreen {
public:
    bool init(AAssetManager* am);          // carrega a arte e compila o programa (contexto GL atual)
    void shutdown();                        // libera tudo (chamar com o contexto GL atual)
    bool ready() const { return prog_ != 0; }
    // Entrega a textura da arte de fundo (e o tamanho) a outro desenho (o menu principal) e deixa de ser dona dela. 0 se não há arte.
    unsigned takeImage(int& w, int& h) { unsigned t = texImg_; w = imgW_; h = imgH_; texImg_ = 0; return t; }
    // Desenha por cima de tudo. stage = texto da etapa (maiúsculas, sem acento), t = tempo em s (anima o círculo), alpha = opacidade geral (fade-out).
    void draw(int w, int h, const char* stage, float t, float alpha);

private:
    unsigned prog_ = 0, vao_ = 0, texImg_ = 0, texTxt_ = 0;
    int imgW_ = 0, imgH_ = 0, txtW_ = 0, txtH_ = 0, txtScale_ = 0;
    std::string txt_;
    int uRect_ = -1, uScreen_ = -1, uMode_ = -1, uColor_ = -1, uTime_ = -1, uUv_ = -1;
    void buildText(const char* s, int scale);
};

}  // namespace e0
