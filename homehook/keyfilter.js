// Home key and bound remote buttons -> apps, as a surface-manager key filter. Root part: watch.sh.
//
// surface-manager builds its key filter chain from the configd value
// com.webos.surfacemanager.keyFilters, a list of {handler, file}, and rebuilds it whenever the
// value changes. watch.sh puts handleMyTVKeys in front of LG's handleSystemKeys, so a Home press
// is taken before LG Home starts. Nothing is patched: the list is LG's own extension point.
//
// A filter cannot read or write files, so both directions go through configd values:
//   CONFIG_NAME  watch.sh -> filter  {"alive": s, "home": bool, "learnUntil": s, "binds": {"<key>": "<app id>"}}
//   EVENT_NAME   filter -> watch.sh  {"type": "home" | "learned", "code": <key>, "t": ms}
//   BOOT_NAME    watch.sh -> filter  {"openMyTV": bool}, kept on disk: where a cold boot should land
//   BOOTED_NAME  both                {"t": ms}, volatile: set once this boot's landing is decided
//
// The chain entry is kept on disk too, so the filter is loaded about 12 s into a cold boot, half a
// minute before root scripts run, and init() opens myTV then if the user was in an app.

var MYTV_APP_ID = "com.viiees.mytvlauncher";
var CONFIG_NAME = "com.viiees.mytvlauncher.keyfilter";
var EVENT_NAME = "com.viiees.mytvlauncher.keyevent";
var BOOT_NAME = "com.viiees.mytvlauncher.boot";
var BOOTED_NAME = "com.viiees.mytvlauncher.booted";
// watch.sh refreshes "alive" every 30 s. Older than this, the watcher is gone and nobody would
// notice a myTV that fails to start: every key goes back to LG.
var ALIVE_SECS = 90;

var cfg = {};
var bindableKeys = [];
var passingHome = true;   // decided on the press, kept for its repeats and release
var takenKey = -1;        // a bindable key taken on its press

function init() {
    // App and assistant buttons only. Their neighbours in the key range are Back, Exit, power
    // and factory keys, so "any LG key" is not a safe rule. A name this firmware lacks is
    // undefined and never matches a key.
    bindableKeys = [
        WebOS.Key_webOS_Netflix, WebOS.Key_webOS_Amazon, WebOS.Key_webOS_Disneyplus,
        WebOS.Key_webOS_RakutenTV, WebOS.Key_webOS_LGChannels, WebOS.Key_webOS_IVI,
        WebOS.Key_webOS_Okko, WebOS.Key_webOS_Kinopoisk, WebOS.Key_webOS_Hotstar,
        WebOS.Key_webOS_Globoplay, WebOS.Key_webOS_ZEE5, WebOS.Key_webOS_Watchaplay,
        WebOS.Key_webOS_UNext, WebOS.Key_webOS_GoogleVoice, WebOS.Key_webOS_ALEXA
    ];
    lunaCall.subscribe("com.webos.service.config", "/getConfigs",
                       JSON.stringify({"configNames": [CONFIG_NAME]}), onConfig, true);
    lunaCall.send("com.webos.service.config", "/getConfigs",
                  JSON.stringify({"configNames": [BOOT_NAME, BOOTED_NAME]}), onBoot);
}

// init() runs on every rebuild of the chain, not only at boot. BOOTED_NAME is volatile, so only
// the first init() after a reboot finds it missing. watch.sh sets it too before it puts the
// filter in, so an install or an update never launches anything.
function onBoot(payload) {
    var configs;
    try {
        configs = JSON.parse(payload).configs || {};
    } catch (e) {
        return;   // configd did not answer: land wherever bootd lands
    }
    if (configs[BOOTED_NAME])
        return;
    var booted = {};
    booted[BOOTED_NAME] = {"t": Date.now()};
    lunaCall.send("com.webos.service.config", "/setConfigs",
                  JSON.stringify({"configs": booted, "volatile": true}), null);
    var boot = configs[BOOT_NAME];
    if (boot && boot.openMyTV === true)
        applicationManager.launch(MYTV_APP_ID, "{}");
}

function onConfig(payload) {
    var value;
    try {
        value = JSON.parse(payload).configs[CONFIG_NAME];
    } catch (e) {
        value = undefined;   // the value is missing: watch.sh has not published it yet
    }
    cfg = (value && typeof value === "object") ? value : {};
}

function watcherAlive() {
    return typeof cfg.alive === "number" && Date.now() / 1000 - cfg.alive < ALIVE_SECS;
}

function report(type, key) {
    var configs = {};
    configs[EVENT_NAME] = {"type": type, "code": key, "t": Date.now()};
    lunaCall.send("com.webos.service.config", "/setConfigs",
                  JSON.stringify({"configs": configs, "volatile": true}), null);
}

function boundApp(key) {
    var id = cfg.binds ? cfg.binds[key] : undefined;
    return (typeof id === "string" && /^[A-Za-z0-9._-]+$/.test(id)) ? id : "";
}

function handleMyTVKeys(key, pressed, autoRepeat) {
    if (key === Qt.Key_Super_L) {
        if (pressed && !autoRepeat) {
            passingHome = !(watcherAlive() && cfg.home === true);
            if (!passingHome) {
                report("home", key);
                applicationManager.launch(MYTV_APP_ID, "{}");
            }
        }
        return passingHome ? KeyPolicy.NextPolicy : KeyPolicy.Accepted;
    }

    if (bindableKeys.indexOf(key) < 0)
        return KeyPolicy.NextPolicy;
    if (pressed && !autoRepeat) {
        takenKey = -1;
        if (watcherAlive()) {
            var app = boundApp(key);
            if (cfg.learnUntil > Date.now() / 1000) {
                cfg.learnUntil = 0;   // one button per learn
                report("learned", key);
                takenKey = key;
            } else if (app) {
                applicationManager.launch(app, "{}");
                takenKey = key;
            }
        }
    }
    if (key !== takenKey)
        return KeyPolicy.NextPolicy;
    if (!pressed)
        takenKey = -1;
    return KeyPolicy.Accepted;
}
