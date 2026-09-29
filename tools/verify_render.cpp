// ===========================================================================
//  PC 端渲染层自检 + 交叉验证数据导出（不参与固件编译）
//
//  编译（在 sunset 工程根目录）：
//    g++ -O2 -Isrc -Iinclude tools/verify_render.cpp src/astro.cpp src/sky.cpp
//        src/proj.cpp src/render.cpp -o tools/verify_render.exe
//
//  用法：
//    verify_render check        快速反投影 / 天空层 / 打包自检
//    verify_render csv          导出逐帧状态 CSV（供 node 端与 HTML 逐点比对）
//    verify_render stars <sec>  导出该时刻全部星点（位置 / 大小 / 颜色 / 闪烁）
//    verify_render ppm <sec> <out.ppm>            导出该时刻整帧图像
//    verify_render moon <sec> <half> <scale> <out.ppm>      月亮局部放大
//    verify_render moonshape <illum> <wax> <r> <scale> <out.ppm>  月相单元测试
//    verify_render pixel|x|y|sec / block|x|y|sec / skymap <sec>
// ===========================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "astro.h"
#include "sky.h"
#include "proj.h"
#include "render.h"
#include "settings.h"
#include "config.h"

// 自检 / 导出用的默认配置（取自 config.h）
static SkyConfig g_cfg;

// 天空层缓存（整帧大小与设备一致）
static uint16_t g_sky[RENDER_RES * RENDER_RES];

static double jdOf(double sec) { return sec / 86400.0 + 2440587.5; }

static void buildSky(FrameState *fs, double sec) {
  frameStateCompute(fs, &g_cfg, jdOf(sec));
  renderSky(g_sky, fs);
}

// ---------------------------------------------------------------------------
// 与渲染层独立的第二套天空计算（精确反投影 + double，不查表），用于对照
// ---------------------------------------------------------------------------
static void skyColorExact(int x, int y, const FrameState *fs, double rgb[3]) {
  double altDeg, azRad;
  projInversePixel(&g_cfg, x, y, &altDeg, &azRad);

  const double cm = cos(fs->sunAz * ASTRO_RAD - azRad);
  double cr = fs->aR + fs->bR * cm;
  double cg = fs->aG + fs->bG * cm;
  double cb = fs->aB + fs->bB * cm;

  double kk = 0;
  if (altDeg > 0) {
    const double t = clampd(altDeg, 0, 90) / 90;
    kk = 1 - pow(1 - t, 1.9);
  }
  cr += (fs->zR - cr) * kk;
  cg += (fs->zG - cg) * kk;
  cb += (fs->zB - cb) * kk;

  if (fs->sunPossible) {
    const double dxp = (x + 0.5) - fs->sunSX;
    const double dyp = (y + 0.5) - fs->sunSY;
    const double d2p = dxp * dxp + dyp * dyp;
    if (fs->glowK > 0.003 && d2p < fs->glowRadiusPx2) {
      const double t = sqrt(d2p / fs->glowRadiusPx2);
      const double u = 1 - t;
      const double glow = u * u * u * u * fs->glowK;
      if (glow > 0.001) {
        cr += 255.0 * glow * fs->gAmp;
        cg += fs->gG * glow * fs->gAmp;
        cb += fs->gB * glow * fs->gAmp;
      }
    }
    if (cr > 255) cr = 255;
    if (cg > 255) cg = 255;
    if (cb > 255) cb = 255;
    if (fs->diskVis > 0.005 && d2p < fs->diskPx2) {
      const double t = sqrt(d2p / fs->diskPx2);
      const double s = 1 - t * t;
      const double disk = s * s * fs->diskVis;
      const double cw = sqrt(1 - t * t);
      const double cR = fs->diskEdgeR + (fs->diskCenterR - fs->diskEdgeR) * cw;
      const double cG = fs->diskEdgeG + (fs->diskCenterG - fs->diskEdgeG) * cw;
      const double cB = fs->diskEdgeB + (fs->diskCenterB - fs->diskEdgeB) * cw;
      cr += (cR - cr) * disk;
      cg += (cG - cg) * disk;
      cb += (cB - cb) * disk;
    }
  }
  rgb[0] = clampd(cr, 0, 255);
  rgb[1] = clampd(cg, 0, 255);
  rgb[2] = clampd(cb, 0, 255);
}

