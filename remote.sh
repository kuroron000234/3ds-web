#!/usr/bin/env bash
# ビルドをラップトップ（Tailscale 経由、~/.ssh/config の Host laptop）で行う補助スクリプト。
# Cloud Shell は 2 コアで /tmp も再起動で消えるため、重いビルドはこちらに任せる。
#
# 使い方: ./remote.sh sync    作業ツリーをラップトップへ送る
#         ./remote.sh build   送ってからビルドし、ログを手元へ流す（終わるまで戻らない）
#         ./remote.sh fetch   成果物（azahar_web.js / .wasm）を web/ へ持ち帰る
#         ./remote.sh status  ラップトップ側のビルドの最終行を表示する
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
HOST="${REMOTE_HOST:-laptop}"
RDIR="${REMOTE_DIR:-3ds-web-build}" # ラップトップのホームからの相対パス
LOG_DIR=/tmp/3ds-remote             # 手元に流すログ（statusline.sh が読む）

sync() {
    # 削除したファイルは消えないが、ビルドに使うファイルは上書きされる
    tar -C "$HERE" --exclude=.git --exclude='web/azahar_web.*' -cf - . |
        ssh "$HOST" "mkdir -p $RDIR/repo && tar -C $RDIR/repo -xf -"
}

build() {
    sync
    mkdir -p "$LOG_DIR"
    : >"$LOG_DIR/build.log"
    # SSH が切れてもビルドが止まらないよう、ラップトップ側で切り離して起動する。
    # 普段使いの邪魔をしないよう優先度を下げる。
    local pid
    pid=$(ssh "$HOST" "cd $RDIR/repo && WORK=\$HOME/$RDIR/work setsid nohup nice -n 15 \
        ./bootstrap.sh build > \$HOME/$RDIR/build.log 2>&1 < /dev/null & echo \$!")
    echo "ラップトップでビルド開始 (pid $pid)"
    ssh "$HOST" "tail -n +1 -F --pid=$pid $RDIR/build.log 2>/dev/null" >"$LOG_DIR/build.log" || true
    status
}

fetch() {
    local f
    for f in azahar_web.js azahar_web.wasm; do
        ssh "$HOST" "cat $RDIR/repo/web/$f" >"$HERE/web/$f.part"
        mv "$HERE/web/$f.part" "$HERE/web/$f"
    done
    ls -la "$HERE/web/azahar_web.js" "$HERE/web/azahar_web.wasm"
}

status() {
    ssh "$HOST" "grep -o '^\[[0-9]*/[0-9]*\]' $RDIR/build.log | tail -1; \
        grep -c '^FAILED' $RDIR/build.log; tail -3 $RDIR/build.log"
}

case "${1:-}" in
sync | build | fetch | status) "$1" ;;
*)
    sed -n '4,8p' "$0"
    exit 1
    ;;
esac
