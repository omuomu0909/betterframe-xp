// betterframe-xp-panel: SteamVR ダッシュボードのパネル。params.conf を編集し、レイヤーの負荷を表示する。
#include "draw.h"
#include "gpu_usage.h"
#include "layer_state.h"
#include "params.h"
#include "update_check.h"
#include "vr_overlay.h"

#include <signal.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kWidth = 1200;
constexpr int kHeight = 888;
constexpr int kThumb = 256;
constexpr const char* kFont = "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc";
constexpr const char* kBold = "/usr/share/fonts/noto-cjk/NotoSansCJK-Bold.ttc";

constexpr Color kBg{0.10, 0.11, 0.13};
constexpr Color kCard{0.16, 0.17, 0.20};
constexpr Color kText{0.93, 0.94, 0.96};
constexpr Color kSub{0.66, 0.69, 0.74};
constexpr Color kAccent{0.35, 0.65, 1.00};
constexpr Color kTrack{0.30, 0.32, 0.36};
constexpr Color kWarn{1.00, 0.70, 0.30};

volatile sig_atomic_t gStop = 0;
void stopHandler(int) { gStop = 1; }

/**
 * パネルのタブ（左から）。画質 = よく使う設定（段落ごとに主設定と副設定を分けている）、実験的 = 確認が十分でない機能、
 * SteamVR 設定 = Frame の SteamVR 自体の設定を書き換えるもの。SteamVR の設定を書くのは SteamVR 設定タブだけ。
 * 更新 = GitHub の最新リリースを確認して、ここから入れ替える。
 */
enum class Tab { Quality, Color, Experimental, SteamVr, Update };
constexpr const char* kTabNames[] = {"画質", "色調整", "実験的", "SteamVR 設定", "更新"};
constexpr const char* kTabNamesEn[] = {"Quality", "Colour", "Experimental", "SteamVR settings", "Update"};
constexpr const char* kTabNamesZh[] = {"画质", "色彩", "实验性", "SteamVR 设置", "更新"};
constexpr int kTabCount = 5;

enum class HitKind { Enabled, ShowSplit, Tab, Slider, Preset, StreamOnly, CasCross, CasLuma, CornerSkip, GrainAnimate,
                     GazeReproject, GazeEyeToHead, Language, UpdateCheck, UpdateInstall, UpdateAuto, ColorPreset };
struct Hit {
    HitKind kind;
    int arg;
    double x, y, w, h;
};

/** 負荷表示に使う値（1 秒おきに更新）。 */
struct Load {
    LayerStats layer;
    double gpuTotalPct = NAN;  ///< 全プロセスの GPU 使用率（fdinfo）
    double gpuFreqMhz = -1, gpuMaxMhz = -1;
    double panelCpuPct = NAN;  ///< このパネル自身の CPU 使用率（1 コア = 100%）
};

/** SteamVR の視線の補正（params.conf ではなく SteamVR の設定。IVRSettings で読み書きする）。 */
struct Gaze {
    bool known = false;      ///< SteamVR から読めた
    bool reproject = false;  ///< 奥行きの補正（steamvr.gazeDepReprojectEnable）
    bool eyeToHead = false;  ///< 目の位置の補正（steamvr.gazeDepEyeToHeadEnable）
    bool failed = false;     ///< 直近の書き込みに失敗した
};

struct UiState {
    std::map<std::string, double> values;  ///< uiLanguage も含む（0 = 日本語、1 = English、2 = 中国語）
    Tab tab = Tab::Quality;
    int activeSlider = -1;
    bool dirty = false;  ///< 未保存の変更あり
    Load load;
    Gaze gaze;
    int gazeRequest = 0;  ///< 1 = 奥行きを反転、2 = 目の位置を反転（メインループが SteamVR に書く）
    LayerState layer;     ///< 動いている SteamVR が、入れたレイヤーを読んでいるか（再起動の要否）
    UpdateInfo update;    ///< 更新タブの内容（メインループが Updater から写す）
    int updateRequest = 0;  ///< 1 = 今すぐ確認、2 = 新しい版を入れる（メインループが Updater に渡す）
};

double value(const UiState& ui, const char* key, double def) {
    const auto it = ui.values.find(key);
    return it == ui.values.end() ? def : it->second;
}

/** 表示言語。drawUi が uiLanguage から毎回決める。 */
bool gEnglish = false;
bool gChinese = false;
/** 今の表示言語の文字列を返す（日本語 = ja、English = en、中国語 = zh）。 */
const char* tr(const char* ja, const char* en, const char* zh) { return gChinese ? zh : (gEnglish ? en : ja); }

// ---- 描画 ----

constexpr double kSliderX = 330, kSliderW = 560, kRowsTop = 196;
constexpr double kTabY = 108, kTabH = 40;

void toggle(const Pen& pen, double x, double y, const char* label, bool on, std::vector<Hit>& hits, HitKind kind) {
    pen.roundedRect(x, y, 64, 34, 17);
    pen.color(on ? kAccent : kTrack);
    cairo_fill(pen.cr);
    cairo_arc(pen.cr, on ? x + 47 : x + 17, y + 17, 13, 0, 2 * M_PI);
    pen.color(kText);
    cairo_fill(pen.cr);
    pen.text(x + 78, y + 26, label, 22, kText);
    hits.push_back({kind, 0, x, y - 6, 78 + pen.measure(label, 22), 46});
}

void drawTabs(const Pen& pen, const UiState& ui, std::vector<Hit>& hits) {
    double x = 40;
    for (int i = 0; i < kTabCount; ++i) {
        const bool on = static_cast<int>(ui.tab) == i;
        const char* name = tr(kTabNames[i], kTabNamesEn[i], kTabNamesZh[i]);
        const double w = pen.measure(name, 22) + 40;
        pen.roundedRect(x, kTabY, w, kTabH, 12);
        pen.color(on ? kAccent : kCard);
        cairo_fill(pen.cr);
        pen.text(x + 20, kTabY + 28, name, 22, on ? kBg : kText);
        hits.push_back({HitKind::Tab, i, x, kTabY, w, kTabH});
        // 新しい版があるのに、更新タブを開いていないとき: タブの角に点を出す
        if (i == static_cast<int>(Tab::Update) && !on && ui.update.available && ui.update.install != "running") {
            cairo_arc(pen.cr, x + w - 8, kTabY + 8, 7, 0, 2 * M_PI);
            pen.color(kWarn);
            cairo_fill(pen.cr);
        }
        x += w + 8;
    }
}

/** 再起動の帯の右端。右上の言語ボタン（日本語 / English / 中文 の 3 つ）と重ならない位置。 */
constexpr double kRestartBannerRight = 860;

/** SteamVR の再起動が要るときの帯（左上。再起動するまで、入れた内容・設定が反映されない）。 */
void drawRestartBanner(const Pen& pen) {
    pen.roundedRect(24, 4, kRestartBannerRight - 24, 28, 10);
    pen.color(kWarn);
    cairo_fill(pen.cr);
    const char* title = tr("SteamVR の再起動が必要", "SteamVR restart required", "SteamVR 需要重启");
    pen.text(40, 25, title, 20, kBg, true);
    pen.text(40 + pen.measure(title, 20, true) + 20, 25,
             tr("再起動するまで、入れた内容は反映されない", "What you installed takes effect after SteamVR restarts", "重启之前，安装的内容不会生效"), 17, kBg);
}

