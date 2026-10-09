#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# 下载 HYG v3 星表（J2000，含 ra/dec/mag/ci）。非最新版本，仅用于肉眼可见亮星。
# 只下载到本地缓存，不做任何修改。镜像顺序：jsDelivr(CDN) → raw.githubusercontent。
import os, sys, urllib.request, gzip, io

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'hygdata_v3.csv')

URLS = [
    'https://raw.githubusercontent.com/astronexus/HYG-Database/main/hyg/v3/hyg_v31.csv.gz',
    'https://cdn.jsdelivr.net/gh/astronexus/HYG-Database@main/hyg/v3/hyg_v31.csv.gz',
]

def fetch(url, headers=None):
    req = urllib.request.Request(url, headers=headers or {'User-Agent': 'trae/1.0'})
    with urllib.request.urlopen(req, timeout=90) as r:
        return r.read()

def main():
    if os.path.exists(OUT) and os.path.getsize(OUT) > 1000000:
        print('已存在缓存:', OUT, os.path.getsize(OUT), 'bytes')
        return 0
    raw = None
    for u in URLS:
        try:
            print('尝试', u)
            data = fetch(u)
            if len(data) < 1000000:
                print('  太小，可能是错误页，跳过')
                continue
            raw = data
            print('  下载成功', len(data), 'bytes')
            break
        except Exception as e:
            print('  失败:', e)
    if raw is None:
        print('!! 所有源都失败。请手动下载 hygdata_v3.csv 放到 tools/ 目录。')
        return 1
    # 若服务器给了 gzip，解压
    if raw[:2] == b'\x1f\x8b':
        raw = gzip.decompress(raw)
    with open(OUT, 'wb') as f:
        f.write(raw)
    print('已保存:', OUT, os.path.getsize(OUT), 'bytes')
    return 0

if __name__ == '__main__':
    sys.exit(main())