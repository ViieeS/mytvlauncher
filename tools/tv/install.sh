#!/bin/sh
# Development install on a rooted TV with telnet: upload build/<ipk> and install it as a developer app.
# usage: tools/tv/install.sh <tv ip>
set -eu
TV=$1
cd "$(dirname "$0")/../.."
IPK=$(ls build/*_arm.ipk | head -n 1)
NAME=$(basename "$IPK")

python3 tools/tv/tvput.py "$TV" "$IPK" "/tmp/$NAME"
python3 tools/tv/tvtelnet.py "$TV" "luna-send -w 120000 -i luna://com.webos.appInstallService/dev/install \
  '{\"id\":\"com.ares.defaultName\",\"ipkUrl\":\"/tmp/$NAME\",\"subscribe\":true}' | grep -m1 -E 'installed|failed|errorText'; \
  rm -f /tmp/$NAME"