/** 表示言語の切り替え（右上。日本語 / English / 中文）。押すと params.conf の uiLanguage が 0 / 1 / 2 になる。 */
void drawLanguage(const Pen& pen, const UiState& ui, std::vector<Hit>& hits) {
    constexpr const char* kNames[3] = {"日本語", "English", "中文"};
    constexpr double kSize = 19, kY = 4, kH = 28;
    const int current = std::clamp(static_cast<int>(value(ui, "uiLanguage", 0)), 0, 2);
    double widths[3];
    for (int i = 0; i < 3; ++i) widths[i] = pen.measure(kNames[i], kSize) + 36;
    double x = kWidth - 40 - (widths[0] + widths[1] + widths[2]);
    for (int i = 0; i < 3; ++i) {
        const bool on = current == i;
        pen.roundedRect(x, kY, widths[i], kH, 10);
        pen.color(on ? kAccent : kCard);
        cairo_fill(pen.cr);
        pen.text(x + 18, kY + 20, kNames[i], kSize, on ? kBg : kText);
        hits.push_back({HitKind::Language, i, x, kY, widths[i], kH});
        x += widths[i];
    }
}

/** 項目の key から paramSpecs() の添字を引く（無ければ -1）。 */
int specIndex(const char* key) {
    const auto& specs = paramSpecs();
    for (size_t i = 0; i < specs.size(); ++i)
        if (std::strcmp(specs[i].key, key) == 0) return static_cast<int>(i);
    return -1;
}

/** 段落の見出し（y は文字の基準線）と、その下の区切り線。note があれば見出しの右に小さく添える。 */
void heading(const Pen& pen, double y, const char* text, const char* note = nullptr) {
    pen.text(40, y, text, 22, kAccent, true);
    if (note) pen.text(40 + pen.measure(text, 22, true) + 24, y, note, 17, kSub);
    cairo_rectangle(pen.cr, 40, y + 10, kWidth - 80, 1);
    pen.color(kTrack);
    cairo_fill(pen.cr);
}

/** 1 本のスライダー。y はつまみの中心、indent はラベルと説明の x（副設定は字下げする）。 */
void drawSliderAt(const Pen& pen, const UiState& ui, size_t i, double y, double indent, std::vector<Hit>& hits) {
    const ParamSpec& s = paramSpecs()[i];
    const double v = value(ui, s.key, s.min);
    const double t = std::clamp((v - s.min) / (s.max - s.min), 0.0, 1.0);
    pen.text(indent, y + 8, tr(s.label, s.labelEn, s.labelZh), 22, kText);
    pen.text(indent, y + 36, tr(s.desc, s.descEn, s.descZh), 17, kSub);
    pen.roundedRect(kSliderX, y - 4, kSliderW, 8, 4);
    pen.color(kTrack);
    cairo_fill(pen.cr);
    pen.roundedRect(kSliderX, y - 4, kSliderW * t, 8, 4);
    pen.color(kAccent);
    cairo_fill(pen.cr);
    cairo_arc(pen.cr, kSliderX + kSliderW * t, y, ui.activeSlider == static_cast<int>(i) ? 15 : 12, 0, 2 * M_PI);
    pen.color(kText);
    cairo_fill(pen.cr);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.*f", s.decimals, v);
    pen.text(kSliderX + kSliderW + 90, y + 8, buf, 22, kText, true, true);
    hits.push_back({HitKind::Slider, static_cast<int>(i), kSliderX - 20, y - 24, kSliderW + 40, 48});
}

/** プリセットのボタンを x から横に並べる（ラベルは呼び出し側）。 */
void drawPresets(const Pen& pen, bool beta, double x, double y, std::vector<Hit>& hits) {
    const auto& list = presets();
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].beta != beta) continue;
        const char* name = tr(list[i].name, list[i].nameEn, list[i].nameZh);
        const double w = pen.measure(name, 22) + 40;
        pen.roundedRect(x, y, w, 44, 10);
        pen.color(kCard);
        cairo_fill(pen.cr);
        pen.text(x + 20, y + 30, name, 22, kText);
        hits.push_back({HitKind::Preset, static_cast<int>(i), x, y, w, 44});
        x += w + 14;
    }
}

/** 設定を上げすぎたときの注意（画質タブ）。負荷カードの上に 1 行で置く。 */
void drawCaution(const Pen& pen) {
    pen.roundedRect(24, 662, kWidth - 48, 86, 10);
    pen.color(kCard);
    cairo_fill(pen.cr);
    pen.text(44, 686, tr("注意: 設定を上げすぎると Frame の負荷が高まり、ガクッが出ることがある。温度が上がりファンが動くことも", "Caution: high settings add load on Frame and can cause stutter, heat, and fan noise.", "注意: 设置过高会加重 Frame 的负载，可能出现卡顿。温度升高时风扇也会转起来"), 19, kWarn);
    pen.text(44, 712, tr("負荷が高いほど、Frame のバッテリーの消費も早くなる", "The higher the load, the faster Frame's battery drains.", "负载越高，Frame 的电池消耗也越快"), 19, kWarn);
    pen.text(44, 738, tr("144 Hz 推奨。低リフレッシュレートでは映像が乱れる", "144 Hz recommended. Low refresh rates garble video", "推荐 144 Hz。刷新率更低时画面会错乱"), 19, kWarn);
}

/** key の項目を、ラベルの x を指定してスライダーとして描く（画質タブの段落ごとに使う）。 */
void drawSliderByKey(const Pen& pen, const UiState& ui, const char* key, double y, double indent, std::vector<Hit>& hits) {
    const int i = specIndex(key);
    if (i >= 0) drawSliderAt(pen, ui, static_cast<size_t>(i), y, indent, hits);
}

/** 副設定の縦線（上の主設定の下に従属することを示す）。 */
void subBar(const Pen& pen, double y0, double y1) {
    cairo_rectangle(pen.cr, 52, y0, 3, y1 - y0);
    pen.color(kTrack);
    cairo_fill(pen.cr);
}

void drawQuality(const Pen& pen, const UiState& ui, std::vector<Hit>& hits) {
    // 段落 1: シャープ。シャープ量が 0 のとき、縦線の下の副設定は効かない
    heading(pen, 176, tr("シャープ", "Sharpen", "锐化"),
            tr("シャープ量を 0 にすると、縦線の下の副設定は効かない", "With Sharpen amount at 0, the sub-settings below the bar do nothing", "锐化量设为 0 时，竖线下的副设置不生效"));
    drawSliderByKey(pen, ui, "sharpenAmount", 214, 40, hits);
    subBar(pen, 258, 382);
    drawSliderByKey(pen, ui, "sharpness", 268, 70, hits);
    pen.text(70, 342, tr("シャープ方式", "Sharpen method", "锐化方式"), 22, kSub);
    // 方式のスイッチ（ON が軽い版。全部 OFF で元の CAS）
    toggle(pen, kSliderX, 316, tr("十字だけ読む", "Cross only", "只读十字"), value(ui, "casCross", 1) >= 0.5, hits, HitKind::CasCross);
    toggle(pen, kSliderX + 270, 316, tr("明るさだけ", "Luma only", "只读亮度"), value(ui, "casLuma", 1) >= 0.5, hits, HitKind::CasLuma);
    toggle(pen, kSliderX + 520, 316, tr("四隅を省く", "Skip corners", "跳过四角"), value(ui, "cornerSkip", 1.15) > 0, hits, HitKind::CornerSkip);
    pen.text(kSliderX, 376, tr("ON で軽く（十字だけ: 9 点→5 点 / 明るさだけ / 四隅を省く）。全部 OFF で元の CAS", "ON = lighter (Cross only: 9 → 5 taps / Luma only / Skip corners). All OFF = original CAS", "开启可减轻负担（只读十字: 9 点→5 点 / 只读亮度 / 跳过四角）。全部关闭则为原始 CAS"), 17, kSub);

    // 段落 2: 中心の重み（3 つの効果すべてに掛かる）
    heading(pen, 418, tr("中心の重み", "Center weight", "中心权重"),
            tr("シャープ・デバンド・グレインの効きを、画面の中央と周辺で変える", "Changes how sharpen, deband and grain apply between the center and the edges", "改变锐化、去色带、颗粒在画面中心与边缘的强度"));
    drawSliderByKey(pen, ui, "centerRadius", 458, 40, hits);
    drawSliderByKey(pen, ui, "edgeAmount", 514, 40, hits);

    // 段落 3: プリセット
    heading(pen, 592, tr("プリセット", "Presets", "预设"), tr("左右比較は右上のスイッチで見比べられる", "Compare with the Split compare switch at the top right", "可用右上角的开关切换左右对比"));
    drawPresets(pen, false, 40, 616, hits);
    drawCaution(pen);
}

