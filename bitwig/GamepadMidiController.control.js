// Bitwig Studio controller script for GamepadMidiController's DAW mode.
//
// The app sends one CC per action on MIDI channel 16 (value 127 = press).
// This script turns those into Bitwig transport / navigation actions.
//
// Install: copy this file into
//   Documents/Bitwig Studio/Controller Scripts/GamepadMidiController/
// then Settings > Controllers > Add controller > "GamepadMidiController" >
// "Steam Controller DAW", and pick the MIDI input the app sends DAW commands
// on ("loopMIDI DAW" if you created that port, otherwise your main port).
//
// "Record take" (left trackpad click by default) records back-to-back audio
// takes on ONE track, always with Arranger Overdub on (recording over a clip
// adds a comp lane instead of cutting it). Three modes (preferences >
// "Take mode"):
//
// Punch in at the cursor (default): playback starts PRE-ROLL beats early as a
// lead-in, recording punches in an EARLY MARGIN before the cursor and runs
// until you stop. Anything before the punch-in point isn't recorded (Bitwig
// discards audio outside a punch range).
//
// Record everything: recording starts with the pre-roll, so nothing is lost,
// but the pre-roll covers the previous take until you swipe the comp boundary.
//
// Punch in/out at markers: like punch-in, plus a cue marker at each take's
// start and end (nearest bar); a take punches out at the next marker, so it
// can't run into material after it even if you stop late. The only mode that
// adds markers.
//
// Bitwig's punch points are the arranger loop brace, so the punch modes move
// the loop brace for the take and restore your loop / punch settings after.
//
// In all modes the cursor returns to where the take started after it stops
// (or the next bar after the stop point; see the preferences), so pressing
// Record take again redoes the same section.
//
// Keep the action numbers in sync with mappings::DawAction in mappings.h.

loadAPI(18);
host.setShouldFailOnDeprecatedUse(false);

host.defineController("GamepadMidiController", "Steam Controller DAW", "1.1",
	"82320C24-030E-476C-BBA1-1B4DA7F679C6", "GamepadMidiController");
host.defineMidiPorts(1, 0);

var DAW_CHANNEL = 15; // 0-indexed: MIDI channel 16

var Action = {
	PLAY_STOP: 1,
	STOP: 2,
	RECORD: 3,
	RECORD_TAKE: 4,
	UNDO: 5,
	JUMP_BACK: 6,
	JUMP_FORWARD: 7,
	JUMP_BACK_BIG: 8,
	JUMP_FORWARD_BIG: 9,
	GOTO_START: 10,
	PREV_MARKER: 11,
	NEXT_MARKER: 12,
	ADD_MARKER: 13,
	TOGGLE_LOOP: 14,
	TOGGLE_METRONOME: 15,
	SELECT_LEFT: 16,
	SELECT_RIGHT: 17,
	SELECT_AT_CURSOR: 18,
	TRACK_PREV: 19,
	TRACK_NEXT: 20,
};

var MARKER_BANK_SIZE = 256;
var EPS = 1e-3; // beats: "same position" tolerance

var transport, application, markers;
var preRollBeats = 2;
var earlyMarginMs = 80;
var bigJumpBars = 4;
var stopBeforeJump = true;
var afterTake = "Take start";
// End marker of the last take, created only when the NEXT take starts, so it
// never sits on top of the recording in Bitwig's undo history (Ctrl+Z right
// after a take should undo the take). { pos, anchor } or null.
var pendingEnd = null;
var markerQueue = [];  // positions waiting to get a marker (one at a time)
var markerBusy = false;
var overdubTakes = true;
var TAKE_MODE_ALL = "Record everything (swipe comp boundaries later)";
var TAKE_MODE_PUNCH_IN = "Punch in at the cursor";
var TAKE_MODE_MARKERS = "Punch in/out at markers (adds cue markers)";
var takeMode = TAKE_MODE_PUNCH_IN;
var lastPlayPos = 0; // last play position seen while playing (survives the stop)

// Non-null while a take is running: what to restore when it stops.
var take = null;
var wasPlaying = false;

