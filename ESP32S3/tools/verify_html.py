#!/usr/bin/env python3
# ===========================================================================
#  与 sunset.html 的独立对照
#
#  按 sunset.html 的源码**另写一套** Python 参考实现（数据表直接从 HTML 里解析，
#  公式逐条照抄），再与固件渲染结果逐像素/逐星比对。用来回答
#  「ESP 和 HTML 显示的内容到底一不一样」。
#
#  用法（在工程根目录）：
#    python tools/verify_html.py <sec> [camAz=.. fov=.. baseAlt=.. lat=.. lon=..]
#  例：
#    python tools/verify_html.py 1790243400 az=270        # 金色时刻朝西
#
#  说明：固件有意放大了天体尺寸（BODY_MAGNIFY 4，HTML 是 2），
#        参考实现跟着用 4，否则太阳圆盘大小会对不上。
# ===========================================================================
import math
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
HTML = ROOT / 'sunset.html'
EXE = ROOT / 'tools' / 'verify_render.exe'

RAD = math.pi / 180
DEG = 180 / math.pi
W = 320
H = 240

# ── 固件的天体放大倍数（与 src/astro.cpp 保持一致）──
BODY_MAGNIFY = 4.0
SUN_ANG_RAD = 0.2665 * BODY_MAGNIFY
MOON_ANG_RAD = 0.2590 * BODY_MAGNIFY
MIN_BODY_RAD_PX = 5.0

# 固件有意偏离 sunset.html 的地方（否则这些像素会被误判成"不一致"）：
#   1) 天体尺寸放大 4 倍（HTML 是 2 倍）
#   2) 金色时刻三档地平色改得更金黄，见 src/sky.cpp 的注释
SKY_H_OVERRIDE = {
    4.0:  (240, 202, 148),
    1.0:  (250, 182, 108),
    -1.0: (246, 148, 80),
}

TOL = 3.0          # 天空允许的级差（量化 + float/double 差异）


def clamp(v, a, b):
    return a if v < a else (b if v > b else v)


def lerp(a, b, t):
    return a + (b - a) * t


def smoothstep(a, b, x):
    t = clamp((x - a) / (b - a), 0, 1)
    return t * t * (3 - 2 * t)


def cos_ease(t):
    return 0.5 - 0.5 * math.cos(t * math.pi)


def hash1(x):
    n = math.sin(x * 12.9898) * 43758.5453
    return n - math.floor(n)


# ---------------------------------------------------------------------------
# 从 sunset.html 解析数据表（保证与 HTML 完全一致，不靠手抄）
# ---------------------------------------------------------------------------
def parse_tables():
    src = HTML.read_text(encoding='utf-8')

    sky_keys = []
    for m in re.finditer(r'\{\s*a:\s*(-?[\d.]+),\s*z:\s*\[([^\]]+)\],\s*h:\s*\[([^\]]+)\]\s*\}', src):
        z = [float(v) for v in m.group(2).split(',')]
        h = [float(v) for v in m.group(3).split(',')]
        sky_keys.append((float(m.group(1)), z, h))

    h_opp = []
    for m in re.finditer(r'\{\s*a:\s*(-?[\d.]+),\s*c:\s*\[([^\]]+)\]\s*\}', src):
        h_opp.append((float(m.group(1)), [float(v) for v in m.group(2).split(',')]))

    stars = []
    block = src[src.index('const STARS = ['):src.index('const STAR_TWINKLE')]
    for m in re.finditer(r'\[([-\d.\s,]+)\]', block):
        vals = [float(v) for v in m.group(1).split(',') if v.strip()]
        if len(vals) == 4:
            stars.append(vals)

    return sky_keys, h_opp, stars