/** 「色調整」タブのショートカット（この 5 つだけを変える。シャープなどは触らない）。 */
struct ColorPreset {
    const char* name;
    const char* nameEn;
    const char* nameZh;
    double contrast, saturation, gamma, temperature, brightness;
};
constexpr ColorPreset kColorPresets[] = {
    {"ニュートラル", "Neutral", "中性", 1.00, 1.00, 1.00, 1.00, 1.00},
    {"ビビッド", "Vivid", "鲜明", 1.03, 1.10, 1.01, 1.01, 1.01},
    {"ソフト", "Soft", "柔和", 0.95, 0.90, 1.05, 1.00, 0.98},
    {"アイケア", "Eye care", "护眼", 1.05, 1.10, 1.00, 1.05, 1.00},
};
constexpr int kColorPresetCount = 4;

/** 色のショートカットを x から横に並べる（ラベルは kColorPresets）。 */
void drawColorPresets(const Pen& pen, double x, double y, std::vector<Hit>& hits) {
    for (int i = 0; i < kColorPresetCount; ++i) {
        const char* label = tr(kColorPresets[i].name, kColorPresets[i].nameEn, kColorPresets[i].nameZh);
        const double w = pen.measure(label, 22) + 48;
        pen.roundedRect(x, y, w, 44, 10);
        pen.color(kCard);
        cairo_fill(pen.cr);
        pen.text(x + 24, y + 30, label, 22, kText);
        hits.push_back({HitKind::ColorPreset, i, x, y, w, 44});
        x += w + 18;
    }
}

/**
 * 「色調整」タブ: コントラスト・彩度・ガンマ・色温度・明るさ。どれもシャープ・デバンド・グレインの
 * あとで最終的な画面に掛かる。1.0 は「変化なし」で、そのときは着色器がこの段落ごと飛ばす。
 */
void drawColor(const Pen& pen, const UiState& ui, std::vector<Hit>& hits) {
    heading(pen, 176, tr("色調整", "Colour", "色彩校正"),
            tr("シャープ・デバンド・グレインのあとで、この順に最終画面を整える",
               "Applied last, after sharpen, deband and grain",
               "在锐化、去色带、颗粒之后，按这个顺序调整最终画面"));
    drawSliderByKey(pen, ui, "contrast", 214, 40, hits);
    drawSliderByKey(pen, ui, "saturation", 272, 40, hits);
    drawSliderByKey(pen, ui, "gamma", 330, 40, hits);
    drawSliderByKey(pen, ui, "temperature", 388, 40, hits);
    drawSliderByKey(pen, ui, "brightness", 446, 40, hits);

    // ショートカット（この 5 つだけを変える）
    pen.text(40, 526, tr("ショートカット", "Quick", "快捷"), 22, kSub);
    drawColorPresets(pen, 212, 496, hits);

    // 説明（負荷カードの上）
    pen.roundedRect(24, 570, kWidth - 48, 170, 12);
    pen.color(kCard);
    cairo_fill(pen.cr);
    pen.text(44, 600, tr("1.0 で変化なし。動かすとすぐ params.conf に書かれ、画面に反映される",
                         "1.0 = unchanged. Dragging writes params.conf immediately.",
                         "1.0 = 不改变。拖动后立即写入 params.conf，画面即时生效"), 18, kText);
    pen.text(44, 628, tr("色温度は明るさを保つように正規化してある（暖色にしても暗くならない）",
                         "Temperature is luma-normalised: warm/cool leaves the overall brightness alone.",
                         "色温按亮度归一化：调暖调冷都不会让整体变亮或变暗"), 18, kSub);
    pen.text(44, 656, tr("中心の重みの影響を受けず、目全体に均一に掛かる",
                         "Applied evenly over the eye; Center weight does not affect it.",
                         "整只眼睛均匀施加，不受「中心权重」影响"), 18, kSub);
    pen.text(44, 684, tr("右上の「左右比較」で、左半分（素通し）と右半分（色調整後）を見比べられる",
                         "Split compare at the top right shows the raw left half next to the graded right half.",
                         "右上角打开「左右对比」：左半原样、右半校色后，可直接比较"), 18, kSub);
    pen.text(44, 712, tr("PC 側のドライバの色設定と重ねると二重になる。どちらか一方で調整する",
                         "Stacks with GPU-driver colour settings; adjust in one place only.",
                         "与显卡驱动的色彩调整叠加会重复，建议只在一处调"), 18, kSub);
}

/** 1 項目分: スイッチと、切り替えたときどうなるかの Tips（3 行まで）。 */
void gazeItem(const Pen& pen, double y, const char* label, bool on, HitKind kind, const char* const (&tips)[3],
              std::vector<Hit>& hits) {
    toggle(pen, 40, y, label, on, hits, kind);
    for (int i = 0; i < 3; ++i) pen.text(40, y + 62 + i * 26, tips[i], 18, i == 0 ? kText : kSub);
}