function init()
{
	transport = host.createTransport();
	application = host.createApplication();

	transport.isPlaying().markInterested();
	transport.isArrangerRecordEnabled().markInterested();
	transport.isArrangerLoopEnabled().markInterested();
	transport.isPunchInEnabled().markInterested();
	transport.isPunchOutEnabled().markInterested();
	transport.isArrangerOverdubEnabled().markInterested();
	transport.arrangerLoopStart().markInterested();
	transport.arrangerLoopDuration().markInterested();
	transport.playPosition().markInterested();
	transport.playStartPosition().markInterested();
	transport.tempo().value().markInterested();
	transport.timeSignature().numerator().markInterested();
	transport.timeSignature().denominator().markInterested();

	markers = host.createArranger().createCueMarkerBank(MARKER_BANK_SIZE);
	for (var i = 0; i < MARKER_BANK_SIZE; i++)
	{
		var m = markers.getItemAt(i);
		m.exists().markInterested();
		m.position().markInterested();
	}

	var prefs = host.getPreferences();
	prefs.getEnumSetting("Take mode", "Record take",
		[TAKE_MODE_PUNCH_IN, TAKE_MODE_ALL, TAKE_MODE_MARKERS], TAKE_MODE_PUNCH_IN)
		.addValueObserver(function (v) { takeMode = v; });
	prefs.getBooleanSetting("Overdub: add takes as comp lanes (nothing overwritten)", "Record take", true)
		.addValueObserver(function (v) { overdubTakes = v; });

	transport.playStartPosition().addValueObserver(onPlayStartChanged);

	transport.playPosition().addValueObserver(function (pos)
	{
		if (transport.isPlaying().get()) lastPlayPos = pos;
	});
	prefs.getNumberSetting("Pre-roll (beats)", "Record take", 0, 16, 1, "beats", 2)
		.addRawValueObserver(function (v) { preRollBeats = v; });
	prefs.getNumberSetting("Early margin (ms)", "Record take", 0, 1000, 10, "ms", 80)
		.addRawValueObserver(function (v) { earlyMarginMs = v; });
	prefs.getEnumSetting("Cursor after a take", "Record take",
		["Take start", "Next bar after stop"], "Take start")
		.addValueObserver(function (v) { afterTake = v; });
	prefs.getNumberSetting("Big jump (bars)", "Navigation", 1, 64, 1, "bars", 4)
		.addRawValueObserver(function (v) { bigJumpBars = v; });
	prefs.getBooleanSetting("Stop playback before jumping", "Navigation", true)
		.addValueObserver(function (v) { stopBeforeJump = v; });

	transport.isPlaying().addValueObserver(function (playing)
	{
		var stopped = wasPlaying && !playing;
		wasPlaying = playing;
		if (stopped)
		{
			try { onStopped(); }
			catch (e) { debugLog("onStopped failed: " + e); take = null; }
		}
	});

	host.getMidiInPort(0).setMidiCallback(onMidi);
	host.getMidiInPort(0).setSysexCallback(onSysex);
	println("GamepadMidiController DAW script ready");

	// Follows (and drives) Bitwig's track selection.
	cursorTrack = host.createCursorTrack("GMC_TRACK", "Selected track", 0, 0, true);
	try
	{
		cursorTrack.setCursorNavigationMode(
			Java.type("com.bitwig.extension.controller.api.CursorNavigationMode").GUI);
	}
	catch (e) {} // default navigation is fine too
	cursorTrack.name().markInterested();

	cursorClip = host.createArrangerCursorClip(1, 1);
	cursorClip.exists().markInterested();
	cursorClip.getPlayStart().markInterested();
	cursorClip.getPlayStop().markInterested();
	cursorClip.getLoopStart().markInterested();
	cursorClip.getLoopLength().markInterested();
	cursorClip.getTrack().name().markInterested();

	// Uncomment to (re)write Bitwig's full action list (id, name, category) to
	// Documents/Bitwig Studio/GamepadMidiController-actions.txt, e.g. after a
	// Bitwig update, to find actions worth binding.
	// host.scheduleTask(dumpActions, 2000);
}

// --- Debug hook --------------------------------------------------------------
// For poking at Bitwig from outside (testing, finding out how an action or
// value behaves) without a controller. Send a SysEx message to the script's
// MIDI input (e.g. "loopMIDI DAW"):
//
//   F0 7D <command as ASCII bytes> F7        (7D = non-commercial manufacturer ID)
//
// Commands:
//   action:<id>        invoke a Bitwig action by id; ids are listed in
//                      GamepadMidiController-actions.txt (see dumpActions)
//   clipinfo           log the arranger cursor clip (track, length, loop) and
//                      the transport (play position, play start, formatted
//                      position, time signature at the cursor)
//   state              log the script's own state (take, queue, guard, ...)
//   setpos:<beats>     move the cursor to a beat position (e.g. setpos:184)
//   probe:<from>:<to>  move to <from>, then <to>, and log the time signature
//                      every 10 ms for 200 ms (how fast it follows the cursor)
//   arrow:left|right   simulate the arrow keys (application.arrowKeyLeft/Right)
//
// Output is appended to Documents/Bitwig Studio/GamepadMidiController-debug.txt.
// Never write files into this script's own folder: Bitwig reloads the script
// whenever anything in that folder changes, which wipes all state (a take in
// progress, the action queue, ...).
//
// Example from Python (python-rtmidi):
//   o.send_message([0xF0, 0x7D] + [ord(c) for c in "setpos:184"] + [0xF7])
var cursorClip = null;
var cursorTrack = null;

