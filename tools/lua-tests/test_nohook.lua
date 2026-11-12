-- Checks what debug.sethook() at the top of a script's main chunk does to the
-- host's stop check, so the documentation of the opt-out stays true. Needs
-- frames to run (for the callback check), so it runs in the ROM launches.

local T = require("testlib")
T.begin("nohook")

local early = coroutine.create(function() coroutine.yield() end)

local before = debug.gethook()
T.check("the main coroutine starts with the host's stop hook", before == "external hook", tostring(before))

debug.sethook()
T.check("debug.sethook() removes it from the main coroutine", debug.gethook() == nil, tostring(debug.gethook()))

local late = coroutine.create(function() end)
T.check("coroutines created afterwards in the main coroutine have no hook", debug.gethook(late) == nil, tostring(debug.gethook(late)))
T.check("coroutines created before the call keep the hook", debug.gethook(early) == "external hook", tostring(debug.gethook(early)))

local inCallback, inCallbackCo
local h
h = event.on("frame_end", function()
    inCallback = debug.gethook()
    inCallbackCo = debug.gethook(coroutine.create(function() end))
    event.off(h)
end)

emu.frameadvance()
emu.frameadvance()
T.check("event callbacks still run with the hook", inCallback == "external hook", tostring(inCallback))
T.check("coroutines created inside callbacks get the hook", inCallbackCo == "external hook", tostring(inCallbackCo))
T.check("emu.frameadvance still works without the hook", emu.framecount() > 0)

T.done()
