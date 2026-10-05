// ===========================================================================
//  天文计算层实现 —— 逐段移植自 sunset.html 的 L360..L650
//  纯 C++：只依赖 <math.h> / <stdint.h>，可在 PC 上用 g++ 编译做交叉验证
// ===========================================================================
#include "astro.h"

// 天体角半径放大倍数：1 = 真实大小；sunset.html 原版是 2；
// 实机 240×240 上太阳月亮太小，这里放大到 4（圆盘直径约 7 px @ 视场 68°）
#define BODY_MAGNIFY 4.0

// 天体真实角半径（度）× 放大倍数（HTML L367/L368）
const double SUN_ANG_RAD  = 0.2665 * BODY_MAGNIFY;
const double MOON_ANG_RAD = 0.2590 * BODY_MAGNIFY;

// 目标屏 240×240：圆盘最小半径像素（HTML L372 为 2.5，随放大倍数同步翻倍）
const double MIN_BODY_RAD_PX = 5.0;

// ---- 标量工具（HTML L362..L378）----
double clampd(double v, double a, double b) { return v < a ? a : (v > b ? b : v); }
double lerpd(double a, double b, double t) { return a + (b - a) * t; }

double smoothstepd(double a, double b, double x) {
  double t = clampd((x - a) / (b - a), 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}

double cosEase(double t) { return 0.5 - 0.5 * cos(t * ASTRO_PI); }

// ---- 0. 单值 hash（HTML L383）----
double hash1(double x) {
  double n = sin(x * 12.9898) * 43758.5453;
  return n - floor(n);
}

// ---- 1. 太阳位置（HTML L391）----
SkyPos sunPosition(double jd, double lat, double lon) {
  const double n  = jd - 2451545.0;

  const double L   = 280.460 + 0.9856474 * n;
  const double g   = (357.528 + 0.9856003 * n) * ASTRO_RAD;
  const double lam = (L + 1.915 * sin(g) + 0.020 * sin(2 * g)) * ASTRO_RAD;
  const double eps = (23.439 - 0.0000004 * n) * ASTRO_RAD;

  const double alpha = atan2(cos(eps) * sin(lam), cos(lam));
  const double delta = asin(clampd(sin(eps) * sin(lam), -1, 1));

  const double gmst = (280.46061837 + 360.98564736629 * n) * ASTRO_RAD;
  const double H    = gmst + lon * ASTRO_RAD - alpha;
  const double phi  = lat * ASTRO_RAD;

  const double sinAlt = sin(phi) * sin(delta) +
                        cos(phi) * cos(delta) * cos(H);
  const double alt = asin(clampd(sinAlt, -1, 1)) * ASTRO_DEG;

  const double az = atan2(
      -cos(delta) * sin(H),
      cos(phi) * sin(delta) - sin(phi) * cos(delta) * cos(H)
  ) * ASTRO_DEG;

  SkyPos p;
  p.alt = alt;
  p.az  = fmod(fmod(az, 360.0) + 360.0, 360.0);
  return p;
}

// ---- 2. 太阳黄经（HTML L422）----
double sunEclipticLon(double jd) {
  const double n = jd - 2451545.0;
  const double L = 280.460 + 0.9856474 * n;
  const double g = (357.528 + 0.9856003 * n) * ASTRO_RAD;
  const double lambda = L + 1.915 * sin(g) + 0.020 * sin(2 * g);
  return fmod(fmod(lambda, 360.0) + 360.0, 360.0);
}

// ---- 3. 月亮位置（Meeus 简化算法，HTML L434）----
MoonPos moonPosition(double jd) {
  const double T = (jd - 2451545.0) / 36525;

  const double Lp = 218.3164477 + 481267.88123421 * T - 0.0015786 * T * T;
  const double D  = 297.8501921 + 445267.1114034  * T - 0.0018819 * T * T;
  const double M  = 357.5291092 +  35999.0502909  * T - 0.0001536 * T * T;
  const double Mp = 134.9633964 + 477198.8675055  * T + 0.0087414 * T * T;
  const double F  =  93.2720950 + 483202.0175233  * T - 0.0036539 * T * T;

  const double Dr = D * ASTRO_RAD, Mr = M * ASTRO_RAD;
  const double Mpr = Mp * ASTRO_RAD, Fr = F * ASTRO_RAD;

  const double dL = 6.289 * sin(Mpr)
                  + 1.274 * sin(2 * Dr - Mpr)
                  + 0.658 * sin(2 * Dr)
                  + 0.214 * sin(2 * Mpr)
                  - 0.186 * sin(Mr)
                  - 0.114 * sin(2 * Fr)
                  + 0.059 * sin(2 * Dr - 2 * Mpr)
                  + 0.057 * sin(2 * Dr - Mr - Mpr)
                  + 0.053 * sin(2 * Dr + Mpr)
                  + 0.046 * sin(2 * Dr - Mr + Mpr)
                  + 0.041 * sin(Mpr - Mr)
                  - 0.035 * sin(Dr)
                  - 0.031 * sin(Mpr + Mr);

  const double dB = 5.128 * sin(Fr)
                  + 0.281 * sin(Mpr + Fr)
                  + 0.278 * sin(Mpr - Fr)
                  + 0.173 * sin(2 * Dr - Fr)
                  + 0.055 * sin(2 * Dr + Fr - Mpr)
                  + 0.046 * sin(2 * Dr - Fr - Mpr)
                  + 0.033 * sin(2 * Dr + Fr)
                  + 0.017 * sin(2 * Mpr + Fr);

  const double dist = 385000
                    - 20905 * cos(Mpr)
                    - 3699  * cos(2 * Dr - Mpr)
                    - 2956  * cos(2 * Dr)
                    - 569   * cos(2 * Mpr);

  const double lambda = fmod(fmod(Lp + dL, 360.0) + 360.0, 360.0);
  const double beta   = dB;

  const double eps = (23.4392911 - 0.0130042 * T) * ASTRO_RAD;
  const double lambdaR = lambda * ASTRO_RAD;
  const double betaR   = beta   * ASTRO_RAD;

  const double sinDec = sin(betaR) * cos(eps) +
                        cos(betaR) * sin(eps) * sin(lambdaR);
  const double dec = asin(clampd(sinDec, -1, 1)) * ASTRO_DEG;

  const double ra = atan2(
      sin(lambdaR) * cos(eps) - tan(betaR) * sin(eps),
      cos(lambdaR)
  ) * ASTRO_DEG;

  MoonPos m;
  m.ra       = fmod(fmod(ra, 360.0) + 360.0, 360.0) / 15;
  m.dec      = dec;
  m.lambda   = lambda;
  m.beta     = beta;
  m.distance = dist;
  return m;
}

// ---- 4. 月相（HTML L503）----
MoonPhase moonPhase(double jd) {
  const MoonPos mp = moonPosition(jd);
  const double sl = sunEclipticLon(jd);
  const double phaseAngle = fmod(fmod(mp.lambda - sl, 360.0) + 360.0, 360.0);

  MoonPhase ph;
  ph.phaseAngle   = phaseAngle;
  ph.illumination = (1 - cos(phaseAngle * ASTRO_RAD)) / 2;
  ph.waxing       = phaseAngle < 180;
  return ph;
}

// ---- 5. 赤道 → 地平（HTML L527/L532）----
double gmstDeg(double jd) {
  const double g = 280.46061837 + 360.98564736629 * (jd - 2451545.0);
  return fmod(fmod(g, 360.0) + 360.0, 360.0);
}

SkyPos equatorialToAltAz(double ra_h, double dec_deg, double lat_deg,
                         double lon_deg, double jd) {
  const double lst = (gmstDeg(jd) + lon_deg) * ASTRO_RAD;
  const double ra  = ra_h * 15 * ASTRO_RAD;
  const double dec = dec_deg * ASTRO_RAD;
  const double lat = lat_deg * ASTRO_RAD;

  const double H = lst - ra;

  const double sinAlt = sin(lat) * sin(dec) +
                        cos(lat) * cos(dec) * cos(H);
  const double sAlt = clampd(sinAlt, -1, 1);
  const double alt = asin(sAlt) * ASTRO_DEG;

  const double cosAlt = fmax(sqrt(fmax(1 - sAlt * sAlt, 0.0)), 1e-6);
  const double cosLat = fmax(cos(lat), 1e-6);

  const double sinAz = -cos(dec) * sin(H) / cosAlt;
  const double cosAz = (sin(dec) - sin(lat) * sAlt) / (cosLat * cosAlt);
  const double az = atan2(sinAz, cosAz) * ASTRO_DEG;

  SkyPos p;
  p.alt = alt;
  p.az  = fmod(fmod(az, 360.0) + 360.0, 360.0);
  return p;
}

// 月亮地平坐标（考虑视差，HTML L558）
SkyPos moonAltAz(double jd, double lat, double lon) {
  const MoonPos mp = moonPosition(jd);
  SkyPos pos = equatorialToAltAz(mp.ra, mp.dec, lat, lon, jd);

  // 月球视差修正
  const double parallax = asin(6378 / mp.distance) * ASTRO_DEG;
  pos.alt -= parallax * cos(pos.alt * ASTRO_RAD);

  return pos;
}

// ---- 6. 星表（83 颗亮星，Vmag < 3.0，J2000）（HTML L573）----
const float STARS[N_STARS][4] = {
  {6.752, -16.716, -1.46,  0.00}, {6.399, -52.696, -0.74,  0.15},
  {14.660, -60.835, -0.27, 0.71}, {14.261,  19.182, -0.05, 1.23},
  {18.615,  38.784,  0.03, 0.00}, {5.278,   45.998,  0.08, 0.80},
  {5.242,   -8.202,  0.12, -0.03},{7.655,    5.225,  0.34, 0.42},
  {1.629,  -57.237,  0.46, -0.16},{5.919,    7.407,  0.50, 1.85},
  {14.064, -60.373,  0.61, -0.23},{19.846,   8.868,  0.77, 0.22},
  {12.443, -63.099,  0.77, -0.24},{4.599,   16.509,  0.85, 1.54},
  {16.490, -26.432,  0.96, 1.83}, {13.420, -11.161,  0.98, -0.23},
  {7.755,   28.026,  1.14, 1.00}, {22.961, -29.622,  1.16, 0.09},
  {20.690,  45.280,  1.25, 0.09}, {12.795, -59.689,  1.25, -0.24},
  {10.139,  11.967,  1.36, -0.11},{6.977,  -28.972,  1.50, -0.21},
  {7.577,   31.888,  1.58, 0.03}, {12.519, -57.113,  1.59, 1.59},
  {17.560, -37.104,  1.62, -0.23},{5.418,    6.350,  1.64, -0.22},
  {5.438,   28.608,  1.65, 1.65}, {9.220,  -69.717,  1.67, 0.00},
  {5.604,   -1.202,  1.69, -0.18},{22.137, -46.961,  1.74, -0.13},
  {5.679,   -1.943,  1.74, -0.21},{12.900,  55.960,  1.77, -0.02},
  {11.062,  61.751,  1.79, 1.07}, {3.405,   49.861,  1.79, 0.48},
  {7.140,  -26.393,  1.83, 0.67}, {18.403, -34.385,  1.85, -0.03},
  {8.375,  -59.510,  1.86, 1.28}, {13.792,  49.313,  1.86, -0.19},
  {5.992,   44.948,  1.90, 0.08}, {16.811, -69.028,  1.92, 1.44},
  {6.629,   16.399,  1.93, 0.00}, {20.427, -56.735,  1.94, -0.12},
  {8.745,  -54.709,  1.96, 0.04}, {6.378,  -17.956,  1.98, -0.24},
  {9.460,   -8.659,  1.98, 1.44}, {2.530,   89.264,  1.98, 0.60},
  {2.120,   23.462,  2.00, 1.15}, {0.726,  -17.987,  2.04, 1.02},
  {13.399,  54.925,  2.04, 0.02}, {18.921, -26.297,  2.05, -0.20},
  {14.111, -36.370,  2.06, 1.01}, {0.140,   29.091,  2.06, -0.04},
  {1.162,   35.620,  2.06, 1.57}, {5.796,   -9.670,  2.06, -0.17},
  {14.845,  74.156,  2.08, 1.47}, {17.582,  12.560,  2.08, 0.15},
  {3.136,   40.956,  2.12, -0.05},{11.818,  14.572,  2.14, 0.09},
  {9.285,  -59.275,  2.21, 0.18}, {15.578,  26.715,  2.22, -0.02},
  {9.133,  -43.433,  2.23, 1.66}, {5.533,   -0.299,  2.23, -0.22},
  {20.370,  40.257,  2.23, 0.68}, {17.943,  51.489,  2.23, 1.52},
  {0.675,   56.537,  2.24, 1.17}, {8.060,  -40.003,  2.25, -0.27},
  {2.065,   42.330,  2.26, 1.37}, {16.836, -34.293,  2.29, 1.15},
  {12.692, -48.960,  2.20, -0.01},{17.622, -42.998,  2.41, -0.22},
  {7.402,  -29.303,  2.45, 0.18}, {5.545,  -17.822,  2.58, 1.17},
  {15.283,  -9.383,  2.61, 1.58}, {16.005, -22.622,  2.43, 1.14},
  {16.091, -19.805,  2.62, 0.16}, {10.716, -64.394,  2.47, -0.24},
  {0.140,  -42.121,  2.40, 0.07}, {3.791,   24.105,  2.85, -0.09},
  {6.732,   25.131,  2.98, 1.43}, {6.383,   22.514,  2.87, 1.62},
  {5.470,  -20.760,  2.84, -0.18},{7.453,    8.289,  2.89, -0.09},
  {18.982,  32.690,  2.99, 0.94},
};

// 闪烁参数（HTML L618：用 hash1 以同样公式算出）
StarTwinkle STAR_TWINKLE[N_STARS];

void astroInit(void) {
  for (int i = 0; i < N_STARS; i++) {
    const double h1 = hash1(i + 0.137);
    const double h2 = hash1(i * 2.71 + 1.37);
    const double h3 = hash1(i * 5.43 + 7.77);
    STAR_TWINKLE[i].phase = h1 * ASTRO_PI * 2;
    STAR_TWINKLE[i].freq  = 0.65 + h2 * 2.05;
    STAR_TWINKLE[i].depth = 0.18 + h3 * 0.42;
  }
}

// 星等 → 亮度（HTML L629）
double magToIntensity(double mag) {
  const double flux = pow(2.512, -mag);
  return clampd(pow(flux, 0.45), 0, 1);
}

// B−V → RGB（HTML L634）
void bvToColor(double bv, double rgb[3]) {
  double r, g, b;
  if (bv < 0.0) {
    const double t = clampd((bv + 0.4) / 0.4, 0, 1);
    r = lerpd(155, 200, t); g = lerpd(180, 215, t); b = 255;
  } else if (bv < 0.65) {
    const double t = bv / 0.65;
    r = lerpd(200, 255, t); g = lerpd(215, 250, t); b = lerpd(255, 245, t);
  } else if (bv < 1.5) {
    const double t = (bv - 0.65) / 0.85;
    r = 255; g = lerpd(250, 210, t); b = lerpd(245, 165, t);
  } else {
    const double t = clampd((bv - 1.5) / 0.8, 0, 1);
    r = 255; g = lerpd(210, 180, t); b = lerpd(165, 130, t);
  }
  rgb[0] = r; rgb[1] = g; rgb[2] = b;
}