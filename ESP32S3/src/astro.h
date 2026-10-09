// ===========================================================================
//  天文计算层 —— 纯 C++（只依赖 <math.h>），不 include Arduino.h
//  逐段移植自 sunset.html 的 L360..L650
// ===========================================================================
#pragma once

#include <math.h>
#include <stdint.h>

#define ASTRO_PI  3.14159265358979323846
#define ASTRO_RAD (ASTRO_PI / 180.0)
#define ASTRO_DEG (180.0 / ASTRO_PI)

// 天体真实角半径（度）× 统一放大倍数（见 astro.cpp 的 BODY_MAGNIFY）
// 真实角直径：太阳 0.533°、月亮 0.518°
extern const double SUN_ANG_RAD;
extern const double MOON_ANG_RAD;

// 目标屏 240×240：圆盘最小半径（以 240 画面为单位的像素）
extern const double MIN_BODY_RAD_PX;

// 星表实际条数 140（原 83 颗亮星 + 主要星座图形星 & 3.0 等内亮星 57 颗）
#define N_STARS 520

struct SkyPos   { double alt, az; };                       // az: 0..360，正北顺时针
struct MoonPos  { double ra, dec, lambda, beta, distance; };
struct MoonPhase{ double illumination, phaseAngle; bool waxing; };
struct StarTwinkle { double phase, freq, depth; };

// ---- 标量工具（对应 HTML L360..L380 / L383）----
double clampd(double v, double a, double b);
double lerpd(double a, double b, double t);
double smoothstepd(double a, double b, double x);
double cosEase(double t);
double hash1(double x);

// ---- 天体位置 ----
SkyPos  sunPosition(double jd, double lat, double lon);
double  sunEclipticLon(double jd);
MoonPos moonPosition(double jd);
MoonPhase moonPhase(double jd);

double  gmstDeg(double jd);
SkyPos  equatorialToAltAz(double ra_h, double dec_deg, double lat, double lon, double jd);
SkyPos  moonAltAz(double jd, double lat, double lon);

// ---- 星表与配色 ----
extern const float STARS[N_STARS][4];              // { ra_h, dec_deg, Vmag, B-V }
extern StarTwinkle STAR_TWINKLE[N_STARS];          // astroInit() 用 hash1 填好
void astroInit(void);

double magToIntensity(double mag);
void   bvToColor(double bv, double rgb[3]);