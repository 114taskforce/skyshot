// ---------------------------------------------------------------------------
// sunset —— ESP32-C6 + ST7789 240×240 天空 / 落日显示
//
//   上电：连 WiFi（STA）→ 用 HTTP API 校时 → 关闭 WiFi → 按「真实时间 +
//         本机经纬度」渲染天空。参数默认值在 config.h 硬编码。
//
//   BOOT 键（GPIO9）：
//     单击 → 重新联网校时（校完再次关闭 WiFi）
//     长按 2 秒 → 打开配置热点 sunset-XXXX（http://192.168.4.1），
//            网页改的参数存 NVS；30 秒没有设备连接就自动关闭热点
//
//   算法：astro.cpp（天文）/ sky.cpp（配色）/ proj.cpp（投影）/ render.cpp（合成）
//   网页源码在 web/index.html，改完跑 tools/html2header.py 生成 include/web_ui.h
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <esp_mac.h>

#include "Arduino_GFX_Library.h"
#include "config.h"
#include "settings.h"
#include "astro.h"
#include "proj.h"
#include "render.h"
#include "web_ui.h"

static Arduino_DataBus *bus = new Arduino_ESP32SPI(PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_SCLK,
                                                   PIN_LCD_MOSI, PIN_LCD_MISO);
static Arduino_GFX *gfx = new Arduino_ST7789(bus, PIN_LCD_RST, LCD_ROTATION, true /*IPS*/,
                                             240, 240, 0, 0, 0, 0);

static SkyConfig g_cfg;
static NetConfig g_netCfg;
static FrameState g_fs;
static uint16_t   g_sky[RENDER_RES * RENDER_RES];   // 天空层缓存（每分钟重建一次）
static uint16_t   g_row565[RENDER_RES];

static uint32_t g_lastFrame   = 0;
static uint32_t g_lastRefresh = 0;
static uint32_t g_fps         = 0;

// ===========================================================================
//  时间：校时成功后用「同步时刻 + millis() 自走」
// ===========================================================================
static bool     g_timeSynced   = false;
static double   g_epochAtSync  = 0;
static uint32_t g_millisAtSync = 0;
static uint32_t g_lastSync     = 0;

static double nowSec(void) {
  return g_epochAtSync + (millis() - g_millisAtSync) / 1000.0;
}
static double jdOf(double sec) { return sec / 86400.0 + 2440587.5; }

static void refresh(void);
static void enterWifiState(void);
static void apStop(void);
static void applyManualTime(void);

// 屏幕状态提示：状态一变就重画一次
static uint32_t g_statusRev   = 0;
static uint32_t g_statusDrawn = 0xFFFFFFFFu;

// ===========================================================================
//  配置（NVS；config.h 里的值是默认值）
// ===========================================================================
static Preferences g_prefs;

static void configLoad(void) {
  skyConfigDefaults(&g_cfg);
  netConfigDefaults(&g_netCfg);
  // 读写方式打开：首次上电命名空间还不存在，只读会报 NOT_FOUND
  if (g_prefs.begin("sunset", false)) {
    g_cfg.lat     = g_prefs.getDouble("lat",     g_cfg.lat);
    g_cfg.lon     = g_prefs.getDouble("lon",     g_cfg.lon);
    g_cfg.camAz   = g_prefs.getDouble("camAz",   g_cfg.camAz);
    g_cfg.baseAlt = g_prefs.getDouble("baseAlt", g_cfg.baseAlt);
    g_cfg.fov     = g_prefs.getDouble("fov",     g_cfg.fov);
    g_cfg.twinkle = g_prefs.getDouble("twinkle", g_cfg.twinkle);

    g_prefs.getString("ssid", g_netCfg.ssid, sizeof(g_netCfg.ssid));
    g_prefs.getString("pass", g_netCfg.pass, sizeof(g_netCfg.pass));
    g_prefs.getString("url",  g_netCfg.url,  sizeof(g_netCfg.url));
    g_netCfg.manual = g_prefs.getBool("manual", g_netCfg.manual);
    g_netCfg.epoch  = g_prefs.getDouble("epoch", g_netCfg.epoch);
    g_netCfg.tz     = g_prefs.getDouble("tz",    g_netCfg.tz);
    g_prefs.end();
  }
  skyConfigClamp(&g_cfg);
  netConfigClamp(&g_netCfg);
}

