-- Tests that need a running game: frame cadence, frame events, memory access,
-- ROM access checked against the file on disk, pad and touch overrides seen by
-- the console, pause, savestates, and a timing comparison of rom.bytes with
-- plain io reads. Written against Pokemon Black (game code IRBO) but only the
-- game code check depends on that.

local T = require("testlib")
T.begin("rom")

local romFile = os.getenv("LUA_TEST_ROM")
local testDir = os.getenv("LUA_TEST_DIR")
T.info("JIT setting for this run: " .. tostring(os.getenv("LUA_TEST_JIT")))

-- wait for the game

local waited = 0
while rom.code() == nil and waited < 500 do
    emu.yield()
    waited = waited + 1
end
for i = 1, 10 do emu.frameadvance() end

T.check("rom.code is IRBO", rom.code() == "IRBO", rom.code())
T.info("rom.title " .. tostring(rom.title()) .. ", rom.size " .. tostring(rom.size()))
local function normPath(p) return (p:gsub("\\", "/"):lower()) end
T.check("rom.path names the ROM file", rom.path() ~= nil and normPath(rom.path()) == normPath(romFile), rom.path())

-- frame cadence and frame events

local f0 = emu.framecount()
for i = 1, 60 do emu.frameadvance() end
T.check("60 emu.frameadvance calls advance emu.framecount by exactly 60", emu.framecount() - f0 == 60, emu.framecount() - f0)

local starts, ends = 0, 0
local hs = event.on("frame_start", function() starts = starts + 1 end)
local he = event.on("frame_end", function() ends = ends + 1 end)
for i = 1, 30 do emu.frameadvance() end
event.off(hs)
event.off(he)
T.check("frame_start fired once per frame", starts == 30, starts)
T.check("frame_end fired once per frame", ends == 30, ends)

-- memory reads

