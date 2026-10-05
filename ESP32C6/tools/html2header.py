# ===========================================================================
#  把 webui.html 压缩成 include/web_ui.h
#
#    1) 去掉注释、折叠空白（HTML / CSS / JS 分别处理，保留外观与功能）
#    2) gzip 压缩成字节数组 WEB_UI_GZ（设备端以 Content-Encoding: gzip 直出）
#
#  用法（项目根目录）：python tools/html2header.py
#  改完网页后重跑一次，再编译固件即可。webui.html 始终是可读的源文件。
# ===========================================================================
import gzip
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / 'webui.html'
DST = ROOT / 'include' / 'web_ui.h'


def strip_css(css: str) -> str:
    """CSS：去 /* */ 注释、折叠空白。不动 calc() 里的运算符间距，安全。"""
    css = re.sub(r'/\*.*?\*/', '', css, flags=re.S)
    css = re.sub(r'\s+', ' ', css)
    css = re.sub(r'\s*([{};:,>])\s*', r'\1', css)
    return css.strip()


def strip_js(js: str) -> str:
    """JS：逐字符扫描去注释与缩进。

    字符串 / 模板字面量原样保留（含换行），换行一律保留 —— 这样不会破坏
    自动分号插入（ASI），也不需要理解正则字面量。"""
    out = []
    i, n = 0, len(js)
    at_line_start = True
    pending_space = False
    newline_pending = False

    while i < n:
        c = js[i]

        if c == '\n':
            newline_pending = True
            at_line_start = True
            pending_space = False
            i += 1
            continue
        if c in ' \t\r':
            pending_space = True
            i += 1
            continue

        if c in '\'"`':                       # 字符串 / 模板
            if newline_pending:
                out.append('\n')
                newline_pending = False
            elif pending_space and not at_line_start:
                out.append(' ')
            pending_space = False
            at_line_start = False
            q = c
            out.append(c)
            i += 1
            while i < n:
                d = js[i]
                out.append(d)
                i += 1
                if d == '\\' and i < n:       # 转义字符：连下一个原样带走
                    out.append(js[i])
                    i += 1
                    continue
                if d == q:
                    break
            continue

        if c == '/' and i + 1 < n and js[i + 1] == '/':      # 行注释
            j = js.find('\n', i)
            i = n if j < 0 else j
            continue

        if c == '/' and i + 1 < n and js[i + 1] == '*':      # 块注释
            j = js.find('*/', i + 2)
            if j < 0:
                break
            if '\n' in js[i:j + 2]:           # 跨行注释补一个换行，保住语句边界
                newline_pending = True
            i = j + 2
            continue

        if newline_pending:
            out.append('\n')
            newline_pending = False
        elif pending_space and not at_line_start:
            out.append(' ')
        pending_space = False
        at_line_start = False
        out.append(c)
        i += 1

    return ''.join(out).strip()


def strip_html(part: str) -> str:
    """纯 HTML 片段：去注释、把连续空白折成一个空格（HTML 本来就按单空格渲染）"""
    part = re.sub(r'<!--.*?-->', '', part, flags=re.S)
    return re.sub(r'\s+', ' ', part)


def minify(html: str) -> str:
    # 按 <style>/<script> 切段，各自用对应的精简规则
    parts = re.split(r'(<(?:style|script)\b[^>]*>)(.*?)(</(?:style|script)>)', html, flags=re.S)
    out = []
    for i, p in enumerate(parts):
        if i % 4 == 0:
            out.append(strip_html(p))
        elif i % 4 == 2:
            out.append(strip_css(p) if parts[i - 1].lower().startswith('<style') else strip_js(p))
        else:
            out.append(re.sub(r'\s+', ' ', p))
    return ''.join(out).strip()


def main() -> None:
    html = SRC.read_text(encoding='utf-8')
    mini = minify(html)
    raw = mini.encode('utf-8')
    gz = gzip.compress(raw, 9, mtime=0)      # mtime=0：输出可复现

    lines = []
    for i in range(0, len(gz), 20):
        lines.append('  ' + ','.join(f'0x{b:02x}' for b in gz[i:i + 20]) + ',')
    body = '\n'.join(lines)

    DST.write_text(
        '// 本文件由 tools/html2header.py 从 webui.html 生成，请勿手改\n'
        f'// 源文件 {len(html.encode("utf-8"))} 字节 → 精简 {len(raw)} 字节 → gzip {len(gz)} 字节\n'
        '#pragma once\n\n'
        '#include <stdint.h>\n\n'
        f'static const uint8_t WEB_UI_GZ[] PROGMEM = {{\n{body}\n}};\n'
        'static const size_t WEB_UI_GZ_LEN = sizeof(WEB_UI_GZ);\n',
        encoding='utf-8', newline='\n')

    print(f'{DST.relative_to(ROOT)} 已生成：'
          f'{len(html.encode("utf-8"))} → 精简 {len(raw)} → gzip {len(gz)} 字节')


if __name__ == '__main__':
    main()