static void configSave(void) {
  g_prefs.begin("sunset", false);
  g_prefs.putDouble("lat",     g_cfg.lat);
  g_prefs.putDouble("lon",     g_cfg.lon);
  g_prefs.putDouble("camAz",   g_cfg.camAz);
  g_prefs.putDouble("baseAlt", g_cfg.baseAlt);
  g_prefs.putDouble("fov",     g_cfg.fov);
  g_prefs.putDouble("twinkle", g_cfg.twinkle);

  g_prefs.putString("ssid", g_netCfg.ssid);
  g_prefs.putString("pass", g_netCfg.pass);
  g_prefs.putString("url",  g_netCfg.url);
  g_prefs.putBool("manual", g_netCfg.manual);
  g_prefs.putDouble("epoch", g_netCfg.epoch);
  g_prefs.putDouble("tz",    g_netCfg.tz);
  g_prefs.end();
}

static void configClear(void) {
  g_prefs.begin("sunset", false);
  g_prefs.clear();
  g_prefs.end();
}

// ===========================================================================
//  联网 / 校时状态机
// ===========================================================================
enum NetState { NET_WIFI, NET_TIME, NET_READY };
static NetState g_net        = NET_WIFI;
static uint32_t g_netTimer   = 0;
static uint32_t g_wifiTries  = 0;
static uint32_t g_syncTries  = 0;
static bool     g_wifiFailed = false;

// 配置热点状态（函数定义在后面）
static WebServer server(80);
static bool     g_apMode       = false;
static bool     g_webRunning   = false;
static uint32_t g_apLastClient = 0;
static uint8_t  g_apClients    = 0;
static char     g_apSsid[24]   = {0};
static bool     g_apClosing    = false;  // 正在分步关闭热点
static uint32_t g_apCloseAt    = 0;      // 保存配置后延迟关热点的时间点
static uint32_t g_apClosingAt  = 0;      // 进入"关闭中"的时间点

// 把时间基准设成 epoch（此刻的 millis 记为起点），之后靠 millis() 自走
static void applyTimeBase(double epoch) {
  g_epochAtSync  = epoch;
  g_millisAtSync = millis();
  g_timeSynced   = true;
  g_lastSync     = millis();
}

static bool wifiConfigured(void) {
  return g_netCfg.ssid[0] != 0 && strcmp(g_netCfg.ssid, "你的WiFi名称") != 0;
}

static void enterWifiState(void) {
  g_net = NET_WIFI;
  g_netTimer = millis();
  WiFi.mode(WIFI_STA);                 // 之前可能已关射频（校时成功后）
  WiFi.setSleep(false);
  g_statusRev++;
}

// 立刻发起一轮连接（开机、单击按钮、重试超时都走这里）
static void startStaConnect(void) {
  g_wifiFailed = false;
  g_wifiTries++;
  enterWifiState();
  WiFi.begin(g_netCfg.ssid, g_netCfg.pass);
  Serial.printf("[net] 连接 %s …（第 %lu 次）\n", g_netCfg.ssid, (unsigned long)g_wifiTries);
}

// ---- HTTP 校时（只用 API；解析响应体里的 13 位毫秒戳，退回读 Date 头）----
static bool parseBodyMs(const String &body, double *outEpoch) {
  for (size_t i = 0; i + 13 <= body.length(); ) {
    if (!isdigit(body[i])) { i++; continue; }
    size_t j = i;
    while (j < body.length() && isdigit(body[j])) j++;
    if (j - i == 13) {
      const double ms = strtod(body.substring(i, j).c_str(), nullptr);
      if (ms > 1.6e12 && ms < 4.1e12) {      // 2020..2100 之间才认
        *outEpoch = ms / 1000.0;
        return true;
      }
    }
    i = j;
  }
  return false;
}