void drawSteamVr(const Pen& pen, const UiState& ui, std::vector<Hit>& hits) {
    // このタブのスイッチだけが、このツールの外（Frame の SteamVR の設定）を書き換える
    heading(pen, 176, tr("Frame の SteamVR の設定", "Frame's SteamVR settings", "Frame 上 SteamVR 的设置"),
            tr("BetterFrame XP ではなく、SteamVR 本体の設定", "Not BetterFrame XP itself, but SteamVR's own settings", "不是 BetterFrame XP 的设置，而是 SteamVR 本体"));
    pen.text(40, 214, tr("このタブのスイッチは、Frame の SteamVR の設定を書き換える", "The switches on this tab rewrite Frame's SteamVR settings", "这个标签页的开关会改写 Frame 上 SteamVR 的设置"), 20, kWarn);
    pen.text(40, 242, tr("最初の値は記録してあり、アンインストールで元に戻る。Valve が設定画面から外した項目", "The first values are saved, and uninstalling restores them. Items Valve removed from the settings screen.", "初始值已记录，卸载时会还原。这些是 Valve 从设置界面移除的项目"), 17, kSub);

    static const char* const kReprojJa[3] = {
        "OFF にすると、視界の端を見ながら頭（HMD）を動かしたときのガクつきが減る",
        "作者の環境で一番効いた変更。デメリットは作者は感じていない（1 人・1 台の体感）",
        "ON（Valve の機能）: フルレートが出ないときのカクつきを減らす狙い"};
    static const char* const kReprojEn[3] = {
        "OFF reduces judder when you move your head (HMD) while looking at the edge of the view",
        "The change that helped most on the author's setup. No downside noticed (one person, one device).",
        "ON (Valve's feature): aims to reduce stutter when the full rate can't be held"};
    static const char* const kReprojZh[3] = {
        "关闭后，一边看着视野边缘一边移动头部（HMD）时的卡顿会减少",
        "在作者环境下最有效的改动。作者没有感到缺点（单人单机的主观感受）",
        "开启（Valve 的功能）: 旨在减少达不到满帧时的抖动"};
    static const char* const kEyeJa[3] = {
        "OFF にすると、頭を動かしたときのガクつきが減る",
        "奥行きと一緒に OFF にして安定した（単独の効果は未確認）。OFF は Valve の既定値と同じ",
        "ON: 眼球の回転に合わせて、映像を作るときの目の位置をずらす"};
    static const char* const kEyeEn[3] = {
        "OFF reduces judder when you move your head",
        "Stable with OFF together with depth correction (effect alone not verified). OFF matches Valve's default.",
        "ON: shifts the eye position used for rendering to follow eye rotation"};
    static const char* const kEyeZh[3] = {
        "关闭后，移动头部时的卡顿会减少",
        "与景深一起关闭后变得稳定（单独效果未验证）。关闭等同于 Valve 的默认值",
        "开启: 跟随眼球转动，偏移成像时使用的眼睛位置"};
    gazeItem(pen, kRowsTop + 80, tr("奥行きの補正（視線認識型の再投影）", "Depth correction (gaze-aware reprojection)", "景深校正（视线追踪重投影）"),
             ui.gaze.reproject, HitKind::GazeReproject, gChinese ? kReprojZh : (gEnglish ? kReprojEn : kReprojJa), hits);
    gazeItem(pen, kRowsTop + 230, tr("目の位置の補正（瞳孔位置補正と思われる）", "Eye position correction (probably pupil position correction)", "眼位校正（推测为瞳孔位置校正）"),
             ui.gaze.eyeToHead, HitKind::GazeEyeToHead, gChinese ? kEyeZh : (gEnglish ? kEyeEn : kEyeJa), hits);

    const double sy = kRowsTop + 374;
    if (!ui.gaze.known) {
        pen.text(40, sy, tr("SteamVR から読めません（SteamVR につながっていない）", "Can't read from SteamVR (not connected to SteamVR)", "无法从 SteamVR 读取（未连接到 SteamVR）"), 18, kWarn);
    } else if (ui.gaze.failed) {
        pen.text(40, sy, tr("Frame の SteamVR の設定を書き換えられませんでした", "Could not rewrite Frame's SteamVR settings", "未能改写 Frame 上 SteamVR 的设置"), 18, kWarn);
    } else {
        pen.text(40, sy, tr("Frame の SteamVR の設定に保存される。効かなければ SteamVR を再起動する", "Saved to Frame's SteamVR settings. If it has no effect, restart SteamVR.", "会保存到 Frame 上 SteamVR 的设置。若不生效请重启 SteamVR"), 18, kSub);
    }

    // 作者の PC 側の SteamVR 設定（この表示は読むだけ。パネルは PC 側の設定を変えない）
    heading(pen, 606, tr("参考: 作者の PC 側の SteamVR 設定", "For reference: the author's PC-side SteamVR settings", "参考: 作者 PC 侧的 SteamVR 设置"),
            tr("このパネルでは変えない。1 人の体感", "Not changed here. One person's view", "本面板不会改动。单人的主观感受"));
    constexpr double kReasonX = 440;
    pen.text(40, 646, tr("高度なスーパーサンプリング: オフ", "Advanced supersampling: Off", "高级超级采样: 关闭"), 20, kText);
    pen.text(kReasonX, 646, tr("ぼやけた感じが消える。SteamVR のアップデートで治る可能性がある", "The blurry look goes away. A SteamVR update may fix it.", "模糊感会消失。SteamVR 更新后可能修复"), 17, kSub);
    pen.text(40, 680, tr("解像度: 200〜300%（作者は 250%）", "Resolution: 200-300% (author: 250%)", "分辨率: 200〜300%（作者为 250%）"), 20, kText);
    pen.text(kReasonX, 680, tr("GPU に余裕があれば高いほうが、圧縮でぼやっとする感覚が減る", "If your GPU has headroom, higher means less of the compression blur.", "GPU 有余量时调高，压缩造成的模糊感会减少"), 17, kSub);
    pen.text(40, 714, tr("リフレッシュレート: 144 Hz", "Refresh rate: 144 Hz", "刷新率: 144 Hz"), 20, kText);
    pen.text(kReasonX, 714, tr("144 Hz と 120 Hz 以外はまだ安定しない", "Other than 144 and 120 Hz, still unstable", "144 Hz 和 120 Hz 以外仍不稳定"), 17, kWarn);
}

void drawExperimental(const Pen& pen, const UiState& ui, std::vector<Hit>& hits) {
    // 段落 1: デバンド。デバンド量が 0 のとき、縦線の下の副設定は効かない
    // 不具合の注意（オレンジ）を先に、副設定の説明をその右に置く
    const char* debandTitle = tr("デバンド", "Deband", "去色带");
    const char* debandBug = tr("デバンド量を調整すると映像が乱れる不具合がある", "Adjusting Deband amount has a bug that garbles the video.", "调去色带量时有画面错乱的问题");
    heading(pen, 176, debandTitle);
    const double debandBugX = 40 + pen.measure(debandTitle, 22, true) + 24;
    pen.text(debandBugX, 176, debandBug, 17, kWarn);
    pen.text(debandBugX + pen.measure(debandBug, 17) + 24, 176,
             tr("0 だと縦線の下の副設定は効かない", "At 0, the sub-settings below the bar do nothing", "为 0 时竖线下的副设置不生效"), 17, kSub);
    drawSliderByKey(pen, ui, "debandAmount", 212, 40, hits);
    subBar(pen, 256, 364);
    drawSliderByKey(pen, ui, "debandRange", 268, 70, hits);
    drawSliderByKey(pen, ui, "debandThreshold", 322, 70, hits);

    // 段落 2: シャープの補助。画質タブのシャープ量の「掛かり方」を変えるだけで、単独では何もしない
    heading(pen, 398, tr("シャープの補助", "Sharpen assist", "锐化辅助"),
            tr("画質タブの「シャープ量」が 0 のときは効かない", "Does nothing when Sharpen amount on the Quality tab is 0", "画质标签页的「锐化量」为 0 时不生效"));
    drawSliderByKey(pen, ui, "detailBoost", 434, 40, hits);

    // 段落 3: グレイン。グレイン量が 0 のとき、縦線の下の副設定は効かない
    heading(pen, 518, tr("グレイン", "Grain", "颗粒"),
            tr("グレイン量を 0 にすると、縦線の下の副設定は効かない", "With Grain amount at 0, the sub-settings below the bar do nothing", "颗粒量设为 0 时，竖线下的副设置不生效"));
    drawSliderByKey(pen, ui, "grainAmount", 554, 40, hits);
    subBar(pen, 598, 696);
    drawSliderByKey(pen, ui, "grainSize", 610, 70, hits);
    pen.text(70, 684, tr("粒の動き", "Grain motion", "颗粒运动"), 22, kSub);
    const char* animLabel = tr("粒を動かす", "Animate grain", "让颗粒动起来");
    toggle(pen, kSliderX, 658, animLabel, value(ui, "grainAnimate", 1) >= 0.5, hits, HitKind::GrainAnimate);
    const double animLabelW = pen.measure(animLabel, 22);
    pen.text(kSliderX + 78 + animLabelW + 24, 684,
             tr("ON: 毎フレーム粒の並びを変える / OFF: 固定", "ON: grain pattern changes every frame / OFF: fixed", "开启: 每帧改变颗粒排列 / 关闭: 固定"), 17, kSub);

    // プリセットと注意（負荷カードの上）
    pen.text(40, 736, tr("プリセット", "Presets", "预设"), 22, kSub);  // 基準線はボタン（y+30）と合わせる
    drawPresets(pen, true, kSliderX, 706, hits);
    const double noteX = gEnglish ? 740 : 640;  // 英語はプリセットのボタンが長いので右へ寄せる
    pen.text(noteX, 724, tr("効果の確認が不十分。乱れたら該当の値を 0 に戻す", "Unverified: if it looks off, set it to 0", "验证不充分。画面错乱时把对应值改回 0"), 17, kWarn);
    pen.text(noteX, 746, tr("3 つとも、画質タブの「中心の重み」で周辺ほど弱まる", "All fade toward the edges (Center weight)", "三者都会因画质标签页的「中心权重」而在边缘减弱"), 17, kSub);
}

