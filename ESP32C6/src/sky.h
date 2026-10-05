// ===========================================================================
//  天空配色层 —— 纯 C++（只依赖 <math.h>）
//  逐段移植自 sunset.html 的 L652..L721（第 7 节）
// ===========================================================================
#pragma once

// 天空配色：z = 天顶色，h = 朝阳侧地平色（HTML L655 的 SKY_KEYS）
void skyColors(double alt, double z[3], double h[3]);

// 背阳侧地平色（HTML L672 的 H_OPP）
void skyHorizonOpp(double alt, double c[3]);
