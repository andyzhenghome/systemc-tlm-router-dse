# -*- coding: utf-8 -*-
"""极简 Markdown -> HTML（只处理本文用到的语法）"""
import re, html, os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "SystemC-TLM-Router建模.md")
DST = os.path.join(ROOT, "SystemC-TLM-Router建模.html")

def inline(t):
    t = html.escape(t, quote=False)
    t = re.sub(r'`([^`]+)`', r'<code>\1</code>', t)
    t = re.sub(r'!\[([^\]]*)\]\(([^)]+)\)', r'<img alt="\1" src="\2">', t)
    t = re.sub(r'\[([^\]]+)\]\(([^)]+)\)', r'<a href="\2">\1</a>', t)
    t = re.sub(r'\*\*([^*]+)\*\*', r'<strong>\1</strong>', t)
    return t

def is_sep(line):
    s = line.strip()
    return s.startswith('|') and bool(re.fullmatch(r'[|\s:\-]+', s))

def cells(line):
    s = line.strip()
    if s.startswith('|'): s = s[1:]
    if s.endswith('|'): s = s[:-1]
    return [c.strip() for c in s.split('|')]

lines = open(SRC, encoding="utf-8").read().splitlines()
out, i, n = [], 0, len(lines)
while i < n:
    ln = lines[i]
    s = ln.strip()
    if not s:
        i += 1; continue
    if s.startswith('```'):
        lang = s[3:].strip()
        i += 1; buf = []
        while i < n and not lines[i].strip().startswith('```'):
            buf.append(lines[i]); i += 1
        i += 1
        out.append(f'<pre class="code"><code>{html.escape(chr(10).join(buf))}</code></pre>')
        continue
    m = re.match(r'^(#{1,6})\s+(.*)$', s)
    if m:
        lv = len(m.group(1))
        out.append(f'<h{lv}>{inline(m.group(2))}</h{lv}>')
        i += 1; continue
    if s in ('---', '***', '___'):
        out.append('<hr>'); i += 1; continue
    if s.startswith('|') and i+1 < n and is_sep(lines[i+1]):
        hdr = cells(s); i += 2
        rows = []
        while i < n and lines[i].strip().startswith('|'):
            rows.append(cells(lines[i].strip())); i += 1
        t = ['<div class="tw"><table><thead><tr>']
        t += [f'<th>{inline(c)}</th>' for c in hdr]
        t.append('</tr></thead><tbody>')
        for r in rows:
            t.append('<tr>')
            t += [f'<td>{inline(c)}</td>' for c in r]
            t.append('</tr>')
        t.append('</tbody></table></div>')
        out.append(''.join(t)); continue
    if s.startswith('> '):
        buf = []
        while i < n and lines[i].strip().startswith('>'):
            buf.append(lines[i].strip().lstrip('>').strip()); i += 1
        out.append('<blockquote>' + inline(' '.join(buf)) + '</blockquote>')
        continue
    if re.match(r'^[-*]\s+', s) or re.match(r'^\d+\.\s+', s):
        ordered = bool(re.match(r'^\d+\.\s+', s))
        tag = 'ol' if ordered else 'ul'
        items = []
        while i < n:
            cur = lines[i].strip()
            mm = re.match(r'^(?:[-*]|\d+\.)\s+(.*)$', cur)
            if not mm: break
            items.append('<li>' + inline(mm.group(1)) + '</li>'); i += 1
        out.append(f'<{tag}>' + ''.join(items) + f'</{tag}>')
        continue
    # paragraph
    buf = [s]; i += 1
    while i < n and lines[i].strip() and not re.match(r'^(#{1,6}\s|```|\||>|[-*]\s|\d+\.\s|---$)', lines[i].strip()):
        buf.append(lines[i].strip()); i += 1
    out.append('<p>' + inline(' '.join(buf)) + '</p>')

CSS = """
:root{--bg:#fff;--fg:#1d2836;--muted:#5f6f82;--line:#e5eaf1;--soft:#f6f8fb;--blue:#2563eb}
*{box-sizing:border-box}
body{margin:0;background:#f2f5f9;color:var(--fg);font-family:'Microsoft YaHei','PingFang SC','Noto Sans CJK SC',-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;line-height:1.85;font-size:16px}
.wrap{max-width:980px;margin:32px auto 80px;background:var(--bg);padding:52px 62px 70px;border-radius:12px;box-shadow:0 6px 28px rgba(25,45,75,.09)}
h1{font-size:30px;line-height:1.4;margin:0 0 26px;padding-bottom:18px;border-bottom:2px solid var(--blue)}
h2{font-size:23px;margin:48px 0 18px;padding-left:12px;border-left:4px solid var(--blue)}
h3{font-size:18.5px;margin:32px 0 12px;color:#23384f}
h4{font-size:16px;margin:24px 0 10px}
p{margin:12px 0}
a{color:var(--blue);text-decoration:none;border-bottom:1px solid #cbdcfb}
a:hover{border-bottom-color:var(--blue)}
code{background:#eef2f7;color:#b1376b;padding:2px 5px;border-radius:4px;font-family:Consolas,'Cascadia Code',Menlo,monospace;font-size:.88em}
pre.code{background:#f7f9fc;border:1px solid var(--line);border-left:3px solid #7c9bd6;border-radius:6px;padding:16px 18px;overflow-x:auto;line-height:1.6}
pre.code code{background:none;color:#26374b;padding:0;font-size:13px}
blockquote{margin:20px 0;padding:12px 20px;background:#f2f7ff;border-left:4px solid #7aa5ea;color:#33475f;border-radius:0 6px 6px 0}
blockquote p{margin:0}
.tw{overflow-x:auto;margin:18px 0}
table{border-collapse:collapse;width:100%;font-size:14.2px}
th,td{border:1px solid var(--line);padding:9px 12px;text-align:left;vertical-align:top}
th{background:#eef3fa;font-weight:700;color:#2c4056;white-space:nowrap}
tr:nth-child(even) td{background:#fafbfd}
img{max-width:100%;height:auto;display:block;margin:22px auto;border:1px solid var(--line);border-radius:8px}
ul,ol{padding-left:26px}li{margin:6px 0}
hr{border:0;border-top:1px solid var(--line);margin:38px 0}
strong{color:#12304f}
@media(max-width:760px){.wrap{padding:26px 20px;margin:0;border-radius:0}body{font-size:15px}}
"""
doc = f"""<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>用一个 Router 学会 TLM 架构探索：transaction 级建模与参数扫描</title>
<style>{CSS}</style></head>
<body><div class="wrap">{''.join(out)}</div></body></html>"""
open(DST, "w", encoding="utf-8").write(doc)
print("wrote", DST, len(doc), "chars")
