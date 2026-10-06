// ===========================================================================
//  运行时配置结构 —— 纯 C++（不依赖 Arduino），PC 端验证工具共用
//
//  参数在 config.h 里硬编码，开机时由 skyConfigDefaults() 灌进来；
//  投影 / 渲染层都从这里取参数，改参数只需改 config.h 后重编。
// ===========================================================================
#pragma once

#include <math.h>
#include <string.h>
#include "config.h"

struct SkyConfig {
  double lat, lon;    // 纬度（北正）/ 经度（东正），度
  double camAz;       // 面对方向：方位角，正北 0°，顺时针
  double baseAlt;     // 底边高度角
  double fov;         // 视场角
  double twinkle;     // 闪烁强度 0..1
  double haze;        // 大气气溶胶（维纳斯带强度）0..1，参考值 0.15
  int    rot;         // 屏幕方向 0..3（= LCD_ROTATION），运行时可改（网页/ NVS）
};

// ===========================================================================
//  屏幕方向 —— SkyConfig::rot，运行时可切（网页「画面」栏四个方向）
//  面板原生 240×320：rotation 0/2 = 竖屏 240×320，1/3 = 横屏 320×240
//  两种方向像素总数相同，故缓冲按最大尺寸分配一份即可
// ===========================================================================
#ifndef LCD_ROTATION
#define LCD_ROTATION 1              // 编译期默认（PC 端验证工具=横屏）
#endif

#define SCREEN_PANEL_W 240          // 面板原生宽（竖屏时的宽）
#define SCREEN_PANEL_H 320          // 面板原生高（竖屏时的高）
#define SCREEN_MAX_DIM 320          // 两个方向的边长上限（旋转后是 320×240 或 240×320）
#define SCREEN_MAX_PX  (SCREEN_PANEL_W * SCREEN_PANEL_H)
// 短边恒为 240（两种方向都一样）：视场角 fov 绑定在短边上
#define SCREEN_SHORT_PX SCREEN_PANEL_W

#define SKY_PI 3.14159265358979323846

// 方向 → 画布尺寸（0/2 竖屏 240×320，1/3 横屏 320×240）
inline void skyScreenSize(int rot, int *w, int *h) {
  if (rot & 1) { *w = 320; *h = 240; }
  else         { *w = 240; *h = 320; }
}
inline int skyScreenW(int rot) { int w, h; skyScreenSize(rot, &w, &h); return w; }
inline int skyScreenH(int rot) { int w, h; skyScreenSize(rot, &w, &h); return h; }

// 投影缩放 k：视场角 fov 对应屏幕短边的角宽（立体投影 ρ(fov/2) = 短边/2）
inline double skyProjK(const SkyConfig *c) {
  return (SCREEN_SHORT_PX / 2.0) / (2.0 * tan((c->fov / 4.0) * (SKY_PI / 180.0)));
}

// 垂直半视场（度）：屏幕上/下边到视线中心的角距离。
// 横屏（高 = 短边）时恰好 = fov/2；竖屏（高 = 长边）时更大，故要单独算
inline double skyHalfV(const SkyConfig *c) {
  return 2.0 * atan((skyScreenH(c->rot) / 2.0) / (2.0 * skyProjK(c))) * (180.0 / SKY_PI);
}

// 联网与时间配置（网页可改，存 NVS；config.h 里的值是默认值）
struct NetConfig {
  char   ssid[33];    // WiFi 名称
  char   pass[64];    // WiFi 密码
  char   url[160];    // 校时接口地址
  bool   manual;      // true = 手动固定时间，不联网校时
  double epoch;       // 手动模式基准时间（Unix 秒，UTC）
  double tz;          // 时区，手动模式换算用
};

inline void skyConfigDefaults(SkyConfig *c) {
  c->lat     = CFG_LAT;
  c->lon     = CFG_LON;
  c->camAz   = CFG_CAM_AZ;
  c->baseAlt = CFG_BASE_ALT;
  c->fov     = CFG_FOV;
  c->twinkle = CFG_TWINKLE;
  c->haze    = CFG_HAZE;
  c->rot     = LCD_ROTATION;
}

static inline void skyStrCpy(char *dst, size_t cap, const char *src) {
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = 0;
}

inline void netConfigDefaults(NetConfig *n) {
  skyStrCpy(n->ssid, sizeof(n->ssid), CFG_WIFI_SSID);
  skyStrCpy(n->pass, sizeof(n->pass), CFG_WIFI_PASS);
  skyStrCpy(n->url,  sizeof(n->url),  CFG_TIME_API_URL);
  n->manual = false;
  n->epoch  = (double)CFG_EPOCH_SEC;
  n->tz     = CFG_TZ_HOURS;
}

static inline double skyClampd(double v, double a, double b) { return v < a ? a : (v > b ? b : v); }

// 约束到合法范围
inline void skyConfigClamp(SkyConfig *c) {
  c->lat = skyClampd(c->lat, -90, 90);
  c->lon = skyClampd(c->lon, -180, 180);

  double az = fmod(c->camAz, 360.0);
  if (az < 0) az += 360.0;
  c->camAz = az;

  c->fov = skyClampd(c->fov, 10, 120);
  // 视线中心不能越过天顶：底边高度角上限 = 90 − 垂直半视场
  // （横屏半视场 = fov/2；竖屏高边更长，半视场更大，上限相应更低）
  c->baseAlt = skyClampd(c->baseAlt, 0, 90 - skyHalfV(c));
  c->twinkle = skyClampd(c->twinkle, 0, 1);
  c->haze    = skyClampd(c->haze, 0, 1);

  c->rot = ((c->rot % 4) + 4) % 4;
}

inline void netConfigClamp(NetConfig *n) {
  n->epoch = (n->epoch > 1e9 && n->epoch < 4.1e9) ? n->epoch : (double)CFG_EPOCH_SEC;
  n->tz    = skyClampd(n->tz, -12, 14);
}
