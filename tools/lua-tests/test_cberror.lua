-- Raises an error from an event callback on the third frame, while holding a
-- pad override. run-tests.sh checks stdout for the error and its traceback;
-- test_rom.lua later checks that the override (R) was dropped with the script.

local T = require("testlib")
T.begin("cberror")

pad.set({R = true})

local frames = 0
local function failingCallback()
    frames = frames + 1
    if frames == 3 then
        local missing = nil
        return missing.field
    end
end

event.on("frame_end", failingCallback)
T.info("callback registered; main chunk returns")