// ===========================================================================
static int check(void) {
  FrameState fs;

  // ---- 1. 快速反投影（设备用）vs 精确反投影（double + asin/atan2）----
  ProjCtx ctx;
  projBegin(&g_cfg, &ctx);
  double maxAltErr = 0, maxAzErr = 0;
  for (int y = 0; y < PROJ_RES; y++) {
    for (int x = 0; x < PROJ_RES; x++) {
      SkyDir d;
      projPixelDir(&ctx, x, y, &d);

      double altDeg, azRad;
      projInversePixel(&g_cfg, x, y, &altDeg, &azRad);

      const double dAlt = fabs(d.sinAlt - sin(altDeg * ASTRO_RAD));
      if (dAlt > maxAltErr) maxAltErr = dAlt;

      const double azFast = g_cfg.camAz * ASTRO_RAD + atan2(d.st, d.ct);
      double dAz = fabs(azFast - azRad);
      dAz = fmin(dAz, 2 * ASTRO_PI - dAz) * ASTRO_DEG;
      if (dAz > maxAzErr) maxAzErr = dAz;
    }
  }
  printf("[1] 快速反投影 vs 精确反投影：方位角最大误差 %.4f°，sin(alt) 最大误差 %.2e\n",
         maxAzErr, maxAltErr);
  printf("    （无量化、无近似反三角，只差 float 精度；参考量级 0.01° / 1e-6）\n");
  if (maxAzErr > 0.05 || maxAltErr > 1e-5) { printf("    !! 误差偏大，快速路径可能写错\n"); return 1; }

  // ---- 2. 天空层：渲染层 vs 独立精确参考（都是 888，不含抖动）----
  double worst = 0, sum = 0;
  int worstX = 0, worstY = 0;
  long nPix = 0;
  const double secs[] = { 1790251200.0, 1790251200.0 + 6 * 3600, 1790251200.0 + 12 * 3600 };
  for (int k = 0; k < 3; k++) {
    frameStateCompute(&fs, &g_cfg, jdOf(secs[k]));
    for (int y = 0; y < RENDER_RES; y++) {
      uint8_t row[RENDER_RES * 3];
      renderSkyRow(row, y, &fs);
      for (int x = 0; x < RENDER_RES; x++) {
        double rgb[3];
        skyColorExact(x, y, &fs, rgb);
        for (int ch = 0; ch < 3; ch++) {
          const double d = fabs(row[x * 3 + ch] - rgb[ch]);
          if (d > worst) { worst = d; worstX = x; worstY = y; }
          sum += d;
          nPix++;
        }
      }
    }
  }
  printf("[2] 天空层 渲染路径 vs 精确参考（RGB888 纯数学）：\n");
  printf("    逐通道最大偏差 %.2f 级（@%d,%d），平均 %.4f 级\n", worst, worstX, worstY, sum / nPix);
  if (worst > 2.0) { printf("    !! 偏差过大，天空层算式可能写错\n"); return 1; }

  // ---- 2b. 缓存 + 打包：565 推屏结果 vs 888 参考 ----
  // 挑一帧没有星也没有月的（正午前后），此时只有天空，逐像素应当几乎恒等
  double packSec = 0;
  bool found = false;
  for (int i = 0; i < 144 && !found; i++) {
    const double s = 1790251200.0 + i * 600.0;
    frameStateCompute(&fs, &g_cfg, jdOf(s));
    if (fs.nStars == 0 && !fs.moon.visible) { packSec = s; found = true; }
  }
  if (found) {
    buildSky(&fs, packSec);
    double packWorst = 0, meanWorst = 0;
    for (int y = 0; y < RENDER_RES; y++) {
      uint8_t row[RENDER_RES * 3];
      uint16_t row565[RENDER_RES];
      renderFrameRow(row, y, &fs, g_sky);
      renderFrameRow565(row565, y, &fs, g_sky);
      for (int x = 0; x < RENDER_RES; x++) {
        const uint16_t c = row565[x];
        const int r5 = (c >> 11) & 0x1F, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
        const double got[3] = {
          (double)((r5 << 3) | (r5 >> 2)),
          (double)((g6 << 2) | (g6 >> 4)),
          (double)((b5 << 3) | (b5 >> 2)),
        };
        for (int ch = 0; ch < 3; ch++) {
          const double d = fabs(got[ch] - row[x * 3 + ch]);
          if (d > packWorst) packWorst = d;
        }
      }
    }
    (void)meanWorst;
    printf("[2b] 缓存 + 565 打包（无星无月帧 @%.0f）：逐像素最大偏差 %.2f 级\n",
           packSec, packWorst);
    printf("     （天空已抖动，推屏用普通取整，理论上应 ≤1 级）\n");
    if (packWorst > 1.5) { printf("    !! 打包不是恒等变换，检查 pack565Round\n"); return 1; }
  } else {
    printf("[2b] 24 h 内没有无星无月的帧，跳过打包检查\n");
  }

  // ---- 3. 状态量值域 ----
  int bad = 0;
  for (int i = 0; i < 144; i++) {
    const double sec = 1790251200.0 + i * 600.0;
    frameStateCompute(&fs, &g_cfg, jdOf(sec));
    if (!(fs.sunAlt >= -90 && fs.sunAlt <= 90)) { printf("    !! sunAlt 越界 %.2f\n", fs.sunAlt); bad++; }
    if (!(fs.sunAz >= 0 && fs.sunAz < 360)) { printf("    !! sunAz 越界 %.2f\n", fs.sunAz); bad++; }
    if (fs.nStars < 0 || fs.nStars > N_STARS) { printf("    !! nStars 越界 %d\n", fs.nStars); bad++; }
    for (int s = 0; s < fs.nStars; s++) {
      const StarDraw &st = fs.stars[s];
      if (!(st.baseIntensity >= 0.02f && st.baseIntensity <= 1.0f)) {
        printf("    !! 星强度越界 %.3f\n", st.baseIntensity); bad++;
      }
      if (!(st.x > -20 && st.x < RENDER_RES + 20 && st.y > -20 && st.y < RENDER_RES + 20)) {
        printf("    !! 星位置越界 (%.1f, %.1f)\n", st.x, st.y); bad++;
      }
    }
    if (fs.moon.visible && !(fs.moon.r >= MIN_BODY_RAD_PX - 0.001)) {
      printf("    !! 月亮半径越界 %.2f（下限 %.2f）\n", fs.moon.r, MIN_BODY_RAD_PX); bad++;
    }
  }
  printf("[3] 24 h 逐 10 min 值域检查：%s\n", bad ? "发现越界" : "全部通过");
  return bad ? 1 : 0;
}

