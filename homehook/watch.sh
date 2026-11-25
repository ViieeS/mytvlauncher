#!/bin/sh
# Home key -> myTV, root part. myTV runs it through the Homebrew Channel's root exec:
#   watch.sh install     link the boot hook, clear the off switch, start the watcher
#   watch.sh uninstall   unlink the boot hook, set the off switch (takes effect at once), stop
#   watch.sh status      one line for myTV's settings: hook | poll | off | lginputhook | none
#   watch.sh learn | bind <code> <app> | unbind <app> | binds   remote buttons bound to apps
# HBC's run-parts calls it with no argument at boot through the init.d link. run-parts waits
# for every hook and skips names with a dot, hence the link name and the detach below.
#
# The watcher puts keyfilter.js into surface-manager's key filter chain, which surface-manager
# builds from the configd value com.webos.surfacemanager.keyFilters. The value is kept on disk, so
# after a cold boot the filter is in the chain before init.d runs and can open myTV at 23 s, not
# 43 s. The price: the stored list replaces LG's whole list until the watcher removes it. Where the
# list cannot be changed the watcher falls back to polling: LG Home in front -> launch myTV. If
# myTV keeps failing to come up after Home, it sets the bypass flag and Home goes to LG Home
# until myTV shows up again.
# Without the app directory it removes its boot link and exits: uninstalling myTV is enough.

APP_ID=com.viiees.mytvlauncher
APP=/media/developer/apps/usr/palm/applications/$APP_ID
HOOK=$APP/homehook
# First in line: run-parts runs hooks one by one, and on a cold boot the last input shows until
# the watcher starts. 1.0.0 used 60-mytv-homehook, after slow hooks such as a debloat sleep.
LINK=/var/lib/webosbrew/init.d/05-mytv-homehook
OLD_LINK=/var/lib/webosbrew/init.d/60-mytv-homehook
# Root-only state and logs live in STATE (0700, root): a shared /tmp path lets any uid feed
# root a symlink, a FIFO or a fake record. Only ALLOW stays in /tmp: myTV writes it under its own
# uid, and root just reads and removes it.
STATE=/var/lib/mytv
DISABLED=$STATE/homehook.disabled
MODE=$STATE/mode                    # hook | poll, what the running watcher does
LOG=$STATE/watch.log
BYPASS=$STATE/homehook.bypass       # myTV keeps failing after Home: the filter passes Home to LG
PRESSED=$STATE/homehook.pressed     # mtime = the last Home press the filter took
LAST_FG=$STATE/last_fg              # the foreground while awake; on disk so a cold boot sees it too
LASTINPUT=/var/lib/eim/lastinput
LASTINPUT_ORIG=$STATE/lastinput.orig  # eim's record while lastinput points at myTV
KEYBINDS=$STATE/keybinds            # "<Qt key> <app id>" per line: remote button -> app
KEYS_LEARN=$STATE/keys.learn        # mtime = when a learn started
KEYS_LEARNED=$STATE/keys.learned
LEARN_SECS=20
ALLOW=/tmp/mytv-allow-lg-home
LGINPUTHOOK=/home/root/.config/lginputhook/keybinds.json
LGHOME=com.webos.app.home
CONFIGD=luna://com.webos.service.config
CONFIGD_DB=/var/preferences/configd_factory_db.json   # configd's stored (non-volatile) values
KF_LIST=com.webos.surfacemanager.keyFilters   # surface-manager's key filter chain
KF_HANDLER=handleMyTVKeys
KF_DATA=com.viiees.mytvlauncher.keyfilter     # watcher -> filter, see keyfilter.js
KF_EVENT=com.viiees.mytvlauncher.keyevent     # filter -> watcher
KF_BOOT=com.viiees.mytvlauncher.boot          # watcher -> filter, on disk: open myTV at a cold boot?
KF_BOOTED=com.viiees.mytvlauncher.booted      # volatile: this boot's landing is decided
export PATH="/bin:/sbin:/usr/bin:/usr/sbin:$PATH"

log() { echo "$(date +%T) $*" >> "$LOG"; }

[ -L "$OLD_LINK" ] && { ln -sf "$HOOK/watch.sh" "$LINK"; rm -f "$OLD_LINK"; }

