// ===========================================================================
//  天空投影层 —— 等角立体投影（azimuthal stereographic）
//  逐段移植自 sunset.html 的 L884..L983（第 10.5 节）
//
//  天空每分钟才重画一次，所以不再预建 240×240 的反投影表（那是 112.5 KB 内存），
//  改成逐像素现算：只保留一张 2 KB 的「地平→天顶过渡系数」查表。
//  由此 RAM 让给天空缓存帧，且不再有方位角量化误差。
// ===========================================================================
#pragma once

#include <stdint.h>
#include "settings.h"

// 画布尺寸由 SkyConfig::rot 决定（见 settings.h 的 skyScreenSize）：
// 0/2 竖屏 240×320、1/3 横屏 320×240 —— 投影中心随之在运行时确定
struct SkyPt { double x, y, t; };   // t = 与投影中心的角距离（弧度）

// 逐像素反投影的预计算常量（每次重建天空前用 projBegin 算好）
// cx / cy = 画面中心像素坐标（旋转后可换向）
struct ProjCtx { float sa0, ca0, k, cx, cy; };

// 单像素方向：sinAlt + 「相机坐标系方位角 θ」的 cos/sin。
// 真实方位 az = camAz + θ，故 cos(az − X) = ct·cos(camAz − X) − st·sin(camAz − X)
// —— 这样每像素完全不需要反三角函数。
struct SkyDir { float sinAlt, ct, st; };

// 视线中心高度角（底边高度角 + 半视场，最高到天顶）
double projCenterAlt(const SkyConfig *cfg);
// 投影缩放：视场半宽对应投影半径 ρ(fov/2)
double projK(const SkyConfig *cfg);
// 立体投影的局部比例（等角）：thetaRad 处的角距离每弧度对应多少像素
double projPxPerDeg(const SkyConfig *cfg, double thetaRad);

// 天体方向 → 240 画布坐标（x 向右，y 向下）
SkyPt projectSky(const SkyConfig *cfg, double az, double alt);

// 单像素反投影（精确版，double 三角，仅验证用）
// altDeg 单位度，azRad 单位弧度（未归一，范围约 az0±π）
void projInversePixel(const SkyConfig *cfg, int x, int y, double *altDeg, double *azRad);

// ---- 设备用的快速路径 ----
void  projInit(const SkyConfig *cfg);                        // 建 kk 查表（上电一次）
void  projBegin(const SkyConfig *cfg, ProjCtx *ctx);         // 每次重建天空前一次
void  projPixelDir(const ProjCtx *ctx, int x, int y, SkyDir *dir);
float projKkFromSinAlt(float sinAlt);                        // 地平→天顶过渡系数 0..1
