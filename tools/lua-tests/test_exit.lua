-- Runs away in an endless loop that never yields. run-tests.sh then asks
-- melonDS to close normally. That must interrupt the loop, fire the stop event
-- (first handler below), cut off a stop handler that never returns (second
-- handler), and let the process exit on its own.

local T = require("testlib")
T.begin("exit")

event.on("stop", function()
    T.check("stop event fires when the emulator exits, after interrupting a runaway loop", true)
    T.done()
end)

event.on("stop", function()
    while true do end
end)

T.info("READY")

local x = 0
while true do
    x = x + 1
end