function onSysex(hex)
{
	hex = hex.toLowerCase();
	if (hex.indexOf("f07d") !== 0) return;
	var body = hex.substring(4, hex.length - 2);
	var s = "";
	for (var i = 0; i + 1 < body.length; i += 2) s += String.fromCharCode(parseInt(body.substr(i, 2), 16));

	if (s.indexOf("action:") === 0)
	{
		var id = s.substring(7);
		var a = application.getAction(id);
		if (a) { a.invoke(); debugLog("invoked " + id); }
		else debugLog("no such action: " + id);
	}
	else if (s.indexOf("probe:") === 0)
	{
		// probe:<from>:<to> - set cursor to <from>, then to <to>, log the
		// signature every 10 ms for 200 ms to see how fast it follows.
		var parts = s.split(":");
		setCursor(parseFloat(parts[1]));
		host.scheduleTask(function ()
		{
			var t0 = Date.now();
			setCursor(parseFloat(parts[2]));
			var log = [];
			function sample()
			{
				log.push((Date.now() - t0) + "ms " + transport.timeSignature().numerator().get() + "/" + transport.timeSignature().denominator().get()
					+ " pos=" + transport.playStartPosition().get());
				if (Date.now() - t0 < 200) host.scheduleTask(sample, 10); else debugLog("probe " + log.join(", "));
			}
			sample();
		}, 300);
	}
	else if (s === "state")
	{
		debugLog("state: isPlaying=" + transport.isPlaying().get() + " wasPlaying=" + wasPlaying
			+ " take=" + (take === null ? "null" : JSON.stringify(take))
			+ " stopGuard=" + JSON.stringify(stopGuard) + " jumpBusy=" + jumpBusy
			+ " queue=" + JSON.stringify(actionQueue) + " lastPlayPos=" + lastPlayPos);
	}
	else if (s.indexOf("setpos:") === 0) { setCursor(parseFloat(s.substring(7))); debugLog("setpos " + s.substring(7)); }
	else if (s === "arrow:right") { application.arrowKeyRight(); debugLog("arrowKeyRight"); }
	else if (s === "arrow:left") { application.arrowKeyLeft(); debugLog("arrowKeyLeft"); }
	else if (s === "clipinfo")
	{
		debugLog("cursor clip exists=" + cursorClip.exists().get()
			+ " track=" + cursorClip.getTrack().name().get()
			+ " playStart=" + cursorClip.getPlayStart().get()
			+ " playStop=" + cursorClip.getPlayStop().get()
			+ " loopStart=" + cursorClip.getLoopStart().get()
			+ " loopLength=" + cursorClip.getLoopLength().get()
			+ " | transport playPos=" + transport.playPosition().get()
			+ " playStart=" + transport.playStartPosition().get()
			+ " formatted=" + transport.playStartPosition().getFormatted()
			+ " sig=" + transport.timeSignature().numerator().get() + "/" + transport.timeSignature().denominator().get());
	}
}

// Where debug output goes: Documents/Bitwig Studio. NOT the script's folder:
// Bitwig reloads a controller script whenever a file in its folder changes,
// which wipes the script's state (e.g. a take in progress).
function outputDir()
{
	var File = Java.type("java.io.File");
	return new File(Java.type("java.lang.System").getProperty("user.home"), "Documents/Bitwig Studio");
}

function debugLog(text)
{
	try
	{
		var File = Java.type("java.io.File");
		var Writer = Java.type("java.io.FileWriter");
		var w = new Writer(new File(outputDir(), "GamepadMidiController-debug.txt"), true);
		w.write(new Date().toISOString() + " " + text + "\n");
		w.close();
	}
	catch (e) { println(text); }
}

function dumpActions()
{
	var actions = application.getActions();
	var lines = [];
	for (var i = 0; i < actions.length; i++)
	{
		var a = actions[i];
		var cat = a.getCategory();
		lines.push(a.getId() + "\t" + a.getName() + "\t" + (cat ? cat.getName() : ""));
	}
	var text = lines.join("\n");
	try
	{
		var File = Java.type("java.io.File");
		var Writer = Java.type("java.io.FileWriter");
		var w = new Writer(new File(outputDir(), "GamepadMidiController-actions.txt"));
		w.write(text);
		w.close();
		notify("GamepadMidiController: wrote " + actions.length + " actions to bitwig-actions.txt");
	}
	catch (e)
	{
		println("Could not write action list (" + e + "), printing instead:");
		println(text);
		notify("GamepadMidiController: " + actions.length + " actions printed to the script console");
	}
}

function exit()
{
	if (take !== null) restoreAfterTake();
}

function onMidi(status, data1, data2)
{
	if ((status & 0xF0) !== 0xB0) return;
	if ((status & 0x0F) !== DAW_CHANNEL) return;
	if (data2 === 0) return; // release
	runAction(data1);
}

