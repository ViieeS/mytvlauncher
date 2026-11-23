#!/bin/sh
# Pre-release frame pacing gate on a real TV: the working tree against a base ref, side by side.
#
# usage: tools/tv/perfgate.sh <tv ip> [base ref, default: the latest v* tag] [rounds, default 2]
#   TOOLCHAIN=... SDL=... as for make. Exit 0 pass, 1 regression, 2 setup error.
#
# Builds both as debug builds (the frame log and test hooks), then per round installs base and
# head in turn, discards one warm-up run after each launch and keeps one run of
# perf_scenario.sh. Base is rebuilt and measured in the same session instead of read from stored
# numbers: the same build spreads by several points between days and TV states. perf_gaps.py
# gives the verdict. The launcher that was installed is put back at the end.
#
# myTV must be in front: the gate relaunches it many times and drives it with the debug hooks.
set -eu
TV=$1
cd "$(dirname "$0")/../.."
ROOT=$PWD
BASE=${2:-$(git describe --tags --abbrev=0 --match 'v*')}
ROUNDS=${3:-2}
ID=$(python3 -c 'import json;print(json.load(open("package/appinfo.json"))["id"])')
APP=/media/developer/apps/usr/palm/applications/$ID
WORK=$(mktemp -d)
OUT=$WORK/runs
mkdir "$OUT"
MAKEVARS="DEBUG=1 ${TOOLCHAIN:+TOOLCHAIN=$TOOLCHAIN} ${SDL:+SDL=$SDL}"

tv() { python3 tools/tv/tvtelnet.py "$TV" "$@"; }

if ! tv "luna-send -n 1 luna://com.webos.applicationManager/getForegroundAppInfo '{}'" | grep -q "\"appId\":\"$ID\""; then
  echo "perfgate: $ID is not in front on $TV, not touching the TV" >&2
  exit 2
fi

echo "perfgate: building head (working tree) and base ($BASE)"
make -s -j8 $MAKEVARS >/dev/null
git worktree add -q --detach "$WORK/base" "$BASE"
restore() {
  tv "if [ -f $APP/launcher.perfgate ]; then luna-send -n 1 luna://com.webos.applicationManager/closeByAppId '{\"id\":\"$ID\"}' >/dev/null; \
      sleep 1; mv $APP/launcher.perfgate $APP/launcher; luna-send -n 1 luna://com.webos.applicationManager/launch '{\"id\":\"$ID\"}' >/dev/null; fi" >/dev/null || true
  git -C "$ROOT" worktree remove --force "$WORK/base" 2>/dev/null || true
}
trap restore EXIT
make -s -j8 -C "$WORK/base" $MAKEVARS >/dev/null

tv "cp $APP/launcher $APP/launcher.perfgate" >/dev/null
python3 tools/tv/tvput.py "$TV" tools/tv/perf_scenario.sh /tmp/perf_scenario.sh >/dev/null

r=1
while [ "$r" -le "$ROUNDS" ]; do
  for build in base head; do
    bin=build-debug/launcher
    [ "$build" = base ] && bin=$WORK/base/build-debug/launcher
    echo "perfgate: round $r/$ROUNDS, $build"
    sh tools/tv/deploy.sh "$TV" "$bin" >/dev/null
    tv "sleep 8; sh /tmp/perf_scenario.sh" >/dev/null                  # warm-up, discarded
    tv "sh /tmp/perf_scenario.sh" > "$OUT/$build-$r.log"
  done
  r=$((r + 1))
done

python3 tools/tv/perf_gaps.py "$OUT"
