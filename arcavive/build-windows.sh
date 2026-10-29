#!/bin/bash
# Build do núcleo PPSSPP do EmulatorJS (ppsspp-thread-wasm.data) direto no Windows, pelo Git Bash.
# Reproduz o fluxo oficial (EmulatorJS/build, cores.json → ppsspp) sem Linux, WSL ou Docker.
#
# Uso: [PROFILING=1] [AUDIO=al|worklet] [ASYNC_MODE=full|min] [SKIP_CORE=1] tools/ppsspp/build-windows.sh <clone do ppsspp-wasm> <clone do EmulatorJS/RetroArch> <pasta de saída>
#
#   AUDIO: driver de áudio do RetroArch. al (padrão do EmulatorJS) é o OpenAL do Emscripten, que roda na thread
#     principal e falha quando ela atrasa; worklet é o audioworklet (thread de áudio própria, buffer compartilhado)
#     e dispensa o Asyncify completo (o HAVE_AL o força). Ver docs/p3-audio.md.
#   ASYNC_MODE: Asyncify do RetroArch. full (padrão do EmulatorJS) instrumenta quase todo o wasm, inclusive o
#     interpretador da CPU; min instrumenta só o laço principal (MIN_ASYNC do Makefile.emulatorjs). Ver docs/p2-desempenho.md.
#   SKIP_CORE=1: não recompila o núcleo; só religa o .bc do último build (para testar opções de ligação).
#
# Pré-requisitos (ver docs/ppsspp-build.md):
#   - emsdk 3.1.74 em ~/tools/emsdk-3.1.74 (o mesmo do EmulatorJS/build; separado do 6.0.13 do arcavive-psp)
#   - ~/tools/bin/make.exe (mingw32-make do emsdk) e ~/tools/bin/7z.exe (7za do pacote npm 7zip-bin)
#   - clones com core.autocrlf=false (os scripts bash quebram com CRLF)
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PPSSPP=$(cd "$1" && pwd)
RETROARCH=$(cd "$2" && pwd)
mkdir -p "$3"
OUT=$(cd "$3" && pwd)
if [ -n "${ASYNCIFY_REMOVE:-}" ]; then ASYNCIFY_REMOVE=$(cd "$(dirname "$ASYNCIFY_REMOVE")" && pwd)/$(basename "$ASYNCIFY_REMOVE"); fi
JOBS=${JOBS:-$(nproc)}  # JOBS=8 se o make falhar com Error -1073741502 (0xC0000142: processos demais)

