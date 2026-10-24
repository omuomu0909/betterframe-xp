#include "params.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <set>
#include <sstream>

namespace {

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

/** "key = value" の行なら key と値を取り出す。 */
bool parseLine(const std::string& raw, std::string& key, double& value) {
    std::string line = raw;
    const auto comment = line.find('#');
    if (comment != std::string::npos) line.erase(comment);
    const auto equal = line.find('=');
    if (equal == std::string::npos) return false;
    key = trim(line.substr(0, equal));
    const std::string val = trim(line.substr(equal + 1));
    char* end = nullptr;
    value = std::strtod(val.c_str(), &end);
    return !key.empty() && !val.empty() && end != val.c_str() && std::isfinite(value);
}

std::string format(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

} // namespace

const std::vector<ParamSpec>& paramSpecs() {
    // 範囲はレイヤー（effect_postfx.cpp の readPostFxParams）の clamp に合わせる
    static const std::vector<ParamSpec> specs = {
        {"sharpenAmount",   "シャープ量",           "Sharpen amount",        "锐化量",                 0.0, 2.0,  0.05, 2,
         "輪郭をくっきりさせる。負荷は増えない「はず」だが体感では要確認。上げすぎると縁が白黒",
         "Crisper outlines. Should add no load, but check by eye. Too high gives black-and-white edges.",
         "让轮廓更清晰。负载「应该」不会增加，但主观感受上仍需确认。调得太高边缘会出现黑白边"},
        {"sharpness",       "シャープの鋭さ",       "Sharpness",             "锐化强度",               0.0, 1.0,  0.05, 2,
         "シャープの効きの鋭さ。上げるほど強くなり、ノイズも目立ちやすい",
         "How sharp the sharpening is. Higher is stronger, and noise shows more easily.",
         "锐化的锐利程度。越高越强，噪点也越明显"},
        {"centerRadius",    "中心の範囲",           "Center range",          "中心范围",               0.0, 1.0,  0.05, 2,
         "目の中央のこの範囲は効果を全力で掛ける（目の半径に対する割合）",
         "The center of the eye within this range gets the full effect (as a fraction of the eye radius).",
         "画面中心的这个范围内效果全开（相对眼睛半径的比例）"},
        {"edgeAmount",      "周辺の強さ",           "Edge strength",         "边缘强度",               0.0, 1.0,  0.05, 2,
         "中央から外れた所の効果の割合。0 で周辺は効かない、1 で全体同じ強さ",
         "Effect strength outside the center. 0 means no effect at the edges; 1 means the same strength everywhere.",
         "中心之外区域的效果比例。0 时边缘不生效，1 时整体强度相同"},
        // 色調整タブ: シャープ・デバンド・グレインのあとで最終画面に掛かる（1.0 = 変化なし）
        {"contrast",        "コントラスト",         "Contrast",              "对比度",                 0.5, 1.5,  0.01, 2,
         "中間グレーを軸に明暗の差を変える。1.0 で変化なし、大きいほど硬調、小さいほど平坦",
         "Contrast around mid grey. 1.0 = unchanged; higher is punchier, lower is flatter.",
         "以中灰为轴调整明暗反差。1.0 不改变，越大越硬朗，越小越平"},
        {"saturation",      "彩度",                 "Saturation",            "饱和度",                 0.0, 2.0,  0.05, 2,
         "色の鮮やかさ。0 で白黒、1.0 で変化なし、1 より大きいと鮮やか",
         "Colour intensity. 0 = greyscale, 1.0 = unchanged, higher is more vivid.",
         "颜色的鲜艳程度。0 为黑白，1.0 不变，大于 1 更鲜艳"},
        {"gamma",           "ガンマ",               "Gamma",                 "伽马",                   0.5, 2.5,  0.05, 2,
         "中間調の明暗カーブ。1.0 で変化なし、1 より大きいと暗部が持ち上がる",
         "Mid-tone curve. 1.0 = unchanged; above 1 lifts shadows, below 1 deepens them.",
         "中间调的明暗曲线。1.0 不变，大于 1 提亮暗部，小于 1 压暗"},
        {"temperature",     "色温度",               "Temperature",           "色温",                   0.8, 1.2,  0.01, 2,
         "暖かい／冷たいの寄せ。1.0 で変化なし、1 より大きいと暖色寄り。明るさ全体は変えない",
         "Warm/cool balance. 1.0 = unchanged, above 1 warmer, below 1 cooler; overall brightness stays.",
         "偏暖还是偏冷。1.0 不变，大于 1 偏暖、小于 1 偏冷；整体亮度不变"},
        {"brightness",      "明るさ",               "Brightness",            "亮度",                   0.5, 1.5,  0.01, 2,
         "画面全体の明るさ。1.0 で変化なし、1 より大きいと明るい",
         "Overall brightness. 1.0 = unchanged; above 1 brighter, below 1 darker.",
         "整体亮度。1.0 不变，大于 1 更亮，小于 1 更暗"},
        // ここから下は実験的タブ（効果の確認が十分でない機能）
        {"debandAmount",    "デバンド量",           "Deband amount",         "去色带量",               0.0, 1.0,  0.05, 2,
         "暗部や空にできる色の段差（帯）をなめらかにする。0 で無効",
         "Smooths color steps (banding) in dark areas and sky. 0 turns it off.",
         "把暗部和天空里出现的色阶（色带）变平滑。0 为关闭", true},
        {"debandRange",     "デバンド範囲 (px)",    "Deband range (px)",     "去色带范围 (px)",        1.0, 32.0, 1.0,  0,
         "なめらかにするときに見る範囲（画素）。大きいほど広くぼかす",
         "Pixel range examined when smoothing. Larger blurs over a wider area.",
         "做平滑时参考的范围（像素）。越大模糊范围越宽", true},
        {"debandThreshold", "デバンドしきい値",     "Deband threshold",      "去色带阈值",             0.5, 8.0,  0.25, 2,
         "これより色差が小さい所だけなめらかにする。大きいほど効く範囲が広い",
         "Only smooths areas where the color difference is below this. Larger covers more area.",
         "只对色差小于此值的区域做平滑。越大生效范围越广", true},
        {"detailBoost",     "ディテール重視",       "Detail boost",          "细节增强",               0.0, 1.0,  0.05, 2,
         "草や地面の細かい模様が残る所だけ強くシャープにする（平らな所は弱める）",
         "Sharpens strongly only where fine grass and ground patterns remain (flat areas are weakened).",
         "只对草地和地面等细密纹理残留的地方做强锐化（平坦处则减弱）", true},
        {"grainAmount",     "グレイン量",           "Grain amount",          "颗粒量",                 0.0, 1.0,  0.05, 2,
         "明るさに細かい粒を足し、圧縮でのっぺりした質感を目立たなくする",
         "Adds fine grain to brightness to make the flat look from compression less noticeable.",
         "给亮度加上细颗粒，让压缩造成的平淡质感不那么明显", true},
        {"grainSize",       "グレインの粒 (px)",    "Grain size (px)",       "颗粒大小 (px)",          1.0, 4.0,  1.0,  0,
         "粒の大きさ（画素）。1 が一番細かい",
         "Grain size in pixels. 1 is the finest.",
         "颗粒的大小（像素）。1 最细", true},
    };
    return specs;
}

const std::vector<Preset>& presets() {
    static const std::vector<Preset> list = {
        // 標準 = 配布する params.conf の既定（ヘッドセットで確認した運用値）
        {"標準", "Standard", "标准", {{"sharpenAmount", 1.2}, {"sharpness", 0.7}, {"debandAmount", 0.0}, {"debandRange", 12},
                  {"debandThreshold", 2.5}, {"centerRadius", 0.55}, {"edgeAmount", 0.4},
                  {"detailBoost", 0.0}, {"grainAmount", 0.0}}},
        {"弱め", "Weak", "较弱", {{"sharpenAmount", 0.6}, {"sharpness", 0.5}, {"debandAmount", 0.0}, {"debandRange", 12},
                  {"debandThreshold", 2.5}, {"centerRadius", 0.45}, {"edgeAmount", 0.3},
                  {"detailBoost", 0.0}, {"grainAmount", 0.0}}},
        {"効果なし", "Off", "关闭", {{"sharpenAmount", 0.0}, {"debandAmount", 0.0}, {"grainAmount", 0.0}}},
        // ここから下は実験的タブのプリセット
        {"暗部重視", "Dark areas", "暗部优先", {{"sharpenAmount", 0.4}, {"sharpness", 0.4}, {"debandAmount", 0.8}, {"debandRange", 16},
                      {"debandThreshold", 3.0}, {"centerRadius", 0.45}, {"edgeAmount", 0.3},
                      {"detailBoost", 0.0}, {"grainAmount", 0.0}}, true},
        // 草や地面の圧縮ボケ向け: 模様が残る所だけシャープ + 細かいグレイン
        {"草・地面", "Grass & ground", "草地地面", {{"sharpenAmount", 0.8}, {"sharpness", 0.5}, {"debandAmount", 0.0}, {"debandRange", 12},
                      {"debandThreshold", 2.5}, {"centerRadius", 0.5}, {"edgeAmount", 0.3},
                      {"detailBoost", 0.8}, {"grainAmount", 0.3}, {"grainSize", 1}}, true},
    };
    return list;
}

std::string paramsPath() {
    if (const char* env = std::getenv("BETTERFRAME_XP_PARAMS"); env && *env) return env;
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return std::string(xdg) + "/betterframe-xp/params.conf";
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "/tmp") + "/.config/betterframe-xp/params.conf";
}

