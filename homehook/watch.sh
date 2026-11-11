#!/bin/sh
# Home key -> myTV, root part. myTV runs it through the Homebrew Channel's root exec:
#   watch.sh install     link the boot hook, clear the off switch, start the watcher
#   watch.sh uninstall   unlink the boot hook, set the off switch (takes effect at once), stop
#   watch.sh status      one line for myTV's settings: hook | poll | off | lginputhook | none
# HBC's run-parts calls it with no argument at boot through the init.d link. run-parts waits
# for every hook and skips names with a dot, hence the link name and the detach below.
#
# The watcher injects libmytvhook.so into lginput2 and micomservice (again whenever one of
# them restarts, since systemd brings it back without the hook). If injection is impossible it
# falls back to polling: LG Home in front -> launch myTV. If myTV keeps failing to come up
# after Home, it sets the bypass flag and Home goes to LG Home until myTV shows up again.
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
TRIES=$STATE/tries                  # "<daemon> <n>" inject attempts this boot
LOG=$STATE/watch.log
BYPASS=$STATE/homehook.bypass       # myTV keeps failing after Home: the hook passes Home to LG
PRESSED=$STATE/homehook.pressed     # mtime = the last Home press the hook took
LAST_FG=$STATE/last_fg              # the foreground while awake; on disk so a cold boot sees it too
LASTINPUT=/var/lib/eim/lastinput
LASTINPUT_ORIG=$STATE/lastinput.orig  # eim's record while lastinput points at myTV
ALLOW=/tmp/mytv-allow-lg-home
LGINPUTHOOK=/home/root/.config/lginputhook/keybinds.json
LGHOME=com.webos.app.home
DAEMONS="lginput2 micomservice"
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
  pwr=$(luna-send -n 1 luna://com.webos.service.tvpower/power/getPowerState '{}' </dev/null 2>/dev/null)
  echo "$pwr" | grep -qE '"processing"[^"]*"[^"]*(Power|Standby|Screen On|LastInput)' && return
  # Any other state is a passage: the nightly QuickStart+ reboot goes through one, and taking it
  # for a wake put lastinput back before the reboot.
  case "$(echo "$pwr" | sed -n 's/.*"state"[^"]*"\([^"]*\)".*/\1/p')" in
    Active|"Screen Saver"|"Screen Off") echo awake ;;
    *Standby*|Suspend*) echo asleep ;;
  esac
}

# bootd opens physicalLastSourceAppId from eim's lastinput after power-on, reading it at that moment
# on a QuickStart+ wake and on a cold boot alike. Pointed at myTV for the standby, it makes bootd
# open myTV itself, with no input on screen first. The rest of eim's record stays as it was.
point_lastinput_at_mytv() {
  [ -f "$LASTINPUT" ] || return
  [ -f "$LASTINPUT_ORIG" ] || cp -p "$LASTINPUT" "$LASTINPUT_ORIG"
  sed -i "s/\"physicalLastSourceAppId\":\"[^\"]*\"/\"physicalLastSourceAppId\":\"$APP_ID\"/" "$LASTINPUT"
  sync
}

# Back to eim's record, unless eim has written a newer one meanwhile.
restore_lastinput() {
  [ -f "$LASTINPUT_ORIG" ] || return
  grep -q "\"physicalLastSourceAppId\":\"$APP_ID\"" "$LASTINPUT" && cp -p "$LASTINPUT_ORIG" "$LASTINPUT" && sync
  rm -f "$LASTINPUT_ORIG"
}

launch_mytv() {
  luna-send -n 1 luna://com.webos.applicationManager/launch "{\"id\":\"$APP_ID\"}" </dev/null >/dev/null 2>&1
}

# A physical input/source the user may want resumed on wake, as opposed to an app or LG Home.
is_source() {
  case "$1" in
    com.webos.app.hdmi*|com.webos.app.externalinput*|com.webos.app.component*|com.webos.app.av*|com.webos.app.livetv*) return 0 ;;
    *) return 1 ;;
  esac
}

MAX_TRIES=2       # per daemon per boot: after this, stay in poll, never ptrace again
INJECT_TIMEOUT=20 # ezinject fails fast; a longer run means it hung with the daemon stopped
                  # busybox timeout wants -t SECS, not a bare number (else it runs "20" as the program)

# Inject into every daemon not already hooked. Returns 0 when the hook is in all of them.
# The -l file holds only what the injected code prints; ezinject's own stdout carries the
# reason an injection fails before the payload runs, so both go to one per-daemon log.
inject_all() {
  ok=0
  for d in $DAEMONS; do
    pid=$(pidof "$d" | awk '{print $1}')
    [ -n "$pid" ] || { log "$d not running"; ok=1; continue; }
    elog=$STATE/inject-$d.log
    # Hooked = the library is in this daemon's map (a pid list on disk outlives a reboot that reuses
    # the pid) and its last injection succeeded (a failed one stays mapped too: the library persists).
    grep -q libmytvhook.so "/proc/$pid/maps" 2>/dev/null && grep -q "lib_main returned: 0" "$elog" 2>/dev/null && continue
    tries=$(sed -n "s/^$d //p" "$TRIES" 2>/dev/null); tries=${tries:-0}
    [ "$tries" -ge "$MAX_TRIES" ] && { ok=1; continue; }

    : > "$elog"     # only this attempt: an older success line must not vouch for it
    timeout -t "$INJECT_TIMEOUT" "$HOOK/ezinject" -l "$elog" "$pid" "$HOOK/libmytvhook.so" >> "$elog" 2>&1
    rc=$?
    if grep -q "lib_main returned: 0" "$elog"; then
      log "hooked $d ($pid)"
      continue
    fi
    ok=1
    sed -i "/^$d /d" "$TRIES" 2>/dev/null; echo "$d $((tries + 1))" >> "$TRIES"
    if [ "$rc" = 124 ] || [ "$rc" = 143 ]; then   # GNU timeout: 124; busybox: the child's SIGTERM
      # ezinject hung with the daemon's main thread stopped: let systemd restart it clean
      log "inject $d ($pid) timed out, killing daemon"
      kill -9 "$pid" 2>/dev/null
    else
      log "inject $d ($pid) failed (rc=$rc): $(grep -m1 'ERR' "$elog")"
    fi
  done
  return $ok
}