// ESP32 的 newlib 没有 timegm，用 Howard Hinnant 的 days-from-civil 自己算
static long daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return (long)era * 146097L + (long)doe - 719468L;
}

static bool parseHttpDate(const char *s, double *outEpoch) {
  int day = 0, year = 0, hh = 0, mm = 0, ss = 0;
  char mon[4] = {0};
  if (sscanf(s, "%*3s, %d %3s %d %d:%d:%d", &day, mon, &year, &hh, &mm, &ss) != 6) return false;
  const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char *m = strstr(months, mon);
  if (!m || day < 1 || day > 31 || year < 2020) return false;

  const int month = (int)(m - months) / 3;          // 0..11
  const long days = daysFromCivil(year, (unsigned)month + 1u, (unsigned)day);
  *outEpoch = (double)(days * 86400L + hh * 3600L + mm * 60L + ss);
  return true;
}

static bool apiSync(double *outEpoch) {
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(g_netCfg.url)) {
    Serial.println("[net] HTTP begin 失败（校时接口地址不对？）");
    return false;
  }
  const char *hdr[] = {"Date"};
  http.collectHeaders(hdr, 1);
  const int code = http.GET();

  bool ok = false;
  if (code == 200) {
    ok = parseBodyMs(http.getString(), outEpoch);
    if (!ok) ok = parseHttpDate(http.header("Date").c_str(), outEpoch);
  } else {
    Serial.printf("[net] HTTP 返回 %d\n", code);
  }
  http.end();
  return ok;
}

static void closeWifi(void) {
  if (g_apMode) return;                // 热点模式下绝不动射频：会把正在回话的连接一起拆掉
  WiFi.disconnect(true);               // 断开并关射频
  WiFi.mode(WIFI_OFF);
}

static void netTick(void) {
  if (g_apMode) return;                // 热点模式优先，暂停 STA 状态机
  if (g_netCfg.manual) return;         // 手动时间模式：不联网

  const uint32_t now = millis();

  switch (g_net) {
    case NET_WIFI:
      if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[net] WiFi 已连上 %s，IP %s\n",
                      WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
        g_wifiFailed = false;
        g_net = NET_TIME;
        g_netTimer = now;
        g_statusRev++;
      } else if (now - g_netTimer >= 8000u) {            // 每 8 秒试一轮
        startStaConnect();
        g_wifiFailed = (g_wifiTries > 1);
      }
      break;

    case NET_TIME: {
      if (WiFi.status() != WL_CONNECTED) { enterWifiState(); break; }

      if (now - g_netTimer < 1000u) break;               // 给 DHCP/DNS 一点时间
      g_netTimer = now;
      g_syncTries++;
      g_statusRev++;

      double epoch = 0;
      if (apiSync(&epoch)) {
        applyTimeBase(epoch);
        g_syncTries = 0;
        g_net = NET_READY;
        g_statusRev++;
        Serial.printf("[net] 校时成功：%.0f（UTC），关闭 WiFi\n", epoch);
        closeWifi();
        refresh();                                       // 立刻按真实时间渲染
      } else if (g_syncTries >= 3) {
        Serial.println("[net] 校时失败，回 WiFi 阶段重试");
        g_wifiFailed = true;
        enterWifiState();
      }
      break;
    }

    case NET_READY:
      if (CFG_TIME_RESYNC_SEC > 0 &&
          millis() - g_lastSync >= (uint32_t)CFG_TIME_RESYNC_SEC * 1000u) {
        Serial.println("[net] 到重新校时时间，重新联网");
        g_syncTries = 0;
        startStaConnect();
      }
      break;
  }
}