/** 押せるボタン。primary は目立たせる色、enabled が false なら暗くして押せない（当たり判定を作らない）。幅を返す。 */
double button(const Pen& pen, double x, double y, const char* label, bool primary, bool enabled, HitKind kind,
              std::vector<Hit>& hits) {
    const double w = pen.measure(label, 22) + 48;
    pen.roundedRect(x, y, w, 44, 10);
    pen.color(primary && enabled ? kAccent : kCard);
    cairo_fill(pen.cr);
    pen.text(x + 24, y + 30, label, 22, primary && enabled ? kBg : (enabled ? kText : kTrack));
    if (enabled) hits.push_back({kind, 0, x, y, w, 44});
    return w;
}

/** 確認できなかった理由（betterframe-xp-update check の error）を文にする。 */
std::string updateErrorText(const std::string& code) {
    if (code == "not-found")
        return tr("リリースが見つからない（まだ公開されていないか、非公開のリポジトリ）", "No release found (not published yet, or the repository is private)", "找不到发行版（尚未发布，或仓库未公开）");
    if (code == "network") return tr("GitHub に繋がらない", "Cannot reach GitHub", "无法连接 GitHub");
    if (code == "rate-limited") return tr("GitHub への問い合わせが多すぎる。しばらくしてから", "GitHub rate limit; try again later", "GitHub 请求过于频繁，请稍后再试");
    if (code == "bad-version") return tr("リリースの版の書き方を読めない", "Cannot read the release's version", "无法解析发行版的版本号");
    return tr("確認できなかった", "Could not check", "检查失败");
}

/** インストールの失敗の理由（betterframe-xp-update の error）を文にする。 */
std::string installErrorText(const std::string& code) {
    if (code == "no-checksums")
        return tr("リリースに、入れるファイルか SHA256SUMS が無い", "The release has no package file or SHA256SUMS", "发行版里没有可安装的文件或 SHA256SUMS");
    if (code == "checksum-mismatch")
        return tr("検証に失敗した（ファイルが壊れている）。何も入れていない", "Checksum mismatch (corrupt download). Nothing was installed", "校验失败（文件损坏）。没有安装任何内容");
    if (code == "download-failed") return tr("ダウンロードに失敗した", "Download failed", "下载失败");
    if (code == "install-failed") return tr("インストールに失敗した", "Install failed", "安装失败");
    if (code == "interrupted") return tr("途中で止まった", "It stopped partway", "中途停止");
    if (code == "busy") return tr("すでに更新を実行中", "An update is already running", "已经在执行更新");
    return tr("更新に失敗した", "Update failed", "更新失败");
}

/** 更新タブの、状態で変わる部分（版・確認・入れる・進み）。見た目の確認は panel_preview.py の見本で行う。 */
void drawUpdateStatus(const Pen& pen, const UiState& ui, std::vector<Hit>& hits) {
    const UpdateInfo& u = ui.update;
    pen.text(40, 232, tr("いまの版", "Installed", "当前版本"), 21, kSub);
    pen.text(200, 232, u.current, 26, kText, true);
    pen.text(40, 274, tr("最新の版", "Latest", "最新版本"), 21, kSub);
    if (!u.error.empty()) {
        pen.text(200, 274, updateErrorText(u.error), 21, kWarn);
    } else if (!u.checked) {
        pen.text(200, 274, u.checking ? tr("確認中…", "Checking...", "检查中…") : tr("まだ確認していない", "Not checked yet", "尚未检查"), 21, kSub);
    } else if (u.available) {
        const double w = pen.text(200, 274, u.latest, 26, kAccent, true);
        pen.text(200 + w + 16, 274, tr("新しい版がある", "A new version is available", "有新版本"), 21, kWarn);
    } else {
        const double w = pen.text(200, 274, u.latest, 26, kText, true);
        pen.text(200 + w + 16, 274, tr("最新です", "Up to date", "已是最新"), 21, kSub);
    }
    const bool running = u.install == "running";
    double x = 40;
    x += button(pen, x, 306, u.checking ? tr("確認中…", "Checking...", "检查中…") : tr("今すぐ確認", "Check now", "立即检查"), false, !u.checking && !running,
                HitKind::UpdateCheck, hits) + 14;
    if (u.available && u.installable)
        button(pen, x, 306, tr("この版に更新する", "Update to this version", "更新到此版本"), true, !running, HitKind::UpdateInstall, hits);
    if (u.available && !u.installable)
        pen.text(x, 336, tr("このリリースには入れるファイルが付いていない", "This release has no package file attached", "该发行版没有附带可安装的文件"), 19, kWarn);
    if (!u.url.empty()) pen.text(40, 384, tr("リリースのページ: ", "Release page: ", "发行版页面: ") + u.url, 17, kSub);

    // 入れた結果・進み
    if (running) {
        const char* steps[][2] = {{"start", "準備中"}, {"download", "ダウンロード中"}, {"verify", "検証中"},
                                  {"extract", "展開中"}, {"install", "ビルドして入れ替え中（数分かかる）"}};
        const char* stepsEn[] = {"Preparing", "Downloading", "Verifying", "Unpacking", "Building and installing (takes a few minutes)"};
        const char* stepsZh[] = {"准备中", "下载中", "校验中", "解压中", "编译并替换中（需数分钟）"};
        std::string text = tr("更新中", "Updating", "更新中");
        for (size_t i = 0; i < 5; ++i)
            if (u.step == steps[i][0]) text = tr(steps[i][1], stepsEn[i], stepsZh[i]);
        pen.text(40, 436, text + "…", 22, kAccent, true);
        pen.text(40, 466, tr("この画面を閉じても続く", "It keeps going if you close this screen", "关闭此界面后仍会继续"), 19, kSub);
    } else if (u.install == "done") {
        pen.text(40, 436, tr("更新した: ", "Updated: ", "已更新: ") + u.installVersion, 22, kAccent, true);
        pen.text(40, 466, tr("SteamVR を再起動すると、新しい版になる（自動では再起動しない）", "Restart SteamVR to run the new version (it is not restarted for you)", "重启 SteamVR 后就会换成新版本（不会自动重启）"), 19, kWarn);
    } else if (u.install == "failed") {
        pen.text(40, 436, installErrorText(u.installError), 22, kWarn, true);
        pen.text(40, 466, tr("詳しいログ: ~/.cache/betterframe-xp/update.log", "Details: ~/.cache/betterframe-xp/update.log", "详细日志: ~/.cache/betterframe-xp/update.log"), 19, kSub);
    }
}

void drawUpdate(const Pen& pen, const UiState& ui, std::vector<Hit>& hits) {
    heading(pen, 176, tr("更新", "Update", "更新"), tr("GitHub の最新リリースを確認して、ここから入れ替える", "Check GitHub's newest release and update from here", "检查 GitHub 的最新发行版，并从这里替换"));
    drawUpdateStatus(pen, ui, hits);
    pen.text(40, 530, tr("ダウンロードしたファイルは SHA256SUMS で検証してから入れる。ホストは github.com だけ", "The download is checked against SHA256SUMS before install. Only github.com is used", "下载的文件会先用 SHA256SUMS 校验再安装。只访问 github.com"), 18, kSub);
    pen.text(40, 558, tr("入れ替えにはビルドが入るので数分かかる。ゲーム中は避けるのがよい", "The update builds on the headset and takes a few minutes. Avoid it during a game", "替换过程包含编译，需要几分钟。最好避开游戏时间"), 18, kSub);
    pen.text(40, 586, tr("SteamVR は自動では再起動しない。入れ終わったら、自分のタイミングで再起動する", "SteamVR is never restarted for you. Restart it yourself when it suits you", "SteamVR 不会自动重启。安装完成后请自行找时间重启"), 18, kSub);
    toggle(pen, 40, 628, tr("1 時間おきに自動で確認する", "Check automatically every hour", "每小时自动检查一次"), value(ui, "updateAutoCheck", 1) >= 0.5, hits, HitKind::UpdateAuto);
}

