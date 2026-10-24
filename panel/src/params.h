// params.conf（レイヤーの設定）と stats（レイヤーが書く負荷）の読み書き。
#pragma once

#include <map>
#include <string>
#include <vector>

/** スライダー 1 本ぶんの定義。名前と説明は日本語・英語・中国語を持つ。 */
struct ParamSpec {
    const char* key;      ///< params.conf のキー
    const char* label;    ///< パネルに出す名前（日本語）
    const char* labelEn;  ///< 同じ（英語）
    const char* labelZh;  ///< 同じ（中国語）
    double min, max, step;
    int decimals;         ///< 表示の小数点以下の桁数
    const char* desc;     ///< パネルの説明（1 行、日本語）
    const char* descEn;   ///< 同じ（英語）
    const char* descZh;   ///< 同じ（中国語）
    bool beta = false;    ///< true なら実験的タブに出す（効果の確認が十分でない機能）
};

/** パネルに並べるスライダー（params.conf の順）。 */
const std::vector<ParamSpec>& paramSpecs();

/** プリセット 1 つ。enabled / showSplit は変えない。 */
struct Preset {
    const char* name;    ///< 日本語の名前
    const char* nameEn;  ///< 英語の名前
    const char* nameZh;  ///< 中国語の名前
    std::map<std::string, double> values;
    bool beta = false;   ///< true なら実験的タブに出す
};

/** 組み込みプリセット（標準 / 弱め / 暗部重視 / 草・地面 / 効果なし）。 */
const std::vector<Preset>& presets();

/**
 * params.conf の場所（$XDG_CONFIG_HOME か ~/.config の下）。
 * @return パス
 */
std::string paramsPath();

/**
 * params.conf を読む。無いキーはそのまま残す。
 * @param path ファイル
 * @param values 読んだ値の書き込み先（key → 値）
 * @return 読めたら true
 */
bool readParams(const std::string& path, std::map<std::string, double>& values);

/**
 * params.conf を書く。コメントや知らないキーは残し、値の行だけ置き換える。
 * 一時ファイル → rename で書くので、レイヤーが途中の状態を読むことはない。
 * @param path ファイル
 * @param values 書く値
 * @return 書けたら true
 */
bool writeParams(const std::string& path, const std::map<std::string, double>& values);

/**
 * SteamVR の視線の補正を最初に書き換える前の値を残すファイル（params.conf と同じ場所の gaze-before.conf）。
 * アンインストールで、ここに残した値に戻す。
 * @return パス
 */
std::string gazeRecordPath();

/**
 * key の値を、まだ残っていなければ残す。もう残っていれば何もしない（最初の値を守る）。
 * @param path ファイル（gazeRecordPath）
 * @param key キー（gazeDepReprojectEnable など）
 * @param value 残す値（1 / 0）
 * @return 残っている（今回残した・前から残っていた）なら true
 */
bool rememberFirstValue(const std::string& path, const std::string& key, double value);

/** レイヤーが書く負荷の値（無いものは負の値）。 */
struct LayerStats {
    bool valid = false;     ///< ファイルがあって新しい（3 秒以内）
    double fps = -1;
    double gpuMsAvg = -1, gpuMsMax = -1;
    double cpuUsAvg = -1, cpuUsMax = -1;
    long gpuSamples = 0;
};

/**
 * $XDG_RUNTIME_DIR/betterframe-xp/stats を読む。
 * @return 読んだ値
 */
LayerStats readLayerStats();
