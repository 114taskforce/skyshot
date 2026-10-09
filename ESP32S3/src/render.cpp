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
#include <math.h>

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

// 抖动噪声：interleaved gradient noise（Jimenez 2014），逐像素 0..1
// 选它而不是 4×4 Bayer：Bayer 是有结构的图案，夜空那种「整屏只跨 4~5 个
// 6 位台阶」的暗渐变会显出「几种纹理不同的宽条带」；无规则噪声的纹理处处
// 均匀，只剩细颗粒，台阶过渡看不出来。纯 (x,y) 函数 → 静止画面不闪。
static inline float ditherNoise(int x, int y) {
  const float f = 0.06711056f * (float)x + 0.00583715f * (float)y;
  const float g = f - (float)(int)f;                 // fract
  const float h = 52.9829189f * g;
  return h - (float)(int)h;                           // fract
}

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

// ---------------------------------------------------------------------------
//  背景缓存：18 位色（6-6-6），按位打包、不抖动 —— 屏幕本身就是 18 位
//  （ST7789 COLMOD=0x66），6 位量化格 255/63≈4.05 级，天渐变足够平滑，
//  再叠加抖动只会引入噪声。整帧 240×320 只需约 169 KB。
//  位流：像素 i 占第 i*18 位起 18 位（R 高 6 位、G 中 6 位、B 低 6 位）
// ---------------------------------------------------------------------------
static inline void sky666Set(uint8_t *buf, int idx, int r6, int g6, int b6) {
  const uint32_t v = ((uint32_t)r6 << 12) | ((uint32_t)g6 << 6) | (uint32_t)b6;
  const uint32_t bit = (uint32_t)idx * 18u;
  const uint32_t byte = bit >> 3, off = bit & 7;
  uint32_t acc = (uint32_t)buf[byte] | ((uint32_t)buf[byte + 1] << 8) |
                 ((uint32_t)buf[byte + 2] << 16) | ((uint32_t)buf[byte + 3] << 24);
  acc = (acc & ~(0x3FFFFu << off)) | (v << off);
  buf[byte]     = (uint8_t)acc;
  buf[byte + 1] = (uint8_t)(acc >> 8);
  buf[byte + 2] = (uint8_t)(acc >> 16);
  buf[byte + 3] = (uint8_t)(acc >> 24);
}

// 6 → 8 位展开表（v6 × 255/63 四舍五入；直接 <<2 到端点只有 252，会多出 3 级误差）
static const uint8_t D6[64] = {
    0,   4,   8,  12,  16,  20,  24,  28,
   32,  36,  40,  45,  49,  53,  57,  61,
   65,  69,  73,  77,  81,  85,  89,  93,
   97, 101, 105, 109, 113, 117, 121, 125,
  130, 134, 138, 142, 146, 150, 154, 158,
  162, 166, 170, 174, 178, 182, 186, 190,
  194, 198, 202, 206, 210, 215, 219, 223,
  227, 231, 235, 239, 243, 247, 251, 255,
};

static inline void sky666Get(const uint8_t *buf, int idx, uint8_t *p) {
  const uint32_t bit = (uint32_t)idx * 18u;
  const uint32_t byte = bit >> 3, off = bit & 7;
  const uint32_t acc = (uint32_t)buf[byte] | ((uint32_t)buf[byte + 1] << 8) |
                       ((uint32_t)buf[byte + 2] << 16) | ((uint32_t)buf[byte + 3] << 24);
  const uint32_t v = (acc >> off) & 0x3FFFFu;      // r6 g6 b6
  p[0] = D6[(v >> 12) & 0x3F];
  p[1] = D6[(v >> 6) & 0x3F];
  p[2] = D6[v & 0x3F];
}

// 888 浮点 → 6 位（四舍五入到最近格；不抖动）
static inline int to6(float v) {
  const int i = (int)(v * (63.0f / 255.0f) + 0.5f);
  return i < 0 ? 0 : (i > 63 ? 63 : i);
}

// 同上，但带抖动：6 位只有 64 级，天空这种横跨几十级的大渐变不加抖动会看到
// 层纹（夜里尤其明显：整屏可能只跨 4~5 级）。做法「加噪声再截断」——噪声在
// 0..1 个量化格上均匀分布，均值不失真；用无规则噪声而不是 Bayer 图案，
// 暗部不会出现纹理条带。
static inline int to6Dither(float v, int x, int y) {
  const int i = (int)(v * (63.0f / 255.0f) + ditherNoise(x, y));
  return i < 0 ? 0 : (i > 63 ? 63 : i);
}

