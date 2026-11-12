# melonDS Lua scripting API

This is a reference for people writing Lua scripts for this build of melonDS. Scripts are Lua 5.4 (the full language and standard libraries) plus a handful of global tables that let them watch and drive the emulator: emu, mem, rom, pad, touch, input and event.

## Security

Scripts run with your full user privileges. Every standard library is open, including io, os, debug and package, so a script can read, write and delete any file you can, start programs, and load native libraries. Only run scripts you trust, exactly as you would only run programs you trust.

## Loading scripts

There are three ways to start a script.

- The Lua console window of an instance lets you load, stop, restart and remove scripts.
- The command line option --lua path starts a script in the first instance. It can be given more than once. Scripts given this way start after any ROM named on the command line has been loaded.
- Each instance keeps a list of scripts to load automatically when it starts, stored in that instance's config under Lua.AutoloadScripts. These start before any ROM is loaded, so they should wait for one (see the rom_changed event, or poll rom.code() with emu.yield()).

Each script gets its own separate Lua state; scripts cannot see each other's globals. A script is loaded as a file chunk, so debug.getinfo(1, "S").source is "@" followed by the script's absolute path with forward slashes. Arguments, when the host passes any, arrive as the chunk's varargs (...). Scripts started with --lua or from the autoload list get no arguments.

The directory containing the script is put at the front of package.path (both ?.lua and ?/init.lua), so require finds pure Lua modules that sit beside the script or in subfolders of its directory.

print writes to the Lua console of the instance, with its arguments separated by tabs as in standard Lua. When melonDS is started from a terminal, console output, errors and notices are also written to standard output. io.write still goes to standard output only.

File names are UTF-8 everywhere, including on Windows: io.open, io.lines, require, dofile, loadfile, os.remove and os.rename all accept paths containing non-ASCII characters.

## How scripts are scheduled

All Lua code runs on the emulator thread, between frames. While a script's code is running the emulator is not, so everything a script reads in one go is consistent, but a script that runs for a long time without yielding stalls the emulator.

A script's main chunk runs as a coroutine. It keeps running until it calls emu.frameadvance() or emu.yield(), returns, or raises an error.

- emu.frameadvance() suspends the script until exactly one frame has been emulated, and resumes it just before the frame after that runs. The script resumes before input is applied to the console, so anything it sets with pad.set or touch.set takes effect on the coming frame. While the emulator is paused no frames run, so a script waiting in emu.frameadvance() waits too; if it was called while paused, it returns only after the first frame emulated once unpaused.
- emu.yield() suspends the script until the next pass of the emulator's main loop, whether the emulator is running or paused. Use it for anything that must keep working while paused, such as hotkeys or speech. While running, a loop pass happens once per frame; while paused, roughly every 10 milliseconds as long as any script is loaded.
- A script is resumed at most once per loop pass. Calling emu.frameadvance() in a loop therefore advances exactly one frame per call, and emu.framecount() goes up by exactly one each time.
- A bare coroutine.yield() in the main chunk (outside any coroutine the script created itself) behaves like emu.yield().
- emu.frameadvance() and emu.yield() only work in the script's main coroutine. Calling them from an event callback, or from a coroutine the script created, raises an error. Every other host function works anywhere, including in coroutines the script creates.

When the main chunk returns, the script stays loaded for as long as it has event callbacks registered, and those callbacks keep firing. Once the main chunk has returned and the last callback is removed, the script is finished and its state is closed.

### Errors

An uncaught error in the main chunk or in any event callback stops the script. The console shows the error message followed by a stack traceback, the script is marked as errored, its pad and touch overrides are dropped, and its Lua state is closed. Its stop handlers do not run.

### Stopping

A script can be stopped from the console at any time, even when it is stuck in an endless loop that never yields: the host checks for a stop request every thousand Lua instructions and breaks out of the loop, including through any pcall. When a script is stopped on request, or when melonDS exits, its stop event handlers run first. They get about one second in total; handlers still running after that are cut off. Then its overrides are dropped and its state is closed.

### The cost of the stop check, and opting out

The stop check is a Lua count hook, and while any count hook is installed Lua 5.4 does a little extra work on every instruction, however rarely the hook itself runs. Tight pure-Lua loops (arithmetic, comparisons, local variable shuffling) run roughly twice as slowly with it; see the timing notes at the end. Code that mostly calls host functions such as mem.u32, or library functions such as string.format and table.concat, pays much less, because time spent inside C functions is not affected.

A script that needs the speed can remove the check by calling debug.sethook() with no arguments at the top of its main chunk. Tested behaviour:

