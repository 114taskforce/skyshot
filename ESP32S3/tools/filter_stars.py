#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# 从 HYG v3 过滤出「常规视星等 mag < 4.0」、且与现有星表无重复的恒星，
# 按 ra_h 升序输出同一顺序的 C++ / HTML 两块。坐标 J2000。
import os, re, math, csv

TOOLS = os.path.dirname(os.path.abspath(__file__))
HYG = os.path.join(TOOLS, 'hygdata_v3.csv')
ASTRO = os.path.join(TOOLS, '..', 'src', 'astro.cpp')
OUT_DIR = os.path.join(TOOLS, 'out')
os.makedirs(OUT_DIR, exist_ok=True)

MAG_MIN = 4.0          # 纳入条件：常规 Vmag < 4.0
DEDUP_DEG = 0.05       # 与现有星角距 < 0.05° 视为同一颗，跳过

# ---- 读现有星表（astro.cpp 内 STARS 块）做去重基准 ----
def parse_existing():
    src = open(ASTRO, encoding='utf-8').read()
    start = src.index('const float STARS[')
    end = src.index('};', start)
    block = src[start:end]
    stars = []
    for ra, dec, mag, bv in re.findall(r'\{([-\d.]+)\s*,\s*([-\d.]+)\s*,\s*([-\d.]+)\s*,\s*([-\d.]+)\}', block):
        stars.append((float(ra), float(dec), float(mag), float(bv)))
    return stars

def ang_sep(ra1, dec1, ra2, dec2):
    # 小角近似（度）：只用于 <0.05° 的判断，足够
    dra = (ra1 - ra2) * math.cos(math.radians(0.5 * (dec1 + dec2)))
    dde = dec1 - dec2
    return math.hypot(dra, dde)

def main():
    existing = parse_existing()
    print(f'现有星表: {len(existing)} 颗（去重基准）')

    new = []
    n_dedup = 0
    n_noCI = 0
    with open(HYG, encoding='utf-8', errors='replace') as f:
        r = csv.reader(f)
        next(r)  # header
        for row in r:
            if len(row) <= 16:
                continue
            proper = row[6].strip()
            if proper == 'Sol':
                continue
            try:
                ra = float(row[7]); dec = float(row[8]); mag = float(row[13])
            except ValueError:
                continue
            if not (0 <= ra < 24) or not (-90 <= dec <= 90):
                continue
            if not (mag < MAG_MIN):
                continue
            ci = row[16].strip()
            if ci == '':
                ci = 0.5; n_noCI += 1
            else:
                ci = float(ci)
            ra_h = ra                       # HYG v3 的 ra 列本身就是小时（0..24）
            # 与现有星去重
            if any(ang_sep(ra_h, dec, e[0], e[1]) < DEDUP_DEG for e in existing):
                n_dedup += 1
                continue
            new.append((ra_h, dec, mag, ci))

    new.sort(key=lambda s: s[0])   # ra_h 升序，保证三处一致
    print(f'HYG 里 mag<{MAG_MIN} 且未重复: {len(new)} 颗（跳过重复 {n_dedup}，缺 B-V 默认0.5 共 {n_noCI}）')

    # 输出 C++ 块
    cpp_lines = ['  {%8.3f, %8.3f, %5.2f, %5.2f},' % (ra_h, dec, mag, ci)
                 for ra_h, dec, mag, ci in new]
    with open(os.path.join(OUT_DIR, 'new_stars.cpp'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(cpp_lines) + '\n')

    # 输出 HTML 块（同一顺序，4 空格缩进）
    html = []
    for ra_h, dec, mag, ci in new:
        html.append('    [{:8.3f}, {:8.3f}, {:5.2f}, {:5.2f}],'.format(ra_h, dec, mag, ci))
    with open(os.path.join(OUT_DIR, 'new_stars.html'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(html) + '\n')

    print(f'输出: {OUT_DIR}\\new_stars.cpp ({len(new)} 行) / new_stars.html ({len(new)} 行)')
    print(f'新 N_STARS = {len(existing) + len(new)}')

if __name__ == '__main__':
    main()