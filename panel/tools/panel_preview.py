#!/usr/bin/env python3
"""パネルの文言と配置を HTML（SVG）にして、ブラウザで確認する。開発用。

  python panel/tools/panel_preview.py [--params contrib/params.conf] [--out tmp/panel-preview.html]
  python panel/tools/panel_preview.py --open [--tab 1]   # 作ったあと Orca 内ブラウザで開く（開いていれば再読み込み）

Frame も SteamVR も要らない。パネルのソースを読んで、描画の呼び出し（座標・文言・スイッチ・スライダー）を
そのまま SVG に写す。
  - 文言・座標・プリセット・スライダーの範囲: panel/src/params.cpp と panel/src/panel_main.cpp から読む（二重管理しない）
  - 値（スライダー位置・スイッチの ON/OFF）: --params の params.conf から
  - タブ: kTabNames と enum class Tab の並びに従う。SVG のタブをクリックすると切り替わる

目安であって、本物の描画ではない:
  - 文字の幅は「全角 = 文字サイズ、半角 = 0.56 倍」で見積もる。実機の Noto Sans CJK と数 % ずれる
  - スライダー 1 本の中の配置（drawSliderAt）、タブの形、負荷カードの中身はこのスクリプトに写してある。
    パネル側を変えたら、ここも合わせる
  - はみ出し・重なりの警告は見積もりなので、出たら実機の --render-png で確かめる
"""
import argparse
import html
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]  # repository root
SRC = ROOT / "panel" / "src"


# ---- C++ の読み取り ----

def strip_comments(s):
    """文字列リテラルを壊さずに // と /* */ を消す。"""
    out, i, n = [], 0, len(s)
    while i < n:
        if s[i] == '"':
            j = i + 1
            while j < n and s[j] != '"':
                j += 2 if s[j] == "\\" else 1
            out.append(s[i:j + 1])
            i = j + 1
        elif s.startswith("//", i):
            j = s.find("\n", i)
            i = n if j < 0 else j
        elif s.startswith("/*", i):
            j = s.find("*/", i + 2)
            i = n if j < 0 else j + 2
        else:
            out.append(s[i])
            i += 1
    return "".join(out)


def match_close(s, i, open_ch, close_ch):
    """s[i] == open_ch の対応する閉じ括弧の位置。文字列リテラルは飛ばす。"""
    depth, n = 0, len(s)
    while i < n:
        c = s[i]
        if c == '"':
            i += 1
            while i < n and s[i] != '"':
                i += 2 if s[i] == "\\" else 1
        elif c == open_ch:
            depth += 1
        elif c == close_ch:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    raise ValueError("括弧が閉じていない")


def split_args(s):
    """トップレベルのカンマで分ける。"""
    args, depth, cur, i, n = [], 0, [], 0, len(s)
    while i < n:
        c = s[i]
        if c == '"':
            j = i + 1
            while j < n and s[j] != '"':
                j += 2 if s[j] == "\\" else 1
            cur.append(s[i:j + 1])
            i = j + 1
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        if c == "," and depth == 0:
            args.append("".join(cur).strip())
            cur = []
        else:
            cur.append(c)
        i += 1
    if "".join(cur).strip():
        args.append("".join(cur).strip())
    return args


def function_body(src, name):
    m = re.search(r"\n(?:void|int|bool|[\w:<>]+)\s+" + re.escape(name) + r"\(", src)
    if not m:
        raise KeyError(f"panel_main.cpp に {name}() が無い")
    p = m.end() - 1
    p_end = match_close(src, p, "(", ")")
    b = src.index("{", p_end)
    return src[b + 1:match_close(src, b, "{", "}")]


STR = re.compile(r'"((?:\\.|[^"\\])*)"')


def unescape(s):
    return re.sub(r"\\(.)", lambda m: {"n": "\n"}.get(m.group(1), m.group(1)), s)


def as_string(arg):
    parts = STR.findall(arg)
    if not parts or not arg.strip().startswith('"'):
        raise ValueError(f"文字列ではない: {arg[:40]}")
    return "".join(unescape(p) for p in parts)