// Every controller action goes through one FIFO queue: while a bar jump is
// still landing, later presses wait, so "RB, RB, Play" plays only after both
// jumps. Consecutive jump presses merge into one jump (four quick RB presses
// = 16 bars).
var actionQueue = [];

function runAction(a)
{
	if (jumpBusy && Date.now() - jumpStartedAt > 5000) jumpBusy = false; // watchdog (refreshed while walking)
	actionQueue.push(a);
	processQueue();
}

function jumpBarsFor(a)
{
	switch (a)
	{
	case Action.JUMP_BACK:        return -1;
	case Action.JUMP_FORWARD:     return 1;
	case Action.JUMP_BACK_BIG:    return -bigJumpBars;
	case Action.JUMP_FORWARD_BIG: return bigJumpBars;
	default:                      return 0;
	}
}

function processQueue()
{
	while (!jumpBusy && actionQueue.length > 0)
	{
		var a = actionQueue.shift();
		var n = jumpBarsFor(a);
		if (n === 0) { doAction(a); continue; }
		while (actionQueue.length > 0 && jumpBarsFor(actionQueue[0]) !== 0) n += jumpBarsFor(actionQueue.shift());
		if (n !== 0) startJump(n); // sets jumpBusy; finishJump() resumes the queue
	}
}

function doAction(a)
{
	switch (a)
	{
	case Action.PLAY_STOP:        transport.togglePlay(); break;
	case Action.STOP:             transport.stop(); break;
	case Action.RECORD:           transport.record(); break;
	case Action.RECORD_TAKE:      recordTake(); break;
	case Action.UNDO:             pendingEnd = null; application.undo(); notify("Undo"); break;
	case Action.GOTO_START:       setCursor(0); break;
	case Action.PREV_MARKER:      transport.jumpToPreviousCueMarker(); break;
	case Action.NEXT_MARKER:      transport.jumpToNextCueMarker(); break;
	case Action.ADD_MARKER:       transport.addCueMarkerAtPlaybackPosition(); break;
	case Action.TOGGLE_LOOP:      transport.isArrangerLoopEnabled().toggle(); break;
	case Action.TOGGLE_METRONOME: transport.isMetronomeEnabled().toggle(); break;
	// Bitwig's "Select item to left/right" actions do nothing when invoked from
	// a script, but simulated arrow keys do the same thing.
	case Action.SELECT_LEFT:      application.arrowKeyLeft(); break;
	case Action.SELECT_RIGHT:     application.arrowKeyRight(); break;
	case Action.SELECT_AT_CURSOR: selectClipAtCursor(); break;
	case Action.TRACK_PREV:       selectTrack(-1); break;
	case Action.TRACK_NEXT:       selectTrack(1); break;
	}
}

// Select the clip under the play cursor, on the track of the currently
// selected clip. The API can't look clips up by position, but:
//   * arrow keys move the selection to the neighbouring clip,
//   * selecting a clip moves the play start to that clip's start (Bitwig
//     preference "Selecting clips / time on Arranger", on by default),
//   * the cursor clip reports the selected clip's length.
// So step left/right until the selected clip contains the cursor, then put
// the cursor back where it was.
var SELECT_STEP_MS = 120;
var SELECT_MAX_STEPS = 64;

function selectClipAtCursor()
{
	if (!cursorClip.exists().get()) { notify("Click a clip on the track first"); return; }
	var target = cursor().get();
	var steps = 0;
	var lastDir = 0;      // -1 left, +1 right
	var lastStart = null; // clip start after the previous step

	function finish(found)
	{
		setCursor(target);
		notify(found ? "Selected the clip at the cursor" : "No clip at the cursor (selected the nearest)");
	}

	function step(dir)
	{
		lastDir = dir;
		if (dir < 0) application.arrowKeyLeft(); else application.arrowKeyRight();
		host.scheduleTask(check, SELECT_STEP_MS);
	}

	function check()
	{
		var start = transport.playStartPosition().get();
		var len = cursorClip.getPlayStop().get();
		var moved = lastStart === null || Math.abs(start - lastStart) > EPS;
		lastStart = start;
		if (steps++ > SELECT_MAX_STEPS) { finish(false); return; }

		if (target >= start - EPS && target < start + len - EPS) { finish(true); return; }
		var dir = target < start ? -1 : 1;
		// Reversing direction means the cursor is in a gap between two clips;
		// not moving means we hit the first/last clip on the track.
		if ((lastDir !== 0 && dir !== lastDir) || !moved) { finish(false); return; }
		step(dir);
	}

	// The play start currently is the cursor, not the selected clip's start.
	// Step once to make Bitwig report a clip start, then search from there.
	// (Left first; at the first clip that doesn't move, so try right instead.)
	// If neither arrow key changes anything, nothing is selected (or the
	// arranger doesn't have keyboard focus), so there's nothing to step from.
	var before = transport.playStartPosition().get();
	function moved() { return Math.abs(transport.playStartPosition().get() - before) > EPS; }
	application.arrowKeyLeft();
	host.scheduleTask(function ()
	{
		if (moved()) { check(); return; }
		application.arrowKeyRight();
		host.scheduleTask(function ()
		{
			if (!moved()) { notify("Click a clip on this track in the arranger first"); return; }
			check();
		}, SELECT_STEP_MS);
	}, SELECT_STEP_MS);
}