// ===========================================================================
static uint8_t g_img[RENDER_RES][RENDER_RES][3];

static int writeZoom(const char *path, int x0, int y0, int n, int scale) {
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x0 + n > RENDER_RES) n = RENDER_RES - x0;
  if (y0 + n > RENDER_RES) n = RENDER_RES - y0;

  FILE *f = fopen(path, "wb");
  if (!f) { printf("!! 无法写 %s\n", path); return 1; }
  fprintf(f, "P6\n%d %d\n255\n", n * scale, n * scale);
  for (int y = 0; y < n * scale; y++) {
    for (int x = 0; x < n * scale; x++) {
      fwrite(g_img[y0 + y / scale][x0 + x / scale], 1, 3, f);
    }
  }
  fclose(f);
  printf("%s: 裁剪(%d,%d)+%d 放大%d×\n", path, x0, y0, n, scale);
  return 0;
}

// ===========================================================================
static void dumpPixel(int x, int y, double sec) {
  static FrameState fs;
  static uint16_t row565[RENDER_RES];
  frameStateCompute(&fs, &g_cfg, jdOf(sec));
  renderSky(g_sky, &fs);
  renderFrameRow565(row565, y, &fs, g_sky);

  double altDeg, azRad;
  projInversePixel(&g_cfg, x, y, &altDeg, &azRad);
  double rgb[3];
  skyColorExact(x, y, &fs, rgb);

  const uint16_t c = row565[x];
  printf("pixel(%d,%d) alt=%.4f°  az=%.4f rad\n", x, y, altDeg, azRad);
  printf("  精确 rgb = (%.3f, %.3f, %.3f)\n", rgb[0], rgb[1], rgb[2]);
  printf("  打包 565 = 0x%04X → r5=%d g6=%d b5=%d\n", c, (c >> 11) & 0x1F, (c >> 5) & 0x3F, c & 0x1F);
  printf("  a=(%.3f %.3f %.3f)  b=(%.4f %.4f %.4f)  z=(%.1f %.1f %.1f)  sunAlt=%.3f sunAz=%.3f\n",
         fs.aR, fs.aG, fs.aB, fs.bR, fs.bG, fs.bB, fs.zR, fs.zG, fs.zB, fs.sunAlt, fs.sunAz);
  printf("  sun=(%.2f, %.2f) glowR2=%.1f diskR2=%.2f diskVis=%.3f glowK=%.3f sunPossible=%d\n",
         fs.sunSX, fs.sunSY, fs.glowRadiusPx2, fs.diskPx2, fs.diskVis, fs.glowK, fs.sunPossible ? 1 : 0);
}