# ---------------------------------------------------------------------------
# HTML 的天文 / 投影 / 配色公式（逐条照抄 sunset.html）
# ---------------------------------------------------------------------------
def sun_position(sec, lat, lon):
    jd = sec / 86400.0 + 2440587.5
    n = jd - 2451545.0
    L = 280.460 + 0.9856474 * n
    g = (357.528 + 0.9856003 * n) * RAD
    lam = (L + 1.915 * math.sin(g) + 0.020 * math.sin(2 * g)) * RAD
    eps = (23.439 - 0.0000004 * n) * RAD
    alpha = math.atan2(math.cos(eps) * math.sin(lam), math.cos(lam))
    delta = math.asin(math.sin(eps) * math.sin(lam))
    gmst = (280.46061837 + 360.98564736629 * n) * RAD
    H = gmst + lon * RAD - alpha
    phi = lat * RAD
    sin_alt = math.sin(phi) * math.sin(delta) + math.cos(phi) * math.cos(delta) * math.cos(H)
    alt = math.asin(clamp(sin_alt, -1, 1)) * DEG
    az = math.atan2(-math.cos(delta) * math.sin(H),
                    math.cos(phi) * math.sin(delta) - math.sin(phi) * math.cos(delta) * math.cos(H)) * DEG
    return alt, (az + 360) % 360


def equatorial_to_altaz(ra_h, dec_deg, lat_deg, lon_deg, jd):
    g = 280.46061837 + 360.98564736629 * (jd - 2451545.0)
    gmst = ((g % 360) + 360) % 360
    lst = (gmst + lon_deg) * RAD
    ra = ra_h * 15 * RAD
    dec = dec_deg * RAD
    lat = lat_deg * RAD
    H = lst - ra
    sin_alt = math.sin(lat) * math.sin(dec) + math.cos(lat) * math.cos(dec) * math.cos(H)
    s_alt = clamp(sin_alt, -1, 1)
    alt = math.asin(s_alt) * DEG
    cos_alt = max(math.sqrt(max(1 - s_alt * s_alt, 0)), 1e-6)
    cos_lat = max(math.cos(lat), 1e-6)
    sin_az = -math.cos(dec) * math.sin(H) / cos_alt
    cos_az = (math.sin(dec) - math.sin(lat) * s_alt) / (cos_lat * cos_alt)
    az = math.atan2(sin_az, cos_az) * DEG
    return alt, ((az % 360) + 360) % 360


def sky_colors(alt, keys):
    if alt >= keys[0][0]:
        return keys[0][1], keys[0][2]
    for i in range(len(keys) - 1):
        A, B = keys[i], keys[i + 1]
        if A[0] >= alt >= B[0]:
            t = cos_ease((A[0] - alt) / (A[0] - B[0]))
            return ([lerp(A[1][k], B[1][k], t) for k in range(3)],
                    [lerp(A[2][k], B[2][k], t) for k in range(3)])
    return keys[-1][1], keys[-1][2]


def lerp_color_keys(keys, alt):
    if alt >= keys[0][0]:
        return keys[0][1]
    for i in range(len(keys) - 1):
        A, B = keys[i], keys[i + 1]
        if A[0] >= alt >= B[0]:
            t = cos_ease((A[0] - alt) / (A[0] - B[0]))
            return [lerp(A[1][k], B[1][k], t) for k in range(3)]
    return keys[-1][1]


def proj_center_alt(base_alt, fov):
    # 与固件一致：垂直半视场（横屏 = fov/2；竖屏高边更长，要按投影反算）
    return clamp(base_alt + half_v(fov), 0, 90)


def proj_k(fov):
    # fov 绑定屏幕短边（横屏=高、竖屏=宽），与固件 settings.h 的 skyProjK 一致
    return (min(W, H) / 2) / (2 * math.tan((fov / 4) * RAD))


def half_v(fov):
    return 2 * math.atan((H / 2) / (2 * proj_k(fov))) * DEG


def proj_px_per_deg(theta, fov):
    c = math.cos(theta / 2)
    return proj_k(fov) * RAD / (c * c)


def project_sky(az, alt, cam_az, base_alt, fov):
    a0 = proj_center_alt(base_alt, fov) * RAD
    sa0, ca0 = math.sin(a0), math.cos(a0)
    a = alt * RAD
    sa, ca = math.sin(a), math.cos(a)
    d = (((az - cam_az + 540) % 360) - 180) * RAD
    cd, sd = math.cos(d), math.sin(d)
    w = clamp(sa0 * sa + ca0 * ca * cd, -1, 1)
    u = ca * sd
    v = ca0 * sa - sa0 * ca * cd
    sin_t = math.sqrt(u * u + v * v)
    rho = 2 * sin_t / max(1 + w, 1e-9) * proj_k(fov)
    inv = rho / sin_t if sin_t > 1e-12 else 0
    return (W / 2 + u * inv, H / 2 - v * inv, math.atan2(sin_t, w))