void drawLoad(const Pen& pen, const Load& load) {
    const double y = kHeight - 132;
    pen.roundedRect(24, y, kWidth - 48, 116, 14);
    pen.color(kCard);
    cairo_fill(pen.cr);
    pen.text(44, y + 34, tr("負荷", "Load", "负载"), 22, kSub, true);
    char buf[256];
    const LayerStats& s = load.layer;
    if (!s.valid) {
        pen.text(120, y + 34, tr("レイヤーの計測値がありません（無効か、SteamVR の再起動待ち）", "No layer measurements (disabled, or waiting for a SteamVR restart)", "没有图层的测量值（已禁用，或正在等待 SteamVR 重启）"), 22, kWarn);
    } else {
        std::string gpu;
        if (s.gpuSamples > 0 && s.gpuMsAvg >= 0) {
            const double share = s.fps > 0 ? s.gpuMsAvg * s.fps / 10.0 : -1;  // ms * fps / 1000 * 100
            std::snprintf(buf, sizeof(buf), tr("GPU %.2f ms/フレーム（最大 %.2f, GPU 時間の %.1f%%）", "GPU %.2f ms/frame (max %.2f, %.1f%% of GPU time)", "GPU %.2f ms/帧（最大 %.2f，占 GPU 时间 %.1f%%）"),
                          s.gpuMsAvg, s.gpuMsMax, share);
            gpu = buf;
        } else {
            gpu = tr("GPU 計測不可（タイムスタンプ非対応）", "GPU timing unavailable (no timestamp support)", "无法测量 GPU（不支持时间戳）");
        }
        std::snprintf(buf, sizeof(buf), tr("   CPU %.0f µs/フレーム   %.0f fps", "   CPU %.0f µs/frame   %.0f fps", "   CPU %.0f µs/帧   %.0f fps"), s.cpuUsAvg, s.fps);
        pen.text(120, y + 34, tr("このレイヤー: ", "This layer: ", "本图层: ") + gpu + buf, 22, kText);
    }
    std::string line = tr("GPU 全体: ", "GPU total: ", "GPU 整体: ");
    if (std::isnan(load.gpuTotalPct)) line += "--";
    else { std::snprintf(buf, sizeof(buf), "%.0f%%", load.gpuTotalPct); line += buf; }
    if (load.gpuFreqMhz > 0) {
        std::snprintf(buf, sizeof(buf), tr("（%.0f / %.0f MHz）", "(%.0f / %.0f MHz)", "（%.0f / %.0f MHz）"), load.gpuFreqMhz, load.gpuMaxMhz);
        line += buf;
    }
    line += tr("     このパネルの CPU: ", "     This panel's CPU: ", "     本面板的 CPU: ");
    if (std::isnan(load.panelCpuPct)) line += "--";
    else { std::snprintf(buf, sizeof(buf), "%.1f%%", load.panelCpuPct); line += buf; }
    pen.text(120, y + 76, line, 22, kText);
    pen.text(120, y + 104, tr("GPU 時間の % = レイヤーの GPU ms × fps。全体 % は全アプリの合計。", "GPU time % = layer GPU ms × fps. Total % is the sum of all apps.", "GPU 时间 % = 图层的 GPU ms × fps。整体 % 是所有应用的合计。"), 17, kSub);
}

void drawUi(cairo_surface_t* surface, const FontSet& fonts, const UiState& ui, std::vector<Hit>& hits) {
    hits.clear();
    const int language = std::clamp(static_cast<int>(value(ui, "uiLanguage", 0)), 0, 2);
    gEnglish = language == 1;
    gChinese = language == 2;
    cairo_t* cr = cairo_create(surface);
    const Pen pen{cr, &fonts};
    pen.color(kBg);
    cairo_paint(cr);
    pen.text(40, 62, "BetterFrame XP", 34, kText, true);
    pen.text(40, 96, tr("Frame の中だけで画質を整える。変更はすぐ反映される", "Tunes image quality inside Frame. Changes apply instantly.", "只在 Frame 内部优化画质。改动会立即生效"), 20, kSub);
    drawLanguage(pen, ui, hits);
    // 入れた内容が、まだ動いている SteamVR に反映されていない（再起動待ち）: 左上に目立つ帯を出す
    if (ui.layer.restartNeeded() || ui.update.install == "done") drawRestartBanner(pen);
    // 右上のスイッチ（左から 有効・左右比較・PC の映像だけ）。次の位置は文字幅で決める（英語は長い）
    const char* enabledLabel = tr("有効", "Enabled", "启用");
    const char* splitLabel = tr("左右比較", "Split compare", "左右对比");
    // 「PC の映像だけ」の右端を、右の注記の右端（kWidth - 40）にそろえ、残りの余白を 3 つの間で等分する
    const char* streamLabel = tr("PC の映像だけ", "PC video only", "仅 PC 画面");
    const double enabledW = 78 + pen.measure(enabledLabel, 22);
    const double splitW = 78 + pen.measure(splitLabel, 22);
    const double streamW = 78 + pen.measure(streamLabel, 22);
    const double gap = ((kWidth - 40) - 420 - enabledW - splitW - streamW) / 2;
    const double splitX = 420 + enabledW + gap;
    const double streamX = splitX + splitW + gap;
    toggle(pen, 420, 40, enabledLabel, value(ui, "enabled", 1) >= 0.5, hits, HitKind::Enabled);
    toggle(pen, splitX, 40, splitLabel, value(ui, "showSplit", 0) >= 0.5, hits, HitKind::ShowSplit);
    // PC の映像が描かれているフレームだけに掛ける。ホーム・ダッシュボードのフレームは素通しにする。
    toggle(pen, streamX, 40, streamLabel, value(ui, "streamOnly", 1) >= 0.5, hits, HitKind::StreamOnly);
    // 補足は右端でそろえる（英語は長く、左の決め打ちだと画面からはみ出す）
    const double noteX = kWidth - 40;
    pen.text(noteX, 92, tr("OFF: Frame 単体のときも掛かる", "OFF: applies on Frame alone too", "关闭: Frame 单独使用时也会生效"), 17, kSub, false, true);
    pen.text(noteX, 114, tr("ON 推奨。OFF だと、Frame 単体で", "ON recommended. With OFF, the", "推荐开启。若关闭，Frame 单独"), 17, kWarn, false, true);
    pen.text(noteX, 136, tr("ダッシュボードを選ぶと映像が乱れる", "dashboard can garble the video", "打开仪表盘时画面会错乱"), 17, kWarn, false, true);
    drawTabs(pen, ui, hits);
    switch (ui.tab) {
        case Tab::Quality: drawQuality(pen, ui, hits); break;
        case Tab::Color: drawColor(pen, ui, hits); break;
        case Tab::SteamVr: drawSteamVr(pen, ui, hits); break;
        case Tab::Experimental: drawExperimental(pen, ui, hits); break;
        case Tab::Update: drawUpdate(pen, ui, hits); break;
    }
    drawLoad(pen, ui.load);
    cairo_destroy(cr);
    cairo_surface_flush(surface);
}

void render(const FontSet& fonts, const UiState& ui, std::vector<uint8_t>& rgba, std::vector<Hit>& hits) {
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, kWidth, kHeight);
    drawUi(surface, fonts, ui, hits);
    surfaceToRgba(surface, rgba);
    cairo_surface_destroy(surface);
}

