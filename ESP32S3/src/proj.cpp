// ===========================================================================
//  天空投影层实现 —— 逐段移植自 sunset.html 的 L884..L983（第 10.5 节）
//
//  与 HTML 的差异：HTML 建 3 张 Float32 表（bgCA/bgSA/bgKk，共 675 KB）供每帧
//  取用；这里天空每分钟才画一次，直接逐像素现算，只留一张 2 KB 的 kk 查表。
//  数值上反而更准（没有方位角量化），速度上也够（全屏约 0.1 s／分钟）。
// ===========================================================================
#include "proj.h"
#include "astro.h"

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

// ---------------------------------------------------------------------------
//  kk（地平→天顶过渡系数）对 sin(alt) 的查表：-1..1 共 513 项，线性插值。
//  直接按 sin 查可以完全避免每像素调用 asin。
// ---------------------------------------------------------------------------
static float KK_SIN_LUT[513];

void projInit(const SkyConfig *cfg) {
  (void)cfg;
  // 只在开机算 513 次 double 反三角，之后每像素只做一次线性插值
  for (int i = 0; i <= 512; i++) {
    const double s = i / 512.0 * 2.0 - 1.0;
    const double altDeg = asin(clampd(s, -1, 1)) * ASTRO_DEG;
    double kk = 0.0;
    if (altDeg > 0) {
      const double t = clampd(altDeg, 0, 90) / 90;
      kk = 1 - pow(1 - t, 1.9);
    }
    KK_SIN_LUT[i] = (float)kk;
  }
}

float projKkFromSinAlt(float sinAlt) {
  const float f = (clampf(sinAlt, -1.0f, 1.0f) + 1.0f) * 256.0f;   // 0..512
  const int i = (int)f;
  const int j = i < 512 ? i + 1 : 512;
  const float fr = f - (float)i;
  return KK_SIN_LUT[i] + (KK_SIN_LUT[j] - KK_SIN_LUT[i]) * fr;
}

double projCenterAlt(const SkyConfig *cfg) {
  // 视线中心 = 底边高度角 + 垂直半视场（横屏 = fov/2，竖屏更大，见 settings.h）
  return clampd(cfg->baseAlt + skyHalfV(cfg), 0, 90);
}

double projK(const SkyConfig *cfg) {
  // fov 语义 = 屏幕短边的角宽（横屏 240×高，竖屏 240×宽）：
  // 横屏时与旧版 240×240 完全一致，竖屏时水平覆盖不变、垂直看到更多天空
  return skyProjK(cfg);
}

double projPxPerDeg(const SkyConfig *cfg, double thetaRad) {
  const double c = cos(thetaRad / 2);
  return projK(cfg) * ASTRO_RAD / (c * c);
}

SkyPt projectSky(const SkyConfig *cfg, double az, double alt) {
  int w, h;
  skyScreenSize(cfg->rot, &w, &h);

  const double a0 = projCenterAlt(cfg) * ASTRO_RAD;
  const double sa0 = sin(a0), ca0 = cos(a0);
  const double a = alt * ASTRO_RAD, sa = sin(a), ca = cos(a);
  const double d = (fmod(az - cfg->camAz + 540.0, 360.0) - 180.0) * ASTRO_RAD;
  const double cd = cos(d), sd = sin(d);

  const double w_ = clampd(sa0 * sa + ca0 * ca * cd, -1, 1);  // cosθ
  const double u = ca * sd;                                   // 向右分量
  const double v = ca0 * sa - sa0 * ca * cd;                  // 向上分量
  const double sinT = sqrt(u * u + v * v);

  const double rho = 2 * sinT / fmax(1 + w_, 1e-9) * projK(cfg); // 2·tan(θ/2)·k
  const double inv = sinT > 1e-12 ? rho / sinT : 0;

  SkyPt p;
  p.x = w / 2.0 + u * inv;
  p.y = h / 2.0 - v * inv;
  p.t = atan2(sinT, w_);
  return p;
}

// 单像素反投影（精确版：double + asin/atan2，只给验证工具用）
void projInversePixel(const SkyConfig *cfg, int x, int y, double *altDeg, double *azRad) {
  int w, h;
  skyScreenSize(cfg->rot, &w, &h);

  const double a0 = projCenterAlt(cfg) * ASTRO_RAD;
  const double sa0 = sin(a0), ca0 = cos(a0);
  const double az0 = cfg->camAz * ASTRO_RAD;
  const double k = projK(cfg);

  const double u = (x + 0.5) - w / 2.0;
  const double v = h / 2.0 - (y + 0.5);
  const double rr = sqrt(u * u + v * v);

  if (rr < 1e-6) {
    *altDeg = a0 * ASTRO_DEG;
    *azRad = az0;
    return;
  }

  const double rhoN = rr / k;                                // = 2·tan(θ/2)
  const double t2 = rhoN * rhoN * 0.25;                      // tan²(θ/2)
  const double sinT = rhoN / (1 + t2);
  const double cosT = (1 - t2) / (1 + t2);
  const double sinPA = u / rr, cosPA = v / rr;

  const double sinAlt = clampd(cosT * sa0 + sinT * ca0 * cosPA, -1, 1);
  *altDeg = asin(sinAlt) * ASTRO_DEG;

  const double y1 = sinT * sinPA;
  const double x1 = cosT * ca0 - sinT * sa0 * cosPA;
  *azRad = az0 + atan2(y1, x1);
}

void projBegin(const SkyConfig *cfg, ProjCtx *ctx) {
  const float a0 = (float)(projCenterAlt(cfg) * ASTRO_RAD);
  int w, h;
  skyScreenSize(cfg->rot, &w, &h);
  ctx->sa0 = sinf(a0);
  ctx->ca0 = cosf(a0);
  ctx->k   = (float)projK(cfg);
  ctx->cx  = w / 2.0f;
  ctx->cy  = h / 2.0f;
}

// 逐像素快速反投影：全程只有一次 sqrtf，没有反三角
// （ESP32-C6 上 newlib 的 asinf/atan2f 会退化成 double，慢约 100 倍）
void projPixelDir(const ProjCtx *ctx, int x, int y, SkyDir *dir) {
  const float u = (x + 0.5f) - ctx->cx;
  const float v = ctx->cy - (y + 0.5f);
  const float rr = sqrtf(u * u + v * v);

  float sinT, cosT, sinPA, cosPA;
  if (rr < 1e-6f) {                        // 画面正中：就是视线中心方向
    sinT = 0.0f;
    cosT = 1.0f;
    sinPA = 0.0f;
    cosPA = 1.0f;
  } else {
    const float rhoN = rr / ctx->k;        // = 2·tan(θ/2)
    const float t2 = rhoN * rhoN * 0.25f;  // tan²(θ/2)
    sinT = rhoN / (1 + t2);
    cosT = (1 - t2) / (1 + t2);
    sinPA = u / rr;
    cosPA = v / rr;
  }

  dir->sinAlt = clampf(cosT * ctx->sa0 + sinT * ctx->ca0 * cosPA, -1, 1);

  // 相机坐标系下的水平分量：cx 指向视线中心、cy 指向右
  const float cx = cosT * ctx->ca0 - sinT * ctx->sa0 * cosPA;
  const float cy = sinT * sinPA;
  const float h = sqrtf(cx * cx + cy * cy) + 1e-20f;
  dir->ct = cx / h;
  dir->st = cy / h;
}
