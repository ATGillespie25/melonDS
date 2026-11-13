-- whoops.lua: fails on purpose, to show how the Lua console reports an error.
--
-- Load it from System > Lua console. It needs no game. It counts down for
-- three seconds using emu.yield (so it keeps running even while the emulator
-- is paused), then raises an error three function calls deep. The console
-- marks the script as "stopped by an error" and prints the message with a
-- traceback naming each function on the way down.

print("A wild SCRIPT appeared!")

for n = 3, 1, -1 do
    print(("SCRIPT is charging up... %d"):format(n))
    local deadline = emu.clock() + 1
    while emu.clock() < deadline do
        emu.yield()
    end
end

local function useSplash()
    error("SCRIPT used Splash! But nothing happened. (Except this error. It's super effective!)")
end

local function chooseMove()
    print("SCRIPT is choosing a move...")
    useSplash()
    print("This line is never reached.")
end

local function battle()
    chooseMove()
    print("Neither is this one.")
end

battle()