// ===========================================================================
static void dumpBlock(int bx, int by, double sec) {
  static FrameState fs;
  frameStateCompute(&fs, &g_cfg, jdOf(sec));
  renderSky(g_sky, &fs);

  printf("块(%d,%d) sec=%.0f  星=%d 月可见=%d  每格：精确(R,G,B) → 565(r5,g6,b5) → 反解(R,G,B)\n",
         bx, by, sec, fs.nStars, fs.moon.visible ? 1 : 0);
  double sumG[3] = {0, 0, 0}, sumE[3] = {0, 0, 0};
  for (int dy = 0; dy < 4; dy++) {
    uint16_t row565[RENDER_RES];
    renderFrameRow565(row565, by + dy, &fs, g_sky);
    for (int dx = 0; dx < 4; dx++) {
      const int x = bx + dx, y = by + dy;
      const uint16_t c = row565[x];
      const int r5 = (c >> 11) & 0x1F, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
      const int got[3] = { (r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2) };
      double rgb[3];
      skyColorExact(x, y, &fs, rgb);
      printf("  %3d,%3d: (%.1f,%.1f,%.1f) → (%2d,%2d,%2d) → (%3d,%3d,%3d)  Δ=(%+.1f,%+.1f,%+.1f)\n",
             x, y, rgb[0], rgb[1], rgb[2], r5, g6, b5, got[0], got[1], got[2],
             got[0] - rgb[0], got[1] - rgb[1], got[2] - rgb[2]);
      for (int ch = 0; ch < 3; ch++) { sumG[ch] += got[ch]; sumE[ch] += rgb[ch]; }
    }
  }
  printf("  块均值：Δ=(%+.2f,%+.2f,%+.2f)\n",
         (sumG[0] - sumE[0]) / 16, (sumG[1] - sumE[1]) / 16, (sumG[2] - sumE[2]) / 16);
}

// ===========================================================================
// 全星表普查：83 颗星的地平坐标与屏坐标，看在框内 / 被哪种渐隐吃掉
static void dumpSkyMap(double sec) {
  static FrameState fs;
  frameStateCompute(&fs, &g_cfg, jdOf(sec));
  const double jd = jdOf(sec);

  int nUp = 0, nIn = 0, nDrawn = 0;
  printf("sec=%.0f sunAlt=%.2f camAz=%.1f fov=%.1f\n", sec, fs.sunAlt, g_cfg.camAz, g_cfg.fov);
  printf("i, ra_h, dec, mag, alt, az, sx, sy, inFrame, drawn\n");
  for (int s = 0; s < N_STARS; s++) {
    const SkyPos pos = equatorialToAltAz(STARS[s][0], STARS[s][1], g_cfg.lat, g_cfg.lon, jd);
    const SkyPt pt = projectSky(&g_cfg, pos.az, pos.alt);
    const bool up = pos.alt >= 0;
    const bool in = pt.x >= 0 && pt.x <= RENDER_RES && pt.y >= 0 && pt.y <= RENDER_RES;
    bool drawn = false;
    for (int k = 0; k < fs.nStars; k++) {
      if (fabs(fs.stars[k].x - (float)pt.x) < 1e-3 && fabs(fs.stars[k].y - (float)pt.y) < 1e-3) { drawn = true; break; }
    }
    if (up) nUp++;
    if (in) nIn++;
    if (drawn) nDrawn++;
    if (up && in) {
      printf("%d, %.3f, %+.2f, %.2f, %+.1f, %.1f, %.1f, %.1f, %d, %d\n",
             s, STARS[s][0], STARS[s][1], STARS[s][2], pos.alt, pos.az, pt.x, pt.y, in ? 1 : 0, drawn ? 1 : 0);
    }
  }
  printf("合计：地平以上 %d 颗，落在画面内 %d 颗，最终绘制 %d 颗\n", nUp, nIn, nDrawn);
}

