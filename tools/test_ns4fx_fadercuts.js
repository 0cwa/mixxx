/* global require, __dirname, process, console */
/* eslint @typescript-eslint/no-require-imports: "off" -- Standalone Node CLI uses the repository script parser mode. */
// AI-generated whole-mapping timer lifecycle regression tests begin.
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const controllerDirectory = path.resolve(__dirname, "../res/controllers");
const mappingPath = process.argv[2] || path.join(controllerDirectory, "Numark-NS4FX-scripts.js");
const results = [];
const initializationMetrics = [];

const loadMapping = function(useStems, initialize) {
    const timers = new Map();
    const values = new Map();
    const connections = [];
    const events = [];
    const writes = [];
    const messages = [];
    let nextTimer = 1;
    const settings = {
        EnableWheel: true,
        ShiftLoadEjects: true,
        OnlyActiveDeckEffect: false,
        displayVUFromBothDecks: false,
        defaultPadMode: "hotcue",
        useFadercutsAsStems: Boolean(useStems),
        useAdditionalHotcues: false,
        exitSlipmodeAfterScratching: false,
    };
    const defaults = {bpm: 120, volume: 0.75, "rate_dir": 1, rateRange: 0.08, "num_samplers": 8};
    const getValue = function(group, key) {
        const id = `${group}:${key}`;
        return values.has(id) ? values.get(id) : (defaults[key] || 0);
    };
    const setValue = function(group, key, value) {
        values.set(`${group}:${key}`, value);
        writes.push({group, key, value});
    };
    const makeConnection = function(group, key, callback) {
        const connection = {
            active: true,
            disconnect: function() { this.active = false; },
            trigger: function() {
                assert.equal(typeof callback, "function");
                if (this.active) {
                    callback(getValue(group, key), group, key);
                }
            },
        };
        connections.push(connection);
        return connection;
    };
    const engine = {
        getSetting: function(key) {
            assert.ok(Object.prototype.hasOwnProperty.call(settings, key), key);
            return settings[key];
        },
        getValue,
        getParameter: getValue,
        setValue,
        setParameter: setValue,
        makeConnection,
        makeUnbufferedConnection: makeConnection,
        connectControl: makeConnection,
        softTakeover: function() {},
        softTakeoverIgnoreNextValue: function() {},
        isScratching: function() { return false; },
        scratchEnable: function() {},
        scratchDisable: function() {},
        scratchTick: function() {},
        beginTimer: function(interval, callback, oneShot) {
            assert.ok(Number.isFinite(interval) && interval > 0);
            assert.equal(typeof callback, "function");
            const id = nextTimer++;
            timers.set(id, {interval, callback, oneShot: Boolean(oneShot)});
            events.push({type: "start", id, interval});
            return id;
        },
        stopTimer: function(id) {
            assert.ok(timers.has(id), `stop must own active timer ${id}`);
            events.push({type: "stop", id});
            timers.delete(id);
        },
    };
    const context = vm.createContext({
        engine,
        midi: {
            sendShortMsg: function(...args) { messages.push(args); },
            sendSysexMsg: function(...args) { messages.push(args); },
        },
        print: function() {},
        console: {
            log: function() {},
            warn: function() {},
            error: function() {},
        },
    });
    for (const name of ["common-controller-scripts.js", "lodash.mixxx.js", "midi-components-0.0.js"]) {
        vm.runInContext(fs.readFileSync(path.join(controllerDirectory, name), "utf8"), context, {filename: name, timeout: 5000});
    }
    vm.runInContext(fs.readFileSync(mappingPath, "utf8"), context, {filename: mappingPath, timeout: 5000});
    if (initialize !== false) {
        context.NS4FX.init("mock-NS4FX");
        for (let number = 1; number <= 4; number++) {
            assert.ok(context.NS4FX.decks[number] instanceof context.components.ComponentContainer);
        }
        // Execute the mapping's deferred startup LED updates, not cut timers.
        let startupCallbacks = 0;
        while (timers.size > 0) {
            const entry = timers.entries().next().value;
            assert.ok(entry[1].oneShot, "startup must not create recurring cut timers");
            assert.ok(++startupCallbacks <= 256, "startup callbacks must settle");
            timers.delete(entry[0]);
            entry[1].callback();
        }
        assert.equal(timers.size, 0, "startup callbacks must settle before pad input");
        assert.ok(connections.length > 0, "whole mapping initialized real components");
        assert.ok(messages.length > 0, "whole mapping initialization emitted mocked MIDI");
        initializationMetrics.push({decks: 4, startupCallbacks, connections: connections.length, midiMessages: messages.length, useStems: Boolean(useStems)});
    }
    writes.length = 0;
    events.length = 0;
    return {
        context, timers, writes, events,
        deck: function(number) { return context.NS4FX.decks[number]; },
        volumeWrites: function() { return writes.filter(write => write.key === "volume"); },
        fire: function(id) {
            const timer = timers.get(id);
            assert.ok(timer, `timer ${id} must still be active`);
            if (timer.oneShot) {
                timers.delete(id);
            }
            timer.callback();
        },
        input: function(number, pad, value) {
            const button = context.NS4FX.decks[number].hotcues[pad];
            assert.ok(button instanceof context.components.Button);
            button.input(number - 1, button.midi[1], value, button.midi[0], `[Channel${number}]`);
            return button;
        },
    };
};

