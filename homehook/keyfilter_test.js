// Runs homehook/keyfilter.js against mocks of what surface-manager gives a key filter.
//   node homehook/keyfilter_test.js homehook/keyfilter.js
const fs = require("fs");
const vm = require("vm");
const assert = require("assert");

const HOME = 0x01000053, IVI = 18874410, NETFLIX = 18874408, BACK = 18874371, OK = 0x01000004;
const MYTV = "com.viiees.mytvlauncher";
const BOOT = "com.viiees.mytvlauncher.boot", BOOTED = "com.viiees.mytvlauncher.booted";

// stored: configd's values for init()'s one-shot read of BOOT and BOOTED, or a raw payload string.
// configd is a mock with memory: what the filter sets is in the next read.
function load(stored = {}) {
    const launches = [], events = [], configd = typeof stored === "string" ? null : { ...stored };
    let onConfig = null;
    const ctx = {
        Qt: { Key_Super_L: HOME },
        WebOS: { Key_webOS_IVI: IVI, Key_webOS_Netflix: NETFLIX, Key_webOS_Back: BACK },
        KeyPolicy: { Accepted: "accepted", NotAccepted: "notAccepted", NextPolicy: "next" },
        applicationManager: { launch: (id, params) => launches.push(id) },
        lunaCall: {
            subscribe: (svc, method, params, cb) => { onConfig = cb; },
            send: (svc, method, params, cb) => {
                const p = JSON.parse(params);
                if (method === "/getConfigs") {
                    if (!configd) return cb(stored);
                    const configs = {}, missingConfigs = [];
                    for (const n of p.configNames) n in configd ? configs[n] = configd[n] : missingConfigs.push(n);
                    return cb(JSON.stringify({ configs, missingConfigs, returnValue: true }));
                }
                if (p.volatile !== true) throw new Error("the filter writes volatile values only");
                if (configd) Object.assign(configd, p.configs);
                if (p.configs["com.viiees.mytvlauncher.keyevent"]) events.push(p.configs["com.viiees.mytvlauncher.keyevent"]);
            },
        },
        JSON, Date,
    };
    vm.createContext(ctx);
    vm.runInContext(fs.readFileSync(process.argv[2], "utf8"), ctx);
    ctx.init();
    ctx.init();   // a rebuild of the chain runs init() again
    const publish = value => onConfig(JSON.stringify(value === undefined
        ? { missingConfigs: ["x"], returnValue: true }
        : { configs: { "com.viiees.mytvlauncher.keyfilter": value }, returnValue: true }));
    const press = key => [ctx.handleMyTVKeys(key, true, false), ctx.handleMyTVKeys(key, true, true), ctx.handleMyTVKeys(key, false, false)];
    return { ctx, launches, events, publish, press };
}
const now = () => Math.floor(Date.now() / 1000);
const all = (results, policy) => results.every(r => r === policy);

// no data published yet: everything passes
let t = load();
assert(all(t.press(HOME), "next") && all(t.press(IVI), "next") && t.launches.length === 0, "inert without data");
t.publish(undefined);
assert(all(t.press(HOME), "next"), "inert when the value is missing");

// live watcher, home on: Home is taken whole and launches myTV once
t = load();
t.publish({ alive: now(), home: true, learnUntil: 0, binds: { [IVI]: "youtube.leanback.v4" } });
assert(all(t.press(HOME), "accepted"), "Home press, repeat and release are taken");
assert.deepStrictEqual(t.launches, [MYTV]);
assert(t.events.length === 1 && t.events[0].type === "home", "Home is reported once");

// bound button launches its app; unbound bindable and system keys pass
assert(all(t.press(IVI), "accepted"), "bound key taken");
assert.deepStrictEqual(t.launches, [MYTV, "youtube.leanback.v4"]);
assert(all(t.press(NETFLIX), "next"), "unbound bindable key passes");
assert(all(t.press(BACK), "next") && all(t.press(OK), "next"), "system keys pass");

// bypass: Home passes, binds still work
t.publish({ alive: now(), home: false, learnUntil: 0, binds: { [IVI]: "youtube.leanback.v4" } });
assert(all(t.press(HOME), "next"), "bypass passes Home");
assert(all(t.press(IVI), "accepted"), "binds work during bypass");

// stale heartbeat: the filter stands down
t.publish({ alive: now() - 91, home: true, learnUntil: 0, binds: { [IVI]: "youtube.leanback.v4" } });
assert(all(t.press(HOME), "next") && all(t.press(IVI), "next"), "stale heartbeat passes everything");

// a Home press decided while taken stays taken even if the data changes before the release
t.publish({ alive: now(), home: true, learnUntil: 0, binds: {} });
assert.strictEqual(t.ctx.handleMyTVKeys(HOME, true, false), "accepted");
t.publish({ alive: now(), home: false, learnUntil: 0, binds: {} });
assert.strictEqual(t.ctx.handleMyTVKeys(HOME, false, false), "accepted", "release follows its press");

// learn: the next bindable press is reported and taken, once; a system key is never learned
t = load();
t.publish({ alive: now(), home: true, learnUntil: now() + 20, binds: { [IVI]: "youtube.leanback.v4" } });
assert(all(t.press(BACK), "next") && t.events.length === 0, "Back is not learnable");
assert(all(t.press(IVI), "accepted"), "learned key taken");
assert(t.events.length === 1 && t.events[0].type === "learned" && t.events[0].code === IVI);
assert.deepStrictEqual(t.launches, [], "a learned press does not launch its old app");
assert(all(t.press(NETFLIX), "next") && t.events.length === 1, "only one key per learn");
t.publish({ alive: now(), home: true, learnUntil: now() - 1, binds: {} });
assert(all(t.press(NETFLIX), "next"), "expired learn window");

// an app id that is not an id is never launched
t = load();
t.publish({ alive: now(), home: true, learnUntil: 0, binds: { [IVI]: 'x","evil":"y' } });
assert(all(t.press(IVI), "next") && t.launches.length === 0, "bad app id ignored");

// cold boot after the user was in an app: myTV once, though init() ran twice
t = load({ [BOOT]: { openMyTV: true } });
assert.deepStrictEqual(t.launches, [MYTV], "boot into myTV, once per boot");

// cold boot after an input, or with no hint stored: bootd's landing stands
assert.deepStrictEqual(load({ [BOOT]: { openMyTV: false } }).launches, [], "input stays");
assert.deepStrictEqual(load({}).launches, [], "no hint, no launch");

// the watcher put the filter in (install, update): BOOTED is already set
assert.deepStrictEqual(load({ [BOOT]: { openMyTV: true }, [BOOTED]: { t: 1 } }).launches, [], "install is not a boot");

// configd failed: no launch
assert.deepStrictEqual(load("not json").launches, [], "configd error");

console.log("keyfilter.js: all scenarios pass");