# procps ps lists only the caller's terminal by default: pgrep sees every process
watcher_pid() { pgrep -f "homehook/watch.sh run" | head -n 1; }
start_watcher() { [ -n "$(watcher_pid)" ] || setsid nohup "$HOOK/watch.sh" run "$1" </dev/null >/dev/null 2>&1 & }

lginputhook_owns_home() { [ -f "$LGINPUTHOOK" ] && grep -q '"773"' "$LGINPUTHOOK"; }

foreground() {
  luna-send -n 1 luna://com.webos.applicationManager/getForegroundAppInfo '{}' </dev/null 2>/dev/null \
    | sed -n 's/.*"appId":"\([^"]*\)".*/\1/p'
}

# awake | asleep, or empty while power is changing (apps close during a power off, so the
# foreground then is not where the user was) or luna fails. "Screen Saver" counts as awake, and so
# does "processing": "Request Screen Saver": LG's power state can sit in either for hours of use.
# QuickStart+ keeps the watcher ticking through standby, so an asleep -> awake edge is the wake.
power_phase() {
  phase_of "$(luna-send -n 1 luna://com.webos.service.tvpower/power/getPowerState '{}' </dev/null 2>/dev/null)"
}
phase_of() {
  # A power-off may go straight to Suspend: "Request Suspend" and "Prepare Suspend" then come
  # with "state": "Active", and the foreground empties under them (24.11, off from Plex). "Prepare
  # Resume" stays asleep: on that path it may be the main loop's only sample of the standby.
  echo "$1" | grep -qE '"processing"[^"]*"[^"]*(Power|Standby|Suspend|Screen On|LastInput)' && return
  # Any other state is a passage: the nightly QuickStart+ reboot goes through one, and taking it
  # for a wake put lastinput back before the reboot.
  case "$(echo "$1" | sed -n 's/.*"state"[^"]*"\([^"]*\)".*/\1/p')" in
    Active|"Screen Saver"|"Screen Off") echo awake ;;
    *Standby*|Suspend*) echo asleep ;;
  esac
}

# bootd opens physicalLastSourceAppId from eim's lastinput after power-on, reading it at that moment
# on a QuickStart+ wake and on a cold boot alike. Pointed at myTV, it makes bootd open myTV itself,
# with no input on screen first. The rest of eim's record stays as it was. eim records physical
# sources only, so it leaves the field alone while an app is in front, and rewrites it on an input.
# Held over a standby only. On a cold boot bootd opens myTV through inputcommon, which leaves SAM
# listing myTV in front with no process and a black screen until the watcher clears it at ~42 s.
# A cold boot from awake is the filter's: it opens myTV at ~23 s, after a few seconds of the input.
point_lastinput_at_mytv() {
  [ -f "$LASTINPUT" ] || return
  grep -q "\"physicalLastSourceAppId\":\"$APP_ID\"" "$LASTINPUT" && return
  cp -p "$LASTINPUT" "$LASTINPUT_ORIG"   # eim's newest record, put back on an input
  sed -i "s/\"physicalLastSourceAppId\":\"[^\"]*\"/\"physicalLastSourceAppId\":\"$APP_ID\"/" "$LASTINPUT"
  sync
}

# Back to eim's record, unless eim has written a newer one meanwhile.
restore_lastinput() {
  [ -f "$LASTINPUT_ORIG" ] || return
  grep -q "\"physicalLastSourceAppId\":\"$APP_ID\"" "$LASTINPUT" && cp -p "$LASTINPUT_ORIG" "$LASTINPUT" && sync
  rm -f "$LASTINPUT_ORIG"
}

# Power-on lands on myTV when the user left the TV in an app or LG Home. On an input, with the
# bypass on, or with no record of where the user is, it lands where bootd puts it.
lands_on_mytv() { [ ! -f "$BYPASS" ] && [ -n "$active_fg" ] && ! is_source "$active_fg"; }

# The filter's half of a cold boot. Stored only on a change: it is a write to configd's file.
# False over a standby: a cold boot from there finds lastinput at myTV, and a launch from the
# filter would wait on the ghost that leaves.
publish_boot_landing() {
  [ "$1" = "$boot_landing" ] && return
  configd setConfigs "{\"configs\":{\"$KF_BOOT\":{\"openMyTV\":$1}}}" | grep -q '"returnValue": *true' \
    && boot_landing=$1
}

