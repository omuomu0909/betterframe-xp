# 技術説明: どう動いているか

## 全体像

```
PC ──Steam Link──▶ vrlink（デコード）──10bit YUV dma-buf──▶ vrcompositor ──▶ ディスプレイ
                                                              │
                                                  Vulkan（Turnip / Adreno）
                                                              │
                                    ┌──── VK_LAYER_BETTERFRAME_XP（このレイヤー）────┐
                                    │  アプリ（vrcompositor）が描く偽の swapchain 画像
                                    │        ↓ 1 パス（CAS シャープ + 重み付け）
                                    │  本物の swapchain 画像 ──▶ present
                                    └────────────────────────────────────────────────┘
```

- vrlink（Steam Link のクライアント）は Vulkan で描画も提示もしない。デコードした YUV 画像を共有メモリで vrcompositor に渡す。**vrlink の中で補正しても表示には効かない**ので、掛ける場所は vrcompositor の出力側になる。
- 映像の描画先は、4320×2160 の 1 枚の画像（左右の目が 2160×2160 ずつ並ぶ、B8G8R8A8_SRGB、2 サンプル MSAA）。シャープはこの解決後の画像に掛かる＝**レンズ歪み補正後**。
- レイヤーは `/proc/self/exe` が `vrcompositor` のときだけ動く。他のプロセス（vrlink、vrserver、パネルなど）では何もしない。

## 1. レイヤーの読み込み

- Vulkan の implicit layer。マニフェスト `~/.local/share/vulkan/implicit_layer.d/betterframe-xp.json`。
- 有効条件は環境変数 `ENABLE_BETTERFRAME_XP=1`。`betterframectl enable` が `steamvr.service` の systemd drop-in（`91-betterframe-xp.conf`）で `~/.config/betterframe-xp/vulkan.env` を読ませる。だから**有効化は SteamVR の次の起動から**。
- 関数は `vkBasalt_GetInstanceProcAddr` / `vkBasalt_GetDeviceProcAddr` を通して取る。レイヤー名・ライブラリ名・環境変数名は FrameColor と被らないようにしてあり、併用も個別の無効化もできる（併用の動作は未確認）。

## 2. 偽の swapchain（`fake_swapchain.cpp`, `logical_swapchain.cpp`）

1. アプリが `vkCreateSwapchainKHR` を呼ぶと、本物の swapchain を `MUTABLE_FORMAT`（UNORM / SRGB の両方を見られる）付きで作る。
2. アプリには本物ではなく**偽の画像**を返す（`vkGetSwapchainImagesKHR`）。アプリはこの偽画像に描く。
3. `vkQueuePresentKHR` で、レイヤーが自前のコマンドバッファを submit する。偽画像を入力に、効果のパスを実行して本物の画像に書く。アプリの描画完了セマフォを待ち、完了したら本物の present を呼ぶ。

コマンドバッファは画像ごとに**あらかじめ記録済み**（実行時に記録し直さない）。パラメータは uniform バッファ（host coherent）に書くだけなので、設定を変えても再記録は要らない。

## 3. 効果のパス（`postfx.frag.glsl`, `effect_postfx.cpp`）

全画面 1 パスのフラグメントシェーダー。1 画素ごとの処理:

```
r = 目の中心からの距離（左右 2 つの目を別々に計算）
weight = 中心 centerRadius までは 1、そこから外へ edgeAmount まで滑らかに下げる
r > cornerSkip なら素通し（四隅は処理しない）
cas = 十字 5 点を輝度に変換 → AMD CAS の式で「掛けたときの差分」を出す
col += cas × sharpenAmount × weight
```