- It removes the check from the main coroutine.
- Coroutines created afterwards from the main coroutine (or from coroutines that themselves have no hook) start without it, because a new coroutine copies the hook of the coroutine that creates it.
- Coroutines created before the call keep the check.
- Event callbacks keep the check, since they run on a separate coroutine the host creates, and so do coroutines created inside callbacks.

The price: a runaway loop in the main coroutine, or in a coroutine created after the call, can then no longer be stopped from the console, nor by closing melonDS; the only way out is to kill the melonDS process. debug.sethook(co) with a coroutine argument removes the check from just that coroutine.

## emu

emu.frameadvance()
Suspends the script until one frame has been emulated; it resumes just before the next one. Main coroutine only.

emu.yield()
Suspends the script until the next pass of the emulator loop, running or paused. Main coroutine only.

emu.framecount()
The number of frames this instance has emulated since melonDS started, as an integer. It is a plain counter kept by the host: savestates and resets do not change it.

emu.pause(), emu.unpause()
Pause or resume emulation, like the Pause menu item. The change takes effect at the end of the current loop pass (any frame already about to run still runs), but emu.paused() reports the new state straight away. Calling pause when already paused, or unpause when not paused or when no game is running, does nothing.

emu.paused()
True when emulation is paused or no game is running.

emu.clock()
Seconds since the instance's scripting host started, as a float, from a monotonic clock with sub-microsecond resolution. Use it for timing; it never jumps backwards.

emu.savestate(path), emu.loadstate(path)
Save the console state to a file or load it from one. They return true on success, or nil and an error message (for example when no game is running or the file cannot be opened). After a successful emu.loadstate the state_loaded event fires once the calling script yields or returns, not during the call.

## mem

The mem functions read and write the ARM9's view of memory, through the same bus routines the emulated ARM9 uses (minus its caches and tightly coupled memory: ITCM and DTCM are not visible here). Addresses and values are integers. Every function raises an error when no game is running.

- mem.u8(addr), mem.u16(addr), mem.u32(addr) read unsigned values.
- mem.s8(addr), mem.s16(addr), mem.s32(addr) read signed values.
- mem.w8(addr, value), mem.w16(addr, value), mem.w32(addr, value) write values; only the low 8, 16 or 32 bits of value are used. Writes keep the JIT's compiled code in step, so patching code in RAM is safe with the JIT enabled.
- mem.bytes(addr, n) returns a Lua string of exactly n bytes starting at addr (n may be at most 16 MB). When the whole range lies in the main RAM region (0x02000000 up to 0x03000000) the bytes are copied straight out of main RAM, honouring its mirroring (4 MB on DS, up to 16 MB on DSi), which is very fast. Any other range is read one byte at a time through the bus.

16 and 32-bit accesses are aligned down the way the hardware does it: mem.u32(0x02000001) reads the word at 0x02000000.

mem.arm7 is a table with the same ten functions using the ARM7's view of memory instead.

Warning: reads go through the real bus, so reading I/O registers (the 0x04000000 region) can have side effects, exactly as if the game had read them. For example, reading the IPC receive FIFO register removes a word from the FIFO. Stick to RAM, VRAM and registers you know to be side-effect free. Writes to I/O registers act as the game writing them.

## rom

These read the cartridge in slot 1. Every function returns nil when no cartridge is inserted.

- rom.code() is the 4-character game code from the cartridge header, such as "IRBO".
- rom.title() is the 12-character game title from the header, without trailing spaces or padding.
- rom.path() is the absolute path of the ROM file as melonDS loaded it. For a ROM inside an archive this is the archive's path, and for a compressed .zst ROM it is the compressed file's path.
- rom.size() is the size of the cartridge image in memory. melonDS pads images up to the next power of two, so this can be larger than the file; for a normal dump it is equal.
- rom.bytes(offset, n) returns n bytes of the ROM as it is on disk, starting at offset, as a Lua string. A range that does not lie entirely inside the image raises an error. It is served from the copy of the ROM already in memory, so it is much faster than readirom.bytes returns exactly the bytes of the ROM file, including the secure area. melonDS itself may change one part of its in-memory copy: if the dump has a decrypted secure area (the first 2 KB of the ARM9 binary, somewhere in 0x4000 to 0x8000, starting with the bytes FF DE FF E7), it re-encrypts those 2 KB when the cartridge is inserted, as a real cartridge would present them. The scripting host keeps the original bytes of that region from before the cartridge was inserted and puts them back in whatever rom.bytes returns, so scripts never see the difference.hanged at all.

## pad

pad.set(buttons)
Overrides buttons. buttons is a table whose keys are button names: A, B, X, Y, L, R, Start, Select, Up, Down, Left, Right (case matters). true forces a button pressed, false forces it released, and a button left out of the table follows the real controller. Each call replaces all of this script's previous overrides, so pad.set({}) is the same as pad.clear(). Overrides stay in effect on every frame until changed. An unknown name or a value that is not a boolean raises an error.

