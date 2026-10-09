# -*- coding: utf-8 -*-
"""把 380 颗新星插入三处星表（astro.cpp / sunset.html / 维纳斯.html），并同步 astro.h N_STARS。

新星块（new_stars.cpp / new_stars.html）已按 ra_h 升序排列，与既有 140 条同序追加，
不改动既有星的相对顺序。三处保持同序同值。
"""
import io, os, re

OUT = os.path.join(os.path.dirname(__file__), "out")
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def read(p):
    with io.open(p, encoding="utf-8") as f:
        return f.read()


def write(p, s):
    with io.open(p, "w", encoding="utf-8", newline="\n") as f:
        f.write(s)


cpp_block = read(os.path.join(OUT, "new_stars.cpp")).rstrip("\n")
html_block = read(os.path.join(OUT, "new_stars.html")).rstrip("\n")

# ---- 1. astro.cpp：插到 Markab 行与 `};` 之间 ----
p = os.path.join(ROOT, "src", "astro.cpp")
s = read(p)
mark = "  {23.081,  15.205,  2.49, 0.00},   // α Peg Markab\n};\n"
assert s.count(mark) == 1, "astro.cpp marker not unique"
s = s.replace(mark, "  {23.081,  15.205,  2.49, 0.00},   // α Peg Markab\n" + cpp_block + "\n};\n")
write(p, s)

# ---- 2. astro.h：N_STARS 140 -> 520 ----
p = os.path.join(ROOT, "src", "astro.h")
s = read(p)
assert re.search(r"#define N_STARS \d+", s), "N_STARS not found"
s = re.sub(r"#define N_STARS \d+", "#define N_STARS 520", s)
write(p, s)

# ---- 3. sunset.html / 维纳斯.html：插到 Markab 行与 `];` 之间 ----
for fn in ("sunset.html", "维纳斯.html"):
    p = os.path.join(ROOT, fn)
    s = read(p)
    mark = "    [23.081,  15.205,  2.49, 0.00],\n  ];\n"
    assert s.count(mark) == 1, f"{fn} marker not unique"
    s = s.replace(mark, "    [23.081,  15.205,  2.49, 0.00],\n" + html_block + "\n  ];\n")
    write(p, s)

# ---- 统计确认 ----
def count_stars(s, pattern):
    return len(re.findall(pattern, s))

cp = read(os.path.join(ROOT, "src", "astro.cpp"))
sunset = read(os.path.join(ROOT, "sunset.html"))
venus = read(os.path.join(ROOT, "维纳斯.html"))
print("astro.cpp stars:", count_stars(cp, r"^\s*\{[0-9-]"))
print("sunset.html stars:", count_stars(sunset, r"\[[0-9]"))
print("维纳斯.html stars:", count_stars(venus, r"\[[0-9]"))
print("done")