def resolve_language(src, lang):
    """tr("日本語", "English", "中文") と gChinese ? A : (gEnglish ? B : C) を、選んだ言語のほうに置き換える（パネルが実行時にやることの写し）。"""
    index = {"ja": 0, "en": 1, "zh": 2}[lang]
    out, pos = [], 0
    for m in re.finditer(r"\btr\(", src):
        if m.start() < pos:
            continue  # 前の tr(...) の中
        end = match_close(src, m.end() - 1, "(", ")")
        args = split_args(src[m.end():end])
        if len(args) == 3 and all(a.startswith('"') for a in args):
            out.append(src[pos:m.start()])
            out.append(args[index])
            pos = end + 1
    out.append(src[pos:])
    text = "".join(out)
    text = re.sub(r"gChinese\s*\?\s*(\w+)\s*:\s*\(\s*gEnglish\s*\?\s*(\w+)\s*:\s*(\w+)\s*\)",
                  lambda m: m.group(1) if index == 2 else (m.group(2) if index == 1 else m.group(3)), text)
    return re.sub(r"gEnglish\s*\?\s*(\w+)\s*:\s*(\w+)", lambda m: m.group(1) if index == 1 else m.group(2), text)


# ---- 文字幅の見積もり ----

def est_width(text, size):
    w = 0.0
    for ch in text:
        o = ord(ch)
        w += size if (o >= 0x2E80 or 0xFF00 <= o <= 0xFFEF or 0x2190 <= o <= 0x2BFF or 0x2010 <= o <= 0x203F) else size * 0.56
    return w


# ---- 式の評価 ----

def eval_num(expr, env):
    def measure(m):
        return str(est_width(as_string(m.group(1)), float(m.group(2))))
    e = re.sub(r'pen\.measure\(\s*((?:"(?:\\.|[^"\\])*"\s*)+),\s*([\d.]+)(?:,\s*(?:true|false))?\s*\)', measure, expr)
    e = re.sub(r"\b([A-Za-z_]\w*)\b", lambda m: str(env[m.group(1)]) if m.group(1) in env else m.group(0), e)
    if re.search(r"[A-Za-z_]", e) or not re.fullmatch(r"[\d+\-*/().\s]+", e):
        raise ValueError(f"評価できない式: {expr}")
    return float(eval(e, {"__builtins__": {}}))


def eval_on(expr, values, gaze):
    expr = expr.strip()
    m = re.fullmatch(r'value\(ui,\s*"(\w+)",\s*([\d.]+)\)\s*(>=|>)\s*([\d.]+)', expr)
    if m:
        v = values.get(m.group(1), float(m.group(2)))
        return v >= float(m.group(4)) if m.group(3) == ">=" else v > float(m.group(4))
    m = re.fullmatch(r"ui\.gaze\.(\w+)", expr)
    if m:
        return gaze.get(m.group(1), False)
    raise ValueError(f"スイッチの状態を評価できない: {expr}")


# ---- params.cpp / params.conf ----

def read_specs(text):
    text = strip_comments(text)
    body = text[text.index("paramSpecs()"):]
    body = body[:body.index("return specs;")]
    specs = []
    lit = r"((?:\"(?:\\.|[^\"\\])*\"\s*)+)"
    for m in re.finditer(r"\{\s*\"(\w+)\",\s*\"([^\"]*)\",\s*\"([^\"]*)\",\s*\"([^\"]*)\",\s*([\d.\-]+),\s*([\d.\-]+),\s*([\d.\-]+),\s*(\d+),\s*"
                         + lit + r",\s*" + lit + r",\s*" + lit + r"(?:,\s*(true|false))?\s*\}", body):
        join = lambda g: "".join(unescape(p) for p in STR.findall(g))
        specs.append(dict(key=m.group(1), label=m.group(2), labelEn=m.group(3), labelZh=m.group(4), min=float(m.group(5)),
                          max=float(m.group(6)), step=float(m.group(7)), decimals=int(m.group(8)),
                          desc=join(m.group(9)), descEn=join(m.group(10)), descZh=join(m.group(11)),
                          beta=m.group(12) == "true"))
    return specs


def read_presets(text):
    text = strip_comments(text)
    body = text[text.index("presets()"):]
    body = body[:body.index("return list;")]
    presets = []
    for m in re.finditer(r'\{\s*"([^"]+)",\s*"([^"]+)",\s*"([^"]+)",\s*\{((?:\{[^{}]*\},?\s*)+)\}\s*(?:,\s*(true|false))?\s*\}', body):
        presets.append(dict(name=m.group(1), nameEn=m.group(2), nameZh=m.group(3), beta=m.group(5) == "true"))
    return presets


def read_color_presets(src):
    """panel_main.cpp の kColorPresets（日本語・英語・中国語の名前）を読む。"""
    block = re.search(r"kColorPresets\[\]\s*=\s*\{(.*?)\n\};", src, flags=re.S)
    if not block:
        return []
    rows = []
    for line in block.group(1).splitlines():
        names = [unescape(s) for s in STR.findall(line)]
        if len(names) >= 3:
            rows.append(names[:3])
    return rows


