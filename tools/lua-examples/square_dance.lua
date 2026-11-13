-- square_dance.lua: shows a script reading the game and driving it.
--
-- Load a game, get to somewhere your character can walk freely, then load
-- this from System > Lua console. It will:
--   1. print the game's title and code, and a hex dump of the ROM header
--      (rom.title, rom.code, rom.bytes);
--   2. find the same header in emulated memory (mem.bytes, mem.u32);
--   3. walk the character around a square twice by holding the D-pad
--      (pad.set and emu.frameadvance), reading the console's own key register
--      after each press to prove the game sees the input (mem.u16);
--   4. hand the controls back (pad.clear).
-- Stopping the script early from the console also hands the controls back.

local LAPS = 2
local FRAMES_PER_SIDE = 48
local SIDES = {"Up", "Right", "Down", "Left"}

if not rom.code() then
    print("No game is loaded. Load a ROM, then restart this script (F5 in the script list).")
    return
end

-- 1. Who are we playing?

print(("Now playing: %s (game code %s, %d MB)"):format(
    rom.title(), rom.code(), rom.size() // (1024 * 1024)))

local function hexDump(data, base)
    for offset = 1, #data, 16 do
        local chunk = data:sub(offset, offset + 15)
        local hex = chunk:gsub(".", function(c) return ("%02X "):format(c:byte()) end)
        local text = chunk:gsub("[^\32-\126]", ".")
        print(("%08X  %-48s %s"):format(base + offset - 1, hex, text))
    end
end

print("First 32 bytes of the ROM header:")
hexDump(rom.bytes(0, 32), 0)

-- 2. A peek at emulated memory.

-- The DS keeps a copy of the cartridge header near the top of main RAM, so
-- the same bytes should turn up when read through the emulated ARM9 bus.
local HEADER_COPY = 0x027FFE00
print(("The same header, read from emulated RAM at 0x%08X:"):format(HEADER_COPY))
hexDump(mem.bytes(HEADER_COPY, 32), HEADER_COPY)
print(("Game code as one 32-bit read: 0x%08X."):format(mem.u32(HEADER_COPY + 12)))

-- 3. The dance.

-- KEYINPUT, the DS button register. One bit per button, 0 means pressed.
local KEYINPUT = 0x04000130
local KEY_BITS = {"A", "B", "Select", "Start", "Right", "Left", "Up", "Down", "R", "L"}

local function buttonsTheGameSees()
    local reg = mem.u16(KEYINPUT)
    local names = {}
    for i, name in ipairs(KEY_BITS) do
        if reg & (1 << (i - 1)) == 0 then
            names[#names + 1] = name
        end
    end
    if #names == 0 then
        return reg, "nothing"
    end
    return reg, table.concat(names, " + ")
end

local stopHandle = event.on("stop", function()
    print("Square dance interrupted. The controls are yours again.")
end)

local startFrame = emu.framecount()
local startTime = emu.clock()

print(("Square dance: %d laps, %d frames per side. Take your partner..."):format(LAPS, FRAMES_PER_SIDE))

for lap = 1, LAPS do
    for _, direction in ipairs(SIDES) do
        pad.set({[direction] = true})
        emu.frameadvance()

        local reg, seen = buttonsTheGameSees()
        print(("Lap %d, frame %d: holding %s. Key register reads 0x%04X, so the game sees %s."):format(
            lap, emu.framecount(), direction, reg, seen))

        for _ = 2, FRAMES_PER_SIDE do
            emu.frameadvance()
        end
    end
end

-- 4. Bow out.

pad.clear()
event.off(stopHandle)

print(("Done: %d frames in %.2f seconds. The controls are yours again."):format(
    emu.framecount() - startFrame, emu.clock() - startTime))