// ===========================================================================
//  配置热点（长按 BOOT 2 秒打开，30 秒无连接自动关闭）
// ===========================================================================
static void apStart(void) {
  if (g_apMode) { g_apLastClient = millis(); return; }     // 已开着：只续期

  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  snprintf(g_apSsid, sizeof(g_apSsid), "sunset-%02X%02X", mac[4], mac[5]);

  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  const bool ok = WiFi.softAP(g_apSsid);                   // 开放热点（无密码）
  Serial.printf("[ap] 热点 %s %s，配置页 http://%s/\n", g_apSsid,
                ok ? "已开启" : "开启失败", WiFi.softAPIP().toString().c_str());

  if (!g_webRunning) { server.begin(); g_webRunning = true; }

  g_apMode       = true;
  g_apClosing    = false;
  g_apClients    = 0;
  g_apLastClient = millis();
  g_statusRev++;
}

// 关闭请求：先停网页服务，真正的接口关闭交给 apTick 下一步做。
// 直接一步 WiFi.mode(WIFI_OFF) 会在手机还挂着 keep-alive 连接时把 lwIP 搞崩
// （esp_pbuf_free / netconn_drain 里跳飞到地址 0）。
static void apStop(void) {
  if (!g_apMode || g_apClosing) return;
  g_apClosing = true;
  g_apClosingAt = millis();
  g_statusRev++;
  Serial.println("[ap] 正在关闭：先停网页服务");
  if (g_webRunning) { server.stop(); g_webRunning = false; }
}

static void apTick(void) {
  if (!g_apMode) return;

  if (g_apClosing) {
    if (millis() - g_apClosingAt < 400u) return;           // 留时间让 socket 收尾
    WiFi.softAPdisconnect(true);                           // 关 AP 接口
    // 刻意不调用 WiFi.mode(WIFI_OFF)：射频留在原地，避免拆 netif 时踩坏 lwIP
    g_apMode = false;
    g_apClosing = false;
    g_statusRev++;
    Serial.println("[ap] 热点已关闭");
    if (!g_netCfg.manual) {
      startStaConnect();                                   // 回去联网校时（可能是刚保存的新 WiFi）
    }
    if (g_timeSynced) {
      refresh();                                           // 立刻重建背景并推屏，别停在热点状态页
    }
    return;
  }

  // 保存配置后延迟关热点（等响应送到手机）
  if (g_apCloseAt && (int32_t)(millis() - g_apCloseAt) >= 0) {
    g_apCloseAt = 0;
    Serial.println("[ap] 配置已保存，关闭热点");
    apStop();
    return;
  }

  const uint8_t clients = WiFi.softAPgetStationNum();
  if (clients != g_apClients) {
    g_apClients = clients;
    g_statusRev++;
    Serial.printf("[ap] 已连接设备数 %u\n", clients);
  }

  if (clients > 0) {
    g_apLastClient = millis();
  } else if (millis() - g_apLastClient > 30000u) {
    Serial.println("[ap] 30 秒无设备连接，自动关闭热点");
    apStop();
  }
}

// ===========================================================================
//  Web 接口
// ===========================================================================
static void handleRoot(void) {
  // 页面 gzip 压缩后内嵌，直接吐二进制（省 flash、省传输时间）
  server.sendHeader("Content-Encoding", "gzip");
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html; charset=utf-8", (PGM_P)WEB_UI_GZ, WEB_UI_GZ_LEN);
}

static bool argD(const char *name, double *dst) {
  if (!server.hasArg(name)) return false;
  const String v = server.arg(name);
  if (v.length() == 0) return false;
  *dst = v.toDouble();
  return true;
}

// 空串一律当"不改动"（网页里 WiFi 密码留空就是保持原密码）
static bool argS(const char *name, char *dst, size_t cap) {
  if (!server.hasArg(name)) return false;
  const String v = server.arg(name);
  if (v.length() == 0) return false;
  skyStrCpy(dst, cap, v.c_str());
  return true;
}

