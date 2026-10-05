// ===========================================================================
//  渲染层实现 —— 逐段移植自 sunset.html 的 L1006..L1366（第 12 / 13 / 14 节）
//
//  与 HTML 的差异（嵌入式侧的有意取舍，均已在本文件内注明）：
//    a) 天空逐像素反投影查 uint8 量化表（见 proj.cpp），颜色差 ≤1 级；
//    b) 天空层压成 RGB565 并加 4×4 有序抖动，避免大渐变出现色带；
//    c) 星点 / 月盘用「距离→覆盖率」与 3×3 超采样代替 canvas 抗锯齿，
//       保证 radius < 1 px 的暗星不会被整像素漏掉；
//    d) 月海 / 环形山的边缘抗锯齿由 3×3 超采样提供，绘制顺序与 HTML 一致。
// ===========================================================================
#include "render.h"
#include "sky.h"
#include "proj.h"

// ---- 月海暗斑（HTML L1221）----
static const float MOON_MARIA[7][5] = {
  { -0.34f, -0.38f, 0.30f, 0.24f, 0.20f },
  {  0.12f, -0.44f, 0.22f, 0.18f, 0.18f },
  {  0.36f, -0.12f, 0.20f, 0.26f, 0.16f },
  { -0.10f,  0.34f, 0.26f, 0.20f, 0.12f },
  {  0.30f,  0.42f, 0.16f, 0.14f, 0.10f },
  { -0.52f,  0.10f, 0.14f, 0.12f, 0.09f },
  {  0.02f, -0.04f, 0.18f, 0.16f, 0.08f },
};

#define N_CRATERS 90

// 环形山 / 亮斑（HTML L1232 确定性生成）
struct MoonPatch { float x, y, r, a; bool bright; };
static MoonPatch MOON_CRATERS[N_CRATERS];

// ---- 4×4 Bayer 抖动矩阵 ----
static const uint8_t BAYER4[4][4] = {
  {  0,  8,  2, 10 },
  { 12,  4, 14,  6 },
  {  3, 11,  1,  9 },
  { 15,  7, 13,  5 },
};

// 星点半径（像素）：HTML 原版 0.55 + intensity × 0.8，实机 240 屏上太小，整体放大一倍
#define STAR_RADIUS_MAGNIFY 2.0f
static inline float starRadius(float intensity) {
  return (0.55f + intensity * 0.8f) * STAR_RADIUS_MAGNIFY;
}

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