def read_conf(path):
    values = {}
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0]
        if "=" in line:
            k, v = line.split("=", 1)
            try:
                values[k.strip()] = float(v.strip())
            except ValueError:
                pass
    return values


# ---- 描画要素の組み立て ----

class Scene:
    def __init__(self, consts, colors, specs, presets, values, gaze, tips, lang="ja", restart=False, update_sample="available"):
        self.consts, self.colors, self.specs, self.presets = consts, colors, specs, presets
        self.values, self.gaze, self.tips = values, gaze, tips
        self.lang = lang
        self.restart = restart              # 「SteamVR の再起動が必要」の帯を出す
        self.update_sample = update_sample  # 更新タブの見本の状態
        self.items = []   # SVG 断片
        self.texts = []   # (x0, x1, y0, y1, text) 重なり検査用
        self.color_presets = []  # [[日本語, 英語, 中国語]] 色調整タブのショートカット

    def color(self, name):
        r, g, b = self.colors[name]
        return "#%02x%02x%02x" % (round(r * 255), round(g * 255), round(b * 255))

    def rect(self, x, y, w, h, r, fill, extra=""):
        self.items.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" rx="{r}" fill="{fill}" {extra}/>')

    def text(self, x, y, s, size, color, bold=False, right=False):
        w = est_width(s, size)
        x0 = x - w if right else x
        self.texts.append((x0, x0 + w, y - size * 0.85, y + size * 0.2, s))
        weight = ' font-weight="700"' if bold else ""
        anchor = ' text-anchor="end"' if right else ""
        self.items.append(f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}" fill="{self.color(color)}"{weight}{anchor}>{html.escape(s)}</text>')

    def toggle(self, x, y, label, on):
        self.rect(x, y, 64, 34, 17, self.color("kAccent" if on else "kTrack"))
        cx = x + 47 if on else x + 17
        self.items.append(f'<circle cx="{cx}" cy="{y + 17}" r="13" fill="{self.color("kText")}"/>')
        self.text(x + 78, y + 26, label, 22, "kText")

    def slider(self, spec, y, indent):
        c = self.consts
        x0, w = c["kSliderX"], c["kSliderW"]
        v = self.values.get(spec["key"], spec["min"])
        t = min(max((v - spec["min"]) / (spec["max"] - spec["min"]), 0), 1)
        self.text(indent, y + 8, spec[{"ja": "label", "en": "labelEn", "zh": "labelZh"}[self.lang]], 22, "kText")
        self.text(indent, y + 36, spec[{"ja": "desc", "en": "descEn", "zh": "descZh"}[self.lang]], 17, "kSub")
        self.rect(x0, y - 4, w, 8, 4, self.color("kTrack"))
        self.rect(x0, y - 4, w * t, 8, 4, self.color("kAccent"))
        self.items.append(f'<circle cx="{x0 + w * t:.1f}" cy="{y}" r="12" fill="{self.color("kText")}"/>')
        self.text(x0 + w + 90, y + 8, f"{v:.{spec['decimals']}f}", 22, "kText", bold=True, right=True)

    def presets_row(self, beta, x, y):
        for p in self.presets:
            if p["beta"] != beta:
                continue
            name = p[{"ja": "name", "en": "nameEn", "zh": "nameZh"}[self.lang]]
            w = est_width(name, 22) + 40
            self.rect(x, y, w, 44, 10, self.color("kCard"))
            self.text(x + 20, y + 30, name, 22, "kText")
            x += w + 14

    def color_presets_row(self, x, y):
        """drawColorPresets の写し（色調整タブのショートカット）。"""
        for names in self.color_presets:
            name = names[{"ja": 0, "en": 1, "zh": 2}[self.lang]]
            w = est_width(name, 22) + 48
            self.rect(x, y, w, 44, 10, self.color("kCard"))
            self.text(x + 24, y + 30, name, 22, "kText")
            x += w + 18

    def update_status(self):
        """drawUpdateStatus の写し（更新タブの版・ボタン・進み）。状態は実機では確認の結果で決まるので、見本の状態で描く。"""
        tr = (lambda ja, en, zh=None: (zh if zh is not None else en) if self.lang == "zh" else (en if self.lang == "en" else ja))
        st = self.update_sample
        cur = (ROOT / "VERSION").read_text(encoding="utf-8").strip()  # 配る版。版を上げても見本が古くならない
        major, minor, _patch = (int(n) for n in cur.split("."))
        latest = f"{major}.{minor + 1}.0"  # 「新しい版がある」の見本用
        self.text(40, 232, tr("いまの版", "Installed", "当前版本"), 21, "kSub")
        self.text(200, 232, cur, 26, "kText", bold=True)
        self.text(40, 274, tr("最新の版", "Latest", "最新版本"), 21, "kSub")
        available = st in ("available", "running", "done", "failed")
        if st == "error":
            self.text(200, 274, tr("GitHub に繋がらない", "Cannot reach GitHub", "无法连接 GitHub"), 21, "kWarn")
        elif available:
            self.text(200, 274, latest, 26, "kAccent", bold=True)
            self.text(200 + est_width(latest, 26) + 16, 274, tr("新しい版がある", "A new version is available", "有新版本"), 21, "kWarn")
        else:
            self.text(200, 274, cur, 26, "kText", bold=True)
            self.text(200 + est_width(cur, 26) + 16, 274, tr("最新です", "Up to date", "已是最新"), 21, "kSub")
        running = st == "running"

        def button(x, label, primary, enabled):
            w = est_width(label, 22) + 48
            on = primary and enabled
            self.rect(x, 306, w, 44, 10, self.color("kAccent" if on else "kCard"))
            self.text(x + 24, 336, label, 22, "kBg" if on else ("kText" if enabled else "kTrack"))
            return w

        x = 40 + button(40, tr("今すぐ確認", "Check now", "立即检查"), False, not running) + 14
        if available:
            button(x, tr("この版に更新する", "Update to this version", "更新到此版本"), True, not running)
            self.text(40, 384, tr("リリースのページ: ", "Release page: ", "发行版页面: ") + "https://github.com/omuomu0909/betterframe-xp/releases/tag/v" + latest, 17, "kSub")
        if running:
            self.text(40, 436, tr("ビルドして入れ替え中（数分かかる）", "Building and installing (takes a few minutes)", "编译并替换中（需数分钟）") + "…", 22, "kAccent", bold=True)
            self.text(40, 466, tr("この画面を閉じても続く", "It keeps going if you close this screen", "关闭此界面后仍会继续"), 19, "kSub")
        elif st == "done":
            self.text(40, 436, tr("更新した: ", "Updated: ", "已更新: ") + latest, 22, "kAccent", bold=True)
            self.text(40, 466, tr("SteamVR を再起動すると、新しい版になる（自動では再起動しない）",
                                  "Restart SteamVR to run the new version (it is not restarted for you)",
                                  "重启 SteamVR 后就会换成新版本（不会自动重启）"), 19, "kWarn")
        elif st == "failed":
            self.text(40, 436, tr("検証に失敗した（ファイルが壊れている）。何も入れていない", "Checksum mismatch (corrupt download). Nothing was installed", "校验失败（文件损坏）。没有安装任何内容"), 22, "kWarn", bold=True)
            self.text(40, 466, tr("詳しいログ: ~/.cache/betterframe-xp/update.log", "Details: ~/.cache/betterframe-xp/update.log", "详细日志: ~/.cache/betterframe-xp/update.log"), 19, "kSub")

    def language_buttons(self):
        """drawLanguage の写し（右上の 日本語 / English / 中文）。クリックで言語を切り替える。"""
        names, size = ("日本語", "English", "中文"), 19
        widths = [est_width(n, size) + 36 for n in names]
        x = self.consts["kWidth"] - 40 - sum(widths)
        langs = ("ja", "en", "zh")
        for i, n in enumerate(names):
            on = langs[i] == self.lang
            self.items.append(f'<g class="lang" data-lang="{langs[i]}" style="cursor:pointer">')
            self.rect(x, 4, widths[i], 28, 10, self.color("kAccent" if on else "kCard"))
            self.text(x + 18, 24, n, size, "kBg" if on else "kText")
            self.items.append("</g>")
            x += widths[i]