- **CAS**: AMD FidelityFX CAS（適応型シャープ）。コントラストが高い所ほど控えめに掛かるので、圧縮ノイズや強い輪郭を強調しすぎにくい。`sharpness` が式の中の係数。
- **軽量版**: 元の CAS は 3×3 の 9 点を色ごとに計算する。配布の既定（`contrib/params.conf`）は「十字の 5 点・輝度 1 つ・目の四隅は処理しない」（`casCross 1` / `casLuma 1` / `cornerSkip 1.15`）。シェーダー自体は 0 / 0 / 0 で元の式に戻る。
- 色調整（コントラスト / 彩度 / ガンマ / 色温度 / 明るさ）は最後にまとめて掛ける。`showSplit` の左半分はその手前で素通しのまま返るので、左側には色調整も掛からない。
- デバンド・グレイン・ディテール重視も同じシェーダーに入っている（実験的な機能。→ [SETTINGS.md](SETTINGS.md) の「実験的な機能」）。
- 各補正の式と読み方は [GUIDE](GUIDE.md) の「補正の技術説明」。
- 色の計算は `mediump`（半精度）。座標とハッシュは `highp`。「半精度で Adreno の ALU レートが約 2 倍」はコード中のコメントで、**作者は測っていない**。

## 4. パラメータの反映（`effect_postfx.cpp`）

- `~/.config/betterframe-xp/params.conf` を 0.25 秒ごとに読み直す（`key = value`、`#` 以降はコメント、範囲外はクランプ）。
- present のたびに、uniform バッファの内容をその場で作る。ファイルの値 + グレインの乱数の種。`streamOnly` が有効で映像が無いフレームは、シャープ・デバンド・グレインの量を 0 にし、色調整もニュートラル（全部 1.0）に戻す。内容が変わったときだけ書き込む。
- 描画中のフレームが次の値を見ることがあるが、差は最大 1 フレーム分。

## 5. 映像の検出（`stream_gate.cpp`）— `streamOnly` の中身

目的: 「今のフレームに PC の映像が描かれているか」を、コンポジタの呼び出しを**変えずに読むだけ**で知る。

PC 映像は、シェーダーから読むために YCbCr 変換付きの image view を通る。ホームやダッシュボードの UI は通常の RGB 画像。だから次の連鎖を追う:

```
vkCreateImageView（YCbCr 変換付き）
   → vkUpdateDescriptorSets（その view をセットに書く）
      → vkCmdBindDescriptorSets（コマンドバッファにセットを bind）
         → vkCmdDraw / vkCmdDrawIndexed（bind されたセットのどれかが YUV view を持つ）
```

- セットごとに「YUV view を持つ binding / 要素」を持ち、書き込み・コピー・再割り当て・view の破棄で正確に付け外しする。「一度でも YUV を参照したセットは永久に印付け」にすると、セットの使い回しで映像が無い秒まで映像ありと数えてしまった（初期のプローブの失敗）。
- 最後の映像の draw から **200 ms 以内**を「映像あり」とする。映像が出ている間は毎フレーム描かれるので、記録と present の時間差を吸収する長さ。
- フックは present が持つロック（`globalLock`）を取らない。デバイスごとのディスパッチ表を、ロック無しで引ける小さな表（`fastDevices`）に登録してある。
- フックするのは 8 個だけ: `CreateImageView` `DestroyImageView` `AllocateDescriptorSets` `UpdateDescriptorSets` `BeginCommandBuffer` `CmdBindDescriptorSets` `CmdDraw` `CmdDrawIndexed`。`streamOnly = 0` でもフックは通る（コストはロック無しの表の検索）。
- 見逃す経路: `vkUpdateDescriptorSetWithTemplate`、プッシュディスクリプタ。
- 未解決: 映像の描画が `vkCmdDraw` か `vkCmdDrawIndexed` かは確定していない（両方を同じ判定に通しているので動く）。

## 6. 効果が 0 のときの「コピーだけ」の経路（プラン A）

`streamOnly` で映像が無いとき、`enabled = 0` のとき、量がすべて 0 のときは、シェーダーのパスを使わず `vkCmdCopyImage`（偽画像 → 本物の画像）だけのコマンドバッファを submit する。

