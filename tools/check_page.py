#!/usr/bin/env python3
"""静态检查 acceptance.html：
   1) JS 里 $('id') 引用的 id 是否都在 HTML 里定义
   2) 剥掉注释与字符串后，括号是否配平
   3) el.xxx 是否都在 el 对象里声明
"""
import re
import sys

path = sys.argv[1] if len(sys.argv) > 1 else 'acceptance.html'
h = open(path, encoding='utf-8').read()

# ---------- 1) DOM id ----------
ids = set(re.findall(r'\bid="([^"]+)"', h))
refs = set(re.findall(r"\$\('([^']+)'\)", h))
print('data-cmd    :', re.findall(r'data-cmd="([^"]+)"', h))
missing = sorted(refs - ids)
print('ids defined :', len(ids), ' referenced:', len(refs))
print('MISSING ids :', missing if missing else 'none')
unused = sorted(ids - refs)
print('unused ids  :', unused if unused else 'none')

m = re.search(r'<script>(.*?)</script>', h, re.S)
if not m:
    print('no <script> block')
    sys.exit(1)
js = m.group(1)


def strip_comments_strings(s):
    """把注释和字符串字面量替换成等长空白，保留换行（这样位置信息不丢）。"""
    out = []
    i, n = 0, len(s)
    while i < n:
        c = s[i]
        two = s[i:i + 2]
        if two == '//':
            j = s.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i))
            i = j
        elif two == '/*':
            j = s.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(''.join('\n' if ch == '\n' else ' ' for ch in s[i:j]))
            i = j
        elif c in '"\'':
            q = c
            j = i + 1
            while j < n:
                if s[j] == '\\':
                    j += 2
                    continue
                if s[j] == q:
                    j += 1
                    break
                j += 1
            out.append(''.join('\n' if ch == '\n' else ' ' for ch in s[i:j]))
            i = j
        elif c == '`':
            j = i + 1
            while j < n:
                if s[j] == '\\':
                    j += 2
                    continue
                if s[j] == '`':
                    j += 1
                    break
                j += 1
            out.append(''.join('\n' if ch == '\n' else ' ' for ch in s[i:j]))
            i = j
        else:
            out.append(c)
            i += 1
    return ''.join(out)


clean = strip_comments_strings(js)
bad = False
for a, b, name in [('{', '}', 'braces'), ('(', ')', 'parens'), ('[', ']', 'brackets')]:
    ca, cb = clean.count(a), clean.count(b)
    ok = ca == cb
    bad |= not ok
    print(f'{name:9s}: {ca:4d} vs {cb:4d}  ' + ('OK' if ok else 'MISMATCH'))

# ---------- 3) el.* 声明 ----------
body = re.search(r'const\s+el\s*=\s*\{(.*?)\n\};', js, re.S)
# 注意：一行里可能有多个 "key:$('id')," —— 不能锚定行首
declared = set(re.findall(r'([A-Za-z_$][\w$]*)\s*:\s*\$\(', body.group(1))) if body else set()
used = set(re.findall(r'\bel\.([A-Za-z_$][\w$]*)', clean))
undeclared = sorted(used - declared)
print('el declared :', len(declared), ' used:', len(used))
print('el UNDECLARED:', undeclared if undeclared else 'none')
bad |= bool(undeclared)

# ---------- 3b) 不应残留中日韩字符（页面要求全英文）----------
CJK = re.compile(r'[\u3400-\u4dbf\u4e00-\u9fff\u3000-\u303f\uff01-\uff60]')
cjk_hits = []
for idx, ln in enumerate(h.split('\n'), 1):
    if CJK.search(ln):
        cjk_hits.append((idx, ln.strip()[:80]))
print('residual CJK  :', len(cjk_hits), 'line(s)')
for ln_no, txt in cjk_hits[:20]:
    print('    line %d: %s' % (ln_no, txt))
bad |= bool(cjk_hits)

# ---------- 4) 注释块外不应出现 " * " 开头的行（块注释被提前闭合的典型症状）----------
lines = js.split('\n')
depth = 0
stray = []
for idx, ln in enumerate(lines, 1):
    s = ln.strip()
    if depth == 0 and (s.startswith('* ') or s.startswith('*/') or s == '*'):
        stray.append((idx, ln[:70]))
    o = ln.find('/*')
    c = ln.find('*/', o + 2) if o >= 0 else -1
    if o >= 0 and c < 0:
        depth += 1
    elif o < 0 and c >= 0 and depth > 0:
        depth -= 1
print('stray comment lines:', stray if stray else 'none')
bad |= bool(stray)

sys.exit(1 if (missing or bad) else 0)