// 手动模式：基准时间从"应用配置的这一刻"起算
static void applyManualTime(void) {
  if (!g_netCfg.manual) return;
  applyTimeBase(g_netCfg.epoch);
  g_net = NET_READY;
  closeWifi();
  refresh();
}

// 应用新配置：重建投影表 + 立刻重画（保存后热点会自动关闭）
static void applyConfig(void) {
  const uint32_t t0 = millis();
  skyConfigClamp(&g_cfg);
  netConfigClamp(&g_netCfg);
  configSave();
  renderInit(&g_cfg);

  if (g_netCfg.manual) {
    applyManualTime();
  }
  if (g_timeSynced) refresh();
  g_statusRev++;

  Serial.printf("[web] 已应用配置（%lu ms）camAz=%.1f fov=%.1f baseAlt=%.1f lat=%.4f "
                "lon=%.4f twinkle=%.2f | %s ssid=%s\n",
                (unsigned long)(millis() - t0), g_cfg.camAz, g_cfg.fov, g_cfg.baseAlt,
                g_cfg.lat, g_cfg.lon, g_cfg.twinkle,
                g_netCfg.manual ? "手动时间" : "联网校时", g_netCfg.ssid);
}

static void handleSave(void) {
  argD("lat", &g_cfg.lat);
  argD("lon", &g_cfg.lon);
  argD("camAz", &g_cfg.camAz);
  argD("baseAlt", &g_cfg.baseAlt);
  argD("fov", &g_cfg.fov);
  argD("twinkle", &g_cfg.twinkle);

  argS("ssid", g_netCfg.ssid, sizeof(g_netCfg.ssid));
  argS("pass", g_netCfg.pass, sizeof(g_netCfg.pass));
  argS("url",  g_netCfg.url,  sizeof(g_netCfg.url));
  double d = 0;
  if (argD("manual", &d)) g_netCfg.manual = (d != 0);
  argD("epoch", &g_netCfg.epoch);
  argD("tz",    &g_netCfg.tz);

  // 先回话再应用配置：应用过程可能重建投影表、甚至用到射频，
  // 反过来做会把响应堵在路上（网页会误报"连不上设备"）
  server.send(200, "application/json; charset=utf-8", "{\"ok\":1}");
  applyConfig();
  g_apCloseAt = millis() + 1000;       // 等响应送到手机，再关热点
}

static void handleReset(void) {
  configClear();
  skyConfigDefaults(&g_cfg);
  netConfigDefaults(&g_netCfg);
  server.send(200, "application/json; charset=utf-8", "{\"ok\":1}");
  applyConfig();
  Serial.println("[web] 已恢复默认配置");
  g_apCloseAt = millis() + 1000;
}

// JSON 字符串转义（WiFi 名称里可能有引号/反斜杠）
static void jsonEscape(const char *src, char *dst, size_t cap) {
  size_t j = 0;
  for (size_t i = 0; src[i] && j + 2 < cap; i++) {
    if (src[i] == '"' || src[i] == '\\') dst[j++] = '\\';
    dst[j++] = src[i];
  }
  dst[j] = 0;
}