# SAM can list myTV as running under another app's pid with no myTV process at all (seen after
# a boot that cut an OLED pixel refresh short). Every launch then waits on that ghost forever.
# Closing it by id clears the entry and releases the waiting launches. A launch in progress
# looks the same for a moment, before its process has exec'd launcher: only an entry whose pid
# is still the same foreign process 5 s later is a ghost.
# A cold boot with lastinput at myTV always leaves one: bootd opens myTV through inputcommon, and
# SAM lists myTV in front under inputcommon's pid on a black screen.
sam_mytv_pid() {
  timeout -t 5 luna-send -n 1 luna://com.webos.service.applicationmanager/running '{}' </dev/null 2>/dev/null \
    | tr '{' '\n' | grep "\"id\":\"$APP_ID\"" | sed -n 's/.*"processid":"\{0,1\}\([0-9][0-9]*\).*/\1/p'
}
pid_runs() { tr '\0' ' ' < "/proc/$1/cmdline" 2>/dev/null | grep -q "^$2"; }
is_mytv_pid() { pid_runs "$1" "$APP/launcher"; }
# Fails when the launch it waited on came up: a second launch would then be stale, and could pull
# myTV over an app the user opened from it meanwhile. $1: SAM's pid for myTV, if already asked.
clear_ghost() {
  ghost=${1:-$(sam_mytv_pid)}
  [ -n "$ghost" ] || return 0
  is_mytv_pid "$ghost" && return 0
  # inputcommon never turns into myTV, so there is no launch to wait for: this saves 5 s of black
  # screen on a cold boot
  if ! pid_runs "$ghost" /usr/bin/com.webos.app.inputcommon; then
    sleep 5
    again=$(sam_mytv_pid)
    is_mytv_pid "$again" && return 1
    [ "$again" = "$ghost" ] || return 0
  fi
  timeout -t 5 luna-send -n 1 luna://com.webos.applicationManager/closeByAppId "{\"id\":\"$APP_ID\"}" </dev/null >/dev/null 2>&1
  log "SAM listed myTV under pid $ghost ($(cat "/proc/$ghost/comm" 2>/dev/null)), not myTV: closed the entry"
}

# A launch that SAM never answers must not stall the main loop. $1 as for clear_ghost.
launch_mytv() {
  clear_ghost "$1" || return
  timeout -t 10 luna-send -n 1 luna://com.webos.applicationManager/launch "{\"id\":\"$APP_ID\"}" </dev/null >/dev/null 2>&1
}

# A physical input/source the user may want resumed on wake, as opposed to an app or LG Home.
is_source() {
  case "$1" in
    com.webos.app.hdmi*|com.webos.app.externalinput*|com.webos.app.component*|com.webos.app.av*|com.webos.app.livetv*) return 0 ;;
    *) return 1 ;;
  esac
}

# When an app closes (e.g. "Exit app"), the foreground goes empty and SAM's last_input_handler
# opens LG Home about a second later. myTV launched in that gap is in front first, and Home is
# not opened at all. Polling would only catch Home after it is on screen, so this subscribes.
# Runs beside the main loop, in the watcher's process group; luna-send is restarted if it dies.
watch_app_close() {
  while [ -d "$APP" ] && [ ! -f "$DISABLED" ]; do
    prev=""
    luna-send -i luna://com.webos.applicationManager/getForegroundAppInfo '{"subscribe":true}' \
      </dev/null 2>/dev/null | while read -r line; do
      fg=$(echo "$line" | sed -n 's/.*"appId":"\([^"]*\)".*/\1/p')
      if [ -z "$fg" ] && [ -n "$prev" ] && [ "$prev" != "$APP_ID" ] && [ "$prev" != "$LGHOME" ] \
        && ! is_source "$prev" && [ ! -f "$DISABLED" ] && [ ! -f "$BYPASS" ] \
        && grep -qE '^(hook|poll)$' "$MODE" 2>/dev/null && [ "$(power_phase)" = awake ]; then
        launch_mytv
        log "closed: $prev -> myTV"
      fi
      echo "$line" | grep -q '"appId"' && prev=$fg
    done
    sleep 2
  done
}

