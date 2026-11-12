# 3DS-web

ブラウザで動く 3DS エミュレーター。[Azahar](https://github.com/azahar-emu/azahar)（Citra の後継）を
Emscripten で WebAssembly に移植した**非公式**のフォークです。Chromebook や古いノート PC など
ロースペックな端末のブラウザで、手持ちのソフトを遊べるようにすることを目指しています。

> [!IMPORTANT]
> これは Azahar チームとは無関係の非公式プロジェクトです。このリポジトリの不具合を
> Azahar の開発者に報告しないでください。また Azahar の [AI ポリシー](https://github.com/azahar-emu/azahar/blob/master/AI-POLICY.md)
> に従い、このリポジトリの変更（AI の支援を受けて書いたもの）を上流へ PR として送ることはしません。

**English:** An unofficial WebAssembly port of the Azahar Nintendo 3DS emulator that runs in the
browser. Not affiliated with the Azahar project or Nintendo. No ROMs, firmware or keys are included.
Licensed under GPL-3.0-or-later (see [License](#ライセンス)).

## できること

- 描画は WebGL2（GPU）とソフトウェア描画（CPU）を選べます。エミュレーションは専用スレッドで
  OffscreenCanvas に描くので、ページの操作が固まりません
- 起動時に GPU を調べて表示します。「自動」では WebGL2 を使い、本物の GPU があれば
  解像度を 2〜4 倍に上げられます。GPU が無くブラウザが CPU で WebGL を代行している場合
  （SwiftShader など）は 1 倍に固定します。代行の WebGL でもソフトウェア描画より速いためです
  （実測で実機比 35% 対 4%）。設定はブラウザに保存され、次の ROM の読み込みから反映されます
- 音声（AudioWorklet）、キーボード・ゲームパッド・タッチ（下画面）での操作
- ROM はメモリに丸ごと載せず、選んだファイルから必要な部分だけを読みます（数 GB の ROM でも開けます）
- 同梱の代替システムフォントで欠けている字（「ん」「ヶ」「～」など 20 字）を補います

### 制限

- CPU は JIT（dynarmic）が使えないため、高速インタプリタ（fastinterp）で動きます。重いタイトルは遅くなります
- 読めるのは**復号済み**の吸い出し（.3ds / .cxi / .3dsx など）だけです。暗号化された ROM の復号機能はなく、追加する予定もありません
- `SharedArrayBuffer` を使うため、配信には COOP/COEP ヘッダが必要です（後述）

## 必要なもの

**ゲームデータは同梱していません。** ROM・システムファイル・暗号鍵は含まれていないので、
ご自身が所有する 3DS 本体とソフトから、ご自身で吸い出したものを使ってください
（例: [GodMode9](https://github.com/d0k3/GodMode9) で復号済みの .3ds を作る）。
他人から入手した ROM の利用は著作権法に違反する場合があります。

ブラウザは WebGL2・`SharedArrayBuffer`・OffscreenCanvas に対応した最近の Chrome / Edge を想定しています。

## 遊び方

1. 下の手順でビルドし、`./serve.py 8000` でサーバーを起動する
2. `http://localhost:8000/` を開き、ROM ファイルを選ぶ

| 3DS | キーボード |
|---|---|
| A / B / X / Y | X / Z / S / A |
| L / R / ZL / ZR | Q / W / 1 / 2 |
| START / SELECT / HOME | Enter / Backspace / Esc |
| 十字キー | T / F / G / H |
| スライドパッド | 矢印キー |
| C スティック | I / J / K / L |
| 下画面 | クリック・タッチ |

ゲームパッド（標準配置）も使えます。音はページを一度クリックするかキーを押すと鳴り始めます。

## ビルド

Linux（x86_64）で確認しています。git・CMake・Python 3 が必要です（ninja が無ければ pip で入れます）。

```sh
./bootstrap.sh build
```

`bootstrap.sh` は次をすべて自動で行います。

1. Emscripten 7.0.0 を取得
2. Azahar を固定リビジョン `6b4e7f2` で clone し、必要なサブモジュールだけを取得
3. `patches/azahar.patch` と `patches/libressl.patch` を当てる
4. libretro コアと `frontend/main.cpp` をリンクして `azahar_web.js` / `azahar_web.wasm` を作り、`web/` にコピー

作業場所の既定は `/tmp/3ds` です（環境変数 `WORK` で変えられます）。2 コアの環境で全体ビルドに
1 時間以上かかります。ccache があれば自動で使います。

### 配信

`SharedArrayBuffer` を使うため、次のヘッダを付けて配信する必要があります。

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

`serve.py` はこれらを付けて `web/` を配信する確認用サーバーです。GitHub Pages のように
ヘッダを設定できないホスティングでは、そのままでは動きません。

## 構成

| パス | 中身 |
|---|---|
| `bootstrap.sh` | 上流の取得からビルドまでを再現するスクリプト |
| `patches/azahar.patch` | Azahar への変更（WebGL2 / GLSL ES 3.00 対応、テクスチャバッファの 2D テクスチャ化、ROM の逐次読み込み、フォント修正など） |
| `patches/libressl.patch` | LibreSSL を Emscripten でビルドするための修正 |
| `frontend/main.cpp` | libretro コアをブラウザで動かすフロントエンド（スレッド・入力・音声） |
| `web/index.html` | ページ |
| `serve.py` | COOP/COEP 付きの確認用サーバー |
| `tools/font/` | 代替システムフォントに足す字形を生成したスクリプト |
| `tools/`, `remote.sh`, `AGENTS.md`, `statusline.sh` | 開発に使った補助スクリプトとメモ（作者の環境向け） |

## ライセンス

このリポジトリは **GNU General Public License v3.0 or later**（[LICENSE](LICENSE)）で配布します。

- Azahar のソースは GPLv2 以降と GPLv3 以降のファイルが混在しています。それに当てるパッチと、
  それとリンクするフロントエンドを含む全体は GPLv3 以降になります
- `frontend/main.cpp` などこのリポジトリで書いたファイルのうち GPLv2 以降と表記したものは、
  単独では GPLv2 以降でも利用できます
- ビルド成果物（`azahar_web.js` / `.wasm`）を配布する場合は、GPL に従って対応するソース
  （このリポジトリと上流の該当リビジョン）を入手できるようにしてください

同梱・取得する第三者のソフトウェアとフォントの権利表記は [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) にあります。

## 商標

Nintendo 3DS は任天堂株式会社の商標です。このプロジェクトは任天堂とは一切関係がなく、
任天堂による承認・支援を受けたものではありません。その他の名称は各権利者の商標です。

## 謝辞

[Azahar](https://azahar-emu.org/) と [Citra](https://github.com/citra-emu/citra) の開発者の皆さんに感謝します。
このプロジェクトはその成果の上に成り立っています。
