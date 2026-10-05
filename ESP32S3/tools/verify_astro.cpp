// ===========================================================================
//  PC 端交叉验证工具（不参与固件编译，放在 src/ 之外）
//
//  编译：g++ -O2 -I../src -I../include verify_astro.cpp ../src/astro.cpp -o verify_astro
//  运行：./verify_astro > pc.csv
//
//  输出：以 CFG_EPOCH_SEC 为起点，10 分钟步长扫 24 小时的 CSV，
//        与 Node 抽取出的 sunset.html 同名函数结果逐点比对。
// ===========================================================================
#include <stdio.h>
#include "../src/astro.h"
#include "../include/config.h"

#define STEP_SEC 600
#define N_STEP   144     // 24h / 10min

int main(void) {
  astroInit();

  printf("sec,sunAlt,sunAz,moonRA,moonDec,eqAlt,eqAz,moonAlt,moonAz,illum,phase,waxing");
  for (int s = 0; s < 6; s++) printf(",s%dAlt,s%dAz", s, s);
  printf("\n");

  for (int i = 0; i < N_STEP; i++) {
    const long sec = (long)CFG_EPOCH_SEC + (long)i * STEP_SEC;
    const double jd = (double)sec / 86400.0 + 2440587.5;

    const SkyPos sun = sunPosition(jd, CFG_LAT, CFG_LON);
    const MoonPos mp = moonPosition(jd);
    const SkyPos eq  = equatorialToAltAz(mp.ra, mp.dec, CFG_LAT, CFG_LON, jd);
    const SkyPos mo  = moonAltAz(jd, CFG_LAT, CFG_LON);
    const MoonPhase ph = moonPhase(jd);

    printf("%ld,%.10f,%.10f,%.10f,%.10f,%.10f,%.10f,%.10f,%.10f,%.10f,%.10f,%d",
           sec, sun.alt, sun.az, mp.ra, mp.dec, eq.alt, eq.az,
           mo.alt, mo.az, ph.illumination, ph.phaseAngle, ph.waxing ? 1 : 0);

    for (int s = 0; s < 6; s++) {
      const SkyPos sp = equatorialToAltAz(STARS[s][0], STARS[s][1],
                                          CFG_LAT, CFG_LON, jd);
      printf(",%.10f,%.10f", sp.alt, sp.az);
    }
    printf("\n");
  }
  return 0;
}