# The one writer of lastinput while the watcher runs. The main loop can miss a standby: on 23.11 it
# saw one only after the wake, bootd had opened HDMI 3, and myTV came 3 s later. tvpower's events
# come in time: the field flips ~0.2 s after "Request Power Off". Every state on the way down points
# the field where the user should land, and awake puts eim's record back after bootd has read it.
# The way up ("Prepare Power On" or "Prepare Resume", "LastInput Ready", "Screen On", the last also
# on leaving the screen saver) is not a way down and leaves the field alone.
watch_power() {
  while [ -d "$APP" ] && [ ! -f "$DISABLED" ]; do
    luna-send -i luna://com.webos.service.tvpower/power/getPowerState '{"subscribe":true}' \
      </dev/null 2>/dev/null | while read -r line; do
      case "$line" in
        *'"processing"'*'Power On'*|*'"processing"'*Resume*|*'"processing"'*LastInput*|*'"processing"'*'Screen On'*) continue ;;
        *'"state"'*|*'"processing"'*) ;;
        *) continue ;;
      esac
      if [ "$(phase_of "$line")" = awake ]; then
        restore_lastinput
        continue
      fi
      active_fg=$(cat "$LAST_FG" 2>/dev/null)
      if lands_on_mytv; then point_lastinput_at_mytv; else restore_lastinput; fi
    done
    sleep 2
  done
}

# A subscriber blocks in luna-send, so its loop never sees the off switch: kill both.
stop_subscribers() {
  for pid in "$@"; do kill "$pid" 2>/dev/null; done
  pkill -f 'luna-send -i luna://com.webos.applicationManager/getForegroundAppInfo .*"subscribe":true'
  pkill -f 'luna-send -i luna://com.webos.service.tvpower/power/getPowerState .*"subscribe":true'
  pkill -f "getConfigs .*$KF_EVENT"
}

# busybox timeout wants -t SECS, not a bare number (else it runs the number as the program)
configd() { timeout -t 10 luna-send -n 1 "$CONFIGD/$1" "$2" </dev/null 2>/dev/null; }

# What the filter may do, as one configd value. "alive" is a heartbeat: the filter stands down
# when it is older than 90 s, so a dead watcher never leaves Home pointing at a myTV that nobody
# checks. configd cannot delete a volatile key it has no default for, hence "home": false and no
# heartbeat instead of a removal.
publish_filter_data() {
  home=true
  { [ -f "$BYPASS" ] || [ -f "$DISABLED" ]; } && home=false
  learn=0
  [ -f "$KEYS_LEARN" ] && learn=$(($(stat -c %Y "$KEYS_LEARN") + LEARN_SECS))
  binds=$(awk '$1 ~ /^[0-9]+$/ && $2 ~ /^[A-Za-z0-9._-]+$/ { printf "%s\"%s\":\"%s\"", sep, $1, $2; sep = "," }' "$KEYBINDS" 2>/dev/null)
  configd setConfigs "{\"configs\":{\"$KF_DATA\":{\"alive\":$(date +%s),\"home\":$home,\"learnUntil\":$learn,\"binds\":{$binds}}},\"volatile\":true}" >/dev/null
}

# Put the filter in front of LG's Home handler. Returns 0 when it is in the chain. surface-manager
# reads the file as root, so it gets a root-only copy, named after its content: an updated filter
# has a new name, the list changes, and surface-manager loads it.
install_filter() {
  sum=$(md5sum "$HOOK/keyfilter.js" 2>/dev/null | cut -c1-12)
  [ -n "$sum" ] || return 1
  kf=$STATE/keyfilter-$sum.js
  if [ ! -f "$kf" ]; then
    rm -f "$STATE"/keyfilter-*.js
    cp "$HOOK/keyfilter.js" "$kf.tmp" && mv "$kf.tmp" "$kf" || return 1
  fi
  list=$(configd getConfigs "{\"configNames\":[\"$KF_LIST\"]}" | sed -n "s/.*\"$KF_LIST\":\(\[[^]]*\]\).*/\1/p")
  case "$list" in
    *"\"$kf\""*) return 0 ;;
    *'"handleSystemKeys"'*) ;;
    *) return 1 ;;   # no list, or not the chain the filter was written for
  esac
  entry="{\"handler\":\"$KF_HANDLER\",\"file\":\"$kf\"}"
  list=$(echo "$list" | sed -e "s/{[^{}]*\"$KF_HANDLER\"[^{}]*},//g" -e "s|{[^{}]*\"handleSystemKeys\"[^{}]*}|$entry,&|")
  configd setConfigs "{\"configs\":{\"$KF_LIST\":$list}}" | grep -q '"returnValue": *true' || return 1
  log "key filter in surface-manager's chain: $kf"
}

