// ===========================================================================
//  PC 端交叉验证：把 sunset.html 里的天文计算段抽出来跑，与 C++ 移植版逐点比对
//
//  用法（在 sunset 工程目录下）：
//    1) g++ -O2 -I../src -I../include tools/verify_astro.cpp src/astro.cpp -o tools/verify_astro.exe
//       （在 src/ 目录下执行，或用绝对路径）
//    2) node tools/compare_astro.mjs
//
//  HTML 是唯一数学参考源，本脚本不改动它，只读取并抽取 L360..L650 段落。
// ===========================================================================
import { readFileSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const proj = join(here, '..');
const HTML = join(proj, 'sunset.html');
const EXE = join(here, 'verify_astro.exe');

const CFG = { CFG_EPOCH_SEC: 1790251200, CFG_LAT: 39.9, CFG_LON: 116.4 };
const STEP_SEC = 600, N_STEP = 144;

// ---- 1. 从 sunset.html 抽取天文计算段（RAD 定义 → 天空配色之前）----
const src = readFileSync(HTML, 'utf8');
const s0 = src.indexOf('const RAD = Math.PI / 180;');
const s1mark = src.indexOf('7. 天空配色');
const s1 = src.lastIndexOf('/* =', s1mark);
if (s0 < 0 || s1 < 0) throw new Error('未能定位 sunset.html 的天文段落');
const block = src.slice(s0, s1);
const lineOf = i => src.slice(0, i).split('\n').length;
console.log(`[ref] 抽取 sunset.html L${lineOf(s0)}..L${lineOf(s1) - 1}（${block.length} 字符）`);

const ref = new Function(
  'CFG_LAT', 'CFG_LON',
  block + '\n; return { sunPosition, moonPosition, moonPhase, equatorialToAltAz, moonAltAz, STARS };'
)(CFG.CFG_LAT, CFG.CFG_LON);

// ---- 2. 跑 C++ 移植版 ----
const out = execFileSync(EXE, { encoding: 'utf8' }).trim().split(/\r?\n/);
const header = out.shift();
console.log(`[pc ] ${EXE} 输出 ${out.length} 行，表头：${header}`);

// ---- 3. 同一批输入跑参考实现 ----
const jdOf = sec => sec / 86400 + 2440587.5;
const expect = [];
for (let i = 0; i < N_STEP; i++) {
  const sec = CFG.CFG_EPOCH_SEC + i * STEP_SEC;
  const jd = jdOf(sec);
  const sun = ref.sunPosition(new Date(sec * 1000), CFG.CFG_LAT, CFG.CFG_LON);
  const mp  = ref.moonPosition(new Date(sec * 1000));
  const eq  = ref.equatorialToAltAz(mp.ra, mp.dec, CFG.CFG_LAT, CFG.CFG_LON, jd);
  const mo  = ref.moonAltAz(new Date(sec * 1000), CFG.CFG_LAT, CFG.CFG_LON);
  const ph  = ref.moonPhase(new Date(sec * 1000));

  const row = [sec, sun.alt, sun.az, mp.ra, mp.dec, eq.alt, eq.az, mo.alt, mo.az,
               ph.illumination, ph.phaseAngle, ph.waxing ? 1 : 0];
  for (let s = 0; s < 6; s++) {
    const sp = ref.equatorialToAltAz(ref.STARS[s][0], ref.STARS[s][1],
                                     CFG.CFG_LAT, CFG.CFG_LON, jd);
    row.push(sp.alt, sp.az);
  }
  expect.push(row);
}

// ---- 4. 逐列比对 ----
// 星表在 C++ 里是 float（省 RAM），星列单独放宽到 1e-3 度（≈0.002 px @240px/68°）
const TOL = new Array(header.split(',').length).fill(1e-9);
for (let c = 12; c < TOL.length; c++) TOL[c] = 1e-3;   // 列 12 起为星列

let worst = 0, worstAt = '', bad = 0;
out.forEach((line, r) => {
  const got = line.split(',').map(Number);
  const exp = expect[r];
  if (!exp || got.length !== exp.length) { console.log(`[!] 行 ${r} 列数不一致`); bad++; return; }
  for (let c = 1; c < got.length; c++) {
    if (c === 11) continue;                          // waxing：布尔，整数列
    const d = Math.abs(got[c] - exp[c]);
    if (d > worst) { worst = d; worstAt = `行${r} 列${c}(${header.split(',')[c]})`; }
    if (d > TOL[c]) {
      if (bad < 10) console.log(`[!] 行${r} ${header.split(',')[c]}: pc=${got[c]} ref=${exp[c]} Δ=${d.toExponential(3)}`);
      bad++;
    }
  }
});

console.log(`\n比对数：${out.length} 行 × ${TOL.length - 1} 数值列 = ${out.length * (TOL.length - 1)}`);
console.log(`最大偏差：${worst.toExponential(3)}  @ ${worstAt}`);
console.log(bad === 0 ? '结果：全部通过（超差列 0）' : `结果：超差 ${bad} 处`);
process.exit(bad === 0 ? 0 : 1);