// 面板伽马校正：ST7789 默认伽马曲线在中段偏线性，同样的 0..255 值在面板上
// 比 sRGB 显示器亮很多，整幅画面显得发白。推屏前把每通道预压暗
//   out = 255 * (v/255)^GAMMA_EXP
// 让面板实际亮度接近网页参考。只作用在最终推屏路径（renderFrameRow666），
// 不参与渲染数学，也不进校验工具（PPM / check 仍是未校正的 888）。
// 实机若矫枉过正（画面过暗），把 GAMMA_EXP 调小（如 1.8）；仍发白则调大。
#define GAMMA_EXP 2.2f
static uint8_t g_gammaLut[256];
static bool    g_gammaReady = false;
static void gammaInit(void) {
  if (g_gammaReady) return;
  for (int i = 0; i < 256; i++)
    g_gammaLut[i] = (uint8_t)(powf((float)i / 255.0f, GAMMA_EXP) * 255.0f + 0.5f);
  g_gammaReady = true;
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
  gammaInit();                              // 面板伽马校正查表（只在最终推屏用）

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
  skyScreenSize(cfg->rot, &fs->w, &fs->h);      // 画布尺寸（旋转后可变）

  fs->haze = (float)cfg->haze;                    // 大气气溶胶（维纳斯带强度）

  const SkyPos sun = sunPosition(jd, cfg->lat, cfg->lon);
  fs->sunAlt = sun.alt;  fs->sunAz  = sun.az;

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

  // 光晕尺度跟投影比例走（= 短边），不是跟屏幕高走：竖屏时屏幕更高，
  // 若按高算光晕会被放大 4/3 倍
  const double glowRadiusPx = 45.0 / cfg->fov * SCREEN_SHORT_PX;
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

  fs->sunPossible = (fs->sunSX > -glowRadiusPx && fs->sunSX < fs->w + glowRadiusPx &&
                     fs->sunSY > -glowRadiusPx && fs->sunSY < fs->h + glowRadiusPx);

  // ---- 星表 ----
  const double starVis = smoothstepd(-8, -16, sun.alt);
  const float  mx = fs->w * 0.05f;             // 边缘渐隐范围（像素，宽）
  const float  my = fs->h * 0.05f;             // 边缘渐隐范围（像素，高）
  fs->nStars = 0;

  if (starVis >= 0.01) {
    for (int s = 0; s < N_STARS; s++) {
      const SkyPos pos = equatorialToAltAz(STARS[s][0], STARS[s][1], cfg->lat, cfg->lon, jd);
      if (pos.alt < 0) continue;

      const SkyPt pt = projectSky(cfg, pos.az, pos.alt);
      const float x = (float)pt.x, y = (float)pt.y;
      if (x < -mx || x > fs->w + mx || y < -my || y > fs->h + my) continue;

      const float edgeFade = fminf(
          fminf(smoothstepf(-mx, mx, x), smoothstepf(fs->w + mx, fs->w - mx, x)),
          fminf(smoothstepf(-my, my, y), smoothstepf(fs->h + my, fs->h - my, y)));
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

    if (x >= -20.0f && x <= fs->w + 20.0f && y >= -20.0f && y <= fs->h + 20.0f) {
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

  // 地影 + 维纳斯带（公式照抄 维纳斯.html renderSky240，气溶胶浓度 = fs->haze，默认 0.15）：
  //   阴影顶随方位收窄成弧：shadowTopH = shadowAlt0·sqrt(max(0,-cm))，反日点最高、90° 处降到地平线
  //   antiW = sstep(-0.05, 0.4, -cm) 平滑过渡；地影乘法压暗 ×(1 - 0.46/0.38/0.14·sh)
  //   可见度窗口：太阳 -1°→-3° 渐显、-6.5°→-10.5° 渐隐
  //   haze：beltW = 3.5+7h、beltSat = beltVis·(0.20+0.75h)、beltC = shadowTopH+2.5+7h
  {
    const float altDeg = asinf(clampf(d.sinAlt, -1.0f, 1.0f)) * 57.29578f;
    const float haze   = fs->haze;
    const float beltVis = smoothstepf(-1.0f, -3.0f, fs->sunAlt) *
                          (1.0f - smoothstepf(-6.5f, -10.5f, fs->sunAlt));
    const float beltW   = 3.5f + 7.0f * haze;
    const float beltSat = beltVis * (0.20f + 0.75f * haze);
    const float shVis   = beltVis * 0.72f;
    if (beltSat > 0.012f) {
      const float cpix = -cm;                          // cos(像素方位 − 反日方位)
      if (cpix > -0.05f) {
        // 东西两侧边缘平滑：用更宽的平滑范围，带子在反日点附近随方位角
        // 平缓渐隐到南北侧，消除原来约 66°/92° 处的硬切
        const float antiW = smoothstepf(-0.1f, 0.9f, cpix);
        const float shadowTopH = fmaxf(0.0f, -fs->sunAlt * 1.15f) * sqrtf(fmaxf(0.0f, cpix));

        // 地球阴影：位于该方位阴影顶之下，乘法压暗
        if (shadowTopH > 0.0f && altDeg < shadowTopH) {
          const float sh = antiW * shVis * smoothstepf(shadowTopH, shadowTopH - 1.8f, altDeg);
          if (sh > 0.004f) {
            cr *= 1.0f - 0.46f * sh;
            cg *= 1.0f - 0.38f * sh;
            cb *= 1.0f - 0.14f * sh;
          }
        }

        // 维纳斯带：贴着阴影顶上方，高斯剖面
        const float beltC  = shadowTopH + 2.5f + 7.0f * haze;
        const float beltLo = beltC - 2.6f * beltW;
        const float beltHi = beltC + 2.6f * beltW;
        if (altDeg > beltLo && altDeg < beltHi) {
          const float gd = (altDeg - beltC) / beltW;
          const float g  = expf(-gd * gd);
          const float amt = g * beltSat * antiW * 0.85f;
          if (amt > 0.004f) {
            const float pk = smoothstepf(0.3f, 1.0f, g) * 0.4f;
            cr += (255.0f - cr) * amt;
            cg += (150.0f + 46.0f * pk - cg) * amt;
            cb += (150.0f + 38.0f * pk - cb) * amt;
          }
        }
      }
    }
  }

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
  for (int x = 0; x < fs->w; x++) {
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
static void drawStarRow(uint8_t *rgb, int y, const StarDraw *s, int w) {
  const float R = fmaxf(s->radius, s->haloR);
  const float yc = y + 0.5f;
  if (yc < s->y - R - 1.0f || yc > s->y + R + 1.0f) return;

  int x0 = (int)floorf(s->x - R - 1.0f);
  int x1 = (int)ceilf(s->x + R + 1.0f);
  if (x0 < 0) x0 = 0;
  if (x1 > w - 1) x1 = w - 1;

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

static void drawMoonRow(uint8_t *rgb, int y, const MoonDraw *mn, int w) {
  const float yc = y + 0.5f;
  if (yc < mn->y - mn->haloR - 1.0f || yc > mn->y + mn->haloR + 1.0f) return;

  int x0 = (int)floorf(mn->x - mn->haloR - 1.0f);
  int x1 = (int)ceilf(mn->x + mn->haloR + 1.0f);
  if (x0 < 0) x0 = 0;
  if (x1 > w - 1) x1 = w - 1;

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
//  ① 背景层（天空 + 太阳 + 月亮）→ RGB666 缓存（18 位打包，不抖动）
//  月亮不闪烁，所以和天空一起烘进背景；只有星星需要逐帧叠加。
//  每分钟重建一次，其余时间设备可以待机。
// ===========================================================================
void renderSky(uint8_t *sky666, const FrameState *fs) {
  static uint8_t row[SCREEN_MAX_DIM * 3];
  int i = 0;
  for (int y = 0; y < fs->h; y++) {
    renderSkyRow(row, y, fs);        // 天空色 + 太阳
    renderMoonRow(row, y, fs);       // 月亮（背景的一部分）
    for (int x = 0; x < fs->w; x++, i++) {
      // 背景层带抖动（消层纹）；逐帧叠上去的星星不抖，直接取整
      sky666Set(sky666, i, to6Dither(row[x * 3], x, y),
                           to6Dither(row[x * 3 + 1], x, y),
                           to6Dither(row[x * 3 + 2], x, y));
    }
  }
}

// ===========================================================================
//  ②③ 独立图层：星星（位置 / 大小 / 颜色 → 直接叠加）/ 月亮
// ===========================================================================
void renderStarRow(uint8_t *rgb, int y, const FrameState *fs) {
  for (int i = 0; i < fs->nStars; i++) drawStarRow(rgb, y, &fs->stars[i], fs->w);
}

void renderMoonRow(uint8_t *rgb, int y, const FrameState *fs) {
  if (fs->moon.visible) drawMoonRow(rgb, y, &fs->moon, fs->w);
}

// ===========================================================================
//  合成一行：缓存背景 → 叠星（月亮已烘进背景，不逐帧画）
// ===========================================================================
void renderFrameRow(uint8_t *rgb, int y, const FrameState *fs, const uint8_t *sky666) {
  const int base = y * fs->w;
  for (int x = 0; x < fs->w; x++) sky666Get(sky666, base + x, rgb + x * 3);

  renderStarRow(rgb, y, fs);
}

// 同上，输出 18 位行：每像素 3 字节，每字节的高 6 位有效（ST7789 COLMOD=0x66）
// 面板伽马只作用在天空背景：星星是点光源 / 高光，不该被预压暗写进量化
// （否则暗/细星点的色差被压缩成灰黑）。顺序 = 天空(伽马) → 星(原色叠加) → 量化。
void renderFrameRow666(uint8_t *row666, int y, const FrameState *fs, const uint8_t *sky666) {
  static uint8_t row[SCREEN_MAX_DIM * 3];
  const int base = y * fs->w;
  for (int x = 0; x < fs->w; x++) {
    uint8_t c[3];
    sky666Get(sky666, base + x, c);
    row[x * 3]     = g_gammaLut[c[0]];
    row[x * 3 + 1] = g_gammaLut[c[1]];
    row[x * 3 + 2] = g_gammaLut[c[2]];
  }
  renderStarRow(row, y, fs);            // 星：原色叠加，不经伽马
  for (int x = 0; x < fs->w; x++) {
    row666[x * 3]     = (uint8_t)(to6(row[x * 3])     << 2);
    row666[x * 3 + 1] = (uint8_t)(to6(row[x * 3 + 1]) << 2);
    row666[x * 3 + 2] = (uint8_t)(to6(row[x * 3 + 2]) << 2);
  }
}