CALL = re.compile(r"\b(pen\.text|pen\.roundedRect|heading|toggleWithNote|toggle|drawSliderByKey|drawSliders|drawPresets|drawColorPresets|subBar|gazeItem|drawCaution|drawRestartBanner|drawUpdateStatus)\s*\(")


def emit_function(scene, src, name, env_base):
    """関数 name の本体にある描画の呼び出しを、書かれた順に scene へ足す。"""
    body = strip_comments(function_body(src, name))
    # 局所の文字列（const char* x = "..."; ）は、使っている所へ文字列そのものを入れる
    for m in list(re.finditer(r'const char\* (\w+) = ((?:"(?:\\.|[^"\\])*"\s*)+);', body)):
        body = body.replace(m.group(0), "")
        body = re.sub(r"\b" + m.group(1) + r"\b", lambda _, lit=m.group(2): lit, body)
    env = dict(env_base)
    for m in re.finditer(r"const double (\w+) = ([^;]+);", body):  # 局所の定数（y の基準など）
        try:
            env[m.group(1)] = eval_num(m.group(2), env)
        except (ValueError, KeyError):
            pass
    num = lambda a: eval_num(a, env)
    for m in CALL.finditer(body):
        fn = m.group(1)
        end = match_close(body, m.end() - 1, "(", ")")
        args = split_args(body[m.end():end])
        before = body[max(0, m.start() - 90):m.start()]
        # 実機では状態で出し分ける文言（SteamVR に繋がらない / 書き込み失敗）は、ここでは出さない
        if re.search(r"if \(!ui\.gaze\.known\)\s*\{\s*$|ui\.gaze\.failed\)\s*\{\s*$", before):
            continue
        try:
            if fn == "pen.text":
                bold = len(args) > 5 and args[5] == "true"
                right = len(args) > 6 and args[6] == "true"
                scene.text(num(args[0]), num(args[1]), as_string(args[2]), num(args[3]), args[4], bold, right)
            elif fn == "pen.roundedRect":
                after = body[end + 1:end + 80]
                fill = re.match(r"\s*;\s*pen\.color\((\w+)\)", after)
                if fill:  # 続けて色を決めて塗る四角（カード）だけ描く。トグルのつまみ等は toggle() で描く
                    scene.rect(num(args[0]), num(args[1]), num(args[2]), num(args[3]), num(args[4]), scene.color(fill.group(1)))
            elif fn == "heading":
                y = num(args[1])
                title = as_string(args[2])
                scene.text(40, y, title, 22, "kAccent", bold=True)
                if len(args) > 3 and args[3].startswith('"'):
                    scene.text(40 + est_width(title, 22) + 24, y, as_string(args[3]), 17, "kSub")
                scene.rect(40, y + 10, scene.consts["kWidth"] - 80, 1, 0, scene.color("kTrack"))
            elif fn == "toggle":
                scene.toggle(num(args[1]), num(args[2]), as_string(args[3]), eval_on(args[4], scene.values, scene.gaze))
            elif fn == "toggleWithNote":
                x, y = scene.consts["kSliderX"], num(args[1])
                scene.toggle(x, y, as_string(args[2]), eval_on(args[4], scene.values, scene.gaze))
                scene.text(x, y + 56, as_string(args[3]), 17, "kSub")
            elif fn == "drawSliderByKey":
                spec = next(s for s in scene.specs if s["key"] == as_string(args[2]))
                scene.slider(spec, num(args[3]), num(args[4]))
            elif fn == "drawSliders":
                row = 0
                for spec in scene.specs:
                    if spec["beta"] == (args[2] == "true"):
                        scene.slider(spec, env["kRowsTop"] + row * env["kRowH"], 40)
                        row += 1
            elif fn == "drawPresets":
                scene.presets_row(args[1] == "true", num(args[2]), num(args[3]))
            elif fn == "drawColorPresets":
                scene.color_presets_row(num(args[1]), num(args[2]))
            elif fn == "subBar":
                scene.rect(52, num(args[1]), 3, num(args[2]) - num(args[1]), 0, scene.color("kTrack"))
            elif fn == "gazeItem":
                y = num(args[1])
                scene.toggle(40, y, as_string(args[2]), eval_on(args[3], scene.values, scene.gaze))
                for i, tip in enumerate(scene.tips[args[5]]):
                    scene.text(40, y + 62 + i * 26, tip, 18, "kText" if i == 0 else "kSub")
            elif fn == "drawCaution":
                emit_function(scene, src, "drawCaution", env_base)
            elif fn == "drawRestartBanner":
                if scene.restart:  # 実機では再起動が要るときだけ出る
                    emit_function(scene, src, "drawRestartBanner", env_base)
            elif fn == "drawUpdateStatus":
                scene.update_status()
        except (ValueError, KeyError, StopIteration) as e:
            print(f"警告: {name}() の {fn}(...) を読めなかった: {e}", file=sys.stderr)