// ===========================================================================
static void dumpCsv(void) {
  FrameState fs;
  printf("sec,sunAlt,sunAz,zR,zG,zB,aR,aG,aB,bR,bG,bB,sunSX,sunSY,diskPx,glowK,gG,gB,gAmp,diskVis,"
         "dcR,dcG,dcB,deR,deG,deB,moonX,moonY,moonR,illum,waxing,moonVis,moonHaloA,nStars");
  for (int s = 0; s < 6; s++) printf(",s%dX,s%dY,s%dI,s%dR,s%dG,s%dB", s, s, s, s, s, s);
  printf("\n");

  for (int i = 0; i < 144; i++) {
    const double sec = 1790251200.0 + i * 600.0;
    frameStateCompute(&fs, &g_cfg, jdOf(sec));
    printf("%.0f,%.10f,%.10f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,"
           "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.10f,%d,%.6f,%.6f,%d",
           sec, fs.sunAlt, fs.sunAz, fs.zR, fs.zG, fs.zB, fs.aR, fs.aG, fs.aB, fs.bR, fs.bG, fs.bB,
           fs.sunSX, fs.sunSY, sqrtf(fs.diskPx2), fs.glowK, fs.gG, fs.gB, fs.gAmp,
           fs.diskVis, fs.diskCenterR, fs.diskCenterG, fs.diskCenterB,
           fs.diskEdgeR, fs.diskEdgeG, fs.diskEdgeB,
           fs.moon.x, fs.moon.y, fs.moon.r, fs.moon.illum, fs.moon.waxing ? 1 : 0,
           fs.moon.vis, fs.moon.haloA, fs.nStars);
    for (int s = 0; s < 6; s++) {
      if (s < fs.nStars) {
        printf(",%.6f,%.6f,%.6f,%d,%d,%d", fs.stars[s].x, fs.stars[s].y, fs.stars[s].baseIntensity,
               fs.stars[s].r, fs.stars[s].g, fs.stars[s].b);
      } else {
        printf(",,,,,");
      }
    }
    printf("\n");
  }
}

// ===========================================================================
static void dumpStars(double sec) {
  FrameState fs;
  frameStateCompute(&fs, &g_cfg, jdOf(sec));
  frameTwinkle(&fs, 3.7, g_cfg.twinkle);      // 与 node 端同一采样时刻

  printf("sec=%.0f nStars=%d  (alpha/radius 取 t=3.7 s)\n", sec, fs.nStars);
  printf("i,x,y,baseI,alpha,radius,haloR,haloA,r,g,b\n");
  for (int i = 0; i < fs.nStars; i++) {
    const StarDraw &s = fs.stars[i];
    printf("%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%d,%d,%d\n",
           i, s.x, s.y, s.baseIntensity, s.alpha, s.radius, s.haloR, s.haloA, s.r, s.g, s.b);
  }
}

// ===========================================================================
static int dumpPpm(double sec, const char *path) {
  static FrameState fs;
  static uint8_t row[RENDER_RES * 3];

  buildSky(&fs, sec);
  frameTwinkle(&fs, 3.7, g_cfg.twinkle);

  FILE *f = fopen(path, "wb");
  if (!f) { printf("!! 无法写 %s\n", path); return 1; }
  fprintf(f, "P6\n%d %d\n255\n", RENDER_RES, RENDER_RES);
  for (int y = 0; y < RENDER_RES; y++) {
    renderFrameRow(row, y, &fs, g_sky);
    fwrite(row, 1, RENDER_RES * 3, f);
  }
  fclose(f);

  printf("%s: sun alt=%.2f az=%.2f | moon vis=%d r=%.2f illum=%.2f %s | stars=%d\n",
         path, fs.sunAlt, fs.sunAz, fs.moon.visible ? 1 : 0, fs.moon.r, fs.moon.illum,
         fs.moon.waxing ? "waxing" : "waning", fs.nStars);
  return 0;
}

