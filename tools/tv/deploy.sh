#!/bin/sh
# Development loop: replace only the launcher binary in the installed app and relaunch it.
# usage: tools/tv/deploy.sh <tv ip> [binary, default build-debug/launcher from `make DEBUG=1`]
set -eu
TV=$1
BIN=${2:-build-debug/launcher}
cd "$(dirname "$0")/../.."
ID=$(python3 -c 'import json;print(json.load(open("package/appinfo.json"))["id"])')
APP=/media/developer/apps/usr/palm/applications/$ID

python3 tools/tv/tvput.py "$TV" "$BIN" "$APP/launcher.new"
python3 tools/tv/tvtelnet.py "$TV" "luna-send -n 1 luna://com.webos.applicationManager/closeByAppId '{\"id\":\"$ID\"}' >/dev/null; \
  sleep 1; chmod 755 $APP/launcher.new && mv $APP/launcher.new $APP/launcher && \
  luna-send -n 1 luna://com.webos.applicationManager/launch '{\"id\":\"$ID\"}' >/dev/null && echo launched"
