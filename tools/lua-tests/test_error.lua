-- Raises an uncaught error from the main coroutine after one yield. run-tests.sh
-- checks melonDS's stdout for the error message and a traceback through both
-- functions, and that the script is reported as stopped by the error.

local T = require("testlib")
T.begin("error")

local function innerFailure()
    error("deliberate test error")
end

local function outerCaller()
    innerFailure()
end

emu.yield()
T.info("about to raise")
outerCaller()

T.check("execution never gets past the error", false)
