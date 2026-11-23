# Instalação — como está hoje (14/11/2026)

Estado real de cada caminho de instalação, para corrigir antes do GitHub.

## PC

### O que o `Preparar.cmd` faz hoje

1. Lê a ISO (ou a pasta com `default.xex`), confere o Title ID e copia o `default.xex` para `game\`.
2. Confere as ferramentas: Visual Studio Build Tools (C++), LLVM 20, CMake, Ninja e Git (`scripts\env.cmd`).
3. Baixa o ReXGlue SDK **v0.10.0**, aplica `patches\rexglue-v0.10.0.diff` e compila/instala em `.sdk\install`.
4. Roda `scripts\build.cmd`: codegen + `cmake --preset win-amd64-release` → `out\build\win-amd64-release\nier.exe`.
5. Grava `game_data_root` no `nier.toml` e cria o atalho com o ícone tirado do disco.

### Problema: o Preparar gera uma versão VELHA

- O patch do SDK é de **29/10**. Tudo o que mudou no SDK depois (Renut no SDK, correções de áudio, entrada, Android etc.) não entra.
- O `build.cmd` usa o preset `win-amd64-release` **sem `NIER_RENUT=ON`** (o padrão é OFF). Sai o renderizador antigo (Xenos), não o Renut.
- O jogo que testamos é o `out\build\win-renut`. Ele usa:
  - o SDK compilado de `H:\NIER PORT\sdk-src` (instalado em `sdk-src\out\install\win-amd64`);
  - `-DNIER_RENUT=ON`;
  - os shaders capturados em `H:\NIER PORT\work\renut-shaders` (`NIER_RENUT_SHADER_DIR`).

### Corrigido em 14/11

1. **Patch do SDK regenerado**: `patches\rexglue-v0.10.0.diff` (9,2 MB) é o `sdk-src` inteiro contra a v0.10.0.
   - Inclui a remoção do D3D12 de 30/10 e 4 arquivos novos.
   - Não tem caminhos da máquina.
   - Aplica limpo numa cópia nova da v0.10.0 (`git apply --check`).
   - O SDL tem o patch à parte, `sdl3-android.diff`, que não mudou.
2. **`build.cmd`**: `-DNIER_RENUT=ON` e `-DNIER_GAME_PATH` (shaders tirados do executável do disco). O `setup.ps1` passa a ISO escolhida. As DLLs são as mesmas: `rexruntime.dll` e `rexgpu-xenos.dll`.
3. **Renderizador padrão** = `renut` (antes era `xenos`; só funcionava porque o seu `nier.toml` mudava isso à mão).
4. **Nome**: o executável virou `NieR Gestalt.exe` e a janela, "NieR Gestalt". O nome interno continua `nier`: pastas, `nier.toml` e saves não mudam.
5. **Ícone do disco**: o `setup.ps1` grava `game\nier.ico` (tirado do `nxeart`) **antes** de compilar, e o CMake o põe dentro do `.exe`.
6. **Tradução**: `traducao\Portugues`, com só os arquivos que mudam. O `setup.ps1` copia para `TEXT\`.

### Ainda falta

- Testar o `Preparar.cmd` do zero numa pasta limpa (ou num Windows limpo), com a ISO: clone do SDK + patch + build completo.

### Para o jogador (depois de corrigido)

1. Baixar o repositório.
2. Abrir o `Preparar.cmd` e escolher a ISO.
3. Esperar de 20 a 60 minutos e usar o atalho **NieR Gestalt**.

Os shaders que faltarem são capturados enquanto o jogador joga; a primeira vez em cada lugar pode dar uma travadinha.

## Android

### Como o APK é feito hoje (só na máquina do desenvolvedor)

1. Precisa do jogo compilado para o PC antes: o Android usa o `shader_cache.cpp` do build do PC (`NIER_RENUT_SHADER_CACHE_CPP=out\build\win-renut\renut_gpu\shader_cache.cpp`).
2. Configuração manual (sem preset) em `out\build\android-arm64-h`:
   - NDK 27.2.12479018, `android-30`, `arm64-v8a`, `c++_shared`, Release, Ninja;
   - `NIER_RENUT=ON`;
   - `REXSDK_DIR=H:\NIER PORT\sdk-src` (o SDK é compilado junto, saindo em `sdk-src\out\linux-arm64`).
3. `cmake --build out\build\android-arm64-h`.
4. `android\build-apk.ps1 -Build out\build\android-arm64-h` gera `out\nier-test.apk`. O script:
   - usa javac, d8, aapt2, zipalign e apksigner, sem Gradle;
   - assina com uma chave de teste local;
   - precisa do Android SDK (build-tools 34, platform android-34) e do JDK 17. Os caminhos padrão são `H:\Android\...`, mas podem ser trocados por parâmetro.

### No celular

1. Instalar o APK e dar a permissão **"Acesso a todos os arquivos"** (o app lê `/sdcard/NierPort`).
2. Copiar a ISO para `/sdcard/NierPort/Nier.iso`. Também serve a pasta extraída em `/sdcard/NierPort/game`.
3. Opcional: tradução em `/sdcard/NierPort/TEXT/Portugues`.
4. As configurações ficam em `/sdcard/NierPort/nier.toml`, e os botões de toque em `touch_controls.txt`.

### O que falta para outras pessoas conseguirem

- O APK **não pode ser distribuído pronto**: ele contém o código do jogo recompilado. Cada pessoa precisa gerar o seu a partir da própria ISO, no PC.
- Falta:
  - um script único (tipo `Preparar-Android.cmd`) que configure o build Android depois do PC e gere o APK;
  - conferir as ferramentas (Android SDK, NDK, JDK) e dizer como instalar o que faltar;
  - tirar os caminhos `H:\` fixos e explicar como instalar o APK (cabo/adb ou copiando o arquivo).
- `patches\sdl3-android.diff` (controle Mamba) também precisa entrar no fluxo.
- Testes de desenvolvimento (`env.txt`, `pad.txt`, `live.txt`, `tools\phone*.ps1`) ficam fora das instruções.

### Decisão do usuário (14/11): APK "Projeto Gestalt" no próprio celular

Nada de gerar no PC. O fluxo desejado:

1. Um APK **Projeto Gestalt**, sem nada do jogo.
2. Escolher a ISO (ou o `default.xex`).
3. Ler o executável:
   - decifrar (AES, como o `nier_shader_extract` já faz);
   - gerar o código (codegen do ReXGlue);
   - **compilar no celular**.
4. Extrair os shaders e o ícone (`nxeart`).
5. Montar e assinar um APK novo **NieR Gestalt** (chave criada no celular e guardada, para atualizações).
6. Instalar pedindo "instalar apps desconhecidos" e, depois, oferecer desinstalar o Projeto Gestalt.

Peças necessárias:
- clang/lld para Android dentro do Projeto Gestalt;
- o SDK e o código do port já compilados (só o código gerado compila no celular);
- `rexglue codegen` para arm64;
- o tradutor de shaders (XenosRecomp + SPIR-V) para arm64;
- um modelo do APK (manifesto, dex, SDL) e um assinador.

Referências (pesquisa de 14/11, a estudar):
- EoWRecomp (Echoes of Wisdom): o Android leva Python e clang dentro do APK e compila o código do jogo no celular — https://github.com/Nycolazs/EoWRecomp/wiki
- BOTWRecompAndroid (base LAS Recomp / LASRecompAndroid): APK + pasta exefs/romfs do jogador — https://raw.githubusercontent.com/mamcjr/botwrecompandroid/main/README.md
- UnleashedRecomp Android (outra recompilação de Xbox 360, a mais parecida com a nossa) — https://github.com/M4cAndCheese/UnleashedRecomp_Android

Os repositórios que o usuário mandou (lidos só pelo README, 14/11):
- **Bloodborne-Android** (https://github.com/ducvd89/Bloodborne-Android) — **o modelo a seguir**:
  - **Compila no celular** desde a 2.1.0, com o mesmo compilador do PC embutido no app (`libbbrecomp.so`).
  - Gera `libbbaot.so` + `.json`, que o próprio app instala e carrega. **Não gera outro APK.**
  - Medido por eles: ~6 min num Snapdragon 8 Gen 2 (na tomada), ~4 GB de RAM livre (não compila com menos de 1,5 GB), ~1 GB livre durante e ~130 MB no fim, CPU a ~95 °C.
  - Aceita os PKGs do jogo ou uma pasta com o `eboot.bin`.
  - Leva o **Turnip** para Adreno (o driver do celular dá tela preta). Mali não é suportado (PanVK só para teste).
  - Padrão: GSR 2 (~1,1 ms), cena em 960x540 com saída 1080p; "detalhe de modelo pesa mais que a resolução".
- **Renut-Android** (https://github.com/Innersargento/Renut-Android): Banjo Nuts & Bolts com Renut no Android.
  - O APK é feito no PC (`tools/prepare.py all --game <ISO>` e depois `prepare.py apk`); confere o SHA-256 do `default.xex`.
  - Leva o **Turnip** via libadrenotools + liblinkernsbypass. **Estudar para o bug do rosto no Snapdragon e para desempenho.**
- **asuras-wrath-recomp** (https://github.com/PauloUbirajara/asuras-wrath-recomp): ReXGlue + Xenos. O Android é experimental, com APK feito no PC (`build_android.sh`).
- **CBFD-Recompiled-Android-Evolved** (https://github.com/codepdbh/CBFD-Recompiled-Android-Evolved): APK nas Releases; o README não diz se compila no celular (ver `android/INSTALL.md` e `HANDOFF-android-compatibility.md`).

Correção do usuário: esses ports entregam um APK feito no PC; dentro dele a pessoa escolhe a ISO ou a pasta, o app
prepara o que precisa e o jogo roda. Ou seja, o código recompilado vai pronto no APK (como no Unleashed Recompiled
do PC) e o disco do jogador dá o resto. Para nós isso seria:
1. APK com o `libmain.so` pronto.
2. Na primeira abertura, um seletor de arquivo (Android SAF) para a ISO ou pasta, em vez de copiar à mão para `/sdcard/NierPort`.
3. Preparação: shaders do executável, ícone etc.
4. Botão para jogar.

É muito menos trabalho que compilar no celular. A decisão (APK pronto vs. compilar no celular) é do usuário.

Consequência para nós: o caminho mais simples é o do Bloodborne. Um app só, que compila o `libmain`/jogo em `.so`, guarda nos arquivos do app e carrega. Isso evita montar, assinar e instalar outro APK. O fluxo que o usuário pediu (gerar o APK "NieR Gestalt" com o ícone e desinstalar o Projeto Gestalt) continua possível por cima disso, se ele preferir.

Medido: 97 MB de C++ gerado (97 arquivos) e `libmain.so` de 121 MB. Compilar isso no celular deve levar de dezenas de minutos a mais de uma hora, com muita RAM e calor. É preciso medir num aparelho.

## Ordem sugerida antes do GitHub

1. Regenerar o patch do SDK.
2. Corrigir o `build.cmd` (Renut + extração de shaders) e testar o `Preparar.cmd` do zero.
3. Criar o script do Android a partir do fluxo acima.
4. Atualizar o README: hoje ele diz que o `Preparar.cmd` instala tudo, e isso só vale depois dos itens 1 e 2.
