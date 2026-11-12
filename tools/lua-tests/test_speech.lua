-- Speech module tests. run-tests.sh runs this in several launches, telling it
-- through LUA_TEST_SPEECH what to expect:
--   missing   no prism.dll beside melonDS
--   bogus     prism.dll present, MELONDS_SPEECH_BACKEND names no real backend
--   best      prism.dll present, normal backend selection
--   nospeech  prism.dll present, MELONDS_SPEECH_BACKEND = "No Speech"
--
-- Safety rule: speech.say and speech.stop are only ever called once
-- speech.available() has reported Prism's silent "No Speech" backend. They
-- must never reach a real screen reader or voice from a test, where they would
-- talk over the user's own screen reader.

local T = require("testlib")
T.begin("speech")

local mode = os.getenv("LUA_TEST_SPEECH") or "missing"
T.info("mode " .. mode)

T.check("speech.say, speech.stop and speech.available exist",
    type(speech) == "table" and type(speech.say) == "function"
    and type(speech.stop) == "function" and type(speech.available) == "function")

local okCall, avail, name = pcall(speech.available)
T.check("speech.available never raises", okCall, avail)

if mode == "missing" then
    T.check("without prism.dll speech.available() is false", avail == false and name == nil, tostring(avail))
    T.check("asking twice is still false and does not raise", pcall(speech.available) and speech.available() == false)

elseif mode == "bogus" then
    T.check("an unknown MELONDS_SPEECH_BACKEND makes speech unavailable", avail == false, tostring(avail) .. " " .. tostring(name))

elseif mode == "best" then
    T.check("with prism.dll speech.available() is true", avail == true, tostring(avail))
    T.check("and names a backend", type(name) == "string" and #name > 0, name)
    T.info("best backend: " .. tostring(name))

elseif mode == "nospeech" then
    if avail == true and name == "No Speech" then
        T.check("speech.available() reports the No Speech backend", true)
        T.check("speech.say with the default interrupt", speech.say("melonDS speech test") == true)
        T.check("speech.say with interrupt true", speech.say("interrupting", true) == true)
        T.check("speech.say queued with interrupt false", speech.say("queued after it", false) == true)
        T.check("speech.say with non-ASCII UTF-8 text", speech.say("Pokémon Schwarz: ポケモン ✓ Ünïcødé", false) == true)
        T.check("speech.say accepts a number", speech.say(42, false) == true)
        T.check("speech.stop", speech.stop() == true)
        T.check("speech.say after stop", speech.say("after stopping") == true)
        local a2, n2 = speech.available()
        T.check("the backend stays the same", a2 == true and n2 == "No Speech", tostring(n2))
        T.check("speech.say with no text raises an argument error", not pcall(speech.say))

        -- from a coroutine and an event-free second thread of control
        local co = coroutine.wrap(function() return speech.say("from a coroutine", false) end)
        T.check("speech.say works inside a script coroutine", co() == true)
    elseif avail == false then
        T.skip("speech.say, speech.stop and speech.available with the No Speech backend",
            "this prism.dll does not include the No Speech backend")
    else
        -- something else was selected: never call say or stop
        T.check("speech.available() reports the No Speech backend", false, tostring(name))
    end

else
    T.check("known LUA_TEST_SPEECH mode", false, mode)
end

T.done()