export EMSDK="$HOME/tools/emsdk-3.1.74"
export EM_CONFIG="$EMSDK/.emscripten"
EMPY_DIR=$(ls -d "$EMSDK"/python/*/ | head -1)
export EMSDK_PYTHON="${EMPY_DIR}python.exe"
export PATH="$HOME/tools/bin:$EMSDK:$EMSDK/upstream/emscripten:$EMPY_DIR:$PATH"
log() { echo "[build] $*"; }
# Subm�dulos clonados aqui herdam o core.autocrlf=true do Git for Windows; for�a LF e caminhos longos.
export GIT_CONFIG_COUNT=2 GIT_CONFIG_KEY_0=core.autocrlf GIT_CONFIG_VALUE_0=false GIT_CONFIG_KEY_1=core.longpaths GIT_CONFIG_VALUE_1=true

log "ppsspp $(git -C "$PPSSPP" log -1 --format='%h %s')"
log "retroarch $(git -C "$RETROARCH" log -1 --format='%h %s')"

if [ "${SKIP_CORE:-0}" != 1 ]; then
# 1. Submódulos limpos (os ajustes abaixo sujam alguns; voltam ao commit fixado a cada build).
cd "$PPSSPP"
# Limpa antes também: um submódulo sujo pelo build anterior impede o update de trocar de commit (busca binária).
git submodule foreach --quiet --recursive 'git checkout -q -- . 2>/dev/null || true'
git submodule update --init --recursive -j8
git submodule foreach --quiet --recursive 'git checkout -q -- . 2>/dev/null || true'

# 2. ffmpeg para wasm32. O emconfigure (Python) não executa ./configure direto no Windows: passa pelo sh.
#    Só recompila se o commit do submódulo mudou.
FFMPEG_REV=$(git -C ffmpeg rev-parse HEAD)
if [ "$(cat ffmpeg/linux/wasm32/.rev 2>/dev/null || true)" != "$FFMPEG_REV" ]; then
  log "ffmpeg $FFMPEG_REV"
  (cd ffmpeg && sed 's|^emconfigure ./configure|emconfigure sh ./configure|' ../libretro/emscripten/linux_wasm32.sh > linux_wasm32.sh \
    && bash linux_wasm32.sh > "$OUT/ffmpeg.log" 2>&1)
  echo "$FFMPEG_REV" > ffmpeg/linux/wasm32/.rev
else
  log "ffmpeg já compilado ($FFMPEG_REV)"
fi

# 3. Os mesmos ajustes do libretro/emscripten/build_emscripten.sh.
sed -i 's|return getauxval(hwcap_type);|return 1;|g' ext/cpu_features/src/hwcaps.c
sed -i 's|#include <sys/auxv.h>||g' ext/cpu_features/src/hwcaps.c
if [ -f ext/aemu_postoffice/client/sock_impl_linux.c ]; then
  sed -i 's|#ifndef __linux__|#ifdef SO_NOSIGPIPE|g' ext/aemu_postoffice/client/sock_impl_linux.c
fi
(cd ext/armips/ext/filesystem && git fetch -q https://github.com/gulrak/filesystem.git HEAD && git checkout -q FETCH_HEAD)

# 4. Núcleo libretro (.bc). O emar final estoura o limite de 8 KB do cmd: refeito com arquivo de resposta.
cd libretro
rm -f ppsspp_libretro_emscripten.bc
emmake make platform=emscripten clean > /dev/null 2>&1 || true
log "make platform=emscripten -j$JOBS"
emmake make platform=emscripten -j"$JOBS" > "$OUT/libretro.log" 2>&1 || true
if [ ! -f ppsspp_libretro_emscripten.bc ]; then
  grep -a "rcs ppsspp_libretro_emscripten.bc " "$OUT/libretro.log" | tail -1 | tr -d '\r' \
    | sed 's/.*ppsspp_libretro_emscripten\.bc //' | tr ' ' '\n' | grep -v '^$' > objs.rsp
  [ -s objs.rsp ] || { log "falha na compilação, ver $OUT/libretro.log"; exit 1; }
  emar rcs ppsspp_libretro_emscripten.bc @objs.rsp
fi
mkdir -p emscripten/build
cp ppsspp_libretro_emscripten.bc emscripten/build/
cp ../ffmpeg/linux/wasm32/lib/*.a emscripten/build/
else
  log "núcleo do build anterior (SKIP_CORE=1)"
  cd "$PPSSPP/libretro"
  [ -f emscripten/build/ppsspp_libretro_emscripten.bc ] || { log "não há .bc anterior em libretro/emscripten/build"; exit 1; }
fi

# 5. Ligação no RetroArch do EmulatorJS. Ajustes para o Git Bash, sem mudar o resultado:
#    UNAME=Linux (senão o Makefile.common entra no bloco MSYS2), CC/LD como os scripts sh do Emscripten
#    (o caminho do emcc.bat chega com barras invertidas ao sh) e a lista de objetos por arquivo de resposta.
rm -rf "$RETROARCH/ffmpeg" && cp -r emscripten/build "$RETROARCH/ffmpeg"
cp emscripten/build/*.bc "$RETROARCH/emulatorjs/"
cd "$RETROARCH"
python - <<'EOF'
p = 'Makefile.emulatorjs'
s = open(p, newline='').read()
old = '\t$(Q)$(LD) -o $@ $(RARCH_OBJ) $(libretro_new)'
if old in s:
    s = s.replace(old, '\t$(file >$@.rsp,$(RARCH_OBJ))\n\t$(Q)$(LD) -o $@ @$@.rsp $(libretro_new)')
if 'EXTRA_LDFLAGS' not in s:
    s += '\n# build-windows.sh: flags extras de ligação (ex.: --profiling-funcs)\nLDFLAGS += $(EXTRA_LDFLAGS)\n'
open(p, 'w', newline='').write(s)
# ASYNC_MODE: o build-emulatorjs.sh fixa async=1 para todo núcleo; aqui vira variável de ambiente.
p = 'emulatorjs/build-emulatorjs.sh'
s = open(p, newline='').read()
s = s.replace('  async=1\r\n', '  async=${EJS_ASYNC:-1}\r\n').replace('  async=1\n', '  async=${EJS_ASYNC:-1}\n')
open(p, 'w', newline='').write(s)
EOF
rm -f ../EmulatorJS/data/cores/ppsspp-thread-wasm.data
# PROFILING=1: mantém os nomes das funções no wasm (--profiling-funcs) para o perfil do Chrome; o código é o mesmo.
# A tabela de funções cresce: o JIT IR -> WebAssembly (Core/MIPS/IR/IRWasmJit.cpp) instala as funções geradas nela.
EXTRA_LDFLAGS="-sALLOW_TABLE_GROWTH=1"
if [ "${PROFILING:-0}" = 1 ]; then EXTRA_LDFLAGS="$EXTRA_LDFLAGS --profiling-funcs"; log "com nomes de funções (--profiling-funcs)"; fi
# ASYNCIFY_REMOVE=<arquivo .json>: funções que o Asyncify completo não instrumenta. Só vale para código que nunca está
# na pilha da thread principal quando ela dorme (emscripten_sleep): a CPU e a GPU emuladas, que rodam na EmuThread.
if [ -n "${ASYNCIFY_REMOVE:-}" ]; then
  EXTRA_LDFLAGS="$EXTRA_LDFLAGS -sASYNCIFY_REMOVE=@$(cygpath -m "$ASYNCIFY_REMOVE")"
  log "Asyncify sem: $(tr -d '\n ' < "$ASYNCIFY_REMOVE")"
fi
AUDIO_FLAGS=""
case "${AUDIO:-al}" in
  al) ;;
  worklet)
    AUDIO_FLAGS="HAVE_AL=0 HAVE_AUDIOWORKLET=1 HAVE_THREADS=1"  # o make clean também precisa das threads
    # Driver audioworklet: trava do buffer sem performance.now() no worklet e volume do EmulatorJS (docs/p3-audio.md).
    PATCH="$SCRIPT_DIR/patches/retroarch-audioworklet.patch"
    if git -C "$RETROARCH" apply --reverse --check "$PATCH" 2>/dev/null; then log "patch do audioworklet já aplicado"
    else git -C "$RETROARCH" apply "$PATCH"; log "patch do audioworklet aplicado"; fi ;;
  *) log "AUDIO inválido: $AUDIO (al|worklet)"; exit 1 ;;
esac
ASYNC_FLAGS=""
EJS_ASYNC=1
case "${ASYNC_MODE:-full}" in
  full) ;;
  min) EJS_ASYNC=0; ASYNC_FLAGS="MIN_ASYNC=1" ;;
  *) log "ASYNC_MODE inválido: $ASYNC_MODE (full|min)"; exit 1 ;;
esac
log "build-emulatorjs.sh --clean --threads (Asyncify ${ASYNC_MODE:-full}, áudio ${AUDIO:-al})"
(cd emulatorjs && EJS_ASYNC=$EJS_ASYNC EXTRA_LDFLAGS="$EXTRA_LDFLAGS" MAKEFLAGS="UNAME=Linux CC=emcc CXX=em++ LD=emcc AR=emar $ASYNC_FLAGS $AUDIO_FLAGS" emmake bash ./build-emulatorjs.sh --clean --threads) \
  > "$OUT/retroarch.log" 2>&1
DATA="$RETROARCH/../EmulatorJS/data/cores/ppsspp-thread-wasm.data"
[ -f "$DATA" ] || { log "falha na ligação, ver $OUT/retroarch.log"; exit 1; }

# 6. Metadados como no pacote oficial (core.json e build.json do @emulatorjs/core-ppsspp 4.2.3).
if [ "${AUDIO:-al}" = worklet ]; then
  # O EmulatorJS 4.2.3 só aceita um .js no pacote: o .aw.js vai embutido no principal (inline-audioworklet.mjs).
  PACK=$(mktemp -d)
  (cd "$PACK" && 7z x "$DATA" > /dev/null)
  node "$SCRIPT_DIR/inline-audioworklet.mjs" "$PACK"
  rm -f "$OUT/ppsspp-thread-wasm.data"
  (cd "$PACK" && 7z a -t7z "$OUT/ppsspp-thread-wasm.data" ./* > /dev/null)
  rm -rf "$PACK"
else
  cp "$DATA" "$OUT/ppsspp-thread-wasm.data"
fi
META=$(mktemp -d)
cp "$PPSSPP/LICENSE.TXT" "$META/license.txt"
cat > "$META/core.json" <<'EOF'
{"name":"ppsspp","extensions":["elf","iso","cso","prx","pbp"],"options":{"defaultWebGL2":true,"requireThreads":true},"save":false,"license":"LICENSE.TXT","repo":"https://github.com/xandps/ppsspp-wasm"}
EOF
printf '{\n    "minimumEJSVersion": "4.2.2",\n    "version": "2.0.2"\n}\n' > "$META/build.json"
(cd "$META" && 7z a -t7z "$OUT/ppsspp-thread-wasm.data" core.json license.txt build.json > /dev/null)
rm -rf "$META"
git -C "$PPSSPP" log -1 --format='ppsspp %H%nretroarch '"$(git -C "$RETROARCH" rev-parse HEAD)"'%nemscripten 3.1.74%nasyncify '"${ASYNC_MODE:-full}"'%naudio '"${AUDIO:-al}"'%ndata %cI' > "$OUT/versions.txt"
log "pronto: $OUT/ppsspp-thread-wasm.data ($(du -h "$OUT/ppsspp-thread-wasm.data" | cut -f1))"
