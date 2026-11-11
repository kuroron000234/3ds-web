#!/usr/bin/env bash
# Claude Code のステータスラインに 3DS-web のビルド進捗バーを出す。
# Cloud Shell のビルド（/tmp/3ds/build.log）と、remote.sh が手元に流している
# ラップトップのビルド（/tmp/3ds-remote/build.log）のうち、更新が新しい方を表示する。
cat >/dev/null # ステータスライン用の JSON 入力は使わない

LOCAL_LOG="${WORK:-/tmp/3ds}/build.log"
REMOTE_LOG=/tmp/3ds-remote/build.log

LOG=""
WHERE=""
for candidate in "$LOCAL_LOG" "$REMOTE_LOG"; do
    [ -f "$candidate" ] || continue
    if [ -z "$LOG" ] || [ "$candidate" -nt "$LOG" ]; then
        LOG="$candidate"
    fi
done
[ -n "$LOG" ] || { echo "3DS-web: ビルドログなし"; exit 0; }

if [ "$LOG" = "$REMOTE_LOG" ]; then
    WHERE="ラップトップ"
    running=$(pgrep -f 'tail -n \+1 -F --pid=' >/dev/null && echo 1)
else
    WHERE="Cloud Shell"
    running=$(pgrep -x ninja >/dev/null && echo 1)
fi

last=$(grep -o '^\[[0-9]*/[0-9]*\]' "$LOG" | tail -1 | tr -d '[]')
[ -n "$last" ] || { echo "3DS-web (${WHERE}): 準備中"; exit 0; }

done_n=${last%/*}
total=${last#*/}
fail=$(grep -c '^FAILED' "$LOG")
pct=$((done_n * 100 / total))
width=24
fill=$((done_n * width / total))

bar=$(printf '%*s' "$fill" '' | tr ' ' '#')$(printf '%*s' $((width - fill)) '' | tr ' ' '-')

if [ -n "$running" ]; then state="ビルド中"; else state="停止"; fi
echo "3DS-web (${WHERE}) [$bar] ${pct}% (${done_n}/${total}) 失敗 ${fail} ・ ${state}"