# A null puts LG's own list back: the stored value only shadows it. Volatile too, for a list a
# development build set that way. The null still leaves both names under usrDefined in configd's
# file, which a stock TV does not have: the file goes when nothing else is in it.
remove_filter() {
  configd setConfigs "{\"configs\":{\"$KF_LIST\":null,\"$KF_BOOT\":null}}" >/dev/null
  configd setConfigs "{\"configs\":{\"$KF_LIST\":null},\"volatile\":true}" >/dev/null
  rest=$(sed -e "s/\"$KF_LIST\"//g" -e "s/\"$KF_BOOT\"//g" "$CONFIGD_DB" 2>/dev/null | tr -d ' \t\n,')
  [ "$rest" = '{"com.webos.service.config":{"usrDefined":[]}}' ] && rm -f "$CONFIGD_DB"
  rm -f "$STATE"/keyfilter-*.js
  boot_landing=""
}

# The filter reports through a configd value. Each report becomes the file the rest of the
# watcher reads: a Home press the filter took, or the button it learned.
watch_key_events() {
  while [ -d "$APP" ] && [ ! -f "$DISABLED" ]; do
    first=1
    luna-send -i "$CONFIGD/getConfigs" "{\"configNames\":[\"$KF_EVENT\"],\"subscribe\":true}" \
      </dev/null 2>/dev/null | while read -r line; do
      [ -n "$first" ] && { first=""; continue; }   # the report from before this subscription
      case "$line" in
        *'"type":"home"'*) touch "$PRESSED" ;;
        *'"type":"learned"'*)
          [ -f "$KEYS_LEARN" ] && echo "$line" | sed -n 's/.*"code":\([0-9]*\).*/\1/p' > "$KEYS_LEARNED.new" \
            && mv "$KEYS_LEARNED.new" "$KEYS_LEARNED" ;;
      esac
    done
    sleep 2
  done
}

# Up to 1.1 the Home key was taken by a library injected into the two input daemons. It cannot
# be unloaded and would keep taking the key, so a daemon that still maps it is restarted: systemd
# brings it back clean.
unhook_daemons() {
  for d in lginput2 micomservice; do
    for pid in $(pidof "$d"); do
      grep -q libmytvhook.so "/proc/$pid/maps" 2>/dev/null || continue
      kill -9 "$pid" 2>/dev/null
      log "$d ($pid) still ran the injected hook, restarted it"
    done
  done
  rm -f "$STATE/tries" "$STATE/pids" "$STATE"/inject-*.log
}

# The injected hook stored evdev codes; the filter sees Qt keys. Only the IVI button's pair is
# known, the rest cannot be translated: those buttons are bound again from myTV. The old table
# stays beside the new one.
migrate_keybinds() {
  awk '$1 < 16777216 { old = 1 } END { exit !old }' "$KEYBINDS" 2>/dev/null || return
  cp -p "$KEYBINDS" "$KEYBINDS.evdev"
  awk '$1 == 1039 { $1 = 18874410 } $1 >= 16777216' "$KEYBINDS.evdev" > "$KEYBINDS.new" && mv "$KEYBINDS.new" "$KEYBINDS"
  log "keybinds: evdev codes translated or dropped, the old table is $KEYBINDS.evdev"
}