// Select the track above (-1) / below (+1), in the order the arranger shows
// them. Bitwig's own record-arm-follows-selection then arms it.
function selectTrack(dir)
{
	var before = cursorTrack.name().get();
	if (dir < 0) cursorTrack.selectPrevious(); else cursorTrack.selectNext();
	waitUntil(function () { return cursorTrack.name().get() !== before; }, 300, function ()
	{
		cursorTrack.makeVisibleInArranger();
		notify("Track: " + cursorTrack.name().get());
	});
}

// Runs one of Bitwig's built-in actions by id (see bitwig-actions.txt).
function invokeAction(id, label)
{
	var a = application.getAction(id);
	if (a) { a.invoke(); notify(label); }
	else notify("Bitwig has no action " + id);
}

function notify(text)
{
	host.showPopupNotification(text);
}

function beatsPerBar()
{
	var num = transport.timeSignature().numerator().get();
	var den = transport.timeSignature().denominator().get();
	if (!num || !den) return 4;
	return num * 4 / den;
}

function msToBeats(ms)
{
	var bpm = transport.tempo().value().getRaw();
	if (!bpm || bpm <= 0) bpm = 120;
	return ms / 1000 * bpm / 60;
}

// The cursor is the play position while playing, the play start marker
// while stopped.
function cursor()
{
	return transport.isPlaying().get() ? transport.playPosition() : transport.playStartPosition();
}

function setCursor(beats)
{
	beats = Math.max(0, beats);
	transport.playStartPosition().set(beats);
	if (transport.isPlaying().get()) transport.playPosition().set(beats);
}

// Bar lines are multiples of beatsPerBar() offset by the position of a real
// bar line learnt from Bitwig (an odd-length bar earlier in the song shifts
// every later bar line). Until a jump has learnt one, assume bar lines at 0.
var knownBarLine = 0;

function barIndex(pos)
{
	return (pos - knownBarLine) / beatsPerBar();
}

function barLineAt(index)
{
	return Math.max(0, knownBarLine + index * beatsPerBar());
}

// Bar jumps that respect time-signature changes (e.g. a single 2/4 bar).
//
// What Bitwig gets right and wrong (tested around a 2/4 bar):
//   * "Jump to beginning of current bar" and "... next bar" are correct.
//   * "... previous bar" always steps back a full bar of the *current* bar's
//     length, so stepping back over a 2/4 bar lands mid-bar.
//   * Its 8-bar jumps and the formatted position text ignore signature
//     changes too.
// The transport's time signature does follow the cursor, though (about 50-70
// ms after a move), so a bar's real length can be read by putting the cursor
// in it. Jumps therefore walk bar by bar with each bar's real length.
//
// Bitwig also applies a jump asynchronously and evaluates a second action
// against the old position, so actions are never chained back-to-back.
// Set while a bar jump is landing; the action queue waits for it.
var jumpBusy = false;
var jumpStartedAt = 0;

function startJump(n)
{
	jumpBusy = true;
	jumpStartedAt = Date.now();
	if (stopBeforeJump && transport.isPlaying().get())
	{
		// Stop first (this also ends a running take), then jump from where
		// the playhead was, not from wherever Bitwig puts the cursor on stop.
		var from = transport.playPosition().get();
		transport.stop();
		waitUntil(function () { return !transport.isPlaying().get(); }, 1000, function ()
		{
			host.scheduleTask(function ()
			{
				cancelCursorRestore(); // the take's restore (started on stop) must not fight this
				setCursor(from);
				waitUntil(function () { return Math.abs(cursor().get() - from) < EPS; }, 300,
					function () { beginJump(n); });
			}, 150); // after Bitwig's / the take's own cursor reset on stop
		});
		return;
	}
	beginJump(n);
}

function beginJump(n)
{
	cancelCursorRestore();
	// Starting from a bar line the previous jump landed on: walk from it
	// directly. Bitwig's own jump actions would be evaluated against its stale
	// pre-jump position right after a jump and lose a bar.
	var pos = cursor().get();
	if (Math.abs(pos - knownBarLine) < EPS) { walkBars(pos, n > 0 ? 1 : -1, Math.abs(n)); return; }
	if (n === 1) jumpOneBar(1); else jumpBars(n);
}

// Polls cond() every 10 ms; calls fn() once it's true or after timeoutMs.
function waitUntil(cond, timeoutMs, fn)
{
	var waited = 0;
	function poll()
	{
		if (cond() || waited >= timeoutMs) { fn(); return; }
		waited += 10;
		host.scheduleTask(poll, 10);
	}
	poll();
}

