# Núcleo PSP do Arcavive

Este repositório é o código-fonte do núcleo PSP usado pelo Arcavive (`ppsspp-thread-wasm.data`, servido na página
Jogar PSP). É um fork do [PPSSPP](https://github.com/hrydgard/ppsspp) (GPL-2.0 ou posterior, `LICENSE.TXT`) a partir
do port libretro mantido pelo [EmulatorJS](https://github.com/EmulatorJS/ppsspp), ligado ao RetroArch do EmulatorJS
(GPL-3.0) e carregado pelo [EmulatorJS 4.2.3](https://github.com/EmulatorJS/EmulatorJS/tree/v4.2.3) (GPL-3.0).

## Mudanças deste fork (branch `wasm`)

- `d8ab69199f` IR interpreter: sem intrínsecos SSE no Emscripten (os lutadores sumiam no Def Jam).
- `f7dcd094cc` libretro: contadores de vblank e flip para medir o desempenho no navegador.
- `8f6c05930b` JIT IR → WebAssembly para o build Emscripten (`Core/MIPS/IR/IRWasmJit.cpp`).

O núcleo publicado no Arcavive foi compilado do commit `8f6c05930b425b303e78b7057485b2745dca5193`.

## Como foi compilado

- RetroArch do EmulatorJS: [`EmulatorJS/RetroArch@6dd4353937`](https://github.com/EmulatorJS/RetroArch/tree/6dd4353937ef48b6ec0bfbdbb15d1c5992d86927)
  com `patches/retroarch-audioworklet.patch` (driver audioworklet: trava do buffer sem `performance.now()` no
  worklet e volume do EmulatorJS) e os ajustes de Makefile que `build-windows.sh` aplica.
- Emscripten 3.1.74, no Windows pelo Git Bash:

  ```bash
  AUDIO=worklet ASYNC_MODE=min JOBS=6 bash arcavive/build-windows.sh <este repositório> <RetroArch do EmulatorJS> <pasta de saída>
  ```

- `inline-audioworklet.mjs` embute o `.aw.js` do audioworklet no script principal (o EmulatorJS 4.2.3 aceita um só
  `.js` no pacote). `asyncify-remove.json` só vale com `ASYNC_MODE=full` (não usado no núcleo publicado).

Pré-requisitos e detalhes estão nos comentários do início de `build-windows.sh`.