static inline float smoothstepf(float a, float b, float x) {
  const float t = clampf((x - a) / (b - a), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

// 888 → 565，带 4×4 有序抖动
// 要点：抖动与取整都在「编码域」做（×31/255 或 ×63/255），而不是 >>3 / >>2。
// 5 bit 的实际量化格是 255/31≈8.23，用 >>3 会引入系统性偏色（最大 1 格）；
// 再叠加「加满一格锯齿」的抖动，4×4 块内均值即严格等于原值。
static inline uint16_t pack565Dither(float r, float g, float b, int x, int y) {
  const float d = ((float)BAYER4[y & 3][x & 3] + 0.5f) * (1.0f / 16.0f);   // 0 .. 1
  int rc = (int)(r * (31.0f / 255.0f) + d);
  int gc = (int)(g * (63.0f / 255.0f) + d);
  int bc = (int)(b * (31.0f / 255.0f) + d);
  if (rc < 0) rc = 0; else if (rc > 31) rc = 31;
  if (gc < 0) gc = 0; else if (gc > 63) gc = 63;
  if (bc < 0) bc = 0; else if (bc > 31) bc = 31;
  return (uint16_t)((rc << 11) | (gc << 5) | bc);
}

// 888 → 565，纯四舍五入（不抖动）。
// 缓存天空层已经抖动过了，推屏时再来一次抖动会把已量化的值整体推高一档，
// 所以这里用普通取整——它对「565 → 888 → 565」是恒等的。
static inline uint16_t pack565Round(float r, float g, float b) {
  int rc = (int)(r * (31.0f / 255.0f) + 0.5f);
  int gc = (int)(g * (63.0f / 255.0f) + 0.5f);
  int bc = (int)(b * (31.0f / 255.0f) + 0.5f);
  if (rc < 0) rc = 0; else if (rc > 31) rc = 31;
  if (gc < 0) gc = 0; else if (gc > 63) gc = 63;
  if (bc < 0) bc = 0; else if (bc > 31) bc = 31;
  return (uint16_t)((rc << 11) | (gc << 5) | bc);
}

static inline void unpack565(uint16_t c, uint8_t *p) {
  const uint8_t r5 = (uint8_t)((c >> 11) & 0x1F);
  const uint8_t g6 = (uint8_t)((c >> 5) & 0x3F);
  const uint8_t b5 = (uint8_t)(c & 0x1F);
  p[0] = (uint8_t)((r5 << 3) | (r5 >> 2));
  p[1] = (uint8_t)((g6 << 2) | (g6 >> 4));
  p[2] = (uint8_t)((b5 << 3) | (b5 >> 2));
}

// 'lighter' 叠加（相当于 canvas globalCompositeOperation = lighter）
static inline void addPix(uint8_t *p, uint8_t r, uint8_t g, uint8_t b, float a) {
  if (a <= 0.0f) return;
  const float nr = p[0] + r * a, ng = p[1] + g * a, nb = p[2] + b * a;
  p[0] = nr > 255.0f ? 255 : (uint8_t)nr;
  p[1] = ng > 255.0f ? 255 : (uint8_t)ng;
  p[2] = nb > 255.0f ? 255 : (uint8_t)nb;
}

void renderInit(const SkyConfig *cfg) {
  projInit(cfg);

  // 环形山 / 亮斑：与 HTML 同一套 hash1
  for (int i = 0; i < N_CRATERS; i++) {
    const double h1 = hash1(i * 1.37 + 3.1);
    const double h2 = hash1(i * 2.71 + 9.4);
    const double h3 = hash1(i * 4.19 + 5.7);
    const double h4 = hash1(i * 7.31 + 1.9);
    const double rr = 0.90 * sqrt(h1);
    const double th = h2 * ASTRO_PI * 2;
    MOON_CRATERS[i].x = (float)(cos(th) * rr);
    MOON_CRATERS[i].y = (float)(sin(th) * rr);
    MOON_CRATERS[i].r = (float)(0.030 + h3 * 0.085);
    MOON_CRATERS[i].a = (float)(0.055 + h4 * 0.075);
    MOON_CRATERS[i].bright = h4 > 0.86;
  }
}

// ===========================================================================
//  整帧状态（HTML L1012..L1069 每帧量 + L1150..L1184 星表量 + L1252..L1294 月亮量）
// ===========================================================================
void frameStateCompute(FrameState *fs, const SkyConfig *cfg, double jd) {
  const SkyPos sun = sunPosition(jd, cfg->lat, cfg->lon);
  fs->sunAlt = sun.alt;
  fs->sunAz  = sun.az;

  // ---- 天空配色 ----
  double z[3], h[3], hCool[3];
  skyColors(sun.alt, z, h);
  skyHorizonOpp(sun.alt, hCool);

  const double azContrast = smoothstepd(45, 5, sun.alt);
  const double glowK      = smoothstepd(-8, 10, sun.alt);
  const double sunRed     = smoothstepd(12, -1, sun.alt);

  fs->zR = (float)z[0]; fs->zG = (float)z[1]; fs->zB = (float)z[2];

  fs->gG   = (float)lerpd(245, 130, sunRed);
  fs->gB   = (float)lerpd(228, 62, sunRed);
  fs->gAmp = (float)lerpd(0.42, 0.30, sunRed);

  // ---- 太阳圆盘 ----
  const SkyPt sunP = projectSky(cfg, sun.az, sun.alt);
  fs->sunSX = (float)sunP.x;
  fs->sunSY = (float)sunP.y;

  const double diskPx = fmax(MIN_BODY_RAD_PX, SUN_ANG_RAD * projPxPerDeg(cfg, sunP.t));
  fs->diskPx2 = (float)(diskPx * diskPx);

  const double glowRadiusPx = 45.0 / cfg->fov * RENDER_RES;
  fs->glowRadiusPx2 = (float)(glowRadiusPx * glowRadiusPx);
  fs->glowK    = (float)glowK;
  fs->diskVis  = (float)smoothstepd(-1.5, 0, sun.alt);

  fs->diskCenterR = 255.0f;
  fs->diskCenterG = (float)lerpd(255, 214, sunRed);
  fs->diskCenterB = (float)lerpd(255, 158, sunRed);
  fs->diskEdgeR   = 255.0f;
  fs->diskEdgeG   = (float)lerpd(250, 150, sunRed);
  fs->diskEdgeB   = (float)lerpd(240, 92,  sunRed);

  // 地平线色的方位混合：基准 + 系数 × cos(像素方位 − 太阳方位)
  const double mixHalf = 0.5 * azContrast;
  fs->aR = (float)(hCool[0] + (h[0] - hCool[0]) * 0.5);
  fs->aG = (float)(hCool[1] + (h[1] - hCool[1]) * 0.5);
  fs->aB = (float)(hCool[2] + (h[2] - hCool[2]) * 0.5);
  fs->bR = (float)((h[0] - hCool[0]) * mixHalf);
  fs->bG = (float)((h[1] - hCool[1]) * mixHalf);
  fs->bB = (float)((h[2] - hCool[2]) * mixHalf);

  // 逐像素反投影常量 + 方位混合常量（天空层每帧计算时复用）
  projBegin(cfg, &fs->proj);
  const double azDiff = (cfg->camAz - sun.az) * ASTRO_RAD;
  fs->azCosC = (float)cos(azDiff);
  fs->azSinS = (float)sin(azDiff);

  fs->sunPossible = (fs->sunSX > -glowRadiusPx && fs->sunSX < RENDER_RES + glowRadiusPx &&
                     fs->sunSY > -glowRadiusPx && fs->sunSY < RENDER_RES + glowRadiusPx);

  // ---- 星表 ----
  const double starVis = smoothstepd(-8, -16, sun.alt);
  const float  m = RENDER_RES * 0.05f;              // 边缘渐隐范围（像素）
  fs->nStars = 0;

  if (starVis >= 0.01) {
    for (int s = 0; s < N_STARS; s++) {
      const SkyPos pos = equatorialToAltAz(STARS[s][0], STARS[s][1], cfg->lat, cfg->lon, jd);
      if (pos.alt < 0) continue;

      const SkyPt pt = projectSky(cfg, pos.az, pos.alt);
      const float x = (float)pt.x, y = (float)pt.y;
      if (x < -m || x > RENDER_RES + m || y < -m || y > RENDER_RES + m) continue;

      const float edgeFade = fminf(
          fminf(smoothstepf(-m, m, x), smoothstepf(RENDER_RES + m, RENDER_RES - m, x)),
          fminf(smoothstepf(-m, m, y), smoothstepf(RENDER_RES + m, RENDER_RES - m, y)));
      const float horizonFade = (float)smoothstepd(0, 10, pos.alt);

      const float intensity = (float)magToIntensity(STARS[s][2]) * (float)starVis * horizonFade * edgeFade;
      if (intensity < 0.02f) continue;

      StarDraw &d = fs->stars[fs->nStars++];
      d.x = x;
      d.y = y;
      d.baseIntensity = intensity;
      d.freq      = (float)STAR_TWINKLE[s].freq;
      d.phase     = (float)STAR_TWINKLE[s].phase;
      d.depth     = (float)STAR_TWINKLE[s].depth;
      d.mag       = STARS[s][2];
      d.magFactor = (float)clampd((STARS[s][2] + 1.5) / 4.0 * 0.7 + 0.3, 0.3, 1.0);

      double rgb[3];
      bvToColor(STARS[s][3], rgb);
      d.r = (uint8_t)(rgb[0] + 0.5);
      d.g = (uint8_t)(rgb[1] + 0.5);
      d.b = (uint8_t)(rgb[2] + 0.5);

      d.alpha = intensity;      // 先给个值，frameTwinkle 每帧覆盖
      d.radius = starRadius(intensity);
      d.haloR = 0.0f;
      d.haloA = 0.0f;
    }
  }

  // ---- 月亮 ----
  MoonDraw &mo = fs->moon;
  mo.visible = false;

  const SkyPos mp = moonAltAz(jd, cfg->lat, cfg->lon);
  if (mp.alt >= -1) {
    const SkyPt pt = projectSky(cfg, mp.az, mp.alt);
    const float x = (float)pt.x, y = (float)pt.y;

    if (x >= -20.0f && x <= RENDER_RES + 20.0f && y >= -20.0f && y <= RENDER_RES + 20.0f) {
      const MoonPhase ph = moonPhase(jd);
      const float r = fmaxf((float)MIN_BODY_RAD_PX, (float)(MOON_ANG_RAD * projPxPerDeg(cfg, pt.t)));

      const float moonBrightness = powf((float)ph.illumination, 0.6f);
      const float skyBrightness  = (float)smoothstepd(-8, 35, sun.alt);
      const float visibility = clampf(moonBrightness / (skyBrightness * 2.2f + 0.06f), 0.0f, 1.0f);

      if (visibility >= 0.03f) {
        const float vis = visibility * (float)smoothstepd(-1, 5, mp.alt);
        if (vis >= 0.02f) {
          mo.visible = true;
          mo.x = x; mo.y = y; mo.r = r;
          mo.illum  = (float)ph.illumination;
          mo.waxing = ph.waxing;
          mo.term   = fmaxf(r * fabsf(1.0f - 2.0f * mo.illum), 0.01f);
          mo.vis    = vis;
          mo.detail = (r >= 5.0f);
          mo.haloR  = r * 5.0f;
          mo.haloA  = 0.16f * mo.illum * vis;
        }
      }
    }
  }
}

void frameTwinkle(FrameState *fs, double tSec, double twinkleAmt) {
  for (int i = 0; i < fs->nStars; i++) {
    StarDraw &s = fs->stars[i];
    const double flicker = sin(tSec * s.freq + s.phase) * 0.62 +
                           sin(tSec * s.freq * 1.83 + s.phase * 1.7) * 0.38;
    const double twinkle = 1.0 + flicker * s.depth * s.magFactor * twinkleAmt;
    const float intensity = (float)clampd(s.baseIntensity * twinkle, 0, 1.35);

    s.alpha  = intensity;
    s.radius = starRadius(intensity);
    if (s.mag < 0.5f && intensity > 0.6f) {
      s.haloR = s.radius * 3.5f;
      s.haloA = intensity * 0.12f;
    } else {
      s.haloR = 0.0f;
      s.haloA = 0.0f;
    }
  }
}

// ===========================================================================
//  ① 天空层（含太阳）—— HTML L1012..L1137
//  历史：这里曾用 240×240 的量化反投影表（112.5 KB）逐帧取色；现在天空每分钟
//  才画一次，改成逐像素现算（projPixelDir），省下的 RAM 正好放天空缓存帧。
// ===========================================================================
static inline void skyPixel(int x, int y, const FrameState *fs,
                            float *outR, float *outG, float *outB) {
  SkyDir d;
  projPixelDir(&fs->proj, x, y, &d);

  // cos(像素方位 − 太阳方位)：az = camAz + θ
  const float cm = d.ct * fs->azCosC - d.st * fs->azSinS;

  float cr = fs->aR + fs->bR * cm;
  float cg = fs->aG + fs->bG * cm;
  float cb = fs->aB + fs->bB * cm;

  const float kk = projKkFromSinAlt(d.sinAlt);
  cr += (fs->zR - cr) * kk;
  cg += (fs->zG - cg) * kk;
  cb += (fs->zB - cb) * kk;

  if (fs->sunPossible) {
    const float dxp = (x + 0.5f) - fs->sunSX;
    const float dyp = (y + 0.5f) - fs->sunSY;
    const float d2p = dxp * dxp + dyp * dyp;

    // 光晕
    if (fs->glowK > 0.003f && d2p < fs->glowRadiusPx2) {
      const float t = sqrtf(d2p / fs->glowRadiusPx2);
      const float u = 1.0f - t;
      const float glow = u * u * u * u * fs->glowK;
      if (glow > 0.001f) {
        const float amp = glow * fs->gAmp;
        cr += 255.0f * amp;
        cg += fs->gG * amp;
        cb += fs->gB * amp;
      }
    }

    if (cr > 255.0f) cr = 255.0f;
    if (cg > 255.0f) cg = 255.0f;
    if (cb > 255.0f) cb = 255.0f;

    // 圆盘：中心白热，边缘暖橙
    if (fs->diskVis > 0.005f && d2p < fs->diskPx2) {
      const float t = sqrtf(d2p / fs->diskPx2);   // 0 = 中心，1 = 边缘
      const float s = 1.0f - t * t;
      const float disk = s * s * fs->diskVis;
      const float centerW = sqrtf(1.0f - t * t);

      const float cR = fs->diskEdgeR + (fs->diskCenterR - fs->diskEdgeR) * centerW;
      const float cG = fs->diskEdgeG + (fs->diskCenterG - fs->diskEdgeG) * centerW;
      const float cB = fs->diskEdgeB + (fs->diskCenterB - fs->diskEdgeB) * centerW;

      cr += (cR - cr) * disk;
      cg += (cG - cg) * disk;
      cb += (cB - cb) * disk;
    }
  }

  if (cr < 0.0f) cr = 0.0f;
  if (cg < 0.0f) cg = 0.0f;
  if (cb < 0.0f) cb = 0.0f;
  *outR = cr;
  *outG = cg;
  *outB = cb;
}

void renderSkyRow(uint8_t *rgb, int y, const FrameState *fs) {
  for (int x = 0; x < RENDER_RES; x++) {
    float r, g, b;
    skyPixel(x, y, fs, &r, &g, &b);
    rgb[x * 3]     = (uint8_t)(r + 0.5f);
    rgb[x * 3 + 1] = (uint8_t)(g + 0.5f);
    rgb[x * 3 + 2] = (uint8_t)(b + 0.5f);
  }
}

// ===========================================================================
//  13. 星点（HTML L1143..L1213，'lighter' 叠加）
// ===========================================================================
static void drawStarRow(uint8_t *rgb, int y, const StarDraw *s) {
  const float R = fmaxf(s->radius, s->haloR);
  const float yc = y + 0.5f;
  if (yc < s->y - R - 1.0f || yc > s->y + R + 1.0f) return;

  int x0 = (int)floorf(s->x - R - 1.0f);
  int x1 = (int)ceilf(s->x + R + 1.0f);
  if (x0 < 0) x0 = 0;
  if (x1 > RENDER_RES - 1) x1 = RENDER_RES - 1;

  for (int x = x0; x <= x1; x++) {
    uint8_t *p = rgb + x * 3;
    const float dx = (x + 0.5f) - s->x;
    const float dy = yc - s->y;
    const float d = sqrtf(dx * dx + dy * dy);

    // 光晕（亮星才有）：内圈满强度 → 边缘线性归零
    if (s->haloR > 0.0f && d < s->haloR) {
      addPix(p, s->r, s->g, s->b, s->haloA * (1.0f - d / s->haloR));
    }

    // 星核：canvas 渐变在 0 / 0.55 / 1 三档间线性插值（alpha 1 → 0.8 → 0）
    const float cov = clampf(s->radius - d + 0.5f, 0.0f, 1.0f);
    if (cov > 0.0f) {
      const float t = clampf(d / s->radius, 0.0f, 1.0f);
      const float prof = (t <= 0.55f) ? (1.0f - 0.2f * (t / 0.55f))
                                      : (0.8f * (1.0f - (t - 0.55f) / 0.45f));
      addPix(p, s->r, s->g, s->b, s->alpha * prof * cov);
    }
  }
}

// ===========================================================================
//  14. 月亮（HTML L1252..L1366）
// ===========================================================================

// 相位明暗界线判定（局部坐标，已按 waxing 镜像）：
//   k ≥ 0.5 凸月 = 右半圆 + 左半椭圆；k < 0.5 残月 = 右半圆 − 右半椭圆
static inline bool moonLit(float X, float Y, float r, float term, float k) {
  const float d2 = X * X + Y * Y;
  if (k < 0.5f) {
    if (X < 0.0f) return false;
    if (d2 > r * r) return false;
    return (X * X) / (term * term) + (Y * Y) / (r * r) >= 1.0f;
  }
  if (X >= 0.0f) return d2 <= r * r;
  return (X * X) / (term * term) + (Y * Y) / (r * r) <= 1.0f;
}

// 月面底色：canvas 的 createRadialGradient((-0.25r,-0.25r), 0.15r) → (0,0, r)
// 三档停靠点 (255,252,238) → (245,240,222) → (215,210,195)
// 两圆不同心，按圆锥渐变的解析解求插值参数
static inline void moonBaseColor(float X, float Y, float r, float *cr, float *cg, float *cb) {
  const float qx = X + 0.25f * r, qy = Y + 0.25f * r;   // q = p − c0
  const float dcx = 0.25f * r, dcy = 0.25f * r;         // Δc
  const float r0 = 0.15f * r, dr = 0.85f * r;

  // |q − tΔc|² = (r0 + tΔr)²  →  A·t² + B·t + C = 0
  const float A = dcx * dcx + dcy * dcy - dr * dr;
  const float B = -2.0f * (qx * dcx + qy * dcy + r0 * dr);
  const float C = qx * qx + qy * qy - r0 * r0;

  float t = 1.0f;
  const float disc = B * B - 4.0f * A * C;
  if (disc >= 0.0f) {
    const float sq = sqrtf(disc);
    const float t1 = (-B - sq) / (2.0f * A);
    const float t2 = (-B + sq) / (2.0f * A);
    const float lo = fminf(t1, t2);
    t = clampf(lo >= 0.0f ? lo : fmaxf(t1, t2), 0.0f, 1.0f);
  }

  if (t <= 0.65f) {
    const float u = t / 0.65f;
    *cr = lerpf(255, 245, u); *cg = lerpf(252, 240, u); *cb = lerpf(238, 222, u);
  } else {
    const float u = (t - 0.65f) / 0.35f;
    *cr = lerpf(245, 215, u); *cg = lerpf(240, 210, u); *cb = lerpf(222, 195, u);
  }
}

static void drawMoonRow(uint8_t *rgb, int y, const MoonDraw *mn) {
  const float yc = y + 0.5f;
  if (yc < mn->y - mn->haloR - 1.0f || yc > mn->y + mn->haloR + 1.0f) return;

  int x0 = (int)floorf(mn->x - mn->haloR - 1.0f);
  int x1 = (int)ceilf(mn->x + mn->haloR + 1.0f);
  if (x0 < 0) x0 = 0;
  if (x1 > RENDER_RES - 1) x1 = RENDER_RES - 1;

  const float r = mn->r;
  const float inner = r * 0.5f;
  const float mirror = mn->waxing ? 1.0f : -1.0f;

  for (int x = x0; x <= x1; x++) {
    uint8_t *p = rgb + x * 3;
    const float px = (x + 0.5f) - mn->x;
    const float py = yc - mn->y;
    const float d = sqrtf(px * px + py * py);

    // 光晕：'lighter'，内圈 0.5r 起满强度，到 5r 线性归零（HTML L1292）
    if (d < mn->haloR) {
      const float g = (d <= inner) ? mn->haloA
                                   : mn->haloA * (1.0f - (d - inner) / (mn->haloR - inner));
      addPix(p, 210, 215, 235, g);
    }

    if (mn->illum <= 0.005f || d > r + 1.0f) continue;

    // 3×3 超采样：相位形状 + 月海 / 环形山共用一套覆盖率
    const float vis = mn->vis;
    float sumR = 0.0f, sumG = 0.0f, sumB = 0.0f;
    for (int sy = 0; sy < 3; sy++) {
      for (int sx = 0; sx < 3; sx++) {
        const float X = mirror * (px + (sx - 1) * (1.0f / 3.0f));
        const float Y = py + (sy - 1) * (1.0f / 3.0f);

        if (!moonLit(X, Y, r, mn->term, mn->illum)) {
          sumR += p[0]; sumG += p[1]; sumB += p[2];
          continue;
        }

        float sr, sg, sb;
        moonBaseColor(X, Y, r, &sr, &sg, &sb);
        sr = lerpf(p[0], sr, vis);
        sg = lerpf(p[1], sg, vis);
        sb = lerpf(p[2], sb, vis);

        if (mn->detail) {
          // 月海（HTML L1344）
          for (int i = 0; i < 7; i++) {
            const float ex = (X - MOON_MARIA[i][0] * r) / (MOON_MARIA[i][2] * r);
            const float ey = (Y - MOON_MARIA[i][1] * r) / (MOON_MARIA[i][3] * r);
            if (ex * ex + ey * ey <= 1.0f) {
              const float a = vis * MOON_MARIA[i][4];
              sr = lerpf(sr, 122, a); sg = lerpf(sg, 124, a); sb = lerpf(sb, 136, a);
            }
          }
          // 环形山 / 亮斑（HTML L1353）
          for (int i = 0; i < N_CRATERS; i++) {
            const float rr = fmaxf(MOON_CRATERS[i].r * r, 0.4f);
            const float ex = X - MOON_CRATERS[i].x * r;
            const float ey = Y - MOON_CRATERS[i].y * r;
            if (ex * ex + ey * ey <= rr * rr) {
              const float a = vis * MOON_CRATERS[i].a;
              if (MOON_CRATERS[i].bright) {
                sr = lerpf(sr, 255, a); sg = lerpf(sg, 255, a); sb = lerpf(sb, 250, a);
              } else {
                sr = lerpf(sr, 148, a); sg = lerpf(sg, 146, a); sb = lerpf(sb, 138, a);
              }
            }
          }
        }

        sumR += sr; sumG += sg; sumB += sb;
      }
    }

    p[0] = (uint8_t)(sumR * (1.0f / 9.0f) + 0.5f);
    p[1] = (uint8_t)(sumG * (1.0f / 9.0f) + 0.5f);
    p[2] = (uint8_t)(sumB * (1.0f / 9.0f) + 0.5f);
  }
}

// ===========================================================================
//  ① 背景层（天空 + 太阳 + 月亮）→ RGB565 缓存（含 4×4 有序抖动）
//  月亮不闪烁，所以和天空一起烘进背景；只有星星需要逐帧叠加。
//  每分钟重建一次，其余时间设备可以待机。
// ===========================================================================
void renderSky(uint16_t *sky565, const FrameState *fs) {
  static uint8_t row[RENDER_RES * 3];
  for (int y = 0; y < RENDER_RES; y++) {
    renderSkyRow(row, y, fs);        // 天空色 + 太阳
    renderMoonRow(row, y, fs);       // 月亮（背景的一部分）
    uint16_t *dst = sky565 + (size_t)y * RENDER_RES;
    for (int x = 0; x < RENDER_RES; x++) {
      dst[x] = pack565Dither(row[x * 3], row[x * 3 + 1], row[x * 3 + 2], x, y);
    }
  }
}

// ===========================================================================
//  ②③ 独立图层：星星（位置 / 大小 / 颜色 → 直接叠加）/ 月亮
// ===========================================================================
void renderStarRow(uint8_t *rgb, int y, const FrameState *fs) {
  for (int i = 0; i < fs->nStars; i++) drawStarRow(rgb, y, &fs->stars[i]);
}

void renderMoonRow(uint8_t *rgb, int y, const FrameState *fs) {
  if (fs->moon.visible) drawMoonRow(rgb, y, &fs->moon);
}

// ===========================================================================
//  合成一行：缓存背景 → 叠星（月亮已烘进背景，不逐帧画）
// ===========================================================================
void renderFrameRow(uint8_t *rgb, int y, const FrameState *fs, const uint16_t *sky565) {
  const uint16_t *src = sky565 + (size_t)y * RENDER_RES;
  for (int x = 0; x < RENDER_RES; x++) unpack565(src[x], rgb + x * 3);

  renderStarRow(rgb, y, fs);
}

// 同上，最后打包成 RGB565（普通取整：对已抖动的天空是恒等变换）
void renderFrameRow565(uint16_t *row565, int y, const FrameState *fs, const uint16_t *sky565) {
  static uint8_t row[RENDER_RES * 3];
  renderFrameRow(row, y, fs, sky565);
  for (int x = 0; x < RENDER_RES; x++) {
    row565[x] = pack565Round(row[x * 3], row[x * 3 + 1], row[x * 3 + 2]);
  }
}