// Every jump ends here: move to `target`, wait until the cursor is really
// there (whatever runs next reads it), then resume the action queue.
function finishJump(target)
{
	target = Math.max(0, target);
	setCursor(target);
	knownBarLine = target;
	var waited = 0;
	function poll()
	{
		if (Math.abs(cursor().get() - target) < EPS || waited >= 300)
		{
			jumpBusy = false;
			if (actionQueue.length === 0) host.scheduleTask(reportCursor, 100);
			processQueue();
			return;
		}
		waited += 10;
		host.scheduleTask(poll, 10);
	}
	host.scheduleTask(poll, 10);
}

function jumpOneBar(dir)
{
	var next = application.getAction("jump_to_beginning_of_next_bar");
	if (dir > 0 && next)
	{
		var before = cursor().get();
		next.invoke();
		afterCursorSettles(before, 1000, function (pos) { finishJump(pos); });
		return;
	}
	jumpBars(dir);
}

function jumpBars(n)
{
	var next = application.getAction("jump_to_beginning_of_next_bar");
	if (!next) { jumpBarsByMath(n); return; }

	// Bitwig's "next bar" is correct and always moves the cursor, so waiting
	// for it is unambiguous (unlike "current bar", which doesn't move when
	// already on a bar line, so a slow Bitwig looked like "no move").
	var before = cursor().get();
	next.invoke();
	afterCursorSettles(before, 1000, function (nextBar)
	{
		if (n > 0) { walkBars(nextBar, 1, n - 1); return; }
		// Start of the bar the cursor was in = next bar line minus that bar's
		// real length. Landing there from mid-bar counts as one step.
		barLengthAt(Math.max(0, nextBar - 0.001), function (len)
		{
			var barStart = Math.max(0, nextBar - len);
			walkBars(barStart, -1, -n - (barStart < before - EPS ? 1 : 0));
		});
	});
}

// From bar line `start`, step `steps` bars in direction `dir`, reading each
// bar's length from the time signature at a position inside that bar.
function walkBars(start, dir, steps)
{
	if (steps <= 0 || (dir < 0 && start <= EPS)) { finishJump(start); return; }
	// Forward: the bar starting at `start`. Backward: the bar ending there.
	var inside = dir > 0 ? start + 0.001 : Math.max(0, start - 0.001);
	barLengthAt(inside, function (len)
	{
		walkBars(start + dir * len, dir, steps - 1);
	});
}

// Moves the cursor to `pos`, waits for the transport's time signature to
// follow, and calls fn(bar length in beats). The signature arrives some time
// after the position (~25 ms when idle, longer when Bitwig is busy), so it
// waits for two equal reads 30 ms apart, starting 60 ms after the position.
function barLengthAt(pos, fn)
{
	jumpStartedAt = Date.now(); // still working: keep the queue watchdog quiet
	setCursor(pos);
	waitUntil(function () { return Math.abs(transport.playStartPosition().get() - pos) < EPS; }, 400, function ()
	{
		var last = null;
		var reads = 0;
		function read()
		{
			var len = beatsPerBar();
			reads++;
			if ((last !== null && Math.abs(len - last) < EPS) || reads >= 8) { fn(len); return; }
			last = len;
			host.scheduleTask(read, 30);
		}
		host.scheduleTask(read, 60);
	});
}

// Calls fn(position) once the cursor has moved away from `before` and stayed
// put for a moment, or after `timeoutMs` if it doesn't move at all.
function afterCursorSettles(before, timeoutMs, fn)
{
	var waited = 0;
	var last = before;
	var stable = 0;
	function poll()
	{
		var pos = cursor().get();
		stable = Math.abs(pos - last) < EPS ? stable + 1 : 0;
		last = pos;
		var moved = Math.abs(pos - before) > EPS;
		if ((moved && stable >= 2) || waited >= timeoutMs) { fn(pos); return; }
		waited += 20;
		host.scheduleTask(poll, 20);
	}
	host.scheduleTask(poll, 20);
}

function reportCursor()
{
	notify("Bar " + cursor().getFormatted());
}

// Fallback if Bitwig ever renames those actions: assumes one time signature.
function jumpBarsByMath(n)
{
	var bpb = beatsPerBar();
	var pos = cursor().get();
	var bar = n > 0 ? Math.floor(barIndex(pos) + EPS) : Math.ceil(barIndex(pos) - EPS);
	finishJump(barLineAt(bar + n));
}