# ---------------------------------------------------------------------------
# HTML 的整帧天空渲染（renderSky240），逐像素
# ---------------------------------------------------------------------------
def render_sky_html(sec, lat, lon, cam_az, base_alt, fov, sky_keys, h_opp):
    sun_alt, sun_az = sun_position(sec, lat, lon)

    z, h = sky_colors(sun_alt, sky_keys)
    h_cool = lerp_color_keys(h_opp, sun_alt)

    az_contrast = smoothstep(45, 5, sun_alt)
    glow_k = smoothstep(-8, 10, sun_alt)
    sun_red = smoothstep(12, -1, sun_alt)

    g_g = lerp(245, 130, sun_red)
    g_b = lerp(228, 62, sun_red)
    g_amp = lerp(0.42, 0.30, sun_red)

    # KK_LUT（HTML L928）：alt = i/2 - 90
    kk_lut = []
    for i in range(361):
        alt = i / 2 - 90
        t = clamp(alt, 0, 90) / 90
        kk_lut.append(1 - math.pow(1 - t, 1.9) if alt > 0 else 0.0)

    a0 = proj_center_alt(base_alt, fov) * RAD
    sa0, ca0 = math.sin(a0), math.cos(a0)
    az0 = cam_az * RAD
    k = proj_k(fov)

    sun_x, sun_y, sun_t = project_sky(sun_az, sun_alt, cam_az, base_alt, fov)
    disk_px = max(MIN_BODY_RAD_PX, SUN_ANG_RAD * proj_px_per_deg(sun_t, fov))
    disk_px2 = disk_px * disk_px
    glow_r = 45 / fov * H
    glow_r2 = glow_r * glow_r
    disk_vis = smoothstep(-1.5, 0, sun_alt)

    dc = (255, lerp(255, 214, sun_red), lerp(255, 158, sun_red))
    de = (255, lerp(250, 150, sun_red), lerp(240, 92, sun_red))

    sun_possible = (-glow_r < sun_x < W + glow_r) and (-glow_r < sun_y < H + glow_r)
    csun, ssun = math.cos(sun_az * RAD), math.sin(sun_az * RAD)
    mix_half = 0.5 * az_contrast
    aR = h_cool[0] + (h[0] - h_cool[0]) * 0.5
    aG = h_cool[1] + (h[1] - h_cool[1]) * 0.5
    aB = h_cool[2] + (h[2] - h_cool[2]) * 0.5
    bR = (h[0] - h_cool[0]) * mix_half
    bG = (h[1] - h_cool[1]) * mix_half
    bB = (h[2] - h_cool[2]) * mix_half

    # 地影 + 维纳斯带（与 HTML renderSky240 / 固件 skyPixel 同一公式，照抄 维纳斯.html）
    haze = 0.15
    belt_vis = smoothstep(-1.0, -3.0, sun_alt) * (1 - smoothstep(-6.5, -10.5, sun_alt))
    shadow_alt0 = max(0.0, -sun_alt * 1.15)
    belt_w = 3.5 + 7 * haze
    belt_sat = belt_vis * (0.20 + 0.75 * haze)
    sh_vis = belt_vis * 0.72
    belt_on = belt_sat > 0.012

    frame = bytearray()
    for yy in range(H):
        v = H / 2 - (yy + 0.5)
        dyp = yy + 0.5 - sun_y
        for xx in range(W):
            u = xx + 0.5 - W / 2
            rr = math.sqrt(u * u + v * v)

            # 逐像素反投影（HTML buildBgTables）
            if rr < 1e-6:
                alt_deg, az = a0 * DEG, az0
            else:
                rho_n = rr / k
                t2 = rho_n * rho_n / 4
                sin_t = rho_n / (1 + t2)
                cos_t = (1 - t2) / (1 + t2)
                sin_pa, cos_pa = u / rr, v / rr
                alt_deg = math.asin(clamp(cos_t * sa0 + sin_t * ca0 * cos_pa, -1, 1)) * DEG
                y1 = sin_t * sin_pa
                x1 = cos_t * ca0 - sin_t * sa0 * cos_pa
                az = az0 + math.atan2(y1, x1)

            cm = math.cos(az) * csun + math.sin(az) * ssun
            cr = aR + bR * cm
            cg = aG + bG * cm
            cb = aB + bB * cm

            kk = kk_lut[int(clamp(round((alt_deg + 90) * 2), 0, 360))]
            cr += (z[0] - cr) * kk
            cg += (z[1] - cg) * kk
            cb += (z[2] - cb) * kk

            # 地影 + 维纳斯带（公式照抄 维纳斯.html renderSky240）
            if belt_on:
                cpix = -cm                       # cos(像素方位 − 反日方位)
                if cpix > -0.05:
                    # 东西两侧边缘平滑：更宽的平滑范围，与固件/HTML 一致
                    anti_w = smoothstep(-0.1, 0.9, cpix)

                    # 该方位上的阴影顶高度：反日点最高，向两侧按 sqrt(cpix) 收窄
                    shadow_top_h = shadow_alt0 * math.sqrt(max(0.0, cpix))

                    # 地球阴影：位于该方位阴影顶之下，乘法压暗
                    if shadow_top_h > 0.0 and alt_deg < shadow_top_h:
                        sh = anti_w * sh_vis * smoothstep(shadow_top_h, shadow_top_h - 1.8, alt_deg)
                        if sh > 0.004:
                            cr *= 1 - 0.46 * sh
                            cg *= 1 - 0.38 * sh
                            cb *= 1 - 0.14 * sh

                    # 维纳斯带：贴着阴影顶上方，高斯剖面
                    belt_c = shadow_top_h + 2.5 + 7 * haze
                    belt_lo = belt_c - 2.6 * belt_w
                    belt_hi = belt_c + 2.6 * belt_w
                    if alt_deg > belt_lo and alt_deg < belt_hi:
                        gd = (alt_deg - belt_c) / belt_w
                        g = math.exp(-gd * gd)
                        amt = g * belt_sat * anti_w * 0.85
                        if amt > 0.004:
                            pk = smoothstep(0.3, 1.0, g) * 0.4
                            cr += (255.0 - cr) * amt
                            cg += (lerp(150, 196, pk) - cg) * amt
                            cb += (lerp(150, 188, pk) - cb) * amt

            if sun_possible:
                dxp = xx + 0.5 - sun_x
                d2p = dxp * dxp + dyp * dyp
                if glow_k > 0.003 and d2p < glow_r2:
                    t = math.sqrt(d2p / glow_r2)
                    uu = 1 - t
                    glow = uu * uu * uu * uu * glow_k
                    if glow > 0.001:
                        cr += 255 * glow * g_amp
                        cg += g_g * glow * g_amp
                        cb += g_b * glow * g_amp
                cr = min(cr, 255)
                cg = min(cg, 255)
                cb = min(cb, 255)
                if disk_vis > 0.005 and d2p < disk_px2:
                    t = math.sqrt(d2p / disk_px2)
                    s = 1 - t * t
                    disk = s * s * disk_vis
                    cw = math.sqrt(1 - t * t)
                    cR = de[0] + (dc[0] - de[0]) * cw
                    cG = de[1] + (dc[1] - de[1]) * cw
                    cB = de[2] + (dc[2] - de[2]) * cw
                    cr += (cR - cr) * disk
                    cg += (cG - cg) * disk
                    cb += (cB - cb) * disk

            frame += bytes((int(clamp(cr, 0, 255)), int(clamp(cg, 0, 255)), int(clamp(cb, 0, 255))))

    return sun_alt, sun_az, bytes(frame)


