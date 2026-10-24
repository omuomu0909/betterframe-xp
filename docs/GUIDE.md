# betterframe-xp 詳しいガイド

> 短い説明は [../README.md](../README.md)。このファイルは、仕組み・置き場所・各補正の技術説明・元に戻し方までを全部書いた版。

Steam Frame（SteamOS / aarch64）で、PC から Steam Link で送られてきた映像に**シャープ（AMD CAS）を後掛け**する Vulkan レイヤーと、SteamVR ダッシュボードの設定パネル。

> 状態: 試作。作者の Frame 1 台で動作確認。他の環境での動作は未確認。下の「できること / できないこと」と「既知の問題」を読んでから使う。

## 何のためのもの

無線 PCVR では、映像は H.264/HEVC で約 232 Mbps（作者の環境で確認した値）に収められる。VRChat の草や葉のような細かい模様は、この上限に収めるために細部が捨てられ、「泥のように」つぶれる。

捨てられた細部は戻せない。ただし、**デコード後（Frame 側）に掛けるシャープは、エンコーダーのビットを使わない**。PC 側でシャープを掛けると、シャープで強調された細部がまたビットを食う。このレイヤーは「PC 側のシャープは切って、Frame 側で掛ける」ための道具。

## 映像が通る道（PC → Frame）

PC で描いた映像が目に届くまでと、このレイヤーが割り込む場所。

```
[PC]    VR アプリ（VRChat など）
          │ 左右の目の映像を描く
          ▼
[PC]    SteamVR（driver_vrlink）
          │ (A) エンコード前のシャープ ……… SteamVR 標準の CAS。任意（SteamVR 2.17 で追加）
          │ (B) HEVC 10bit にエンコード … 作者の PC は NVIDIA の NVENC
          ▼
        ═══ 無線（Steam Link）═══  作者の環境でエンコーダーの目標は約 232 Mbps
          ▼
[Frame] vrlink（Steam Link のクライアント）
          │ 受信 → V4L2 のハードウェアデコーダー（qcom-iris）でデコード
          │ ※ vrlink は Vulkan で描画も表示もしない
          ▼
        10bit YUV の画像 ……… dma-buf（共有メモリ）で vrcompositor に渡す
          ▼
[Frame] vrcompositor（SteamVR のコンポジタ。表示するフレームを組み立てる）
          │ 再投影・レンズ歪み補正などで、4320×2160 の 1 枚を描く
          │ （左右の目が 2160×2160 ずつ並ぶ。MSAA の解決後の画像）
          ▼
        ┌─────────────────────────────────────────────────────┐
        │ (C) ★ betterframe-xp（Vulkan レイヤー）                 │
        │     present の直前に 1 パスで補正を掛ける                │
        └─────────────────────────────────────────────────────┘
          ▼
        ディスプレイ
```

補正を掛けられる場所は 3 つある。このレイヤーは (C)。