/** ダッシュボードのサムネイル（256 px）を描いた surface を返す（呼び出し側が cairo_surface_destroy する）。 */
cairo_surface_t* drawThumbnail(const FontSet& fonts) {
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, kThumb, kThumb);
    cairo_t* cr = cairo_create(surface);
    const Pen pen{cr, &fonts};
    pen.roundedRect(16, 16, kThumb - 32, kThumb - 32, 40);
    pen.color(kCard);
    cairo_fill(cr);
    const double w = pen.measure("FX", 110, true);
    pen.text((kThumb - w) / 2, 168, "FX", 110, kAccent, true);
    cairo_destroy(cr);
    cairo_surface_flush(surface);
    return surface;
}

void thumbnail(const FontSet& fonts, std::vector<uint8_t>& rgba) {
    cairo_surface_t* surface = drawThumbnail(fonts);
    surfaceToRgba(surface, rgba);
    cairo_surface_destroy(surface);
}

// ---- 操作 ----

const Hit* hitTest(const std::vector<Hit>& hits, double x, double y) {
    for (const auto& h : hits)
        if (x >= h.x && x <= h.x + h.w && y >= h.y && y <= h.y + h.h) return &h;
    return nullptr;
}

void updateSlider(UiState& ui, int index, double x) {
    const ParamSpec& s = paramSpecs()[index];
    const double t = std::clamp((x - kSliderX) / kSliderW, 0.0, 1.0);
    double v = s.min + t * (s.max - s.min);
    v = std::round(v / s.step) * s.step;
    ui.values[s.key] = std::clamp(v, s.min, s.max);
    ui.dirty = true;
}

/** 0/1 のスイッチを反転する。def は params.conf に無いときの値。 */
void flip(UiState& ui, const char* key, double def) {
    ui.values[key] = value(ui, key, def) >= 0.5 ? 0 : 1;
    ui.dirty = true;
}

void activate(const Hit& hit, double x, UiState& ui) {
    switch (hit.kind) {
        case HitKind::Enabled: flip(ui, "enabled", 1); break;
        case HitKind::ShowSplit: flip(ui, "showSplit", 0); break;
        case HitKind::GrainAnimate: flip(ui, "grainAnimate", 1); break;
        case HitKind::StreamOnly: flip(ui, "streamOnly", 1); break;
        case HitKind::CasCross: flip(ui, "casCross", 1); break;
        case HitKind::CasLuma: flip(ui, "casLuma", 1); break;
        case HitKind::GazeReproject: if (ui.gaze.known) ui.gazeRequest = 1; break;
        case HitKind::GazeEyeToHead: if (ui.gaze.known) ui.gazeRequest = 2; break;
        case HitKind::CornerSkip: ui.values["cornerSkip"] = value(ui, "cornerSkip", 1.15) > 0 ? 0 : 1.15; ui.dirty = true; break;
        case HitKind::Language: ui.values["uiLanguage"] = hit.arg; ui.dirty = true; break;
        case HitKind::UpdateCheck: ui.updateRequest = 1; break;
        case HitKind::UpdateInstall: ui.updateRequest = 2; break;
        case HitKind::UpdateAuto: flip(ui, "updateAutoCheck", 1); break;
        case HitKind::Tab:
            ui.tab = static_cast<Tab>(hit.arg);
            ui.activeSlider = -1;
            break;
        case HitKind::Slider: ui.activeSlider = hit.arg; updateSlider(ui, hit.arg, x); break;
        case HitKind::Preset:
            for (const auto& [k, v] : presets()[hit.arg].values) ui.values[k] = v;
            ui.dirty = true;
            break;
        case HitKind::ColorPreset:
            ui.values["contrast"]    = kColorPresets[hit.arg].contrast;
            ui.values["saturation"]  = kColorPresets[hit.arg].saturation;
            ui.values["gamma"]       = kColorPresets[hit.arg].gamma;
            ui.values["temperature"] = kColorPresets[hit.arg].temperature;
            ui.values["brightness"]  = kColorPresets[hit.arg].brightness;
            ui.dirty = true;
            break;
    }
}

/** ポインタの 1 件を処理する（ダッシュボードとブラウザ確認で共通）。描き直しが要れば true。 */
bool handlePointer(const PointerInput& e, UiState& ui, const std::vector<Hit>& hits) {
    if (e.type == PointerInput::Type::Move) {
        if (ui.activeSlider < 0) return false;
        updateSlider(ui, ui.activeSlider, e.x);
        return true;
    }
    if (e.type == PointerInput::Type::Leave || e.type == PointerInput::Type::Up) {
        ui.activeSlider = -1;
        return true;
    }
    if (const Hit* hit = hitTest(hits, e.x, e.y)) {
        activate(*hit, e.x, ui);
        return true;
    }
    return false;
}

/** ドラッグ中は 150 ms おき、離したらすぐ書く（レイヤーは 0.25 s おきに読む）。 */
void saveIfDue(UiState& ui, const std::string& path, std::chrono::steady_clock::time_point& lastSave) {
    const auto now = std::chrono::steady_clock::now();
    if (ui.dirty && (ui.activeSlider < 0 || now - lastSave >= std::chrono::milliseconds(150))) {
        if (writeParams(path, ui.values)) ui.dirty = false;
        lastSave = now;
    }
}

// ---- 負荷の読み取り ----

double readNumber(const char* path) {
    std::ifstream in(path);
    double v = -1;
    return (in >> v) ? v : -1;
}

double processCpuSeconds() {
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_utime.tv_sec + ru.ru_stime.tv_sec + (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6;
}

double monoSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/** 負荷表示の値を読む（1 秒おきに sample を呼ぶ）。 */
struct LoadSampler {
    GpuUsage gpu;
    double lastCpu = processCpuSeconds(), lastCpuTime = monoSeconds();

    void sample(Load& load, double t) {
        load.layer = readLayerStats();
        const double f = readNumber("/sys/class/devfreq/3d00000.gpu/cur_freq");
        load.gpuTotalPct = gpu.update(t, f);
        const double m = readNumber("/sys/class/devfreq/3d00000.gpu/max_freq");
        load.gpuFreqMhz = f > 0 ? f / 1e6 : -1;
        load.gpuMaxMhz = m > 0 ? m / 1e6 : -1;
        const double cpu = processCpuSeconds();
        load.panelCpuPct = (cpu - lastCpu) / (t - lastCpuTime) * 100.0;
        lastCpu = cpu;
        lastCpuTime = t;
    }
};

/** 二重起動を防ぐ（$XDG_RUNTIME_DIR/betterframe-xp-panel.lock）。 */
bool takeLock() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    const std::string path = std::string(runtime && *runtime ? runtime : "/tmp") + "/betterframe-xp-panel.lock";
    const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    return fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) == 0;  // fd はプロセス終了まで開いたまま
}

/** --render-png で描くタブ名から Tab を選ぶ（無ければ画質）。 */
Tab tabFromName(const std::string& name) {
    if (name == "color" || name == "colour") return Tab::Color;
    if (name == "steamvr") return Tab::SteamVr;
    if (name == "experimental" || name == "beta") return Tab::Experimental;
    if (name == "update") return Tab::Update;
    return Tab::Quality;
}

} // namespace