local function perByte(read8, addr, n)
    local t = {}
    for i = 0, n - 1 do
        t[#t + 1] = string.char(read8(addr + i))
    end
    return table.concat(t)
end

local a = 0x02100000
local v32 = mem.u32(a)
T.check("mem.u32 returns an unsigned 32-bit integer", math.type(v32) == "integer" and v32 >= 0 and v32 <= 0xFFFFFFFF, v32)
T.check("mem.u32 agrees with mem.bytes read as little-endian", string.unpack("<I4", mem.bytes(a, 4)) == v32)
T.check("mem.u16 agrees with two mem.u8 reads", mem.u16(a) == (mem.u8(a) | (mem.u8(a + 1) << 8)))
T.check("mem.s8 is the signed view of mem.u8", (mem.s8(a) & 0xFF) == mem.u8(a) and mem.s8(a) >= -128 and mem.s8(a) <= 127)
T.check("mem.s16 is the signed view of mem.u16", (mem.s16(a) & 0xFFFF) == mem.u16(a))
T.check("mem.s32 is the signed view of mem.u32", (mem.s32(a) & 0xFFFFFFFF) == mem.u32(a))
T.check("mem.arm7 sees the same main RAM", mem.arm7.u32(0x02000000) == mem.u32(0x02000000))

local bulk = mem.bytes(0x02001234, 0x4000)
T.check("mem.bytes returns exactly n bytes", #bulk == 0x4000, #bulk)
T.check("mem.bytes in main RAM matches per-byte reads", bulk == perByte(mem.u8, 0x02001234, 0x4000))
T.check("mem.bytes of length 0 is an empty string", mem.bytes(0x02000000, 0) == "")
T.check("mem.bytes honours the main RAM mirror", mem.bytes(0x02400000 + 0x5000, 0x800) == mem.bytes(0x02000000 + 0x5000, 0x800))
T.check("mem.bytes wraps across a mirror boundary",
    mem.bytes(0x023FFF00, 0x200) == mem.bytes(0x023FFF00, 0x100) .. mem.bytes(0x02000000, 0x100))

-- ranges that are not entirely main RAM take the per-byte path
local cross = mem.bytes(0x02FFFF80, 0x100)
T.check("mem.bytes across the end of main RAM matches per-byte reads", cross == perByte(mem.u8, 0x02FFFF80, 0x100))
T.check("mem.bytes of palette RAM matches per-byte reads", mem.bytes(0x05000000, 0x400) == perByte(mem.u8, 0x05000000, 0x400))
T.check("mem.bytes of VRAM matches per-byte reads", mem.bytes(0x06000000, 0x800) == perByte(mem.u8, 0x06000000, 0x800))
T.check("mem.arm7.bytes of ARM7 WRAM matches per-byte reads",
    mem.arm7.bytes(0x03800000, 0x400) == perByte(mem.arm7.u8, 0x03800000, 0x400))

-- writes (restored straight away; the game does not run in between)

local wa = 0x0237F000
local orig8, orig16, orig32 = mem.u8(wa), mem.u16(wa + 2), mem.u32(wa + 4)
mem.w8(wa, orig8 ~ 0xA5)
T.check("mem.w8 reads back", mem.u8(wa) == (orig8 ~ 0xA5))
T.check("mem.w8 is visible to mem.bytes", mem.bytes(wa, 1):byte() == (orig8 ~ 0xA5))
mem.w16(wa + 2, 0xBEEF)
T.check("mem.w16 reads back", mem.u16(wa + 2) == 0xBEEF)
mem.w32(wa + 4, 0xDEADBEEF)
T.check("mem.w32 reads back", mem.u32(wa + 4) == 0xDEADBEEF and mem.s32(wa + 4) == -559038737)
mem.w8(wa, 0x1FF)
T.check("mem.w8 keeps only the low byte", mem.u8(wa) == 0xFF)
mem.w8(wa, orig8)
mem.w16(wa + 2, orig16)
mem.w32(wa + 4, orig32)
T.check("written bytes restored", mem.u8(wa) == orig8 and mem.u16(wa + 2) == orig16 and mem.u32(wa + 4) == orig32)

-- ROM image against the file on disk

local fh = assert(io.open(romFile, "rb"))
local fileSize = fh:seek("end")
T.info(string.format("ROM file size 0x%X, in-memory image size 0x%X", fileSize, rom.size()))
T.check("rom.size covers the whole file", rom.size() >= fileSize, rom.size())

-- melonDS re-encrypts the 2 KB secure area at the start of the ARM9 binary
-- when a dump has it decrypted (marked by FF DE FF E7 FF DE FF E7)
local arm9Offset = string.unpack("<I4", rom.bytes(0x20, 4))
fh:seek("set", arm9Offset)
local decryptedDump = fh:read(8) == "\xFF\xDE\xFF\xE7\xFF\xDE\xFF\xE7"
local secureStart, secureEnd = arm9Offset, arm9Offset + 0x800
T.info(string.format("ARM9 binary at 0x%X; secure area in the file is %s", arm9Offset,
    decryptedDump and "decrypted, so melonDS re-encrypts it in memory" or "encrypted, so it is left alone"))

for _, off in ipairs({0, 0x200, 0x1000, 0x4000, 0x4800, 0x5000, 0x7800, 0x7FF0, 0x8000, 0x10000, 0x123457, fileSize - 0x100}) do
    fh:seek("set", off)
    local fromFile = fh:read(0x100)
    local same = rom.bytes(off, #fromFile) == fromFile
    if decryptedDump and off < secureEnd and off + #fromFile > secureStart then
        T.info(string.format("rom.bytes at 0x%X %s the file (inside the re-encrypted secure area)", off, same and "matches" or "differs from"))
    else
        T.check(string.format("rom.bytes matches the file at 0x%X", off), same)
    end
end

-- whole-image comparison, reporting differing byte ranges if there are any
local diffs = {}
local chunk = 0x100000
local pos = 0
fh:seek("set", 0)
while pos < fileSize do
    local n = math.min(chunk, fileSize - pos)
    local fromFile = fh:read(n)
    local fromROM = rom.bytes(pos, n)
    if fromFile ~= fromROM then
        for b = 0, n - 1, 0x1000 do
            local m = math.min(0x1000, n - b)
            if fromFile:sub(b + 1, b + m) ~= fromROM:sub(b + 1, b + m) then
                local i = b + 1
                while i <= b + m do
                    if fromFile:byte(i) ~= fromROM:byte(i) then
                        local s = i
                        while i <= b + m and fromFile:byte(i) ~= fromROM:byte(i) do i = i + 1 end
                        diffs[#diffs + 1] = {pos + s - 1, pos + i - 2}
                    else
                        i = i + 1
                    end
                end
            end
        end
    end
    pos = pos + n
end
local diffText, outsideSecure = {}, 0
for _, d in ipairs(diffs) do
    diffText[#diffText + 1] = string.format("0x%X..0x%X", d[1], d[2])
    if not (decryptedDump and d[1] >= secureStart and d[2] < secureEnd) then
        outsideSecure = outsideSecure + 1
    end
end
if #diffs > 0 then
    T.info("in-memory image differs from the file at " .. table.concat(diffText, ", "))
end
T.check("in-memory ROM image matches the file byte for byte outside a re-encrypted secure area",
    outsideSecure == 0, table.concat(diffText, ", "))

T.check("rom.bytes past the end raises an error", not pcall(rom.bytes, rom.size() - 2, 4))
T.check("rom.bytes with a negative offset raises an error", not pcall(rom.bytes, -1, 4))
T.check("rom.bytes reaching exactly the end is allowed", #rom.bytes(rom.size() - 4, 4) == 4)

-- pad overrides, seen through KEYINPUT (ARM9 0x04000130) and EXTKEYIN (ARM7 0x04000136)

local function keyinput() return mem.u16(0x04000130) end
local function extkeyin() return mem.arm7.u16(0x04000136) end

pad.set({A = true, Up = true})
emu.frameadvance()
local k = keyinput()
T.check("pad.set A=true shows A pressed in KEYINPUT", k & 0x001 == 0, string.format("%04X", k))
T.check("pad.set Up=true shows Up pressed in KEYINPUT", k & 0x040 == 0, string.format("%04X", k))
T.check("buttons not named stay released", k & 0x002 ~= 0, string.format("%04X", k))
T.check("pad.get ignores overrides", pad.get().A == false)

pad.set({X = true, Y = false})
emu.frameadvance()
k = keyinput()
local e = extkeyin()
T.check("pad.set X=true shows X pressed in EXTKEYIN", e & 0x1 == 0, string.format("%04X", e))
T.check("Y forced released", e & 0x2 ~= 0, string.format("%04X", e))
T.check("a new pad.set replaces the previous overrides", k & 0x041 == 0x041, string.format("%04X", k))

-- an override persists across frames until changed
emu.frameadvance()
emu.frameadvance()
T.check("overrides persist across frames", extkeyin() & 0x1 == 0)

pad.clear()
emu.frameadvance()
k = keyinput()
e = extkeyin()
T.check("pad.clear releases every forced button", k & 0x3FF == 0x3FF and e & 0x3 == 0x3, string.format("%04X %04X", k, e))
T.check("overrides from the script that errored were dropped (R released)", k & 0x100 ~= 0, string.format("%04X", k))

-- touch override, seen through the pen-down bit of EXTKEYIN

touch.set(128, 96)
pad.set({B = true})
emu.frameadvance()
e = extkeyin()
k = keyinput()
T.check("touch.set shows the pen down", e & 0x40 == 0, string.format("%04X", e))
T.check("touch.set leaves button state alone", k & 0x002 == 0 and k & 0x001 ~= 0, string.format("%04X", k))
touch.release()
pad.clear()
emu.frameadvance()
T.check("touch.release lifts the pen", extkeyin() & 0x40 ~= 0, string.format("%04X", extkeyin()))

-- pause and unpause from a script

emu.pause()
T.check("emu.paused is true right after emu.pause", emu.paused() == true)
emu.yield()
local pausedAt = emu.framecount()
for i = 1, 5 do emu.yield() end
T.check("no frames run while paused", emu.framecount() == pausedAt, emu.framecount() - pausedAt)
T.check("emu.paused stays true", emu.paused() == true)
emu.unpause()
T.check("emu.paused is false right after emu.unpause", emu.paused() == false)
local beforeAdvance = emu.framecount()
emu.frameadvance()
emu.frameadvance()
T.check("frames run again after emu.unpause", emu.framecount() - beforeAdvance == 2, emu.framecount() - beforeAdvance)

-- savestates and state_loaded

local statePath = testDir .. "/état_тест.mln"
local loadedCount = 0
local hl = event.on("state_loaded", function() loadedCount = loadedCount + 1 end)
local marker = 0x0237F010
local markerOrig = mem.u32(marker)
local okSave, errSave = emu.savestate(statePath)
T.check("emu.savestate to a UTF-8 path returns true", okSave == true, errSave)
mem.w32(marker, markerOrig ~ 0x5A5A5A5A)
local okLoad, errLoad = emu.loadstate(statePath)
T.check("emu.loadstate returns true", okLoad == true, errLoad)
T.check("emu.loadstate restored memory", mem.u32(marker) == markerOrig, string.format("%08X vs %08X", mem.u32(marker), markerOrig))
T.check("state_loaded waits until the script yields", loadedCount == 0, loadedCount)
emu.frameadvance()
T.check("state_loaded fired once after the yield", loadedCount == 1, loadedCount)
event.off(hl)
local okMissing, errMissing = emu.loadstate(testDir .. "/missing.mln")
T.check("emu.loadstate of a missing file returns nil and a message", okMissing == nil and type(errMissing) == "string", errMissing)

-- timing: scattered 4 KB reads through rom.bytes and through io

local N = 4000
local offsets = {}
local seed = 12345
local blocks = (fileSize - 0x2000) // 0x1000
for i = 1, N do
    seed = (seed * 1103515245 + 12345) % 0x80000000
    offsets[i] = (seed % blocks) * 0x1000 + (seed % 61)
end

local accROM, accIO = 0, 0
local t0 = emu.clock()
for i = 1, N do
    local s = rom.bytes(offsets[i], 4096)
    accROM = accROM + s:byte(1) + s:byte(4096)
end
local tROM = emu.clock() - t0

t0 = emu.clock()
for i = 1, N do
    fh:seek("set", offsets[i])
    local s = fh:read(4096)
    accIO = accIO + s:byte(1) + s:byte(4096)
end
local tIO = emu.clock() - t0
fh:close()

T.check("timed reads agree", accROM == accIO)
T.info(string.format("%d scattered 4 KB reads: rom.bytes %.2f ms, io seek+read %.2f ms (file already in the OS cache)",
    N, tROM * 1000, tIO * 1000))

-- cost of the count hook that makes runaway scripts stoppable
local function spin()
    local x = 0
    for i = 1, 20000000 do x = x + i end
    return x
end
t0 = emu.clock()
spin()
local tHook = emu.clock() - t0
local co = coroutine.create(spin)
debug.sethook(co)
t0 = emu.clock()
coroutine.resume(co)
local tNoHook = emu.clock() - t0
T.info(string.format("20M-iteration loop: %.0f ms with the stop hook, %.0f ms without", tHook * 1000, tNoHook * 1000))

-- callbacks keep a script alive after its main chunk returns

local n = 0
local h
h = event.on("frame_end", function()
    n = n + 1
    if n == 10 then
        T.check("callbacks keep running after the main chunk returns", true)
        event.off(h)
        T.done()
    end
end)