# ---------------------------------------------------------------------------
# HTML 的星表（drawStars 到强度计算），返回 [(x, y, intensity, r, g, b)]
# ---------------------------------------------------------------------------
def star_list_html(sec, lat, lon, cam_az, base_alt, fov, sun_alt, twinkle_amt, t_sec, stars):
    star_vis = smoothstep(-8, -16, sun_alt)
    if star_vis < 0.01:
        return []
    jd = sec / 86400.0 + 2440587.5
    mx = W * 0.05
    my = H * 0.05
    out = []
    for i, st in enumerate(stars):
        ra_h, dec, mag, bv = st
        alt, az = equatorial_to_altaz(ra_h, dec, lat, lon, jd)
        if alt < 0:
            continue
        x, y, _ = project_sky(az, alt, cam_az, base_alt, fov)
        if x < -mx or x > W + mx or y < -my or y > H + my:
            continue
        edge = min(smoothstep(-mx, mx, x), smoothstep(W + mx, W - mx, x),
                   smoothstep(-my, my, y), smoothstep(H + my, H - my, y))
        horizon = smoothstep(0, 10, alt)
        intensity = mag_to_intensity(mag) * star_vis * horizon * edge
        if intensity < 0.02:
            continue
        out.append((x, y, intensity, mag, bv))
    return out