int main(int argc, char** argv) {
    signal(SIGINT, stopHandler);
    signal(SIGTERM, stopHandler);
    // Layout check without SteamVR: betterframe-xp-panel --render-png out.png [quality|experimental|steamvr|update] [ja|en|zh]
    if ((argc >= 3 && argc <= 5) && std::string(argv[1]) == "--render-png") {
        FontSet fonts;
        fonts.load(kFont, kBold);
        UiState ui;
        ui.tab = tabFromName(argc >= 4 ? argv[3] : "basic");
        ui.gaze.known = true;  // SteamVR に繋がないので、見た目確認用の状態（奥行き OFF・目の位置 ON）
        ui.gaze.eyeToHead = true;
        readParams(paramsPath(), ui.values);
        if (argc == 5)  // 言語は引数で上書き（ja / en / zh）
            ui.values["uiLanguage"] = std::string(argv[4]) == "en" ? 1 : (std::string(argv[4]) == "zh" ? 2 : 0);
        ui.load.layer = readLayerStats();
        ui.update.current = installedVersion();
        GpuUsage gpu;
        const double freqHz = readNumber("/sys/class/devfreq/3d00000.gpu/cur_freq");
        gpu.update(monoSeconds(), freqHz);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        ui.load.gpuTotalPct = gpu.update(monoSeconds(), readNumber("/sys/class/devfreq/3d00000.gpu/cur_freq"));
        std::vector<Hit> hits;
        cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, kWidth, kHeight);
        drawUi(s, fonts, ui, hits);
        const bool ok = cairo_surface_write_to_png(s, argv[2]) == CAIRO_STATUS_SUCCESS;
        cairo_surface_destroy(s);
        std::printf("%s, %zu hit areas\n", ok ? "written" : "write failed", hits.size());
        return ok ? 0 : 1;
    }
    // Write the icon for the dashboard's "+" list (used by install.sh): betterframe-xp-panel --write-icon out.png [size]
    if ((argc == 3 || argc == 4) && std::string(argv[1]) == "--write-icon") {
        const int size = argc == 4 ? std::atoi(argv[3]) : kThumb;
        if (size < 16 || size > 1024) return 2;
        FontSet fonts;
        fonts.load(kFont, kBold);
        cairo_surface_t* big = drawThumbnail(fonts);
        cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
        cairo_t* cr = cairo_create(s);
        cairo_scale(cr, static_cast<double>(size) / kThumb, static_cast<double>(size) / kThumb);
        cairo_set_source_surface(cr, big, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
        cairo_paint(cr);
        cairo_destroy(cr);
        cairo_surface_flush(s);
        const bool ok = cairo_surface_write_to_png(s, argv[2]) == CAIRO_STATUS_SUCCESS;
        cairo_surface_destroy(s);
        cairo_surface_destroy(big);
        return ok ? 0 : 1;
    }
    if (!takeLock()) {
        std::fprintf(stderr, "betterframe-xp-panel is already running\n");
        return 0;
    }
    VrOverlay overlay;
    std::string error;
    while (!gStop) {
        const auto result = overlay.connect(kWidth, kHeight, error);
        if (result == VrOverlay::ConnectResult::Ok) break;
        if (result == VrOverlay::ConnectResult::Error) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }
    if (gStop) return 0;

    FontSet fonts;
    fonts.load(kFont, kBold);
    std::vector<uint8_t> pixels;
    thumbnail(fonts, pixels);
    overlay.submitThumbnail(pixels.data(), kThumb);

    const std::string path = paramsPath();
    UiState ui;
    readParams(path, ui.values);
    LoadSampler sampler;
    Updater updater(installedVersion());
    double lastLoad = 0, lastLayer = -1e9;
    auto lastSave = std::chrono::steady_clock::now();
    std::vector<Hit> hits;
    bool redraw = true, wasVisible = false;
    double lastGaze = -1e9;

    while (!gStop && overlay.steamVrAlive()) {
        const VrEvents events = overlay.pollEvents();
        if (events.quit) break;
        if (events.closeRequested) {
            // The dashboard icon's "close": turn the effects off (nobody can adjust them once the panel is gone),
            // then exit cleanly (exit code 0, so the service is not restarted).
            // Start it again from the dashboard's "+" button, or from the next SteamVR start; "Enabled" is then OFF.
            std::fprintf(stderr, "closing because of the dashboard's close button; setting enabled = 0\n");
            readParams(path, ui.values);  // pick up changes made from outside, so only "enabled" differs
            ui.values["enabled"] = 0;
            if (!writeParams(path, ui.values)) std::fprintf(stderr, "cannot write %s\n", path.c_str());
            break;
        }
        for (const auto& e : events.pointer)
            if (handlePointer(e, ui, hits)) redraw = true;
        // 視線の補正: ユーザーがスイッチを押したときだけ SteamVR の設定を書く（反転した値を、書いたあとに読み直して表示）
        if (ui.gazeRequest != 0) {
            const bool reproj = ui.gazeRequest == 1;
            ui.gazeRequest = 0;
            bool r = false, e = false;
            if (overlay.readGazeSettings(r, e)) {
                std::string message;
                const char* key = reproj ? "gazeDepReprojectEnable" : "gazeDepEyeToHeadEnable";
                const bool before = reproj ? r : e;
                // 最初に書き換える前の値を残す（アンインストールで戻すため）。残せなければ書き換えない
                if (!rememberFirstValue(gazeRecordPath(), key, before ? 1 : 0)) {
                    ui.gaze.failed = true;
                    std::fprintf(stderr, "[SteamVR settings] cannot save the original value of %s; not written\n", key);
                } else {
                    ui.gaze.failed = !overlay.setSteamVrBool(key, !before, message);
                    if (ui.gaze.failed) std::fprintf(stderr, "[SteamVR settings] write failed: %s\n", message.c_str());
                }
            }
            ui.gaze.known = overlay.readGazeSettings(ui.gaze.reproject, ui.gaze.eyeToHead);
            lastGaze = monoSeconds();
            redraw = true;
        }
        saveIfDue(ui, path, lastSave);

        const bool visible = overlay.panelVisible();
        const double t = monoSeconds();
        // 更新: 確認とインストールは別スレッド。ここでは頼むのと、結果を受け取るのだけ
        if (ui.updateRequest == 1) updater.startCheck();
        if (ui.updateRequest == 2) updater.startInstall();
        if (ui.updateRequest != 0) { ui.updateRequest = 0; redraw = true; }
        updater.setAutoCheck(value(ui, "updateAutoCheck", 1) >= 0.5);
        if (updater.tick(t)) redraw = true;
        ui.update = updater.info();
        // 再起動の要否は、見えているときだけ 2 秒おきに読む（/proc を数えるので、隠れている間は読まない）
        if (visible && t - lastLayer >= 2.0) {
            lastLayer = t;
            const LayerState now = readLayerState();
            if (now.restartNeeded() != ui.layer.restartNeeded()) redraw = true;
            ui.layer = now;
        }
        // 負荷は見えているときだけ 1 秒おきに読む（fdinfo の読み取り自体が重いので）
        if (visible && t - lastLoad >= 1.0) {
            sampler.sample(ui.load, t);
            lastLoad = t;
            redraw = true;
        }
        if (visible && !wasVisible) {
            readParams(path, ui.values);  // betterframectl などで変えられていても追従する
            redraw = true;
        }
        // 視線の補正は、見え始めたときと 3 秒おきに読み直す（vrcmd など外で変えられても追いつく）
        if (visible && (!wasVisible || t - lastGaze >= 3.0)) {
            lastGaze = t;
            Gaze now = ui.gaze;
            now.known = overlay.readGazeSettings(now.reproject, now.eyeToHead);
            if (now.known != ui.gaze.known || now.reproject != ui.gaze.reproject || now.eyeToHead != ui.gaze.eyeToHead) {
                ui.gaze = now;
                redraw = true;
            }
        }
        if (visible && redraw) {
            render(fonts, ui, pixels, hits);
            overlay.submitPanel(pixels.data());
            redraw = false;
        }
        wasVisible = visible;
        std::this_thread::sleep_for(std::chrono::milliseconds(visible ? 33 : 250));
    }
    overlay.shutdown();
    return 0;
}