run() {
  mkdir -p "$STATE"
  unhook_daemons
  migrate_keybinds
  mode=""
  tick=0
  prev_fg=""
  strikes=0; strike_start=0; last_launch=0; mytv_seen=0
  # a press from before this watcher started (it survives standby in /tmp) is not a new failure
  seen_press=$(stat -c %Y "$PRESSED" 2>/dev/null || echo 0)
  # Power phase last seen. When QuickStart+ did not hold, power-on is a cold boot: init.d runs well
  # after the screen is on, so a boot start counts as coming out of standby.
  prev_phase=awake
  [ "$1" = boot ] && prev_phase=asleep
  active_fg=$(cat "$LAST_FG" 2>/dev/null)
  boot_landing=""
  # before the filter goes in: its init() must not take an install for a boot
  configd setConfigs "{\"configs\":{\"$KF_BOOTED\":{\"t\":$(date +%s)}},\"volatile\":true}" >/dev/null
  watch_app_close &
  close_pid=$!
  watch_key_events &
  events_pid=$!
  watch_power &
  power_pid=$!
  while [ -d "$APP" ] && [ ! -f "$DISABLED" ]; do
    now=$(date +%s)
    phase=$(power_phase)
    fg=$(foreground)

    # Wake landing, decided from where we were before standby (active_fg). A real input (HDMI etc.)
    # is left to bootd as usual. From an app or LG Home, lastinput (watch_power) tells bootd to open
    # myTV, and on the wake the watcher launches myTV itself if bootd did not. SAM's foreground is
    # checked against the process: after a cold boot it names myTV with only a ghost behind it.
    if [ "$phase" = asleep ] && [ "$prev_phase" = awake ]; then
      log "standby: was in $active_fg"
    elif [ "$phase" = awake ] && [ "$prev_phase" = asleep ]; then
      if lands_on_mytv; then
        sam_pid=$(sam_mytv_pid)
        if [ "$fg" != "$APP_ID" ] || ! is_mytv_pid "$sam_pid"; then
          launch_mytv "$sam_pid"
          log "wake: was in $active_fg, $fg in front -> myTV"
        fi
      fi
      tick=0   # the heartbeat may have gone stale over the standby
    elif [ "$phase" = awake ] && [ -n "$fg" ] && [ "$fg" != "$active_fg" ]; then
      active_fg=$fg           # steady awake: remember where we are, for the next wake
      echo "$fg" > "$LAST_FG.new" && mv "$LAST_FG.new" "$LAST_FG"   # watch_power reads it any time
    fi
    if [ "$phase" = awake ]; then
      if lands_on_mytv; then publish_boot_landing true; else publish_boot_landing false; fi
    elif [ "$phase" = asleep ]; then
      publish_boot_landing false
    fi
    [ -n "$phase" ] && prev_phase=$phase

    # Every 30 s: the heartbeat, which a reconfigure in configd drops, and the chain again, which
    # an update of the TV may rewrite. Every 5 s until the filter is in.
    if [ $((tick % 30)) -eq 0 ] || { [ "$mode" != hook ] && [ $((tick % 5)) -eq 0 ]; }; then
      if lginputhook_owns_home; then
        new=lginputhook
        [ "$mode" = hook ] && remove_filter
      elif install_filter; then
        new=hook
        publish_filter_data
      else
        new=poll
      fi
      [ "$new" != "$mode" ] && { log "mode $new"; mode=$new; echo "$mode" > "$MODE"; }
    fi
    tick=$((tick + 1))

    # myTV is up again: whatever made it fail is over, give Home back to it.
    if [ "$fg" = "$APP_ID" ]; then
      mytv_seen=$now
      [ -f "$BYPASS" ] && { rm -f "$BYPASS"; publish_filter_data; log "myTV is back, bypass off"; }
      strikes=0
    fi

    # Hook mode: a Home press the filter took should bring myTV up within a few seconds. myTV seen
    # since the press counts even if the user has already left it for an app.
    if [ "$mode" = hook ] && [ -f "$PRESSED" ]; then
      pressed=$(stat -c %Y "$PRESSED")
      if [ "$pressed" -gt "$seen_press" ] && [ $((now - pressed)) -ge 4 ]; then
        seen_press=$pressed
        if [ "$fg" != "$APP_ID" ] && [ "$mytv_seen" -lt "$pressed" ]; then
          [ $((now - strike_start)) -gt 60 ] && { strikes=0; strike_start=$now; }
          strikes=$((strikes + 1))
          log "Home taken but myTV not in front ($fg), strike $strikes"
          if [ "$strikes" -ge 3 ]; then
            [ -f "$BYPASS" ] || { touch "$BYPASS"; publish_filter_data; log "bypass on: Home goes to LG Home"; }
          else
            launch_mytv   # the filter's own launch may be stuck behind a ghost entry
          fi
        fi
      fi
    fi

    # Catch LG Home after the fact. Poll mode has no filter at all; in hook mode LG Home still opens
    # without a Home press, e.g. when an app's "Exit app" closes it.
    if { [ "$mode" = poll ] || [ "$mode" = hook ]; } && [ "$fg" = "$LGHOME" ] && [ "$prev_fg" != "$LGHOME" ] && [ ! -f "$BYPASS" ]; then
      if [ -f "$ALLOW" ] && [ $((now - $(stat -c %Y "$ALLOW"))) -lt 30 ]; then
        rm -f "$ALLOW"
      elif [ $((now - last_launch)) -ge 3 ]; then
        last_launch=$now
        launch_mytv
      fi
    fi
    prev_fg=$fg
    sleep 1
  done
  log "stopping: app gone or turned off"
  stop_subscribers "$close_pid" "$events_pid" "$power_pid"
  remove_filter
  restore_lastinput
  [ -d "$APP" ] || rm -f "$LINK"
  rm -f "$MODE"
}