def read_consts(src):
    consts = {}
    for m in re.finditer(r"constexpr\s+(?:int|double)\s+([^;]+);", strip_comments(src)):
        for decl in split_args(m.group(1)):
            if "=" in decl and "[" not in decl.split("=")[0]:
                k, v = decl.split("=", 1)
                try:
                    consts[k.strip()] = eval_num(v, consts)
                except (ValueError, KeyError):
                    pass
    return consts


def read_colors(src):
    return {m.group(1): tuple(float(x) for x in m.group(2).split(",")) for m in
            re.finditer(r"constexpr Color (\w+)\{([^}]*)\}", src)}


def read_tips(src):
    tips = {}
    for m in re.finditer(r"static const char\* const (\w+)\[\d+\] = \{([^;]*)\};", strip_comments(src)):
        tips[m.group(1)] = [as_string(a) for a in split_args(m.group(2))]
    return tips


def build_tab(src, consts, colors, specs, presets, values, gaze, tips, tab_index, tab_names, tab_funcs, lang="ja",
              restart=False, update_sample="available"):
    s = Scene(consts, colors, specs, presets, values, gaze, tips, lang, restart, update_sample)
    s.color_presets = read_color_presets(src)
    W, H = int(consts["kWidth"]), int(consts["kHeight"])
    s.rect(0, 0, W, H, 0, s.color("kBg"))
    emit_function(s, src, "drawUi", consts)         # ヘッダー（タイトル・有効・左右比較）
    s.language_buttons()
    x = 40.0                                        # タブ（drawTabs の形を写してある）
    for i, name in enumerate(tab_names):
        w = est_width(name, 22) + 40
        on = i == tab_index
        s.items.append(f'<g class="tab" data-tab="{i}" style="cursor:pointer">')
        s.rect(x, consts["kTabY"], w, consts["kTabH"], 12, s.color("kAccent" if on else "kCard"))
        s.text(x + 20, consts["kTabY"] + 28, name, 22, "kBg" if on else "kText")
        # 新しい版があるのに更新タブを開いていないとき、タブの角に点（実機は drawTabs が描く）
        if tab_funcs[i] == "drawUpdate" and not on and update_sample == "available":
            s.items.append(f'<circle cx="{x + w - 8:.1f}" cy="{consts["kTabY"] + 8}" r="7" fill="{s.color("kWarn")}"/>')
        s.items.append("</g>")
        x += w + 8
    emit_function(s, src, tab_funcs[tab_index], consts)
    y = H - 132                                     # 負荷カード（drawLoad の形だけ。値は実機で入る）
    s.rect(24, y, W - 48, 116, 14, s.color("kCard"))
    s.text(44, y + 34, {"ja": "負荷", "en": "Load", "zh": "负载"}[lang], 22, "kSub", bold=True)
    s.text(120, y + 34, {"ja": "（ここに、実機では計測値が入る）", "en": "(on the device, measured values appear here)",
                              "zh": "（这里在真机上会填入测量值）"}[lang], 22, "kSub")
    return s, W, H