const test = function(name, callback) {
    try {
        callback();
        results.push({name, result: "passed"});
    } catch (error) {
        results.push({name, result: "failed", error: error.stack});
    }
};

test("duplicate same-pad press preserves timer identity and toggle state", function() {
    const h = loadMapping();
    h.deck(1).change_padmode("fadercuts");
    const button = h.input(1, 1, 0x7F);
    const id = button.faderCutInterval;
    h.fire(id);
    h.input(1, 1, 0x7F);
    assert.equal(button.faderCutInterval, id);
    assert.equal(h.timers.size, 1);
    h.fire(id);
    assert.deepEqual(h.volumeWrites().map(write => write.value), [1, 0]);
    h.input(1, 1, 0);
    assert.equal(h.timers.size, 0);
    assert.equal(button.faderCutInterval, null);
    assert.equal(h.volumeWrites().pop().value, 1);
});

test("independent held pads preserve intervals and release ownership", function() {
    const h = loadMapping();
    h.deck(1).change_padmode("fadercuts");
    const first = h.input(1, 1, 0x7F).faderCutInterval;
    const second = h.input(1, 2, 0x7F).faderCutInterval;
    assert.notEqual(first, second);
    assert.equal(h.timers.get(first).interval, 31.25);
    assert.equal(h.timers.get(second).interval, 125 / 3);
    h.fire(first);
    h.fire(second);
    h.fire(second);
    assert.deepEqual(h.volumeWrites().map(write => write.value), [1, 1, 0]);
    h.input(1, 1, 0);
    assert.equal(h.timers.size, 1);
    assert.ok(h.timers.has(second));
    h.fire(second);
    h.input(1, 2, 0);
    assert.equal(h.timers.size, 0);
});

test("all four pad intervals survive duplicate starts independently", function() {
    const h = loadMapping();
    h.deck(1).change_padmode("fadercuts");
    const ids = [];
    for (let pad = 1; pad <= 4; pad++) {
        ids.push(h.input(1, pad, 0x7F).faderCutInterval);
    }
    assert.deepEqual(ids.map(id => h.timers.get(id).interval), [31.25, 125 / 3, 62.5, 125]);
    for (let pad = 1; pad <= 4; pad++) {
        assert.equal(h.input(1, pad, 0x7F).faderCutInterval, ids[pad - 1]);
    }
    assert.equal(h.timers.size, 4);
    for (let pad = 1; pad <= 4; pad++) {
        h.input(1, pad, 0);
    }
    assert.equal(h.timers.size, 0);
});

test("all four decks have independent timer identities and volume targets", function() {
    const h = loadMapping();
    const ids = [];
    for (let number = 1; number <= 4; number++) {
        h.deck(number).change_padmode("fadercuts");
        ids.push(h.input(number, 1, 0x7F).faderCutInterval);
        h.fire(ids[number - 1]);
    }
    assert.equal(new Set(ids).size, 4);
    assert.deepEqual(h.volumeWrites().map(write => write.group), ["[Channel1]", "[Channel2]", "[Channel3]", "[Channel4]"]);
    h.input(2, 1, 0);
    assert.equal(h.timers.size, 3);
    for (const number of [1, 3, 4]) {
        assert.ok(h.timers.has(ids[number - 1]));
        h.input(number, 1, 0);
    }
    assert.equal(h.timers.size, 0);
});

