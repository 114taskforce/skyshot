// ===========================================================================
//  渲染层 —— 背景层 + 星图层，两层
//
//    ① 背景层（天空色 + 太阳 + 月亮）：都不闪烁，renderSky() 整帧渲染成
//       RGB565 缓存，每分钟一次；没有星星时设备其余时间待机，只推屏一次
//    ② 星图层：frameStateCompute() 算出每颗星的「位置 / 大小 / 颜色 / 闪烁量」
//               （StarDraw 列表），renderStarRow() 只负责把它叠加到某一行上
//
//  每帧只需：取缓存背景行 → 叠星 → 推屏（见 renderFrameRow565）。
//  HTML 的画布分辨率与目标屏一致（SZ = 240），故取 sc = 1、dpr = 1。
// ===========================================================================
#pragma once

#include <stdint.h>
#include "astro.h"
#include "proj.h"
#include "settings.h"

#define RENDER_RES 240

// 单颗星：位置 / 大小 / 颜色 / 闪烁量（每帧由 frameTwinkle 刷新后半段）
struct StarDraw {
  float x, y;              // 240 坐标系（= 显示坐标）
  float baseIntensity;     // 未含闪烁的强度（已乘星等、地平、边缘渐隐）
  float freq, phase, depth, magFactor, mag;
  uint8_t r, g, b;         // 颜色（B−V 转 RGB）
  // ---- 每帧由 frameTwinkle 刷新 ----
  float alpha;             // 峰值不透明度
  float radius;            // 星点半径（像素）
  float haloR, haloA;      // haloR = 0 表示不画光晕
};

// 月亮绘制参数
struct MoonDraw {
  bool  visible;
  float x, y, r;           // 圆盘中心 / 半径（像素）
  float term;              // 相位明暗界线（半椭圆横半轴）
  float illum;             // 照亮比例
  bool  waxing;
  float vis;               // 整体不透明度
  float haloR, haloA;
  bool  detail;            // r ≥ 5 时画月海 / 环形山
};

// 一次刷新（默认 60 s）算好的整帧状态
struct FrameState {
  double sunAlt, sunAz;
  // 天空逐像素计算用的每帧常量
  ProjCtx proj;                              // 反投影常量
  float   azCosC, azSinS;                    // cos/sin(camAz − sunAz)
  // 天空配色
  float zR, zG, zB;                          // 天顶色
  float aR, aG, aB, bR, bG, bB;              // 地平色方位混合
  // 太阳圆盘 / 光晕
  float sunSX, sunSY;
  float diskPx2, glowRadiusPx2, glowK, diskVis, gG, gB, gAmp;
  float diskCenterR, diskCenterG, diskCenterB;
  float diskEdgeR, diskEdgeG, diskEdgeB;
  bool  sunPossible;
  // 星 / 月
  int       nStars;
  StarDraw  stars[N_STARS];
  MoonDraw  moon;
};

// 建 kk 查表 + 月面环形山表（上电一次，内部会调 projInit）
void renderInit(const SkyConfig *cfg);

// 按儒略日算好整帧的几何 / 配色 / 星表（不含闪烁）
void frameStateCompute(FrameState *fs, const SkyConfig *cfg, double jd);

// 按当前时间刷新星星的闪烁量（每帧调用）
void frameTwinkle(FrameState *fs, double tSec, double twinkleAmt);

// ① 背景层（天空色 + 太阳 + 月亮）：整帧 → RGB565 缓存（含 4×4 抖动）
void renderSky(uint16_t *sky565, const FrameState *fs);

// 背景层单行 → RGB888（天空色 + 太阳，不含月亮；与 renderSky 同一套公式，验证用）
void renderSkyRow(uint8_t *rgb, int y, const FrameState *fs);

// ② 星图层叠加 → RGB888 行；③ 月亮（renderSky 内部用，也可单独调用）
void renderStarRow(uint8_t *rgb, int y, const FrameState *fs);
void renderMoonRow(uint8_t *rgb, int y, const FrameState *fs);

// 缓存背景行 → 叠星 → 打包 RGB565（设备推屏直接用）
void renderFrameRow565(uint16_t *row565, int y, const FrameState *fs, const uint16_t *sky565);

// 同上，但输出 RGB888（放大看图 / 验证用）
void renderFrameRow(uint8_t *rgb, int y, const FrameState *fs, const uint16_t *sky565);