// 当前状态 + 当前配置（网页据此预填表单、判断太阳在不在画面里）
static void handleState(void) {
  double sunAlt = 0, sunAz = 0, moonAlt = 0, moonAz = 0, moonIllum = 0;
  int moonWaxing = 0;
  if (g_timeSynced) {
    const double jd = jdOf(nowSec());
    const SkyPos sun = sunPosition(jd, g_cfg.lat, g_cfg.lon);
    const SkyPos moon = moonAltAz(jd, g_cfg.lat, g_cfg.lon);
    const MoonPhase ph = moonPhase(jd);
    sunAlt = sun.alt;
    sunAz = sun.az;
    moonAlt = moon.alt;
    moonAz = moon.az;
    moonIllum = ph.illumination;
    moonWaxing = ph.waxing ? 1 : 0;
  }

  char ssid[80], url[200];
  jsonEscape(g_netCfg.ssid, ssid, sizeof(ssid));
  jsonEscape(g_netCfg.url, url, sizeof(url));

  char buf[720];
  snprintf(buf, sizeof(buf),
           "{\"synced\":%d,\"manual\":%d,\"now\":%.0f,\"fps\":%lu,\"heap\":%lu,\"stars\":%d,"
           "\"sunAlt\":%.2f,\"sunAz\":%.2f,\"moonAlt\":%.2f,\"moonAz\":%.2f,"
           "\"moonIllum\":%.3f,\"moonWaxing\":%d,"
           "\"lat\":%.6f,\"lon\":%.6f,\"camAz\":%.2f,\"baseAlt\":%.2f,\"fov\":%.2f,"
           "\"twinkle\":%.3f,\"ssid\":\"%s\",\"url\":\"%s\",\"tz\":%.2f,\"epoch\":%.0f,"
           "\"ap\":%d,\"apClients\":%u}",
           g_timeSynced ? 1 : 0, g_netCfg.manual ? 1 : 0,
           g_timeSynced ? nowSec() : 0.0,
           (unsigned long)g_fps, (unsigned long)ESP.getFreeHeap(), g_fs.nStars,
           sunAlt, sunAz, moonAlt, moonAz, moonIllum, moonWaxing,
           g_cfg.lat, g_cfg.lon, g_cfg.camAz, g_cfg.baseAlt, g_cfg.fov, g_cfg.twinkle,
           ssid, url, g_netCfg.tz, g_netCfg.epoch,
           g_apMode ? 1 : 0, (unsigned)g_apClients);
  server.send(200, "application/json; charset=utf-8", buf);
}

static void startWebServer(void) {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/save", HTTP_GET, handleSave);
  server.on("/api/reset", HTTP_GET, handleReset);
  server.on("/api/state", HTTP_GET, handleState);
  server.onNotFound([]() { server.send(404, "text/plain", "not found"); });
}

// ===========================================================================
//  BOOT 按键：短按重新校时，长按 2 秒开配置热点
// ===========================================================================
static bool     g_btnDown     = false;
static uint32_t g_btnEdgeMs   = 0;
static uint32_t g_btnDownAt   = 0;
static bool     g_btnLongDone = false;   // 本次按住是否已经触发过长按

#define BTN_LONG_MS 2000u                // 长按判定时长

static void onSingleClick(void) {
  Serial.println("[btn] 短按：重新联网校时");
  if (g_netCfg.manual) {
    Serial.println("[btn] 当前是手动时间模式，忽略（断开热点后在网页里切回自动）");
    return;
  }
  if (g_apMode) {                       // 热点开着：先按分步流程关掉，关完会自动重连
    apStop();
    return;
  }
  if (!wifiConfigured()) {
    Serial.println("[btn] WiFi 还没配置，长按 2 秒进配置热点");
    return;
  }
  g_syncTries = 0;
  g_wifiTries = 0;
  startStaConnect();
}

static void onLongPress(void) {
  Serial.println("[btn] 长按：打开配置热点");
  apStart();
}

static void btnTick(void) {
  const uint32_t now = millis();
  const bool down = (digitalRead(PIN_BTN_BOOT) == LOW);

  if (down != g_btnDown && now - g_btnEdgeMs > 30u) {      // 30 ms 去抖
    g_btnDown = down;
    g_btnEdgeMs = now;
    if (down) {
      g_btnDownAt = now;
      g_btnLongDone = false;
    } else if (!g_btnLongDone) {
      onSingleClick();                                     // 松手时还没到长按：算短按
    }
  }

  if (g_btnDown && !g_btnLongDone && now - g_btnDownAt >= BTN_LONG_MS) {
    g_btnLongDone = true;
    onLongPress();
  }
}