function recordTake()
{
	cancelCursorRestore();
	if (take !== null)
	{
		transport.stop(); // onStopped() restores everything
		return;
	}

	var bpb = beatsPerBar();
	var margin = msToBeats(earlyMarginMs);
	var playing = transport.isPlaying().get();
	var punching = takeMode !== TAKE_MODE_ALL;
	var useMarkers = takeMode === TAKE_MODE_MARKERS;

	// Where the take "starts" musically. Stopped: the cursor. Already playing:
	// punch mode waits for the next bar line still far enough ahead; record-
	// everything mode starts right now.
	var anchor;
	if (!playing)
		anchor = transport.playStartPosition().get();
	else if (punching)
		anchor = barLineAt(Math.ceil(barIndex(transport.playPosition().get() + margin + 0.25)));
	else
		anchor = transport.playPosition().get();

	var takeStartLabel = playing ? "the next bar" : cursor().getFormatted();
	var playFrom = Math.max(0, anchor - preRollBeats);
	// Where recording actually begins.
	var punchAt = punching ? Math.max(0, anchor - margin) : (playing ? anchor : playFrom);

	// Create the previous take's end marker now, unless this take redoes the
	// same section (then that take was most likely undone or is being replaced).
	var flushed = [];
	if (pendingEnd !== null)
	{
		if (Math.abs(pendingEnd.anchor - anchor) > EPS && !markerAt(pendingEnd.pos))
		{
			addMarkerAt(pendingEnd.pos);
			flushed.push(pendingEnd.pos);
		}
		pendingEnd = null;
	}

	// Punch mode: punch out where the next take (marker) begins. Without
	// overdub, stop the same early margin before it so its first notes aren't
	// overwritten; with overdub nothing is overwritten (it becomes a comp
	// lane), so stop exactly on the marker.
	var nextMarker = useMarkers ? markerAfter(anchor, flushed) : null;
	var punchOutAt = nextMarker !== null ? nextMarker - (overdubTakes ? 0 : margin) : null;
	if (punchOutAt !== null && punchOutAt <= punchAt + EPS) punchOutAt = null;

	take = {
		punching: punching,
		useMarkers: useMarkers,
		anchor: anchor,
		punchAt: punchAt,
		punchOutAt: punchOutAt,
		// Where Bitwig will reset the cursor to on stop: where playback started.
		playFrom: playing ? transport.playStartPosition().get() : Math.min(playFrom, punchAt),
		loopStart: transport.arrangerLoopStart().get(),
		loopDuration: transport.arrangerLoopDuration().get(),
		loopEnabled: transport.isArrangerLoopEnabled().get(),
		punchIn: transport.isPunchInEnabled().get(),
		punchOut: transport.isPunchOutEnabled().get(),
		overdub: transport.isArrangerOverdubEnabled().get(),
	};
	if (overdubTakes) setBool(transport.isArrangerOverdubEnabled(), true);

	setBool(transport.isArrangerLoopEnabled(), false);
	if (punching)
	{
		// Punch in at the loop start, punch out at the loop end (if any).
		setBeats(transport.arrangerLoopStart(), punchAt);
		if (punchOutAt !== null) setBeats(transport.arrangerLoopDuration(), punchOutAt - punchAt);
		setBool(transport.isPunchOutEnabled(), punchOutAt !== null);
		setBool(transport.isPunchInEnabled(), true);
	}
	else
	{
		// Record everything: no punch points at all.
		setBool(transport.isPunchInEnabled(), false);
		setBool(transport.isPunchOutEnabled(), false);
	}
	transport.isArrangerRecordEnabled().set(true);

	if (!playing)
	{
		if (useMarkers && !markerAt(anchor)) addMarkerAt(anchor);
		transport.playStartPosition().set(Math.min(playFrom, punchAt));
		// Start playback once the loop / punch changes have landed and the
		// markers are in place, so all of that sits before the recording in
		// the undo history.
		whenMarkersIdle(function () { transport.play(); });
	}
	notify("Take at " + takeStartLabel
		+ (punchOutAt !== null ? ", ends at the next marker" : ""));
}

function markerPositions()
{
	var list = [];
	for (var i = 0; i < MARKER_BANK_SIZE; i++)
	{
		var m = markers.getItemAt(i);
		if (m.exists().get()) list.push(m.position().get());
	}
	return list;
}

// The API can only add a marker at the *playback position*, which while
// stopped is wherever playback last stopped, not the cursor. So add one, then
// find the marker that wasn't there before and move it to `pos`. Queued, so
// two additions never confuse each other's "new" marker.
function addMarkerAt(pos)
{
	markerQueue.push(pos);
	if (!markerBusy) nextMarkerJob();
}

function nextMarkerJob()
{
	if (markerQueue.length === 0) { markerBusy = false; return; }
	markerBusy = true;
	var pos = markerQueue.shift();
	if (markerAt(pos)) { nextMarkerJob(); return; }
	var before = markerPositions();
	transport.addCueMarkerAtPlaybackPosition();
	host.scheduleTask(function () { moveNewMarker(before, pos, 0); }, 100);
}

