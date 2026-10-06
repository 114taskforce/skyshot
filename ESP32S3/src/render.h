// ===========================================================================
//  渲染层 —— 背景层 + 星图层，两层
//
//    ① 背景层（天空色 + 太阳 + 月亮）：都不闪烁，renderSky() 整帧渲染成
//       RGB565 缓存，每分钟一次；没有星星时设备其余时间待机，只推屏一次
//    ② 星图层：frameStateCompute() 算出每颗星的「位置 / 大小 / 颜色 / 闪烁量」
//               （StarDraw 列表），renderStarRow() 只负责把它叠加到某一行上
//
//  每帧只需：取缓存背景行 → 叠星 → 推屏（见 renderFrameRow565）。
//  HTML 的画布为 240×240（SZ = 240）；本工程为横屏 320×240：垂直方向与
//  HTML/旧版 240×240 的尺度完全一致（projK 只与高度绑定），水平方向多出
//  的 80 列按同一等角立体投影真实渲染更宽的方位角，不拉伸、不复制像素。
// ===========================================================================
#pragma once

#include <stdint.h>
#include "astro.h"
#include "proj.h"
#include "settings.h"

// 画布尺寸随 SkyConfig::rot 在运行时确定（FrameState 的 w / h）；
// 缓冲按面板最大尺寸分配一份即可（两种方向像素总数相同）
#define RENDER_MAX_PX SCREEN_MAX_PX

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
  float  haze;                                // 大气气溶胶（维纳斯带强度），0..1
  int w, h;                                  // 画布尺寸（由 rot 决定，运行时可变）
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

// ① 背景层（天空色 + 太阳 + 月亮）→ RGB666 缓存
// 屏幕是 18 位色（ST7789 COLMOD=0x66），所以背景按 6-6-6 存、不抖动：
// 每像素 18 位、按位打包（4 像素 9 字节），整帧约 169 KB（两种方向像素数相同）
#define RENDER_SKY_BYTES(px) (((px) * 18 + 7) / 8 + 8)
void renderSky(uint8_t *sky666, const FrameState *fs);

// 背景层单行 → RGB888（天空色 + 太阳，不含月亮；与 renderSky 同一套公式，验证用）
void renderSkyRow(uint8_t *rgb, int y, const FrameState *fs);

// ② 星图层叠加 → RGB888 行；③ 月亮（renderSky 内部用，也可单独调用）
void renderStarRow(uint8_t *rgb, int y, const FrameState *fs);
void renderMoonRow(uint8_t *rgb, int y, const FrameState *fs);

// 缓存背景行 → 叠星 → 输出 18 位行（每像素 3 字节，设备推屏直接用；无抖动）
void renderFrameRow666(uint8_t *row666, int y, const FrameState *fs, const uint8_t *sky666);

// 同上，但输出 RGB888（放大看图 / 验证用）
void renderFrameRow(uint8_t *rgb, int y, const FrameState *fs, const uint8_t *sky666);
