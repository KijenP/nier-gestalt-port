# Créditos

Este projeto é distribuído sob a GPL-3.0 (LICENSE), porque inclui o renderizador Renut, que é GPL-3.0.
Nenhum arquivo do jogo faz parte do projeto: o aplicativo pede a ISO do próprio jogador e gera o jogo a partir dela.

## Renderizador nativo (src/renut_engine, experimental)

- **reNut** — renderizador em nível de Direct3D para jogos de Xbox 360 recompilados, de masterspike52
  (github.com/masterspike52/reNut). Sem arquivo de licença; o README dele libera forks e o autor confirmou
  na issue #74 que qualquer um pode usar, por qualquer motivo.
- **banjo-nx**, **sa2-nx** e **mgr-nx-deko** — os ports (Banjo-Kazooie: Nuts & Bolts, Sonic Adventure 2 e Metal Gear
  Rising) por onde o Renut chegou até aqui, de onde vieram o código e a integração com o ReXGlue; mgr-nx-deko via o
  projeto Zandatsu (github.com/dfdx047/Zandatsu). GPL-3.0.
- Adaptação para o NieR Gestalt: endereços das funções D3D e layout do dispositivo do NieR (src/renut_engine, comentários
  "NieR"; o mapa completo fica fora do repositório).

## Bibliotecas de terceiros (thirdparty/)

- **plume** — camada D3D12/Vulkan (MIT).
- **XenosRecomp** (reblue-XenosRecomp) — tradutor de shaders do Xenos (MIT), com zstd, smol-v, xxHash e fmt.
- **Dear ImGui** (MIT).
- Avisos completos: THIRD_PARTY_NOTICES.md.

## Base

- **ReXGlue SDK** — recompilação estática (BSD-3-Clause).