// Runs `fn` once no marker is being added (or after ~1.5 s at most).
function whenMarkersIdle(fn, waited)
{
	waited = waited || 0;
	if ((!markerBusy && markerQueue.length === 0) || waited >= 1500)
	{
		host.scheduleTask(fn, 50);
		return;
	}
	host.scheduleTask(function () { whenMarkersIdle(fn, waited + 50); }, 50);
}

function moveNewMarker(before, pos, attempt)
{
	var remaining = before.slice();
	for (var i = 0; i < MARKER_BANK_SIZE; i++)
	{
		var m = markers.getItemAt(i);
		if (!m.exists().get()) continue;
		var p = m.position().get();
		var k;
		for (k = 0; k < remaining.length; k++)
		{
			if (Math.abs(remaining[k] - p) < EPS) break;
		}
		if (k < remaining.length) { remaining.splice(k, 1); continue; } // old marker
		m.position().set(pos); // the new one
		host.scheduleTask(nextMarkerJob, 50);
		return;
	}
	// Not visible yet: try again a few times, then give up on this one.
	if (attempt < 10) host.scheduleTask(function () { moveNewMarker(before, pos, attempt + 1); }, 100);
	else nextMarkerJob();
}

// Position (beats) of the first cue marker after `pos`, or null. `extra` are
// marker positions that are being created right now.
function markerAfter(pos, extra)
{
	var best = null;
	var list = extra || [];
	for (var j = 0; j < list.length; j++)
	{
		if (list[j] > pos + EPS && (best === null || list[j] < best)) best = list[j];
	}
	for (var i = 0; i < MARKER_BANK_SIZE; i++)
	{
		var m = markers.getItemAt(i);
		if (!m.exists().get()) continue;
		var p = m.position().get();
		if (p > pos + EPS && (best === null || p < best)) best = p;
	}
	return best;
}

function markerAt(pos)
{
	for (var i = 0; i < MARKER_BANK_SIZE; i++)
	{
		var m = markers.getItemAt(i);
		if (m.exists().get() && Math.abs(m.position().get() - pos) < EPS) return true;
	}
	return false;
}

// Only touch what differs: every change made after a take lands on top of the
// recording in Bitwig's undo history, so Ctrl+Z would hit it first.
function setBool(value, wanted)
{
	if (value.get() !== wanted) value.set(wanted);
}

function setBeats(value, wanted)
{
	if (Math.abs(value.get() - wanted) > EPS) value.set(wanted);
}

function restoreAfterTake()
{
	setBool(transport.isArrangerRecordEnabled(), false);
	setBool(transport.isArrangerOverdubEnabled(), take.overdub);
	setBool(transport.isPunchInEnabled(), take.punchIn);
	setBool(transport.isPunchOutEnabled(), take.punchOut);
	setBeats(transport.arrangerLoopStart(), take.loopStart);
	setBeats(transport.arrangerLoopDuration(), take.loopDuration);
	setBool(transport.isArrangerLoopEnabled(), take.loopEnabled);
}

// After a take stops, Bitwig resets the cursor to the point playback started
// from (the pre-roll point), sometimes after our own move to the take start.
// So: move to the take start right away, then watch the play start position
// and undo exactly that reset (a change to the pre-roll point) whenever it
// comes. Other changes (a click elsewhere, jumps, new takes) are left alone,
// and anything that moves the cursor on purpose ends the watch.
var stopGuard = null; // { from: pre-roll point, target: take start } or null

function cancelCursorRestore()
{
	stopGuard = null;
}

function restoreCursorAfterStop(target, from)
{
	stopGuard = { from: from, target: target };
	setBeats(transport.playStartPosition(), target);
}

function onPlayStartChanged(pos)
{
	if (stopGuard === null) return;
	if (transport.isPlaying().get()) { stopGuard = null; return; }
	if (Math.abs(pos - stopGuard.from) < EPS) transport.playStartPosition().set(stopGuard.target);
}

function onStopped()
{
	if (take === null) return;

	var bpb = beatsPerBar();
	var stopPos = lastPlayPos;

	// Punch mode: remember where this take ends, so takes recorded in front of
	// it punch out there. The marker itself is created when the next take
	// starts (see pendingEnd). Skip it when punch-out already ended the take at
	// an existing marker, or when nothing got recorded.
	var cutByPunchOut = take.punchOutAt !== null && stopPos >= take.punchOutAt - EPS;
	if (take.useMarkers && !cutByPunchOut && stopPos > take.punchAt + EPS)
	{
		var endBar = barLineAt(Math.round(barIndex(stopPos)));
		if (endBar > take.anchor + EPS && !markerAt(endBar)) pendingEnd = { pos: endBar, anchor: take.anchor };
	}

	var target = take.anchor;
	if (afterTake === "Next bar after stop") target = barLineAt(Math.ceil(barIndex(stopPos) - EPS));
	var playFrom = take.playFrom;
	restoreAfterTake();
	take = null;
	restoreCursorAfterStop(target, playFrom);
}