test("mode exit stops active cuts before real component disconnection and rebinding", function() {
    const h = loadMapping();
    const deck = h.deck(1);
    deck.change_padmode("fadercuts");
    h.input(1, 1, 0x7F);
    h.input(1, 2, 0x7F);
    const countsAtDisconnect = [];
    deck.hotcues.forEachComponent(function(button) {
        const disconnect = button.disconnect;
        button.disconnect = function() {
            countsAtDisconnect.push(h.timers.size);
            h.events.push({type: "disconnect"});
            disconnect.call(this);
        };
    });
    deck.change_padmode("hotcue");
    assert.equal(h.timers.size, 0);
    assert.ok(countsAtDisconnect.length > 0);
    assert.ok(countsAtDisconnect.every(count => count === 0));
    assert.equal(deck.hotcues, deck.hotcue_buttons);
    assert.equal(h.events.filter(event => event.type === "stop").length, 2);
    assert.ok(h.events.findIndex(event => event.type === "stop") < h.events.findIndex(event => event.type === "disconnect"));
});

test("mode exit cleans only its deck and leaves another deck timer active", function() {
    const h = loadMapping();
    h.deck(1).change_padmode("fadercuts");
    h.deck(2).change_padmode("fadercuts");
    h.input(1, 1, 0x7F);
    const other = h.input(2, 1, 0x7F).faderCutInterval;
    h.deck(1).change_padmode("autoloop");
    assert.equal(h.timers.size, 1);
    assert.ok(h.timers.has(other));
    assert.deepEqual(h.volumeWrites().map(write => write.group), ["[Channel1]"]);
    h.fire(other);
    h.input(2, 1, 0);
    assert.equal(h.timers.size, 0);
});

test("same-mode rebinding preserves the held pad timer", function() {
    const h = loadMapping();
    h.deck(1).change_padmode("fadercuts");
    const id = h.input(1, 1, 0x7F).faderCutInterval;
    h.deck(1).change_padmode("fadercuts");
    assert.ok(h.timers.has(id));
    assert.equal(h.deck(1).hotcues[1].faderCutInterval, id);
    h.input(1, 1, 0);
    assert.equal(h.timers.size, 0);
});

test("inactive mode cleanup and repeated shutdown do not write deck volume", function() {
    const h = loadMapping();
    for (let number = 1; number <= 4; number++) {
        h.deck(number).change_padmode("fadercuts");
        h.deck(number).change_padmode("hotcue");
    }
    h.context.NS4FX.shutdown();
    h.context.NS4FX.shutdown();
    assert.equal(h.timers.size, 0);
    assert.equal(h.volumeWrites().length, 0);
});

test("shutdown stops all held pads and repeated cleanup is idempotent", function() {
    const h = loadMapping();
    for (let number = 1; number <= 4; number++) {
        h.deck(number).change_padmode("fadercuts");
        h.input(number, 1, 0x7F);
        h.input(number, 2, 0x7F);
    }
    assert.equal(h.timers.size, 8);
    h.context.NS4FX.shutdown();
    assert.equal(h.timers.size, 0);
    assert.equal(h.events.filter(event => event.type === "stop").length, 8);
    assert.equal(h.volumeWrites().length, 8);
    h.context.NS4FX.shutdown();
    assert.equal(h.volumeWrites().length, 8);
    assert.equal(h.events.filter(event => event.type === "stop").length, 8);
});

test("stem mode initializes empty cut containers and cleans without volume writes", function() {
    const h = loadMapping(true);
    for (let number = 1; number <= 4; number++) {
        const deck = h.deck(number);
        let cutButtons = 0;
        deck.fadercuts_buttons.forEachComponent(function() { cutButtons++; });
        assert.equal(cutButtons, 0);
        deck.change_padmode("stems");
        deck.change_padmode("hotcue");
    }
    h.context.NS4FX.shutdown();
    h.context.NS4FX.shutdown();
    assert.equal(h.timers.size, 0);
    assert.equal(h.volumeWrites().length, 0);
});

test("shutdown tolerates an uninitialized mapping", function() {
    const h = loadMapping(false, false);
    h.context.NS4FX.shutdown();
    assert.equal(h.timers.size, 0);
    assert.equal(h.volumeWrites().length, 0);
});

const failed = results.filter(result => result.result === "failed").length;
console.log(JSON.stringify({mapping: mappingPath, passed: results.length - failed, failed, initializationMetrics, results}, null, 2));
process.exitCode = failed ? 1 : 0;
// End AI-generated whole-mapping timer lifecycle regression tests.