run() {
  mkdir -p "$STATE"
  # ezinject's tempnam() payload file lands here (root-only 0700), not in shared /tmp
  export TMPDIR="$STATE"
  mode=""
  prev_fg=""
  strikes=0; strike_start=0; last_launch=0; mytv_seen=0
  # a press from before this watcher started (it survives standby in /tmp) is not a new failure
  seen_press=$(stat -c %Y "$PRESSED" 2>/dev/null || echo 0)
  # Power phase last seen. When QuickStart+ did not hold, power-on is a cold boot: init.d runs well
  # after the screen is on, so a boot start counts as coming out of standby.
  prev_phase=awake
  [ "$1" = boot ] && prev_phase=asleep
  # a watcher stopped during standby left lastinput pointing at myTV
  [ "$1" != boot ] && [ "$(power_phase)" = awake ] && restore_lastinput
  active_fg=$(cat "$LAST_FG" 2>/dev/null)
  while [ -d "$APP" ] && [ ! -f "$DISABLED" ]; do
    now=$(date +%s)
    phase=$(power_phase)
    fg=$(foreground)

    # Wake landing, decided from where we were before standby (active_fg). A real input (HDMI etc.)
    # is left to bootd as usual. From an app or LG Home, bootd is told to open myTV instead, and on
    # the wake the watcher launches myTV itself if bootd did not (a cold boot that skipped standby).
    if [ "$phase" = asleep ] && [ "$prev_phase" = awake ]; then
      [ ! -f "$BYPASS" ] && [ -n "$active_fg" ] && ! is_source "$active_fg" \
        && { point_lastinput_at_mytv; log "standby: was in $active_fg, bootd opens myTV"; }
    elif [ "$phase" = awake ] && [ "$prev_phase" = asleep ]; then
      restore_lastinput
      [ ! -f "$BYPASS" ] && [ -n "$active_fg" ] && ! is_source "$active_fg" && [ "$fg" != "$APP_ID" ] \
        && { launch_mytv; log "wake: was in $active_fg, $fg in front -> myTV"; }
    elif [ "$phase" = awake ] && [ -n "$fg" ] && [ "$fg" != "$active_fg" ]; then
      active_fg=$fg           # steady awake: remember where we are, for the next wake
      echo "$fg" > "$LAST_FG"
    fi
    [ -n "$phase" ] && prev_phase=$phase

    # After the wake landing: a hung injection can take INJECT_TIMEOUT, and the input shows meanwhile.
    if lginputhook_owns_home; then
      new=lginputhook
    elif inject_all; then
      new=hook
    else
      new=poll
    fi
    [ "$new" != "$mode" ] && { log "mode $new"; mode=$new; echo "$mode" > "$MODE"; }

    # myTV is up again: whatever made it fail is over, give Home back to it.
    if [ "$fg" = "$APP_ID" ]; then
      mytv_seen=$now
      [ -f "$BYPASS" ] && { rm -f "$BYPASS"; log "myTV is back, bypass off"; }
      strikes=0
    fi

    # Hook mode: a Home press the hook took should bring myTV up within a few seconds. myTV seen
    # since the press counts even if the user has already left it for an app.
    if [ "$mode" = hook ] && [ -f "$PRESSED" ]; then
      pressed=$(stat -c %Y "$PRESSED")
      if [ "$pressed" -gt "$seen_press" ] && [ $((now - pressed)) -ge 4 ]; then
        seen_press=$pressed
        if [ "$fg" != "$APP_ID" ] && [ "$mytv_seen" -lt "$pressed" ]; then
          [ $((now - strike_start)) -gt 60 ] && { strikes=0; strike_start=$now; }
          strikes=$((strikes + 1))
          log "Home taken but myTV not in front ($fg), strike $strikes"
          [ "$strikes" -ge 3 ] && [ ! -f "$BYPASS" ] && { touch "$BYPASS"; log "bypass on: Home goes to LG Home"; }
        fi
      fi
    fi

    # Poll mode: the hook is not in place, so catch LG Home after the fact.
    if [ "$mode" = poll ] && [ "$fg" = "$LGHOME" ] && [ "$prev_fg" != "$LGHOME" ] && [ ! -f "$BYPASS" ]; then
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
  restore_lastinput
  [ -d "$APP" ] || rm -f "$LINK"
  rm -f "$MODE"
}

case "$1" in
  run) run "$2" ;;
  install)
    mkdir -p "$STATE" /var/lib/webosbrew/init.d
    chmod 700 "$STATE"
    rm -f "$DISABLED" "$BYPASS" "$TRIES"
    ln -sf "$HOOK/watch.sh" "$LINK"
    start_watcher
    ;;
  uninstall)
    mkdir -p "$STATE"
    touch "$DISABLED"
    rm -f "$LINK"
    pid=$(watcher_pid); [ -n "$pid" ] && kill "$pid"
    restore_lastinput
    rm -f "$MODE"
    ;;
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
    rm -f "$TRIES"   # a reboot earns the daemons a fresh set of attempts
    start_watcher boot
    ;;
  *) echo "usage: $0 [install|uninstall|status]" >&2; exit 2 ;;
esac
exit 0