def check(scene, W, tab):
    warns = []
    for x0, x1, y0, y1, t in scene.texts:
        if x1 > W - 20:
            warns.append(f"[{tab}] 右にはみ出す恐れ（見積もり {x1:.0f} px / 幅 {W}）: {t[:30]}")
    ts = scene.texts
    for i in range(len(ts)):
        for j in range(i + 1, len(ts)):
            a, b = ts[i], ts[j]
            if a[0] < b[1] - 2 and b[0] < a[1] - 2 and a[2] < b[3] - 2 and b[2] < a[3] - 2:
                warns.append(f"[{tab}] 文字が重なる恐れ: 「{a[4][:20]}」と「{b[4][:20]}」")
    return warns


PREVIEW_TITLE = "BetterFrame XP パネル プレビュー"


def orca(*args):
    """orca CLI を呼んで JSON を返す。失敗したら None。"""
    exe = shutil.which("orca")
    if not exe:
        return None
    try:
        r = subprocess.run([exe, *args, "--json"], capture_output=True, text=True, encoding="utf-8", timeout=30)
        return json.loads(r.stdout) if r.returncode == 0 else None
    except (OSError, subprocess.SubprocessError, ValueError):
        return None


def open_in_orca(path, tab):
    """Orca 内ブラウザでプレビューを開く。同じタイトルのタブがあれば移動して再読み込み、なければ新規作成。"""
    url = path.resolve().as_uri() + (f"#{tab}" if tab else "")
    listed = orca("tab", "list")
    existing = [t for t in (listed or {}).get("result", {}).get("tabs", []) if t.get("title") == PREVIEW_TITLE]
    if existing:
        page = existing[0]["browserPageId"]
        ok = orca("goto", "--url", url, "--page", page) and orca("reload", "--page", page)
        if not ok:  # 古いタブが閉じかけなどで失敗したら、新しく作る
            ok = orca("tab", "create", "--url", url)
    else:
        ok = orca("tab", "create", "--url", url)
    print("Orca のブラウザで開いた" if ok else "警告: Orca のブラウザを開けなかった（orca CLI が無い、または Orca が起動していない）")


EDGE_CANDIDATES = (
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
)


def find_browser():
    for name in ("msedge", "chrome", "chromium", "google-chrome"):
        exe = shutil.which(name)
        if exe:
            return exe
    return next((p for p in EDGE_CANDIDATES if Path(p).exists()), None)