// ===========================================================================
//  渲染
// ===========================================================================
// 重算整帧几何 / 配色 + 重建背景层（天空色 + 太阳 + 月亮）
// 重建后标记需要推一次屏
static bool g_pushNeeded = false;

static void refresh(void) {
  const uint32_t t0 = millis();
  frameStateCompute(&g_fs, &g_cfg, jdOf(nowSec()));
  const uint32_t t1 = millis();
  renderSky(g_sky, &g_fs);
  const uint32_t t2 = millis();
  g_pushNeeded = true;

  Serial.printf("[sky] sun alt=%.1f az=%.1f | moon %.0f%% 可见=%d | stars=%d | 重算 %lums 背景 %lums\n",
                g_fs.sunAlt, g_fs.sunAz, g_fs.moon.illum * 100.0, g_fs.moon.visible ? 1 : 0,
                g_fs.nStars, (unsigned long)(t1 - t0), (unsigned long)(t2 - t1));
}

// 合成一帧并推屏：缓存背景行 → 叠星
static void pushFrame(double tSec) {
  frameTwinkle(&g_fs, tSec, g_cfg.twinkle);
  for (int y = 0; y < RENDER_RES; y++) {
    renderFrameRow565(g_row565, y, &g_fs, g_sky);
    gfx->draw16bitRGBBitmap(0, y, g_row565, RENDER_RES, 1);
  }
}

// 状态页（屏幕自带字体只有 ASCII，提示用英文）：热点模式 / 未校时 / 校时中
static void drawStatusScreen(void) {
  if (g_statusDrawn == g_statusRev) return;
  g_statusDrawn = g_statusRev;

  gfx->fillScreen(RGB565_BLACK);

  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(2);

  if (g_apMode) {                                   // 热点模式：告诉用户怎么连
    gfx->setCursor(16, 48);
    gfx->print("Config AP");
    gfx->setTextColor(RGB565_CYAN);
    gfx->setCursor(16, 84);
    gfx->print(g_apSsid);
    gfx->setTextColor(RGB565_WHITE);
    gfx->setCursor(16, 116);
    gfx->print("192.168.4.1");

    char buf[32];
    if (g_apClosing) {
      snprintf(buf, sizeof(buf), "closing...");
      gfx->setTextColor(RGB565_LIGHTGREY);
    } else {
      snprintf(buf, sizeof(buf), "clients: %u", (unsigned)g_apClients);
      gfx->setTextColor(g_apClients ? RGB565_GREEN : RGB565_LIGHTGREY);
    }
    gfx->setCursor(16, 148);
    gfx->print(buf);

    gfx->setTextColor(RGB565_LIGHTGREY);
    gfx->setTextSize(1);
    gfx->setCursor(16, 178);
    gfx->print("open http://192.168.4.1 in browser");
    gfx->setCursor(16, 190);
    gfx->print("auto close after 30s idle");
    return;
  }

  gfx->setCursor(16, 54);
  gfx->print("Sunset sky");

  char buf[32];
  const char *line1 = "";
  bool warn = false;
  if (!wifiConfigured()) {
    line1 = "WiFi not set";
    warn = true;
  } else if (g_net == NET_WIFI) {
    snprintf(buf, sizeof(buf), "WiFi %s #%lu", g_wifiFailed ? "FAILED" : "...",
             (unsigned long)g_wifiTries);
    line1 = buf;
    warn = g_wifiFailed;
  } else if (g_net == NET_TIME) {
    snprintf(buf, sizeof(buf), "Time sync #%lu", (unsigned long)g_syncTries);
    line1 = buf;
  }

  gfx->setTextColor(warn ? RGB565_RED : RGB565_CYAN);
  gfx->setCursor(16, 96);
  gfx->print(line1);

  gfx->setTextColor(RGB565_LIGHTGREY);
  gfx->setTextSize(1);
  gfx->setCursor(16, 140);
  gfx->print("check CFG_WIFI_SSID/PASS in config.h");
  gfx->setCursor(16, 154);
  gfx->print("single click BOOT = resync time");
  gfx->setCursor(16, 166);
  gfx->print("hold BOOT 2s = config AP");
}

