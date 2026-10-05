# ===========================================================================
#  PPM(P6) → PNG（无第三方依赖，只用 zlib/struct）
#  用法：python tools/ppm2png.py in.ppm out.png [x y w h scale]
#        给了裁剪参数就按最近邻放大导出局部
# ===========================================================================
import struct
import sys
import zlib


def read_ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    # 头：P6 <w> <h> <max>，允许 # 注释
    fields, i = [], 2
    while len(fields) < 3:
        while data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b'#':
            while data[i:i + 1] != b'\n':
                i += 1
            continue
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        fields.append(int(data[i:j]))
        i = j
    i += 1
    w, h, maxv = fields
    assert maxv == 255, maxv
    px = data[i:i + w * h * 3]
    assert len(px) == w * h * 3, (len(px), w * h * 3)
    return w, h, px


def write_png(path, w, h, px):
    raw = bytearray()
    for y in range(h):
        raw.append(0)                       # filter = none
        raw += px[y * w * 3:(y + 1) * w * 3]

    def chunk(tag, body):
        return (struct.pack('>I', len(body)) + tag + body
                + struct.pack('>I', zlib.crc32(tag + body) & 0xFFFFFFFF))

    ihdr = struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)   # 8 bit, truecolor
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', ihdr))
        f.write(chunk(b'IDAT', zlib.compress(bytes(raw), 9)))
        f.write(chunk(b'IEND', b''))


def crop_scale(px, w, h, x, y, cw, ch, scale):
    out = bytearray()
    for yy in range(ch * scale):
        sy = min(y + yy // scale, h - 1)
        for xx in range(cw * scale):
            sx = min(x + xx // scale, w - 1)
            off = (sy * w + sx) * 3
            out += px[off:off + 3]
    return cw * scale, ch * scale, bytes(out)


if __name__ == '__main__':
    w, h, px = read_ppm(sys.argv[1])
    if len(sys.argv) >= 8:
        x, y, cw, ch, scale = (int(v) for v in sys.argv[3:8])
        w, h, px = crop_scale(px, w, h, x, y, cw, ch, scale)
    write_png(sys.argv[2], w, h, px)
    print(f'{sys.argv[2]}: {w}x{h}')
