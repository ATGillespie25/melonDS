-- Engine tests that need no ROM: loading, require, scheduling, coroutines,
-- clock, UTF-8 file paths, event registration, and behaviour with no console.

local T = require("testlib")
T.begin("core")

local testDir = os.getenv("LUA_TEST_DIR")

-- the chunk and its arguments

T.check("main chunk is a vararg chunk with no arguments from --lua", select("#", ...) == 0, select("#", ...))

local src = debug.getinfo(1, "S").source
T.check("chunk source is @ plus an absolute path", src:match("^@%a:/") ~= nil or src:match("^@/") ~= nil, src)
T.check("chunk source uses forward slashes", not src:find("\\", 1, true), src)
T.check("chunk source names this file", src:sub(-#"test_core.lua") == "test_core.lua", src)

T.check("require found the sibling module", package.loaded.testlib == T)
T.check("package.path starts with the script directory", package.path:find("lua-tests/?.lua", 1, true) ~= nil, package.path)
T.check("standard libraries are all open",
    io ~= nil and os ~= nil and debug ~= nil and package ~= nil and string ~= nil
    and table ~= nil and math ~= nil and utf8 ~= nil and coroutine ~= nil)
T.check("host modules are all present",
    emu ~= nil and mem ~= nil and mem.arm7 ~= nil and rom ~= nil and pad ~= nil
    and touch ~= nil and input ~= nil and event ~= nil)

-- clock

local c0 = emu.clock()
local c1 = c0
local spins = 0
while c1 == c0 and spins < 10000000 do
    c1 = emu.clock()
    spins = spins + 1
end
T.check("emu.clock is a float", math.type(c0) == "float", math.type(c0))
T.check("emu.clock ticks in steps under a millisecond", c1 > c0 and (c1 - c0) < 0.001, c1 - c0)
T.info(string.format("smallest observed emu.clock step: %.9f s", c1 - c0))

-- with no game running

local ok, err = pcall(mem.u8, 0x02000000)
T.check("mem.u8 raises an error with no game running", not ok)
T.check("that error says no game is running", tostring(err):find("no game is running", 1, true) ~= nil, err)
T.check("mem.arm7.u32 also raises", not pcall(mem.arm7.u32, 0x02000000))
T.check("rom.code is nil with no cart", rom.code() == nil)
T.check("rom.bytes is nil with no cart", rom.bytes(0, 4) == nil)
local sok, smsg = emu.savestate(testDir .. "/never.mln")
T.check("emu.savestate returns nil and a message with no game", sok == nil and type(smsg) == "string", smsg)
T.check("emu.paused is true with no game", emu.paused() == true)
T.check("emu.framecount is an integer", math.type(emu.framecount()) == "integer")

-- input APIs that work without a console

T.check("input.keys returns a table", type(input.keys()) == "table")
local g = pad.get()
local nbtn = 0
for _, name in ipairs({"A", "B", "X", "Y", "L", "R", "Start", "Select", "Up", "Down", "Left", "Right"}) do
    if type(g[name]) == "boolean" then nbtn = nbtn + 1 end
end
T.check("pad.get reports all 12 buttons as booleans", nbtn == 12, nbtn)
T.check("pad.set rejects unknown button names", not pcall(pad.set, {Turbo = true}))
T.check("pad.set rejects non-boolean values", not pcall(pad.set, {A = 1}))
T.check("pad.set accepts a valid table", pcall(pad.set, {A = true, Left = false}))
pad.clear()
T.check("touch.set rejects out-of-range coordinates", not pcall(touch.set, 256, 10) and not pcall(touch.set, 10, 192))
T.check("touch.set and touch.release accept valid input", pcall(touch.set, 255, 191) and pcall(touch.release))

-- events

local h1 = event.on("frame_start", function() end)
local h2 = event.on("stop", function() end)
T.check("event.on returns distinct integer handles", math.type(h1) == "integer" and h1 ~= h2)
T.check("event.off removes a callback once", event.off(h1) == true and event.off(h1) == false)
T.check("event.on rejects unknown event names", not pcall(event.on, "frame_middle", function() end))
T.check("event.on requires a function", not pcall(event.on, "reset", 42))
event.off(h2)

-- coroutines created by the script

local co = coroutine.create(function()
    local t = emu.clock()
    print("print from a script coroutine works")
    local keys = input.keys()
    local okY, errY = pcall(emu.yield)
    local okF, errF = pcall(emu.frameadvance)
    coroutine.yield(t, type(keys), okY, errY, okF, errF)
    return "finished"
end)
local okc, t, keysType, okY, errY, okF, errF = coroutine.resume(co)
T.check("host functions work inside a script coroutine", okc and type(t) == "number" and keysType == "table", t)
T.check("emu.yield from a script coroutine raises a clear error",
    okY == false and tostring(errY):find("main coroutine", 1, true) ~= nil, errY)
T.check("emu.frameadvance from a script coroutine raises a clear error",
    okF == false and tostring(errF):find("main coroutine", 1, true) ~= nil, errF)
local okc2, res = coroutine.resume(co)
T.check("script coroutine yields and resumes normally", okc2 and res == "finished", res)

-- coroutine.wrap and pcall around host calls
local w = coroutine.wrap(function(a) local b = coroutine.yield(a + 1) return b * 2 end)
T.check("coroutine.wrap works", w(1) == 2 and w(5) == 10)

-- emu.yield scheduling while paused with no game

local t0 = emu.clock()
local passes = 0
local minGap, last = math.huge, emu.clock()
while emu.clock() - t0 < 1.0 do
    emu.yield()
    passes = passes + 1
    local now = emu.clock()
    if now - last < minGap then minGap = now - last end
    last = now
end
T.info(string.format("emu.yield resumed %d times in 1 s while paused; shortest gap %.4f s", passes, minGap))
T.check("emu.yield resumes about every 10 ms while paused", passes >= 40 and passes <= 110, passes)
T.check("emu.yield never resumes twice in one loop pass", minGap > 0.005, minGap)

-- a bare coroutine.yield from the main chunk acts like emu.yield
local before = emu.clock()
coroutine.yield()
T.check("bare coroutine.yield in the main chunk waits for the next pass", emu.clock() - before > 0.005, emu.clock() - before)

-- pcall around emu.yield in the main coroutine is allowed (pcall is yieldable)
local okp = pcall(function() emu.yield() end)
T.check("emu.yield works inside pcall in the main coroutine", okp)

-- io with UTF-8 paths

local name = testDir .. "/écrit_тест_✓.txt"
local f, ferr = io.open(name, "w")
T.check("io.open creates a file with a non-ASCII UTF-8 name", f ~= nil, ferr)
if f then
    f:write("written by test_core")
    f:close()
    local r = io.open(name, "r")
    T.check("that file reads back", r ~= nil and r:read("a") == "written by test_core")
    if r then r:close() end
end

local pre = io.open(testDir .. "/préparé_файл.txt", "r")
T.check("io.open reads a non-ASCII file created outside Lua", pre ~= nil and pre:read("l") == "from the harness")
if pre then pre:close() end

local renamed = testDir .. "/renommé_✓.txt"
T.check("os.rename handles UTF-8 paths", os.rename(name, renamed) == true)
local lines = 0
for _ in io.lines(renamed) do lines = lines + 1 end
T.check("io.lines handles UTF-8 paths", lines == 1)
-- leave renommé_✓.txt in place; run-tests.sh checks its name on disk

T.done()
