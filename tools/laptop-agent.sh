#!/usr/bin/env bash
# ラップトップ側で、エージェントごとの作業ツリー・ビルド・ブラウザテストを扱う。
# Cloud Shell の tools/agent.sh から ssh 経由で呼ばれる（直接は使わない）。
#
#   laptop-agent.sh setup <name>
#   laptop-agent.sh build <name>
#   laptop-agent.sh test  <name> [webtest.mjs のオプション...]
#
# 配置（~/3ds-web-build/agents/<name>/）:
#   azahar/   Azahar の作業ツリー（work/azahar の複製。Cloud Shell の変更が同期される）
#   frontend/ フロントエンドのソース（同期される）
#   web/      index.html（同期される）
#   build/    ビルドディレクトリ   out/  成果物（azahar_web.js / .wasm）
#   emsdk, deps -> work/ のものを共有
set -euo pipefail

ROOT="$HOME/3ds-web-build"
REPO="$ROOT/repo"
cmd="${1:?}"
name="${2:?}"
shift 2
A="$ROOT/agents/$name"

export PATH="$ROOT/bin:$PATH"
# どのエージェントのツリーでも ccache が当たるよう、共通の親を基準にパスを相対化する
export CCACHE_BASEDIR="$ROOT"

# ビルドは P コア（0-11）に、Chromebook 相当の計測は E コア（12,13）に分けて互いに干渉させない
BUILD_CPUS=0-11
CHROMEBOOK_CPUS=12,13
CHROMEBOOK_MEM=4G

case "$cmd" in
setup)
    if [ ! -d "$A/azahar" ]; then
        mkdir -p "$A"
        cp -a "$ROOT/work/azahar" "$A/azahar"
        ln -sfn "$ROOT/work/emsdk" "$A/emsdk"
        ln -sfn "$ROOT/work/deps" "$A/deps"
        cp -a "$REPO/frontend" "$A/frontend"
        mkdir -p "$A/web" "$A/out"
        cp "$REPO/web/index.html" "$A/web/"
        echo "作成: $A"
    else
        echo "既存: $A"
    fi
    ;;
build)
    cd "$REPO"
    start=$(date +%s)
    status=0
    WORK="$A" FRONTEND_DIR="$A/frontend" OUT_DIR="$A/out" NINJA_JOBS=10 \
        nice -n 15 taskset -c "$BUILD_CPUS" ./bootstrap.sh build >"$A/run.log" 2>&1 || status=$?
    fails=$(grep -c '^FAILED' "$A/build.log" 2>/dev/null || true)
    echo "ビルド: $(($(date +%s) - start)) 秒、失敗 ${fails:-?} 件（終了コード $status）"
    if [ "${fails:-0}" != 0 ] || [ "$status" != 0 ]; then
        grep -E '^FAILED|error:|undefined symbol' "$A/build.log" "$A/run.log" 2>/dev/null |
            sed "s|$A/||g" | head -40
        echo "全文: ssh laptop cat 3ds-web-build/agents/$name/build.log"
        exit 1
    fi
    ls -l "$A/out"
    ;;
test)
    chromebook=0
    args=()
    for a in "$@"; do
        if [ "$a" = --chromebook ]; then chromebook=1; else args+=("$a"); fi
    done
    mkdir -p "$A/test"
    if [ "$chromebook" = 1 ]; then
        # 計測が重ならないよう、Chromebook 相当の計測は同時に 1 つだけ
        exec 9>"$ROOT/chromebook.lock"
        flock 9
        systemd-run --user --scope --quiet -p MemoryMax="$CHROMEBOOK_MEM" -- \
            taskset -c "$CHROMEBOOK_CPUS" node "$REPO/tools/webtest.mjs" --agent "$name" \
            --chromebook "${args[@]}"
    else
        taskset -c "$BUILD_CPUS" node "$REPO/tools/webtest.mjs" --agent "$name" "${args[@]}"
    fi
    ;;
*)
    echo "不明なコマンド: $cmd" >&2
    exit 2
    ;;
esac