- 切り替えはフレーム単位。どちらのバッファも入口は偽画像 PRESENT_SRC、出口は PRESENT_SRC なので、順序は問わない。
- 効果: GPU 時間が約 0.53 → 0.3〜0.6 ms。**差は小さい**（4320×2160 を読み書きすること自体のコストが残る）。ガクッへの寄与も小さかった。

## 7. 計測（`frame_stats.cpp`）

すべて tmpfs（`$XDG_RUNTIME_DIR/betterframe-xp/`）。ディスクには書かない。

| ファイル | 内容 |
|---|---|
| `stats` | 1 秒ごとに更新する `key = value`。fps、レイヤーの GPU ms（平均・最大）、CPU µs、present の待ち、フレーム間隔（平均・最大・p99・標準偏差・遅れた数） |
| `stats.late` | フレーム間隔 > 10 ms の行（テキスト）。上限 8 MB、超えたら `.1` に 1 世代退避 |
| `stats.frames` | 1 フレーム 24 バイトの記録 `<ffffd`（間隔, present 入口の間隔, レイヤー GPU ms, present の待ち, 時刻）。上限 32 MB、`.1` に 1 世代退避 |

GPU 時間は、コマンドバッファの前後のタイムスタンプ。前の待ちを `ALL_COMMANDS` にして、アプリの描画を待つ時間を含めないようにしている。結果は取れたぶんだけ読み、GPU の完了は待たない。

## 8. 性能の目安（作者の Frame、4320×2160）

| 条件 | レイヤーの GPU 時間 |
|---|---|
| 効果なし（シェーダーのパス、量 0） | 約 0.53 ms |
| コピーのみ | 0.3〜0.6 ms |
| 軽い版（既定）、シャープ 1.2 | 約 0.98 ms |
| 元の CAS（3×3 RGB）、シャープ 1.2 | 約 1.42 ms |
| 運用値（映像あり） | 約 1.4〜1.5 ms |

144 Hz のフレーム予算は 6.94 ms。

## 9. パネル（`panel/`）

OpenVR のダッシュボードオーバーレイ。`params.conf` を書き換え（ドラッグ中は 150 ms ごと）、`stats` を 1 秒ごとに読んで表示する。パネルが見えていないときは何も読まない。レイヤーとは `params.conf` と `stats` のファイルだけでつながっていて、直接は通信しない。

### 再起動の要否と更新

- **再起動の要否**（`panel/src/layer_state.*`）: 有効 = drop-in `91-betterframe-xp.conf` があり `guard-disabled` が無い。読み込み済み = `/proc/<vrcompositor>/maps` に `libbetterframexp` がある。古い = その行が `(deleted)`（`install.sh` は一時ファイルを `mv -f` で入れ替えるので、動いている vrcompositor は古いファイルを持ち続ける）。`vrcompositor` が見つからない / maps が読めないときは判断しない。`再起動が必要 = 有効 != 読み込み済み || (読み込み済み && 古い)`。2 秒ごとに調べ、パネルが見えているときだけ動く。
- **更新**（`panel/src/update_check.*` と `contrib/betterframe-xp-update`）: パネルは POSIX sh の更新コマンドを `popen`（`std::async`、30 秒で打ち切り）で呼び、1 行の JSON を読む。入れ替えは `systemd-run --user` の一時ユニットで、パネルとは別のプロセス。ダウンロード元は `github.com` だけ、`SHA256SUMS` と合わないものは展開しない。ネットワークはこの更新コマンド以外では使わない（レイヤーは使わない）。

例外として、「SteamVR 設定」タブの「視線の補正」は、レイヤーではなく SteamVR の設定（`steamvr.gazeDepReprojectEnable` / `steamvr.gazeDepEyeToHeadEnable`）を `IVRSettings::SetBool` で書く。押したときだけ書き、最初の値を `~/.config/betterframe-xp/gaze-before.conf` に一度だけ残す。`betterframectl uninstall` がその値に戻す。レイヤーの処理とは無関係。