case "$1" in
  run) run "$2" ;;
  install)
    mkdir -p "$STATE" /var/lib/webosbrew/init.d
    chmod 700 "$STATE"
    rm -f "$DISABLED" "$BYPASS"
    ln -sf "$HOOK/watch.sh" "$LINK"
    start_watcher
    ;;
  uninstall)
    mkdir -p "$STATE"
    touch "$DISABLED"
    rm -f "$LINK"
    pkill -f "homehook/watch.sh run"   # the main loop and its subscribers
    stop_subscribers
    remove_filter
    restore_lastinput
    rm -f "$MODE"
    ;;
  # Remote buttons -> apps. Every command prints the table after it: "<code> <app id>" per line.
  # learn waits for the next bindable press (the filter takes it from the TV) and prints
  # "learned <code>" first, or nothing when no button came in time.
  learn)
    mkdir -p "$STATE"
    rm -f "$KEYS_LEARNED"
    touch "$KEYS_LEARN"
    publish_filter_data
    n=0
    # busybox sleep takes whole seconds; 18 s stays inside the filter's LEARN_SECS window
    while [ ! -s "$KEYS_LEARNED" ] && [ "$n" -lt 18 ]; do sleep 1; n=$((n + 1)); done
    [ -s "$KEYS_LEARNED" ] && echo "learned $(cat "$KEYS_LEARNED")"
    rm -f "$KEYS_LEARN" "$KEYS_LEARNED"
    publish_filter_data
    cat "$KEYBINDS" 2>/dev/null
    ;;
  bind|unbind)
    code=$2; id=$3
    [ "$1" = unbind ] && { id=$2; code=0; }
    case "$code" in ''|*[!0-9]*) exit 2 ;; esac
    case "$id" in ''|*[!A-Za-z0-9._-]*) exit 2 ;; esac
    [ "$1" = bind ] && [ "$code" -lt 16777216 ] && exit 2   # a Qt key, not an evdev code
    mkdir -p "$STATE"
    # one button per app and one app per button
    awk -v c="$code" -v i="$id" '$1 != c && $2 != i' "$KEYBINDS" 2>/dev/null > "$KEYBINDS.new"
    [ "$1" = bind ] && echo "$code $id" >> "$KEYBINDS.new"
    mv "$KEYBINDS.new" "$KEYBINDS"
    publish_filter_data
    cat "$KEYBINDS"
    ;;
  binds) cat "$KEYBINDS" 2>/dev/null ;;
  status)
    if [ -f "$DISABLED" ] || [ ! -L "$LINK" ]; then echo off
    elif lginputhook_owns_home; then echo lginputhook
    elif [ -n "$(watcher_pid)" ] && [ -f "$MODE" ]; then cat "$MODE"
    else echo none
    fi
    ;;
  "")
    # boot, from run-parts: never block it
    [ -d "$APP" ] || { rm -f "$LINK"; exit 0; }
    [ -f "$DISABLED" ] && exit 0
    [ -n "$(watcher_pid)" ] && exit 0
    start_watcher boot
    ;;
  *) echo "usage: $0 [install|uninstall|status|learn|bind|unbind|binds]" >&2; exit 2 ;;
esac
exit 0
