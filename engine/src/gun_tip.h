// gun_tip.h - BACAMARTE (assets/tool_gun.e0m) no espaço de BIND da engine (skinning pelo osso da mão direita, índice 34).
// Autogerado por tools/export_gun.py - NÃO EDITE MANUALMENTE
#pragma once
static const float kGunMuzzle[3] = {1.53815f, 1.42878f, 0.26493f};     // boca do cano
static const float kGunMuzzleDir[3] = {0.94893f, 0.04923f, 0.31162f};  // direção do cano (para fora da boca)
// quadro da arma (origem no punho; X = para a boca, Y = esquerda da arma, Z = topo) -> bind (linha a linha): guardar nas costas (Renderer::drawStowed)
static const float kGunGrip[16] = {0.948931f, -0.000085f, 0.315483f, 0.869825f, 0.049226f, -0.987712f, -0.148331f, 1.401682f, 0.311619f, 0.156286f, -0.937266f, 0.093337f, 0.000000f, 0.000000f, 0.000000f, 1.000000f};
