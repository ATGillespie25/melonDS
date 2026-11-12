-- Shared helpers for the Lua scripting self-tests. Loaded with require from the
-- test scripts beside it, which is itself one of the things being tested.
--
-- Results go to the file named by the LUA_TEST_RESULTS environment variable,
-- one line per check: PASS name, FAIL name -- detail, INFO text, and finally
-- DONE suite when a suite has run to the end. run-tests.sh waits for the DONE
-- lines it expects.

local M = {}

local outPath = os.getenv("LUA_TEST_RESULTS")
assert(outPath, "LUA_TEST_RESULTS is not set; run the tests through run-tests.sh")

local suite = "?"

local function emit(line)
    local f = assert(io.open(outPath, "a"))
    f:write(line, "\n")
    f:close()
end

function M.begin(name)
    suite = name
    emit("BEGIN " .. name)
end

function M.check(name, cond, detail)
    if cond then
        emit("PASS " .. suite .. ": " .. name)
    else
        emit("FAIL " .. suite .. ": " .. name .. (detail ~= nil and (" -- " .. tostring(detail)) or ""))
    end
    return cond
end

function M.info(text)
    emit("INFO " .. suite .. ": " .. text)
end

function M.done()
    emit("DONE " .. suite)
end

-- Runs fn and turns an unexpected error into a FAIL line instead of stopping the suite.
function M.try(name, fn, ...)
    local ok, err = pcall(fn, ...)
    if not ok then
        M.check(name .. " (raised an error)", false, err)
    end
    return ok
end

return M
