// ===========================================================================
//  天空配色层实现 —— 逐段移植自 sunset.html 的 L652..L721（第 7 节）
//  纯 C++：只依赖 <math.h>，可在 PC 上用 g++ 编译做交叉验证
// ===========================================================================
#include "sky.h"
#include "astro.h"

// SKY_KEYS（HTML L655）：a = 太阳高度角，z = 天顶色，h = 朝阳侧地平色
static const struct { double a; float z[3], h[3]; } SKY_KEYS[] = {
  {  90, { 28,  84, 168}, {128, 186, 232} },
  {  60, { 34,  92, 176}, {136, 190, 234} },
  {  30, { 42, 100, 182}, {148, 194, 234} },
  {  15, { 52, 106, 184}, {166, 200, 230} },
  {   8, { 58, 106, 178}, {196, 198, 214} },
  {   4, { 56,  94, 158}, {226, 184, 168} },
  {   1, { 46,  72, 130}, {244, 156, 116} },
  {  -1, { 34,  52, 102}, {240, 122,  88} },
  {  -4, { 22,  34,  76}, {212,  88,  82} },
  {  -7, { 12,  19,  50}, {152,  60,  78} },
  { -11, {  5,   9,  28}, { 92,  44,  72} },
  { -15, {  2,   4,  14}, { 46,  28,  58} },
  { -18, {  1,   2,   8}, { 22,  18,  40} },
  { -90, {  0,   1,   4}, {  6,   7,  18} },
};
static const int N_SKY_KEYS = (int)(sizeof(SKY_KEYS) / sizeof(SKY_KEYS[0]));

// H_OPP（HTML L672）：背阳侧地平色
static const struct { double a; float c[3]; } H_OPP[] = {
  {  90, {120, 178, 232} },
  {  60, {116, 174, 230} },
  {  30, {112, 168, 228} },
  {  15, {106, 160, 224} },
  {   8, { 96, 146, 216} },
  {   4, { 82, 126, 202} },
  {   1, { 68, 104, 178} },
  {  -1, { 54,  80, 146} },
  {  -4, { 34,  52, 104} },
  {  -7, { 18,  28,  66} },
  { -11, {  8,  13,  36} },
  { -15, {  3,   6,  18} },
  { -18, {  1,   2,   9} },
  { -90, {  0,   1,   4} },
};
static const int N_H_OPP = (int)(sizeof(H_OPP) / sizeof(H_OPP[0]));

// 关键帧插值（HTML L691 / L708）：区间内用 cosEase 缓动
static void lerpKey3(const float A[3], const float B[3], double t, double out[3]) {
  out[0] = lerpd(A[0], B[0], t);
  out[1] = lerpd(A[1], B[1], t);
  out[2] = lerpd(A[2], B[2], t);
}

void skyColors(double alt, double z[3], double h[3]) {
  if (alt >= SKY_KEYS[0].a) {
    for (int i = 0; i < 3; i++) { z[i] = SKY_KEYS[0].z[i]; h[i] = SKY_KEYS[0].h[i]; }
    return;
  }
  for (int i = 0; i < N_SKY_KEYS - 1; i++) {
    const double A = SKY_KEYS[i].a, B = SKY_KEYS[i + 1].a;
    if (alt <= A && alt >= B) {
      const double t = cosEase((A - alt) / (A - B));
      lerpKey3(SKY_KEYS[i].z, SKY_KEYS[i + 1].z, t, z);
      lerpKey3(SKY_KEYS[i].h, SKY_KEYS[i + 1].h, t, h);
      return;
    }
  }
  const int last = N_SKY_KEYS - 1;
  for (int i = 0; i < 3; i++) { z[i] = SKY_KEYS[last].z[i]; h[i] = SKY_KEYS[last].h[i]; }
}

void skyHorizonOpp(double alt, double c[3]) {
  if (alt >= H_OPP[0].a) {
    for (int i = 0; i < 3; i++) c[i] = H_OPP[0].c[i];
    return;
  }
  for (int i = 0; i < N_H_OPP - 1; i++) {
    const double A = H_OPP[i].a, B = H_OPP[i + 1].a;
    if (alt <= A && alt >= B) {
      const double t = cosEase((A - alt) / (A - B));
      lerpKey3(H_OPP[i].c, H_OPP[i + 1].c, t, c);
      return;
    }
  }
  const int last = N_H_OPP - 1;
  for (int i = 0; i < 3; i++) c[i] = H_OPP[last].c[i];
}