// ===========================================================================
// 月亮局部放大导出（最近邻）：用来看相位形状 / 月海 / 环形山
static int dumpZoom(double sec, int half, int scale, const char *path) {
  static FrameState fs;
  static uint8_t row[RENDER_RES * 3];
  buildSky(&fs, sec);
  if (!fs.moon.visible) { printf("该时刻月亮不可见（vis=0）\n"); return 1; }
  frameTwinkle(&fs, 3.7, g_cfg.twinkle);

  for (int y = 0; y < RENDER_RES; y++) {
    renderFrameRow(row, y, &fs, g_sky);
    memcpy(g_img[y], row, RENDER_RES * 3);
  }

  printf("月亮 @(%.2f,%.2f) r=%.2f illum=%.2f %s detail=%d vis=%.2f\n",
         fs.moon.x, fs.moon.y, fs.moon.r, fs.moon.illum,
         fs.moon.waxing ? "waxing" : "waning", fs.moon.detail ? 1 : 0, fs.moon.vis);
  const SkyPos mp = moonAltAz(jdOf(sec), g_cfg.lat, g_cfg.lon);
  printf("    月亮地平坐标：alt=%.2f° az=%.2f°（相机 az=%.1f fov=%.1f）\n",
         mp.alt, mp.az, g_cfg.camAz, g_cfg.fov);
  return writeZoom(path, (int)fs.moon.x - half, (int)fs.moon.y - half, half * 2, scale);
}

// ===========================================================================
// 月相形状单元测试：合成 illum / waxing / r，只画圆盘（黑底）
static int dumpMoonShape(double illum, int waxing, double r, int scale, const char *path) {
  static FrameState fs;
  static uint8_t row[RENDER_RES * 3];

  memset(&fs, 0, sizeof(fs));
  projBegin(&g_cfg, &fs.proj);          // 天空算式要有效常量（此处配色全 0，天空为纯黑）

  fs.moon.visible = true;
  fs.moon.x = 60.0f;
  fs.moon.y = 60.0f;
  fs.moon.r = (float)r;
  fs.moon.illum = (float)illum;
  fs.moon.waxing = waxing != 0;
  fs.moon.term = fmaxf((float)r * (float)fabs(1.0 - 2.0 * illum), 0.01f);
  fs.moon.vis = 1.0f;
  fs.moon.detail = (r >= 5.0);
  fs.moon.haloR = (float)r * 5.0f;
  fs.moon.haloA = 0.16f * (float)illum;
  fs.nStars = 0;

  renderSky(g_sky, &fs);                // 背景 = 黑天空 + 月亮
  for (int y = 0; y < RENDER_RES; y++) {
    renderFrameRow(row, y, &fs, g_sky);
    memcpy(g_img[y], row, RENDER_RES * 3);
  }
  printf("合成月亮：illum=%.2f %s r=%.1f detail=%d\n",
         illum, waxing ? "waxing" : "waning", r, fs.moon.detail ? 1 : 0);
  return writeZoom(path, 0, 0, 120, scale);
}

// ===========================================================================
int main(int argc, char **argv) {
  astroInit();
  skyConfigDefaults(&g_cfg);
  skyConfigClamp(&g_cfg);
  renderInit(&g_cfg);

  const char *mode = argc > 1 ? argv[1] : "check";

  if (strcmp(mode, "check") == 0) return check();
  if (strcmp(mode, "pixel") == 0 && argc > 4) { dumpPixel(atoi(argv[2]), atoi(argv[3]), atof(argv[4])); return 0; }
  if (strcmp(mode, "block") == 0 && argc > 4) { dumpBlock(atoi(argv[2]), atoi(argv[3]), atof(argv[4])); return 0; }
  if (strcmp(mode, "skymap") == 0 && argc > 2) { dumpSkyMap(atof(argv[2])); return 0; }
  if (strcmp(mode, "csv") == 0) { dumpCsv(); return 0; }
  if (strcmp(mode, "stars") == 0 && argc > 2) { dumpStars(atof(argv[2])); return 0; }
  if (strcmp(mode, "ppm") == 0 && argc > 3) return dumpPpm(atof(argv[2]), argv[3]);
  if (strcmp(mode, "moon") == 0 && argc > 5) { return dumpZoom(atof(argv[2]), atoi(argv[3]), atoi(argv[4]), argv[5]); }
  if (strcmp(mode, "moonshape") == 0 && argc > 6) {
    return dumpMoonShape(atof(argv[2]), atoi(argv[3]), atof(argv[4]), atoi(argv[5]), argv[6]);
  }

  printf("用法：verify_render check | csv | stars <sec> | ppm <sec> <out.ppm> | "
         "moon <sec> <half> <scale> <out.ppm> | moonshape <illum> <wax> <r> <scale> <out.ppm>\n");
  return 1;
}