bool readParams(const std::string& path, std::map<std::string, double>& values) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line, key;
    double v = 0;
    while (std::getline(in, line))
        if (parseLine(line, key, v)) values[key] = v;
    return true;
}

bool writeParams(const std::string& path, const std::map<std::string, double>& values) {
    std::vector<std::string> lines;
    std::set<std::string> written;
    {
        std::ifstream in(path);
        std::string line, key;
        double v = 0;
        while (std::getline(in, line)) {
            if (parseLine(line, key, v) && values.count(key)) {
                lines.push_back(key + " = " + format(values.at(key)));
                written.insert(key);
            } else {
                lines.push_back(line);
            }
        }
    }
    for (const auto& [key, v] : values)
        if (!written.count(key)) lines.push_back(key + " = " + format(v));

    const std::string tmp = path + ".panel.tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return false;
        for (const auto& l : lines) out << l << '\n';
        if (!out) return false;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

std::string gazeRecordPath() {
    const std::string params = paramsPath();
    return params.substr(0, params.rfind('/') + 1) + "gaze-before.conf";
}

bool rememberFirstValue(const std::string& path, const std::string& key, double value) {
    std::map<std::string, double> values;
    readParams(path, values);  // 無いファイルは空として扱う
    if (values.count(key)) return true;
    values[key] = value;
    return writeParams(path, values);
}

LayerStats readLayerStats() {
    LayerStats s;
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (!runtime || !*runtime) return s;
    std::map<std::string, double> v;
    if (!readParams(std::string(runtime) + "/betterframe-xp/stats", v)) return s;
    auto get = [&](const char* k) { return v.count(k) ? v[k] : -1.0; };
    const double t = get("time");
    s.valid = t > 0 && std::difftime(std::time(nullptr), static_cast<time_t>(t)) <= 3.0;
    s.fps = get("fps");
    s.gpuMsAvg = get("gpuMsAvg");
    s.gpuMsMax = get("gpuMsMax");
    s.cpuUsAvg = get("cpuUsAvg");
    s.cpuUsMax = get("cpuUsMax");
    s.gpuSamples = static_cast<long>(get("gpuSamples"));
    return s;
}
