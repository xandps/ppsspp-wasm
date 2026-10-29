// Núcleo com AUDIO=worklet (docs/p3-audio.md): o Emscripten 3.1.74 gera, além do ppsspp_libretro.js, o
// ppsspp_libretro.aw.js (processador do AudioWorklet) e o ppsspp_libretro.ww.js (bootstrap de Wasm Worker). O
// EmulatorJS 4.2.3 não sabe lidar com eles: usa como script principal o último .js do pacote (o .ww.js) e o .aw.js é
// pedido por caminho relativo fixo (`addModule("ppsspp_libretro.aw.js")`), que na página do jogo dá 404.
//
// Este passo embute o .aw.js no script principal (blob criado na hora) e apaga o .aw.js e o .ww.js: o pacote volta a
// ter um .js só, e o EmulatorJS carrega o núcleo sem mudança. O .ww.js só serve a emscripten_create_wasm_worker, que o
// RetroArch não usa (o worklet sobe pelo AudioWorkletNode e as threads são pthreads).
//
// Uso: node tools/ppsspp/inline-audioworklet.mjs <pasta com ppsspp_libretro.js e ppsspp_libretro.aw.js>
import fs from 'node:fs';
import path from 'node:path';

const dir = process.argv[2];
if (!dir) {
  console.error('Uso: node inline-audioworklet.mjs <pasta do núcleo>');
  process.exit(2);
}
const mainFile = path.join(dir, 'ppsspp_libretro.js');
const awFile = path.join(dir, 'ppsspp_libretro.aw.js');
const wwFile = path.join(dir, 'ppsspp_libretro.ww.js');

const main = fs.readFileSync(mainFile, 'utf8');
const aw = fs.readFileSync(awFile, 'utf8');
const needle = 'audioWorklet.addModule("ppsspp_libretro.aw.js")';
const count = main.split(needle).length - 1;
if (count !== 1) throw new Error(`esperava 1 ocorrência de ${needle}, achei ${count}`);
if (/ppsspp_libretro\.ww\.js/.test(main.replace(needle, ''))) {
  throw new Error('o script principal cita o .ww.js: ele é usado e não pode sair do pacote');
}

const blob = `URL.createObjectURL(new Blob([${JSON.stringify(aw)}],{type:"text/javascript"}))`;
fs.writeFileSync(mainFile, main.replace(needle, `audioWorklet.addModule(${blob})`));
fs.rmSync(awFile);
fs.rmSync(wwFile, { force: true });
console.log(`AudioWorklet embutido em ${mainFile} (${aw.length} bytes); .aw.js e .ww.js removidos`);