static void reportPerf(uint32_t frameMs) {
  static uint32_t frames = 0, sum = 0, start = 0;
  frames++;
  sum += frameMs;
  const uint32_t now = millis();
  if (start == 0) {
    start = now;
  } else if (now - start >= 1000u) {
    g_fps = frames;
    Serial.printf("[sky] %lu 帧/秒  平均 %.1f ms/帧  heap=%lu\n",
                  (unsigned long)frames, (double)sum / frames,
                  (unsigned long)ESP.getFreeHeap());
    frames = 0;
    sum = 0;
    start = now;
  }
}

// ===========================================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("[sky] boot");

  ledcAttach(PIN_BK_LIGHT, 1000, 10);
  ledcWrite(PIN_BK_LIGHT, 1000);

  pinMode(PIN_BTN_BOOT, INPUT_PULLUP);

  const bool ok = gfx->begin(LCD_SPI_HZ);
  Serial.printf("[sky] gfx->begin = %d (%dx%d)\n", ok, gfx->width(), gfx->height());

  astroInit();
  configLoad();                         // NVS 有就用 NVS，没有就用 config.h

  const uint32_t tInit = millis();
  renderInit(&g_cfg);
  Serial.printf("[sky] 建 kk 查表 %lu ms（天空缓存 %u KB）\n",
                (unsigned long)(millis() - tInit),
                (unsigned)sizeof(g_sky) / 1024u);

  startWebServer();

  if (g_netCfg.manual) {
    applyTimeBase(g_netCfg.epoch);      // 手动时间：开机就用设定时间，不联网
    g_net = NET_READY;
    Serial.println("[net] 手动时间模式，不联网校时");
  } else if (!wifiConfigured()) {
    Serial.println("[net] WiFi 名称是空的，长按 BOOT 2 秒进配置热点");
    g_net = NET_READY;
    g_wifiFailed = true;
    g_statusRev++;
  } else {
    startStaConnect();
  }
  if (g_timeSynced) {
    refresh();                          // 手动模式：开机立刻按设定时间渲染
  } else {
    drawStatusScreen();
  }

  g_lastFrame = g_lastRefresh = millis();
}

void loop() {
  btnTick();
  apTick();
  netTick();
  if (g_webRunning) server.handleClient();

  if (g_apMode) {                       // 热点模式：停止渲染，只显示热点状态页
    drawStatusScreen();
    delay(20);
    return;
  }

  const uint32_t now = millis();

  if (g_timeSynced) {                   // 校上时间后正常渲染
    if (now - g_lastRefresh >= (uint32_t)CFG_REFRESH_SEC * 1000u) {
      g_lastRefresh = now;
      refresh();                        // 每分钟重建一次背景（天空 + 太阳 + 月亮）
    }

    if (g_pushNeeded) {                 // 背景刚重建过：推一帧
      g_pushNeeded = false;
      g_lastFrame = now;
      const uint32_t t0 = millis();
      pushFrame(millis() / 1000.0);
      reportPerf(millis() - t0);
    } else if (g_fs.nStars > 0 && now - g_lastFrame >= 1000u / CFG_FPS) {
      g_lastFrame = now;                // 有星星才需要逐帧刷新闪烁
      const uint32_t t0 = millis();
      pushFrame(millis() / 1000.0);
      reportPerf(millis() - t0);
    } else {
      delay(5);                         // 无星：画面是静态的，待机（仍响应按键/网页）
    }
  } else {                              // 没校上之前只显示状态页
    drawStatusScreen();
    delay(20);
  }
}