def png_size(path):
    """PNG の幅と高さ（ヘッダーを読むだけ）。"""
    head = Path(path).read_bytes()[:24]
    if head[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    return int.from_bytes(head[16:20], "big"), int.from_bytes(head[20:24], "big")


def take_shots(page_path, shots, outdir, browser):
    """HTML をヘッドレスのブラウザで開いて、パネルの大きさ（1200x888）の PNG にする。shots = [(ファイル名, タブ, 言語)]。"""
    outdir = outdir.resolve()  # Edge は自分の作業フォルダ基準で解釈するので、絶対パスで渡す
    outdir.mkdir(parents=True, exist_ok=True)
    ok = True
    for name, tab, lang in shots:
        target = outdir / name
        url = page_path.resolve().as_uri() + f"#{tab}-{lang}-shot"
        cmd = [browser, "--headless", "--disable-gpu", "--hide-scrollbars", "--force-device-scale-factor=1",
               "--window-size=1200,888", "--virtual-time-budget=2000", f"--screenshot={target}", url]
        try:
            subprocess.run(cmd, capture_output=True, timeout=60)
        except (OSError, subprocess.SubprocessError) as e:
            print(f"警告: {name} を撮れなかった: {e}")
            ok = False
            continue
        size = png_size(target) if target.exists() else None
        if size != (1200, 888):
            print(f"警告: {name} の大きさが {size}（1200x888 のはず）")
            ok = False
        else:
            print(f"画像: {target}")
    return ok


def build_html(a, raw, specs, presets, values, funcs, start, restart, sample, gaze):
    """全タブ・両言語の SVG を 1 つの HTML にする。(HTML, 警告, タブ名) を返す。"""
    svgs, warns, names = [], [], {}
    for lang in ("ja", "en", "zh"):
        src = resolve_language(raw, lang)  # tr("日本語", "English", "中文") を選んだ言語に置き換えたソース
        consts, colors, tips = read_consts(src), read_colors(src), read_tips(src)
        array = {"ja": "kTabNames", "en": "kTabNamesEn", "zh": "kTabNamesZh"}[lang]
        names[lang] = [unescape(s) for s in STR.findall(re.search(array + r"\[\]\s*=\s*\{([^}]*)\}", raw).group(1))]
        if len(funcs) != len(names[lang]):
            sys.exit(f"タブ名 {len(names[lang])} 個と enum {len(funcs)} 個が合わない")
        for i in range(len(funcs)):
            scene, W, H = build_tab(src, consts, colors, specs, presets, values, gaze, tips, i, names[lang], funcs, lang,
                                    restart, sample)
            warns += check(scene, W, f"{lang} {names[lang][i]}")
            svgs.append(f'<svg data-lang="{lang}" data-tab="{i}" viewBox="0 0 {W} {H}" width="{W}" height="{H}" '
                        f'font-family="\'Noto Sans CJK JP\',\'Yu Gothic UI\',Meiryo,sans-serif" '
                        f'style="display:none;max-width:100%;height:auto">' + "\n".join(scene.items) + "</svg>")
    warn_html = "".join(f"<li>{html.escape(w)}</li>" for w in warns) or "<li>なし</li>"
    state = f"再起動の帯: {'あり' if restart else 'なし'}、更新タブの見本: {sample}"
    page = f"""<!doctype html>
<html lang="ja"><head><meta charset="utf-8"><title>{PREVIEW_TITLE}</title>
<style>body{{margin:0;background:#000;color:#ccc;font:14px sans-serif}}
.note{{padding:8px 16px}}.note ul{{margin:4px 0 0 18px;padding:0}}
body.shot .note{{display:none}}body.shot svg{{max-width:none!important;width:1200px;height:888px}}</style></head><body>
<div class="note">パネルの文言と配置の<b>目安</b>（実機の描画ではない）。タブと右上の「日本語 / English」はクリックで切り替わる。値は {html.escape(Path(a.params).name)}（{state}）。
文字幅は見積もりなので、はみ出し・重なりの警告が出たら実機の <code>--render-png</code> で確かめる。
<br>警告:<ul>{warn_html}</ul></div>
{"".join(svgs)}
<script>
const svgs=[...document.querySelectorAll('svg')];
let lang={json.dumps(start)},tab=0;
function show(){{svgs.forEach(s=>s.style.display=(s.dataset.lang===lang&&+s.dataset.tab===tab)?'block':'none');}}
// URL の #1 や #1-en で、最初に開くタブと言語を選べる。末尾に -shot を付けると説明を消す（画像を撮る用）
const m=location.hash.slice(1).match(/^(\\d+)?(?:-?(ja|en|zh))?(-shot)?$/);
if(m){{if(m[1]&&+m[1]<svgs.length/2)tab=+m[1];if(m[2])lang=m[2];if(m[3])document.body.classList.add('shot');}}
svgs.forEach(s=>s.addEventListener('click',e=>{{
  const t=e.target.closest('.tab'),l=e.target.closest('.lang');
  if(t)tab=+t.dataset.tab;else if(l)lang=l.dataset.lang;else return;show();}}));
show();
</script></body></html>"""
    return page, warns, names


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--params", default=str(ROOT / "contrib" / "params.conf"), help="値を読む params.conf")
    ap.add_argument("--out", default=str(ROOT / "tmp" / "panel-preview.html"), help="出力先（tmp/ は git 管理外）")
    ap.add_argument("--lang", choices=("ja", "en", "zh"), default=None,
                    help="最初に開く言語（既定は params.conf の uiLanguage）。ブラウザ上のボタンでも切り替わる")
    ap.add_argument("--gaze-reproject", action="store_true", help="奥行きの補正スイッチを ON で描く")
    ap.add_argument("--gaze-eye-to-head-off", action="store_true", help="目の位置の補正スイッチを OFF で描く")
    ap.add_argument("--restart", action="store_true", help="「SteamVR の再起動が必要」の帯を出して描く")
    ap.add_argument("--update-sample", choices=("uptodate", "available", "running", "done", "failed", "error"), default="available",
                    help="更新タブの見本の状態（実機は GitHub の確認結果で決まる）")
    ap.add_argument("--open", action="store_true", help="作ったあと Orca 内ブラウザで開く（開いていれば再読み込み）")
    ap.add_argument("--tab", type=int, default=0, help="--open で最初に開くタブ（0 から。画質=0 実験的=1 SteamVR 設定=2 更新=3）")
    ap.add_argument("--shots", metavar="DIR", default=None,
                    help="GitHub に載せる画像を DIR に PNG（1200x888）で書く。ヘッドレスの Edge/Chrome を使う")
    a = ap.parse_args()

    raw = (SRC / "panel_main.cpp").read_text(encoding="utf-8")
    pcpp = (SRC / "params.cpp").read_text(encoding="utf-8")
    specs, presets = read_specs(pcpp), read_presets(pcpp)
    if not specs:
        sys.exit("params.cpp からスライダーを読めなかった")
    values = read_conf(a.params)
    start = a.lang or {0: "ja", 1: "en", 2: "zh"}.get(int(values.get("uiLanguage", 0)), "ja")
    gaze = {"known": True, "reproject": a.gaze_reproject, "eyeToHead": not a.gaze_eye_to_head_off}
    enum = re.search(r"enum class Tab\s*\{([^}]*)\}", raw).group(1)
    funcs = ["draw" + e.strip() for e in enum.split(",") if e.strip()]

    page, warns, names = build_html(a, raw, specs, presets, values, funcs, start, a.restart, a.update_sample, gaze)
    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(page, encoding="utf-8")
    print(f"written {out}  (tabs: {', '.join(names['ja'])} / {', '.join(names['en'])}; specs {len(specs)}, presets {len(presets)})")
    for w in warns:
        print("警告:", w)
    if a.shots:
        browser = find_browser()
        if not browser:
            sys.exit("画像を撮るブラウザ（Edge / Chrome）が見つからない")
        slugs = [f[4:].lower() for f in funcs]  # drawQuality -> quality
        outdir = Path(a.shots)
        # 通常の画像（再起動の帯なし・更新タブは --update-sample の見本）
        normal = out.with_name(out.stem + "-shots.html")
        normal.write_text(build_html(a, raw, specs, presets, values, funcs, start, False, a.update_sample, gaze)[0], encoding="utf-8")
        shots = [(f"panel-{slug}-{lang}.png", i, lang) for lang in ("ja", "en", "zh") for i, slug in enumerate(slugs)]
        ok = take_shots(normal, shots, outdir, browser)
        # 再起動の帯つき（画質タブ）
        banner = out.with_name(out.stem + "-shots-restart.html")
        banner.write_text(build_html(a, raw, specs, presets, values, funcs, start, True, a.update_sample, gaze)[0], encoding="utf-8")
        ok = take_shots(banner, [(f"panel-restart-{lang}.png", 0, lang) for lang in ("ja", "en", "zh")], outdir, browser) and ok
        print("画像を書いた" if ok else "警告: 撮れなかった画像がある")
    if a.open:
        open_in_orca(out, f"{a.tab}-{start}")


if __name__ == "__main__":
    main()
