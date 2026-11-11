# 3DS-web 並列作業のルール

Azahar（3DS エミュレーター、GPL）を Emscripten で WebAssembly に移植し、
ロースペックのブラウザ（Chromebook 並み）で市販タイトルを遊べるようにするプロジェクト。
複数のエージェントが並列に作業する。各エージェントはこの文書を最初に読むこと。

## 構成

| 場所 | 中身 |
|---|---|
| `~/games/3ds-web`（Cloud Shell、永続） | このリポジトリ。再構築スクリプト、パッチ、フロントエンド、ツール |
| `/tmp/3ds/azahar`（Cloud Shell、消える） | Azahar の統合ツリー。ブランチ `web`（オーケストレーター専用。触らない） |
| `/tmp/3ds/wt/<name>` | 各エージェントの Azahar 作業ツリー。ブランチ `wip/<name>`（`web` から分岐） |
| `/tmp/3ds/wt-home/<name>` | フロントエンドを専用に持つエージェントだけ: このリポジトリの作業ツリー |
| ラップトップ `~/3ds-web-build/agents/<name>` | 各エージェントのビルドとテストの場所（`ssh laptop`） |

上流は Azahar `6b4e7f2`（`bootstrap.sh` の `AZAHAR_REV`）。Emscripten 7.0.0。
ビルドは libretro コア + 自作フロントエンド（`frontend/main.cpp`）を `azahar_web.js/.wasm` にリンクする。
フラグは `-pthread -fwasm-exceptions -msimd128`。CPU は dynarmic（JIT）が使えないので fastinterp。

## 使うコマンド（`~/games/3ds-web` で実行）

```
tools/agent.sh <name> build              # 自分の作業ツリーの変更をラップトップへ送ってビルド
tools/agent.sh <name> test --graphics OpenGL --seconds 10
tools/agent.sh <name> test --graphics Software
tools/agent.sh <name> test ... --option citra_use_hw_shader=disabled   # コア設定の上書き（何度でも）
tools/agent.sh <name> test ... --chromebook   # Chromebook 相当（E コア 2 つ・メモリ 4GB）で速度を測る
tools/agent.sh <name> backup             # ブランチをラップトップへ退避（コミットのたびに）
```

- `build` は差分のあるファイルだけを送る。ccache が効くので、変更が少なければ 1〜2 分
- `test` は JSON を返す: `loaded`、`fps`、`errors`（ログの ERROR を種類ごとに集計）、
  `glslErrors`（シェーダーのコンパイルエラー）、`pageErrors`（JS 例外・wasm のトラップ）、
  `logTail`、`webgl`（使った GPU）。スクリーンショットは `/tmp/3ds-agents/<name>/shot.png`（Read で見られる）
- 既定の ROM は Homebrew Launcher（`boot.3dsx`）。他の ROM はラップトップの `~/3ds-web-build/roms/` に置く
- コア設定のキーは `src/citra_libretro/core_settings.cpp`（例: `citra_graphics_api`、`citra_use_hw_shader`、
  `citra_resolution_factor`）。フロントエンドの既定値は `frontend/main.cpp` の `options`
- テストのブラウザは Google Chrome（ヘッドレス）、WebGL は内蔵の Intel UHD（Mesa、ANGLE 経由）

## ルール

1. **自分の作業ツリーだけを編集する。** 統合ツリー `/tmp/3ds/azahar`、`~/games/3ds-web` 本体、
   他のエージェントのツリーは触らない。担当外のファイルを直す必要があれば最小限にして、報告に書く
2. **Web 向けの変更はガードする。** `#ifdef __EMSCRIPTEN__` か、`Driver` の機能フラグで分岐し、
   デスクトップ版の挙動は変えない
3. **こまめにコミットして退避する。** ブランチ `wip/<name>` にコミットし、`tools/agent.sh <name> backup`。
   Cloud Shell は数時間おきに再起動して `/tmp` が消える。コミット文の末尾に
   `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` を付ける
4. **外へ出さない。** push は `backup`（ラップトップ）だけ。Azahar 上流への PR や issue は禁止
   （Azahar の AI-POLICY.md が AI 生成コードの投稿を禁じている）
5. **ラップトップはユーザーの私物。** `~/3ds-web-build` の外は触らない。sudo は使わない。
   ビルドは P コア、`--chromebook` の計測は E コアで行われ、計測は同時に 1 つだけ（自動で順番待ち）
6. **Cloud Shell でブラウザやビルドを動かさない。** メモリが少ない（他の処理で 4GB 使用中）
7. ssh が `Tailscale SSH requires an additional check` と URL を出して止まったら、作業を止めて
   その URL を報告する（ユーザーが承認する必要がある）

## 既知の落とし穴

- glad は WebGL に無い関数のポインタを null で読む。呼ぶと wasm がトラップする
  （`pageErrors` に `null function or function signature mismatch` などが出る）
- WebGL2 は OpenGL ES 3.0 相当。GLSL は `#version 300 es`（`layout(binding=)`、テクスチャバッファ、
  イメージ読み書き、ジオメトリシェーダー、`gl_ClipDistance` は使えない）
- emscripten の GL 拡張名は `GL_` 付き（例: `GL_EXT_color_buffer_float`）。`WEBGL_` 系の拡張は
  `emscripten_webgl_enable_extension` で有効化が要る
- 今のフロントエンドはメインスレッドで `retro_run` を回している。メインスレッドは待機（Atomics.wait）が
  できないので、スレッド待ちが空回りして遅い（perf 担当が直す予定）

## 報告

終わったら（または行き詰まったら）次をまとめて返す:
何をしたか、コミット（ハッシュと要約）、テスト結果（変更前後の `glslErrors`・`errors`・`pageErrors`・`fps`）、
残っている問題、統合時に気をつけること（他の担当と衝突しそうな箇所）。