def mag_to_intensity(mag):
    flux = math.pow(2.512, -mag)
    return clamp(math.pow(flux, 0.45), 0, 1)


def bv_to_color(bv):
    if bv < 0.0:
        t = clamp((bv + 0.4) / 0.4, 0, 1)
        return (lerp(155, 200, t), lerp(180, 215, t), 255.0)
    if bv < 0.65:
        t = bv / 0.65
        return (lerp(200, 255, t), lerp(215, 250, t), lerp(255, 245, t))
    if bv < 1.5:
        t = (bv - 0.65) / 0.85
        return (255.0, lerp(250, 210, t), lerp(245, 165, t))
    t = clamp((bv - 1.5) / 0.8, 0, 1)
    return (255.0, lerp(210, 180, t), lerp(165, 130, t))


# ---------------------------------------------------------------------------
def read_ppm(path):
    data = pathlib.Path(path).read_bytes()
    parts = data.split(b'\n', 3)
    w, h = (int(v) for v in parts[1].split())
    return w, h, parts[3]


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1

    sec = float(sys.argv[1])
    cfg = {'camAz': 180.0, 'fov': 68.0, 'baseAlt': 0.0, 'lat': 39.9, 'lon': 116.4, 'rot': 1}
    mapping = {'az': 'camAz', 'fov': 'fov', 'baseAlt': 'baseAlt',
               'lat': 'lat', 'lon': 'lon', 'rot': 'rot'}
    for a in sys.argv[2:]:
        if '=' in a:
            k, v = a.split('=', 1)
            if k in mapping:
                cfg[mapping[k]] = float(v)

    # 画布尺寸随 rot（0/2 竖屏 240×320，1/3 横屏 320×240），与固件一致
    global W, H
    W, H = (320, 240) if int(cfg['rot']) & 1 else (240, 320)

    sky_keys, h_opp, stars = parse_tables()
    sky_keys = [(a, z, list(SKY_H_OVERRIDE.get(a, h))) for a, z, h in sky_keys]   # 同步固件的有意偏离
    print(f'[html] 解析到 {len(sky_keys)} 个天空关键帧 / {len(h_opp)} 个 H_OPP / {len(stars)} 颗星')

    # ---- 参考实现 ----
    sun_alt, sun_az, ref = render_sky_html(sec, cfg['lat'], cfg['lon'], cfg['camAz'],
                                          cfg['baseAlt'], cfg['fov'], sky_keys, h_opp)
    print(f'[html] 太阳 alt={sun_alt:.6f} az={sun_az:.6f}')

    # ---- 固件侧：导出同一帧 ----
    out_ppm = ROOT / 'tools' / 'out' / 'html_cmp.ppm'
    out_ppm.parent.mkdir(exist_ok=True)
    args = [str(EXE), 'sky888', str(sec), str(out_ppm),
            f'az={cfg["camAz"]}', f'fov={cfg["fov"]}', f'baseAlt={cfg["baseAlt"]}',
            f'lat={cfg["lat"]}', f'lon={cfg["lon"]}', f'rot={int(cfg["rot"])}']
    run = subprocess.run(args, capture_output=True, text=True, encoding='utf-8', errors='replace')
    print('[ fw ] ' + run.stdout.strip().replace('\n', '\n[fw ] '))

    m = re.search(r'sun alt=([-\d.]+) az=([-\d.]+)', run.stdout)
    fw_alt, fw_az = float(m.group(1)), float(m.group(2))
    print(f'\n[1] 太阳位置：固件 ({fw_alt:.4f}, {fw_az:.4f}) vs HTML ({sun_alt:.4f}, {sun_az:.4f})'
          f'  ← Δalt={abs(fw_alt-sun_alt):.2e}° Δaz={abs(fw_az-sun_az):.2e}°')

    w, h, fw = read_ppm(out_ppm)
    assert (w, h) == (W, H) and len(fw) == W * H * 3

    worst, wsum, wpos = 0, 0.0, (0, 0)
    for i in range(W * H):
        for ch in range(3):
            d = abs(fw[i * 3 + ch] - ref[i * 3 + ch])
            wsum += d
            if d > worst:
                worst, wpos = d, (i % W, i // W)
    print(f'[2] 天空逐像素：最大偏差 {worst:.0f} 级 @{wpos}，平均 {wsum / (W*H*3):.3f} 级')
    print(f'    （固件无量化、HTML 有 0.5° 的 kk 量化 + 1.4° 方位量化，'
          f'所以允许到 {TOL:.0f} 级）')

    # ---- 星表 ----
    tw = 0.55
    run2 = subprocess.run([str(EXE), 'stars', str(sec), f'az={cfg["camAz"]}',
                           f'fov={cfg["fov"]}', f'baseAlt={cfg["baseAlt"]}',
                           f'lat={cfg["lat"]}', f'lon={cfg["lon"]}', f'twinkle={tw}',
                           f'rot={int(cfg["rot"])}'],
                          capture_output=True, text=True, encoding='utf-8', errors='replace')
    fw_stars = []
    for line in run2.stdout.splitlines()[2:]:
        p = line.split(',')
        if len(p) >= 6:
            fw_stars.append((float(p[1]), float(p[2]), float(p[3])))   # x, y, baseIntensity

    ref_stars = star_list_html(sec, cfg['lat'], cfg['lon'], cfg['camAz'], cfg['baseAlt'],
                              cfg['fov'], sun_alt, tw, 3.7, stars)
    print(f'\n[3] 星表：固件 {len(fw_stars)} 颗 vs HTML {len(ref_stars)} 颗')
    if len(fw_stars) == 0 and len(ref_stars) == 0:
        print('    这一帧画面里没有星星（两边一致）')
    elif len(fw_stars) == len(ref_stars):
        dx = max(abs(a[0] - b[0]) for a, b in zip(fw_stars, ref_stars))
        dy = max(abs(a[1] - b[1]) for a, b in zip(fw_stars, ref_stars))
        di = max(abs(a[2] - b[2]) for a, b in zip(fw_stars, ref_stars))
        print(f'    最大偏差：位置 Δx={dx:.4f}px Δy={dy:.4f}px，强度 Δ={di:.2e}')
        for k, (a, b) in enumerate(zip(fw_stars, ref_stars)):
            print(f'      #{k}  固件({a[0]:.2f},{a[1]:.2f},I={a[2]:.4f})  '
                  f'HTML({b[0]:.2f},{b[1]:.2f},I={b[2]:.4f})')
    else:
        print('    !! 数量不一致，逐条列出来：')
        print('    固件:', [f'({a[0]:.1f},{a[1]:.1f})' for a in fw_stars])
        print('    HTML:', [f'({b[0]:.1f},{b[1]:.1f})' for b in ref_stars])

    ok = worst <= TOL and len(fw_stars) == len(ref_stars)
    # 机器可读的 ASCII 结果行（供脚本调用，避开控制台编码问题）
    print(f'RESULT sec={sec:.0f} sunAlt={fw_alt:.4f} sunAz={fw_az:.4f} '
          f'skyMax={worst:.0f} skyMean={wsum / (W*H*3):.3f} '
          f'stars={len(fw_stars)}/{len(ref_stars)} ok={1 if ok else 0}')
    print('\n结论：' + ('一致（差异都在量化/精度范围内）' if ok else '存在超出容差的差异'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
