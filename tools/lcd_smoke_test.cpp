// ---------------------------------------------------------------------------
// 点屏冒烟测试（备份件，不参与构建：PlatformIO 只编译 src/）
// 原始位置 src/main.cpp，被正式的天空渲染主程序替换后移到这里留存。
// 需要时把它拷回 src/main.cpp 覆盖，即可重新验证"屏幕能否红/绿/蓝"。
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include "Arduino_GFX_Library.h"

#define PIN_LCD_MISO  5
#define PIN_LCD_MOSI  6
#define PIN_LCD_SCLK  7
#define PIN_LCD_CS    14
#define PIN_LCD_DC    15
#define PIN_LCD_RST   21
#define PIN_BK_LIGHT  22

Arduino_DataBus *bus = new Arduino_ESP32SPI(PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_SCLK,
                                            PIN_LCD_MOSI, PIN_LCD_MISO);
Arduino_GFX *gfx = new Arduino_ST7789(bus, PIN_LCD_RST, 0 /*rotation*/, true /*IPS*/,
                                      240, 240, 0, 0, 0, 0);

void setup()
{
  Serial.begin(115200);
  delay(500);
  Serial.println("[smoke] boot");

  ledcAttach(PIN_BK_LIGHT, 1000, 10);
  ledcWrite(PIN_BK_LIGHT, 1000);

  bool ok = gfx->begin(80000000);
  Serial.printf("[smoke] gfx->begin = %d (w=%d h=%d)\n", ok, gfx->width(), gfx->height());

  gfx->fillScreen(RGB565_RED);
  Serial.println("[smoke] RED");
  delay(800);

  gfx->fillScreen(RGB565_GREEN);
  Serial.println("[smoke] GREEN");
  delay(800);

  gfx->fillScreen(RGB565_BLUE);
  Serial.println("[smoke] BLUE");
  delay(800);

  gfx->fillScreen(RGB565_BLACK);
  Serial.println("[smoke] done");
}

void loop()
{
  static uint32_t n = 0;
  Serial.printf("[smoke] heartbeat %lu  heap=%lu\n", (unsigned long)n++, (unsigned long)ESP.getFreeHeap());
  delay(1000);
}