pad.clear()
Drops all of this script's button overrides.

pad.get()
Returns the state of the physical controls as a table of button name to boolean (true for pressed), combining keyboard and gamepad mappings, and ignoring every script's overrides.

Overrides belong to the script that set them and are dropped when it stops, finishes or fails. If several scripts override the same button, the one loaded last wins. Overrides only matter while frames are running; set them before emu.frameadvance() and they apply to the next frame.

## touch

touch.set(x, y)
Holds the stylus down at x from 0 to 255 and y from 0 to 191 (bottom screen pixels), replacing physical touch input, until touch.release(). Out-of-range coordinates raise an error. Button state is not affected.

touch.release()
Removes the override, so physical touch input (the mouse on the bottom screen) works again. If nobody is touching the screen physically, the stylus is lifted.

Like pad overrides, touch overrides belong to the script and are dropped when it stops.

## input

input.keys()
Returns a table of every keyboard key currently held down in this instance's windows, as name to true, whether or not the key is mapped to anything in melonDS. Key repeat is ignored. It keeps working while the emulator is paused, so a script can implement its own hotkeys with emu.yield(). All keys are considered released whenever a melonDS window loses keyboard focus, whether to another application or to another melonDS window such as the Lua console, because the key releases would go to the other window.

Key names:

- Letters: "A" to "Z", always upper case.
- Digits on the main keyboard: "0" to "9". The keypad digits are "Numpad0" to "Numpad9".
- Function keys: "F1" to "F12" (and up to "F35" where keyboards have them).
- Navigation: "Up", "Down", "Left", "Right", "Home", "End", "PageUp", "PageDown", "Insert", "Delete".
- Editing and control: "Space", "Escape", "Enter", "Tab", "Backspace".
- Punctuation, named after the unshifted key on a US layout so that the name stays the same when Shift is held: "LeftBracket", "RightBracket", "Semicolon", "Apostrophe", "Comma", "Period", "Slash", "Backslash", "Backquote", "Minus", "Equals".
- Keypad: "NumpadEnter", "NumpadPlus", "NumpadMinus", "NumpadMultiply", "NumpadSlash", "NumpadPeriod".
- Modifiers: "Shift", "Ctrl", "Alt" (left and right are not distinguished; AltGr counts as "Alt"), and "Meta" for the Windows or Command key.
- Others: "CapsLock", "NumLock", "ScrollLock", "Pause", "PrintScreen", "Menu".
- Any other key gets Qt's portable name for it with spaces removed, for example "VolumeUp" or "MediaPlay".

With Shift held, keys on a US layout still report their unshifted names, so Shift plus 1 shows as "Shift" and "1". On other layouts the shifted punctuation of some keys may report a different name; letters, digits and the named keys above are reliable.

Gamepad buttons are not reported yet.

## event

event.on(name, callback)
Registers callback to be called on an event and returns an integer handle. The callback gets no arguments. Event names:

- frame_start: just before each frame is emulated, after scripts waiting in emu.frameadvance() have run and before input is applied, so callbacks can still change overrides for this frame.
- frame_end: right after each frame is emulated.
- state_loaded: after a savestate was loaded, from the menu, a hotkey, or emu.loadstate. Also after the "undo state load" command.
- rom_changed: after a ROM was booted, a cartridge was inserted or ejected, or the firmware was booted.
- reset: after the console was reset with the Reset command.
- stop: when the script is stopped on request (from the console, or because it is being restarted or removed) or because melonDS is exiting, just before its state is closed. Not when the script fails or finishes on its own.

state_loaded, rom_changed and reset fire on the next loop pass after the action completes, which is a few milliseconds at most, and before any script waiting in emu.frameadvance() resumes for the next frame.

event.off(handle)
Removes a callback. Returns true if the handle was registered, false otherwise.

Callbacks run in their own coroutine, separate from the main chunk, and may run while the main chunk is suspended in emu.frameadvance() or emu.yield(). They cannot call emu.frameadvance() or emu.yield() and must not yield. An error in a callback stops the whole script.

## Timing notes

Measured on the development machine with Pokemon Black: 4000 scattered 4 KB reads took about 2.5 ms with rom.bytes against about 19 ms with io.open, seek and read on the same file (already in the operating system's cache).

The stop check is not free. A 20-million-iteration pure-Lua counting loop took about 87 ms with it installed against 40 ms without, a bit over twice as long. Code that mostly calls host functions, builds strings or works on tables pays proportionally much less, because time spent inside C functions is not affected.
