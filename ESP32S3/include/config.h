#pragma once

// ===========================================================================
//  默认参数（改完重新编译烧录）
//
//  上电流程：连 WiFi（STA）→ 用 HTTP API 校时 → 关闭 WiFi → 按「真实时间 +
//  本机经纬度」渲染天空。连不上时屏幕显示状态提示。
//  长按 BOOT 2 秒可开配置热点，网页上改的参数存 NVS（这里的值是默认值/恢复默认用）。
// ===========================================================================

// ---- WiFi（STA）----
// 自己的 WiFi / 校时设置写进 include/config.local.h（已在 .gitignore 里），
// 这里保留占位符版本；没有那个文件时就是下面这些值
#if __has_include("config.local.h")
#include "config.local.h"
#endif

#ifndef CFG_WIFI_SSID
#define CFG_WIFI_SSID   ""
#endif
#ifndef CFG_WIFI_PASS
#define CFG_WIFI_PASS   "你的WiFi密码"
#endif

// ---- 校时（只用 HTTP API，不用 NTP）----
// 连上 WiFi 后请求这个地址：优先解析响应体里的 13 位毫秒时间戳
// （本接口返回 {"api":"mtop.common.getTimestamp",...,"data":{"t":"1790669461369"}}），
// 解析不到就退回读 HTTP 响应的 Date 头
#define CFG_TIME_API_URL "http://acs.m.taobao.com/gw/mtop.common.getTimestamp/"
// 校时成功后关闭 WiFi（省电、无射频）。要定时再校时就把这里改成秒数
// （如 21600 = 每 6 小时重新联网校一次），0 = 一直不再联网
#define CFG_TIME_RESYNC_SEC 0

// ---- 手动时间模式的默认值（网页里选「手动 · 固定时间」时用）----
// CFG_EPOCH_SEC：Unix 秒（UTC）。1790251200 = 2026-09-24 20:00 (CST)
#define CFG_EPOCH_SEC   1790251200L
#define CFG_TZ_HOURS    8.0           // 时区偏移

// ---- 地点与相机 ----
#define CFG_LAT         39.9          // 纬度，北正
#define CFG_LON         116.4         // 经度，东正
#define CFG_CAM_AZ      270.0         // 面对方向：方位角，正北 0°，顺时针
                                      // 270 = 正西（看日落/金色时刻）；90 = 正东（看日出）；180 = 正南
#define CFG_FOV         68.0          // 视场角
#define CFG_BASE_ALT    0.0           // 底边高度角
#define CFG_TWINKLE     0.55          // 闪烁强度，0 = 不闪
#define CFG_HAZE        0.15          // 大气气溶胶（维纳斯带强度），0..1

// ---- 刷新节奏 ----
#define CFG_FPS         20            // 闪烁刷新率
#define CFG_REFRESH_SEC 60            // 天体位置重算周期（秒）

// ===========================================================================
//  引脚 —— 本工程仅支持微雪 ESP32-S3-LCD-2.8（ST7789 320×240 横屏）
// ===========================================================================
#if defined(CONFIG_IDF_TARGET_ESP32S3)
// ST7789 原生 240×320，SPI 单写（无 MISO 引出）
#define PIN_LCD_MISO  -1            // 屏幕 SPI 无 MISO（单写）
#define PIN_LCD_MOSI  45
#define PIN_LCD_SCLK  40
#define PIN_LCD_CS    42
#define PIN_LCD_DC    41
#define PIN_LCD_RST   39
#define PIN_BK_LIGHT  5
#define LCD_SPI_HZ    80000000

// BOOT 按键（按下接地）。短按 = 重新联网校时；长按 2 秒 = 开配置热点
#define PIN_BTN_BOOT  0

// 屏幕方向（全工程唯一开关：画布尺寸与投影都跟它走，见 include/settings.h）
//   1 / 3 = 横屏 320×240（宽幅看日落）
//   0 / 2 = 竖屏 240×320（立式摆放：垂直看到更多天空，水平视野与横屏一致）
// fov 语义 = 短边角宽（横屏是高度、竖屏是宽度）
#define LCD_ROTATION  0

#elif defined(ARDUINO)
#error "本工程仅支持 ESP32-S3（微雪 ESP32-S3-LCD-2.8，board=esp32-s3-devkitc-1），已移除 ESP32-C6 支持"
#endif