| 場所 | 掛けられるか | 理由 |
|---|---|---|
| (A) PC 側のエンコード前 | できる（SteamVR 標準の CAS） | 強調された細部もエンコーダーのビットを使う。ビットの枠は決まっているので、シャープのぶんほかの細部が削られる |
| (B') Frame の vrlink（デコード直後） | 意味がない | vrlink は描画も表示もしない。ここで画像を変えても、vrcompositor が表示する画像には届かない |
| **(C) vrcompositor の出力** | **できる（これを使う）** | デコード後なのでエンコーダーのビットを使わない。表示する直前の最終画像に掛かる |

(C) の注意点は 2 つ。

- 掛かるのは**レンズ歪み補正と再投影のあとの画像**。圧縮ノイズも一緒に強調される。作者が調べたところ、Frame は届いたフレームの約 98〜99% を Frame 側で再投影して表示している。レイヤーは vrcompositor が present するたびに動くので、再投影したフレームにも掛かる。
- 同じ画像にホーム・ダッシュボードの UI も入る。PC の映像が出ているフレームだけに限る仕組みが `streamOnly`（→ 「補正の技術説明」）。

このため、使うときは SteamVR 側の (A)「エンコード前のシャープ」を切る（`useSharpening`）のが前提。両方掛けると二重になる。

## できること / できないこと

| できる | できない |
|---|---|
| PC の映像が出ているフレームだけに CAS シャープを掛ける（ホーム・ダッシュボードは素通し） | 失われた細部を作る（超解像ではない） |
| 目の中心は強く、周辺は弱く掛ける | PC 側のエンコード品質を上げる（ビットレートの上限は変えない） |
| ダッシュボードのパネルから、掛け具合をその場で変える | Frame 以外の機器 / SteamVR 以外のアプリに掛ける |
| レイヤー自身の GPU 時間・フレーム間隔を見る | 映像の上に重なる UI だけを除外する（同じフレームなので UI にも掛かる） |
| パネルから、Frame のアイトラッキングの補正（奥行き / 目の位置）をオン/オフする（SteamVR の設定） | 補正の効果を保証する（作者の環境ではオフのほうが揺れが減った。1 人・1 台の体感） |

## 必要なもの

- Steam Frame（SteamOS、aarch64）。ルートファイルシステムは変更しない（`~/.local` と `~/.config` だけ）。root 不要。
- Frame 上でビルドする: `meson` `ninja` `cmake` `gcc` `glslangValidator` と Vulkan ヘッダー（`vulkan-headers`）、`cairo` `freetype2`。作者の Frame（SteamOS 0.5.6、BUILD_ID 20261015）では、これらは**最初から入っていた**（`pacman -Qi` の導入日時が OS イメージの展開と同じ分。`pacman.log` に作者の導入記録はない）。別の版の SteamOS で同じかは未確認。無ければ `pip install --user meson ninja` で補えるが、`glslangValidator` と `vulkan-headers` は補う手順が未整備。
- ビルド済みバイナリの配布はまだ無い（ビルドは Frame 上で 1 分ほど）。
- ダッシュボードのパネル（既定で入る）に CJK フォント（`/usr/share/fonts/noto-cjk/`）。

## インストール

1 コマンドで済む。

```sh
git clone https://github.com/omuomu0909/betterframe-xp.git && cd betterframe-xp
sh contrib/setup.sh                  # 確認 → ビルド → 導入 → 有効化（パネルも）
# → 遊んでいないときに SteamVR を再起動する（1 回でレイヤーとパネルの両方が有効になる）
~/.local/bin/betterframectl status   # "loaded in vrcompositor" と出れば成功
```

`setup.sh` がやること:

1. **確認**: ユーザー（root でない）、CPU（aarch64）、SteamOS、ビルド道具、Vulkan ヘッダー、パネルに要るもの（cairo / freetype2 / OpenVR / CJK フォント）、空き容量。足りないものは読める文で出し、**何も変えずに止まる**。パネルは必ず出す前提なので、パネルに要るものが足りないときも止まる（レイヤーだけで良ければ `--no-panel`）。
2. **ビルドと導入**: `contrib/install.sh` を呼ぶ。失敗したら、ログの最後と全文の場所を出して止まり、有効化まで進まない。レイヤーは最初から無効で入るので、SteamVR には影響しない。ビルドが途中で止まっても、すでに入っている版は置き換わらない（ビルドが終わってから `~/.local` に置く）。置いたあとの失敗（パネルのビルドなど）では、レイヤーだけ入った状態になりうる。その場合は `betterframectl uninstall` で消せる。
3. **有効化**: `betterframectl enable` と、パネルの有効化（`install.sh` が `systemctl --user enable` まで行う。再実行しても有効に戻る）。次の SteamVR 起動で効く。有効になっていなければ最後に警告し、`betterframectl status` も `panel: NOT enabled` と対処を出す。パネルを出したくないときだけ `betterframectl panel-off` か `--no-panel`。
4. **やらないこと**: SteamVR の再起動、`sudo`、ルート FS への書き込み、PC 側の設定変更、SteamVR の設定の書き換え。

| オプション | 内容 |
|---|---|
| `--check` | 確認だけ。何も変えない・ビルドしない |
| `--no-panel` | レイヤーだけ（パネルなし） |
| `--no-enable` | 導入だけ。有効化は後で `betterframectl enable` |

何度実行してもよい（更新になる）。`setup.sh` を使わずに手順を分けて実行するなら、`sh contrib/install.sh`（パネルも有効になる）→ `betterframectl enable`。

**レイヤー本体の有効化・更新は SteamVR の再起動が必要**。再起動すると遊んでいる最中の音と映像が切れるので、遊んでいないときにやる。設定値（`params.conf`）の変更は再起動不要で、0.25 秒以内に反映される。

## 使い方

- 設定は `~/.config/betterframe-xp/params.conf`、またはダッシュボードの「BetterFrame XP」パネル、または次のコマンド。
- 最初に `streamOnly = 1` にしておくのを勧める（理由は [docs/SETTINGS.md](SETTINGS.md)）。

```sh
betterframectl set streamOnly 1        # PC の映像が出ているフレームだけに掛ける
betterframectl set sharpenAmount 0.6   # シャープ量
betterframectl set showSplit 1         # 各目の右半分だけ処理 → 境目で見比べる
betterframectl stats                   # レイヤーの GPU 時間とフレーム間隔
betterframectl off                     # すぐ素通し（レイヤーは残る）
betterframectl on
```

各設定の意味と、**どれが実際に効いていると言えるか**は [docs/SETTINGS.md](SETTINGS.md)。

「ガクッ」と書いてあるものが何の現象かは [STUTTER.md](STUTTER.md)。原因の違う 5 種類があり、レイヤー（シャープ）が関係するのは主に 1 種類だけ。首振りや視界の端でのぶれは、作者の体感では視線の補正のオフで治る。

パネルは「画質 / 実験的 / SteamVR 設定 / 更新」の 4 タブ。入れた内容が SteamVR の再起動待ちのときは、パネルの上に「SteamVR の再起動が必要」の帯が出る（[下の更新の節](#更新パネルから)）。右上に「有効」「左右比較」「PC の映像だけ」のスイッチがある。画質タブは段落で主設定と副設定を分けている（シャープ量 0 では、シャープの方式の 3 スイッチと鋭さは効かない）。実験的タブも同じ作りで、デバンド量 0 ではデバンドの範囲としきい値、グレイン量 0 ではグレインの粒と「粒を動かす」が効かない。「ディテール重視」は画質タブのシャープ量の掛かり方を変えるだけで、シャープ量 0 では何も起きない。「SteamVR 設定」タブの 2 つのスイッチ（視線の補正）だけは、このレイヤーの設定ではなく **SteamVR の設定**（`~/.config/openvr/config/steamvr.vrsettings`）を書き換える。スイッチを押したときだけ書く。戻し方は [docs/SETTINGS.md](SETTINGS.md) の「視線の補正」。

## 更新（パネルから）

「更新」タブは、いま入っている版（`~/.local/share/betterframe-xp/VERSION`）と、GitHub の最新リリースを比べる。

1. 「今すぐ確認」（または 1 時間ごとの自動確認）で `https://api.github.com/repos/<owner>/<repo>/releases/latest` を 1 回読む。タグは `v0.2.0` か `0.2.0` の形だけ受け付ける。
2. 新しい版があれば、「この版に更新する」を押す。`contrib/betterframe-xp-update start` が、systemd のユーザー用の一時ユニット `betterframe-xp-update.service`（`Nice=19`）で動き出す。パネルを閉じても続く。
3. リリースの `betterframe-xp-<版>.tar.gz` と `SHA256SUMS` を `github.com` からダウンロードし、**ハッシュが合うときだけ**展開する（絶対パスや `..` を含むアーカイブは断る）。展開した中の `contrib/install.sh` を実行する（手でやる更新と同じ手順）。
4. 終わると、パネルに「SteamVR の再起動が必要」の帯が出る。**再起動は自分でやる。** このツールは SteamVR を再起動しない。

リリースを作る側は `sh scripts/package.sh <版>` で、この 2 ファイルを `dist/` に作る（`git archive` なのでコミット済みの内容が入る）。

- 経過と失敗の理由は `~/.cache/betterframe-xp/update.log` と `update-state.json`。
- 確認できなかったとき（ネットワーク / 見つからない / 回数制限）や、失敗したときは、理由をタブに出す。入れ替えに失敗しても、すでに入っている版は置き換わらない（ビルドが終わってから置く作りは `install.sh` のまま）。
- 確認できるのは**公開リポジトリのリリースだけ**（トークンは扱わない）。
- 確認先のリポジトリ（`omuomu0909/betterframe-xp`）は、`BETTERFRAME_XP_REPO` で変えられる（フォークや試験用）。
- 動作確認は、偽の GitHub API を立てた Frame 上のオフライン試験（確認 / 更新 / 改ざん検知 / 最新のとき何もしない / 404 / 接続失敗）まで。**実際の GitHub への接続と、ヘッドセット内の表示は未確認。**
- 仕組みを参考にしたのは frameeyeosc の更新機能。コードは使っていない。

## 補正の技術説明

ここにある処理は、すべて `layer/src/shader/postfx.frag.glsl` の 1 パスで、1 画素ずつ計算する。上から順に掛かる。

**読み方**: 各補正に「変えるとこうなる『はず』」と「作者が体感したこと」を**分けて**書く。

- 「はず」= 計算式やコードの作りから言えること。ヘッドセット内では確かめていない**予想**。他の人が試すときの目安。
- 「作者の体感」= 作者（1 人、Frame 1 台）が実際に見て、または測ったこと。他の環境で同じとは限らない。
- **体感できていない = 効果が無い、ではない**（見分けにくいだけかもしれない）。体感の記録が無いものは「記録なし」と書く。
- 表形式の一覧は [SETTINGS.md](SETTINGS.md)。

```
画素 uv
 ├─ 左右どちらの目か → 目の中の座標 eyeUv
 ├─ showSplit = 1 なら、境目の線を描いて終わり（左は素通し・右は処理）
 ├─ 目の中心からの距離 r ─┬─ r > cornerSkip ─────────────▶ 素通し（四隅）
 │                        └─ 重み weight（中心は 1、縁は edgeAmount）
 ├─ 十字 5 点（casCross=0 なら 3×3 の 9 点）を読む → contrast（コントラスト）
 ├─ デバンド（debandAmount > 0 かつ contrast が小さいとき）
 ├─ シャープ（CAS）× sharpenAmount × weight × detail（detailBoost）
 ├─ グレイン（grainAmount > 0 かつ平らな所）
 └─ 0〜1 に収めて出力
```

### 映像の検出（streamOnly）

`streamOnly = 1` のとき、PC の映像が描かれたフレームだけに効果を掛ける。ホーム・ダッシュボードは素通し。

- PC の映像は、シェーダーから読むために YCbCr 変換付きの画像ビューを通る。UI は普通の RGB 画像。その違いで判定する。
- 判定は、レイヤーがコンポジタの呼び出しを**読むだけ**で行う（呼び出しは変えない）。`ビュー作成 → セットへの書き込み → セットの bind → draw` の順に、YUV を持つセットが draw されたかを追う。
- 最後の映像の draw から 200 ms 以内を「映像あり」とする。映像が出ている間は毎フレーム描かれるので、この長さで十分。
- 見逃す経路が 2 つある（テンプレート更新、プッシュディスクリプタ）。見逃すと映像が出ているのに効果が掛からない。そのときは `streamOnly = 0` にする。
- 映像がないフレームは、シャープ・デバンド・グレインの量を 0 にする。さらに、効果のパスを使わず「コピーだけ」（`vkCmdCopyImage`）の経路に切り替える。GPU 時間は約 0.53 → 0.3〜0.6 ms。差は小さい。
- **はず**: 1 のとき、ホーム・ダッシュボード・読み込み画面は素通しになり、効果は PC の映像だけに掛かる。映像の上に重なる UI は同じフレームなので一緒に掛かる。0 にすると全フレームに掛かる。
- **既知の不具合（映像の乱れ）**: 0 にすると Frame 単体（PC につなげていないホーム）にも効果が掛かるが、**その状態でダッシュボードを選ぶと映像が乱れる**（作者の報告。乱れ方の詳細と、どの条件で出るかは未記録）。パネルの右上のスイッチの下にも注意を出している。映像の検出が外れたとき以外は、1 のままにする。
- **作者の体感**: 効果は PC の映像にだけ掛かり、鮮明さは変わらなかった。ホームで頭を振ったときのガクッが多少減った（残りは「通常プレイの範囲内」と判断）。

### シャープ（CAS）

AMD FidelityFX CAS（適応型シャープ）を、中心の 1 画素の周りの読み取りで計算する。

1. **十字 5 点**（上下左右と中心）の最小・最大を求める。`casCross = 0` なら四隅も読んで 3×3 の 9 点にする。
2. **振幅** `amp = √(min(最小, 2 − 最大) / 最大)`（0〜1）を、最小と最大から決める。コントラストが高い所（強い輪郭）は `amp` が小さくなり、掛かりが控えめになる。平らな所は `amp` が 1 に近く、重みが最大になる（ノイズも一緒に強くなりうる）。
3. 重み `w = amp × (−1 / (8 − 3 × sharpness))`。`sharpness` を上げると、重みの絶対値が大きくなる（強くなる）。
4. 出力 = ((上下左右の和) × w + 中心) / (1 + 4w) − 中心。この差分に `sharpenAmount × weight × detail` を掛けて足す。

- **`casLuma = 1`（既定）**: 色ごとに計算せず、輝度 1 つで重みを決め、3 色に同じ重みを使う。色のにじみが出にくい。
- **`casCross = 1`（既定）**: 四隅を読まない。読む画素は 5 個（元の CAS は 9 個）。
- **`cornerSkip`（既定 1.15）**: 目の中心から 1.15（目の半径の 1.15 倍）より外は処理しない。内接円（半径 1）の外側の四隅は効果が弱いと作者が判断し、読み飛ばして軽くしている。
- 3 つとも 0 にすると、元の 3×3 RGB の CAS と同じ式になる。レイヤーの GPU 時間（シャープ 1.2）は、元の CAS 1.42 ms → 3 つとも既定で 0.98 ms。**3 つの個別の寄与は測っていない**。

**変えるとこうなる「はず」**

| 変えるもの | はず |
|---|---|
| `sharpenAmount` を上げる | 輪郭がくっきりする。1 を超えると縁に白黒の縁取り（リンギング）が出やすい。失われた細部は作れない。**強さを上げても負荷は増えない**（強さは最後の掛け算に使うだけで、読む画素も計算の量も同じ。負荷が変わるのは 0 か、0 より大きいかだけ） |
| `sharpness` を上げる | 掛かり方が積極的になり、ノイズも目立ちやすくなる。負荷は変わらない |
| `casCross` / `casLuma` / `cornerSkip` を ON にする | 軽くなり、見た目はほとんど変わらない |

**作者の体感**: シャープが映像に掛かることは確認した。**効果は体感できる**。強さごとの見た目の比較の記録は薄い。強さを変えたときの GPU 時間は測っていない（負荷が同じなのは、式から言える予想）。さらに、作者の体感は「強めの設定ほどガクッとなりやすい」で、この予想と食い違っている（未解決。[STUTTER.md](STUTTER.md) の A-3）。3 つの軽量化は、込みで「だいぶ安定した」。

### 目の中心の重み（centerRadius / edgeAmount）

- 4320×2160 の画像を、左右の目 2 つとして別々に扱う（`eyeUv.x = fract(uv.x × 2)`）。目の中心は、それぞれの目の中心。Frame は常にこの並びなので、設定項目にはしていない。
- `r` = 目の中心からの距離（目の半径 = 1 の単位）。
- `weight = mix(1, edgeAmount, smoothstep(centerRadius, 1, r))`。中心から `centerRadius` までは 1（全力）、それより外は `edgeAmount` まで滑らかに下げる。
- **視線は使っていない。** 「中心」は各目の画像の真ん中で、目玉を動かしても動かない。レンズの真ん中は「だいたい見ている所」という前提の近似。
- `centerRadius = 1` のままだと、強さが下がり始めるのは `r` が 0.95 のあたりから。変わるのは四隅と縁だけで、`edgeAmount` を変えても画面の中は同じに見える。違いを見るには `centerRadius` を 0.3 前後に下げる。
- 重みは、シャープとデバンドとグレインの全部に掛かる。見た目を「視野の中心を強く、周辺を弱く」にする狙い。
- **はず**: `centerRadius` を下げると、全力で効く範囲が目の真ん中だけに狭まり、周辺はシャープが弱くなる（周辺のノイズ強調も減る）。`edgeAmount` を下げると周辺がさらに弱くなる。負荷は変わらない（位置で掛ける強さが変わるだけ）。
- **作者の体感**: 見た目の評価の記録なし。`edgeAmount` を 0 と 1 で切り替えても違いが分からなかった（作者の設定は `centerRadius = 1` 相当で、この組み合わせでは変わるのは四隅だけのはず）。

### デバンド（実験的）

色の段差（帯）を、平らな所だけなめらかにする。haasn のデバンド（MIT）を元に簡略化した。

1. 画素ごとにランダムな向きと距離（最大 `debandRange` px）で 4 点を読む。
2. 4 点の平均を作り、4 点すべてが元の画素と `debandThreshold`（8 ビット換算）以内なら、平均へ寄せる。どれか 1 点でも大きく違えば（エッジ）寄せない。
3. 三角形のディザ（±1 段階）を足し、8 ビットの出力で新しい帯が出ないようにする。
4. コントラストが `3 × debandThreshold` 以上の所（エッジや模様）は、この処理を飛ばす。

- **はず**: 暗部や空の色の段差（帯）がなめらかになる。`debandThreshold` を上げすぎると細かい模様までぼやける。`debandAmount` が 0 より大きいと読む画素が増えるので、シャープだけのときより負荷は増える（増えた分は測っていない）。
- **作者の体感**: 個別の評価なし。

### ディテール重視（detailBoost、実験的）

- 「模様が少し残る所」（草・地面）だけを強く掛ける。テクスチャの量を、コントラストの範囲（0.006〜0.03 で立ち上がり、0.12〜0.30 で落ちる）から決める。
- `detailBoost = 0` は通常の CAS。`1` で、模様の所は約 2 倍（0.25 + 1.75 × 模様の量）、平らな所（ノイズ）と強い輪郭は約 0.25 倍。
- **はず**: 草・地面のような「模様が少し残る所」が、平らな所より強くシャープになる。追加で読む画素は無いので、負荷はほぼ増えない。
- **作者の体感**: 0.8 で運用していた「良い版」があるが、見た目の個別評価の記録は薄い。

### グレイン（実験的）

- 輝度だけに細かい粒を足し、圧縮ののっぺり感を目立たなくする。
- 粒は `grainSize` 画素のセルごとに 1 つ（−1〜1 の三角分布）。`grainAnimate = 1` のときはフレームごとにずらし、64 通りで循環する。
- 効く場所: 中間調（輝度 0.5 付近で最大）かつ平らな所（コントラストが 0.08〜0.25 より小さい所で 1）。エッジには掛けない。
- 量は `0.04 × grainAmount` が振幅。増えたぶんの GPU 時間は測っていない。
- **はず**: 圧縮ののっぺり感が目立たなくなる。上げすぎると粒が見える。`grainAnimate = 0`（固定）は顔に貼り付いて見え、`1`（毎フレーム変える）はちらつきに見えることがある。
- **作者の体感**: 個別の評価なし。固定が「顔に貼り付いて」見えるのは作者の観察。

### 色調整（コントラスト / 彩度 / ガンマ / 色温度 / 明るさ）

- シャープ・デバンド・グレインのあとで、最終的な画面に 1 回だけ色の補正を掛ける。順番は「コントラスト → 彩度 → ガンマ → 色温度 → 明るさ」で固定。
- どの項目も `1.0` で変化なし。**5 つとも 1.0 のときは着色器がこの段落ごと飛ばす**ので、負荷は増えない（レイヤーは「素通しコピー」のまま）。
- 計算（`layer/src/shader/postfx.frag.glsl` の `grade()`）:
  - コントラスト: `c = (c - 0.5) * contrast + 0.5`（中間グレー 0.5 を軸に拡大・縮小）
  - 彩度: `c = mix(luma(c), c, saturation)`、`luma = 0.299R + 0.587G + 0.114B`
  - ガンマ: `c = pow(c, 1 / gamma)`（1 より大きいと中間調が持ち上がる）
  - 色温度: RGB のゲイン `(t, 1, 1/t)` を、そのゲインの輝度で割って正規化する。**暖色・寒色に寄せても全体の明るさは変わらない**
  - 明るさ: 最後に `brightness` を掛ける
- 色調整は**「目の中心の重み」の影響を受けず、目全体に均一に掛かる**。シャープなどは `centerRadius` / `edgeAmount` で周辺が弱まるが、色まで弱めると中心から周辺への色のグラデーションが見えてしまうため。
- 目の半径が `cornerSkip` を超える四角も色調整は掛かる（そこでは画素を読む処理だけを飛ばす）。色調整の範囲が円環状に途切れない。
- `showSplit = 1` のとき、線の左側は完全な素通し（色調整も掛からない）、右側は処理後。**色調整の左右比較にもそのまま使える。**
- `enabled = 0`、または `streamOnly = 1` で PC の映像が無いフレームでは、5 つとも 1.0 として扱う（映像が戻った瞬間に色が飛ばないように）。
- PC 側のドライバの色設定（デジタルバイブランスなど）と重ねると二重になる。どちらか一方で調整する。

### 境目の線（showSplit）

確認用。各目の中央に縦線を引く（白 3 px、両側に黒 3 px）。線の左は素通し、右は処理後。左右で見比べて効果の違いを見るためのもの。

### 視線の補正（SteamVR の設定。レイヤーの処理ではない）

パネルの「SteamVR 設定」タブの視線の補正は、このレイヤーの処理とは別の、SteamVR の設定（`steamvr` セクション）を書き換える。

| スイッチ | 設定のキー | 内容 |
|---|---|---|
| 奥行き | `steamvr.gazeDepReprojectEnable` | 視線の奥行きに合わせて再投影する。Valve の説明では、フルレートが出ていないときのカクつきを減らす |
| 目の位置 | `steamvr.gazeDepEyeToHeadEnable` | 眼球の回転で瞳の位置がずれる分を、映像を作るときの目の位置に反映する（瞳孔位置補正に当たると思われる。確定ではない） |

- どちらも Valve が設定画面から外したが、裏の設定としては今も読まれる。
- 作者の環境では、オンのままだと視界の端を見ながら頭を動かしたとき揺れ、オフにしたら体感が良くなった。1 人・1 台の体感。
- 効果は保証しない。切り替えは、揺れを感じたときに比べるための道具。詳細は [SETTINGS.md](SETTINGS.md) の「視線の補正」。
- **はず**: OFF にすると、視界の端を見ながら頭を動かしたときのガクつきが減る。ただし、これは作者の体感から来る予想で、**仕組みから確かめたものではない**。ON のほうが合う人もいるかもしれない。
- **作者の体感**: 奥行きの補正を OFF にしたのが一番効いた変更。目の位置の補正は奥行きと一緒に OFF にして安定した（単独の効果は切り分けていない）。

## どこに何があるか

### Frame 側（SteamOS、root なし）

| 置き場所 | 中身 | 誰が使う |
|---|---|---|
| `~/.local/share/betterframe-xp/lib/libbetterframexp.so` | レイヤー本体 | vrcompositor（Vulkan が読む） |
| `~/.local/share/vulkan/implicit_layer.d/betterframe-xp.json` | レイヤーのマニフェスト（Vulkan の登録） | Vulkan ローダー |
| `~/.config/betterframe-xp/vulkan.env` | `ENABLE_BETTERFRAME_XP=1`（有効化）。無効のときは `vulkan.env.disabled` | SteamVR の起動時 |
| `~/.config/systemd/user/steamvr.service.d/91-betterframe-xp.conf` | SteamVR の起動時に `vulkan.env` を読ませる drop-in。ガードの呼び出しも入る | systemd |
| `~/.config/betterframe-xp/params.conf` | 設定値。0.25 秒ごとに読み直す | レイヤー、パネル、`betterframectl` |
| `~/.config/betterframe-xp/gaze-before.conf` | 視線の補正を最初に切り替える前の値（`uninstall` で戻す） | パネル、`betterframectl` |
| `~/.config/betterframe-xp/guard-disabled` | ガードがレイヤーを外した記録 | `betterframectl status` |
| `~/.local/libexec/betterframe-xp-guard` | SteamVR の起動前後に動く自動ガード | systemd（drop-in から） |
| `~/.local/libexec/betterframe-xp-update` | パネルの「更新」タブが呼ぶ更新コマンド（確認 / 入れ替え） | パネル、systemd（一時ユニット） |
| `~/.local/share/betterframe-xp/VERSION` | 入っている版 | 更新コマンド、パネル |
| `~/.cache/betterframe-xp/` | 更新の状態（`update-state.json`）、ログ（`update.log`）、作業用の展開先 | 更新コマンド |
| `~/.local/bin/betterframectl` | 操作用コマンド | 手で実行 |
| `~/.local/bin/betterframe-xp-panel` と `~/.config/systemd/user/betterframe-xp-panel.service` | ダッシュボードのパネル（既定で有効。外すのは `--no-panel` / `panel-off`） | SteamVR（OpenVR オーバーレイ） |
| `~/.config/openvr/config/steamvr.vrsettings` | SteamVR の設定。視線の補正の 2 キーだけを書く（押したときのみ） | SteamVR |
| `$XDG_RUNTIME_DIR/betterframe-xp/`（tmpfs） | `stats`、`stats.late`、`stats.frames`。ディスクには書かない | `betterframectl stats`、パネル |
| `$XDG_RUNTIME_DIR/betterframe-xp.log` | レイヤーのログ（映像あり / なしの切り替えなど） | 確認用 |
| `$XDG_RUNTIME_DIR/betterframe-xp-panel.lock` | パネルの二重起動を防ぐロック | パネル |

- ルートのファイルシステムには何も入れない。`sudo` は使わない。
- `~/.local/share/betterframe-xp/` の中身はビルド結果。`install.sh` が Frame 上でビルドして入れる。

### PC 側

PC 側には、このツールの何も入れない。PC 側の設定は SteamVR の `driver_vrlink` の既存の項目（`useSharpening`、`sharpeningStrength`、帯域など）だけで、このツールは書き換えない。

## 元に戻す

```sh
betterframectl panel-off     # パネルをすぐ止め、次の SteamVR 起動からも出さない
                             # （今だけ消すなら、ダッシュボードのアイコンの「閉じる」。パネルが終わり「有効」が OFF になる。
                             #   「＋」でまた出せるが、「有効」は自分で ON にする）
betterframectl disable       # 次の SteamVR 起動からレイヤーを読ませない
betterframectl uninstall     # 完全削除（下の「視線の補正」も元の値に戻す。SteamVR が動いているときに実行）
```

`uninstall` は、**視線の補正を最初に切り替える前の値**（パネルか `betterframectl gaze` が自動で `~/.config/betterframe-xp/gaze-before.conf` に残す）に SteamVR の設定を戻してから、このツールが入れたものを全部消す。視線の補正を一度も切り替えていなければ、SteamVR の設定には触らない。SteamVR が動いていないと戻せないので、そのときは止まって知らせる（戻さずに消すなら `betterframectl uninstall --skip-gaze`）。動いている SteamVR のレイヤーは、再起動するまで外れない。

### 視線の補正を手で切り替える（SteamVR の設定）

パネルの「SteamVR 設定」タブの代わりに、コマンドでも切り替えられる。SteamVR が動いているときに使う。どちらも書く前に元の値を残す。

```sh
betterframectl gaze                  # 今の値（奥行き / 目の位置）を見る
betterframectl gaze reproj off       # 奥行きの補正をオフ（on でオン）
betterframectl gaze eye off          # 目の位置の補正をオフ（on でオン）
```

`betterframectl` を使わない場合は、SteamVR 付属の `vrcmd` で同じことができる（値は `true` / `false` ではなく `1` / `0`）:

```sh
/opt/steamvr/bin/linuxarm64/vrcmd --settings-bool steamvr.gazeDepReprojectEnable          # 読む
/opt/steamvr/bin/linuxarm64/vrcmd --set-settings-bool steamvr.gazeDepReprojectEnable 0    # 書く（0 = オフ、1 = オン）
/opt/steamvr/bin/linuxarm64/vrcmd --set-settings-bool steamvr.gazeDepEyeToHeadEnable 0
```

効かないときは SteamVR を再起動する（再起動は自分でやる。このツールはしない）。

### 起動に失敗したとき（自動ガード）

`betterframectl enable` は、SteamVR の起動前後に `~/.local/libexec/betterframe-xp-guard` を呼ぶ設定も入れる。SteamVR が**起動から 10 秒以内に異常終了することが 2 回続く**と、ガードがレイヤーを外す（`vulkan.env` を `vulkan.env.disabled` に改名）。次の起動からレイヤーは読み込まれない。`betterframectl status` に「disabled by guard」と出る。原因を直したら `betterframectl enable` で戻す。

- SteamOS 自身も、起動失敗が 4 回続くと前の OS スロットに戻す（`steamvr-health-check`）。ガードは 2 回で外すので、その手前で止まる。
- ガードが無い / 壊れていても SteamVR の起動は止まらない（systemd の `-` 指定。実機で確認）。
- 自分で SteamVR を終了 / 再起動したときは失敗と数えない。10 秒より長く動いたあとの異常終了も数えない（レイヤー以外の原因が多いため）。
- ガードはレイヤーが原因かどうかは見分けない。短時間の失敗が続いたら外すだけ。
- 実際に SteamVR を失敗させたテストはしていない（論理の検証と、隔離した環境でのスクリプトの動作確認のみ）。
- 手動で外すときは、別端末から ssh して上の `disable` を実行する。

## 既知の問題と注意

- **設定を上げすぎると、Frame の GPU 負荷が高まり、ガクッが出る可能性がある。** 負荷が上がると Frame の温度も上がり、ファンが動くことがある。作者の Frame で、重い設定ほど遅れたフレームが増えたことは確認している（[STUTTER.md](STUTTER.md) の A-1）。ファンの動きは、このツールでは測っていない。
- **今のビルドは、ヘッドセット内で未確認**。SteamVR を再起動すると読み込まれる。`betterframectl status` に `loaded in vrcompositor` と出れば、読み込まれている。
- **画面を 144 Hz と 120 Hz 以外にすると、シャープを掛けているときに一定の間隔で引っかかることがある。** 確認したのは 72 Hz と 80 Hz。作者の Frame では、72 Hz で 4 フレームに 1 回、コンポジタの present 待ちが失敗していた。シャープを切ると止まった。原因は未確定。144 Hz か 120 Hz で使うか、それ以外では `sharpenAmount` を 0 にする。→ [STUTTER.md](STUTTER.md) の F
- 「補正の技術説明」の中で、効果の個別の評価が無いものは `SETTINGS.md` の △ / ○ のとおり。実験的な機能は、見た目の評価が十分ではない。
- ホーム / ダッシュボードで頭を振ったときの小さなガクッは、このレイヤーを `streamOnly` で止めても残る。作者が調べたところ、残りの大半はコンポジタの画面切り替え時の仕事（シェーダー生成、パススルー接続など）と重なっている。「通常プレイの範囲内」と作者は判断した。
- シャープは**レンズ歪み補正後**に掛かる。圧縮ノイズも一緒に強調するので、`sharpenAmount` 1 超はリンギング（縁の白黒の縁取り）が出やすい。
- vrcompositor 内蔵のシャープと二重掛けになる場合は、どちらかを切る。
- FrameColor（別の vkBasalt 改造レイヤー）との併用は未確認。
- OS / SteamVR / ドライバー（Turnip）の更新で動かなくなる可能性がある。映像の検出は、コンポジタの内部の描画の仕方（YUV 画像を読む view の使い方）に依存している。
- 検出器が見逃す描画経路がある（テンプレート更新、プッシュディスクリプタ）。見逃すと、映像が出ているのに効果が掛からない。そのときは `betterframectl set streamOnly 0` で全フレームに掛ける。

## 構成

| パス | 内容 |
|---|---|
| `layer/` | Vulkan レイヤー本体（C++、meson）。技術説明は [docs/ARCHITECTURE.md](ARCHITECTURE.md) |
| `panel/` | ダッシュボードのパネル（C++、cmake、OpenVR オーバーレイ）。 |
| `contrib/` | `install.sh`、`betterframectl`、既定の `params.conf`、パネルの systemd ユニット |
| `docs/` | 技術説明、設定リファレンス、「ガクッ」の種類 |

## ライセンス

このリポジトリ自体は MIT License（[LICENSE](../LICENSE)、Copyright (c) 2026 omuomu0909）。

`layer/` は vkBasalt（zlib）→ FrameColor の改造を元にしている。パネルの一部は FrameColor（MIT）、OpenVR ヘッダは BSD-3-Clause。シェーダーの CAS（AMD）と deband（haasn）は MIT。元のコードはそれぞれのライセンスのまま。全文と出どころは [THIRD_PARTY.md](../THIRD_PARTY.md)。
