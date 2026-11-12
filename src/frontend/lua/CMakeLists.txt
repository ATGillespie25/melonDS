# Lua 5.4.8, vendored unmodified from https://www.lua.org/ftp/lua-5.4.8.tar.gz
# (the src directory, minus the standalone interpreter lua.c and compiler luac.c).
# Built as a static library and linked into the frontend only; the emulator core
# does not depend on it.

add_library(lua54 STATIC
    lapi.c
    lauxlib.c
    lbaselib.c
    lcode.c
    lcorolib.c
    lctype.c
    ldblib.c
    ldebug.c
    ldo.c
    ldump.c
    lfunc.c
    lgc.c
    linit.c
    liolib.c
    llex.c
    lmathlib.c
    lmem.c
    loadlib.c
    lobject.c
    lopcodes.c
    loslib.c
    lparser.c
    lstate.c
    lstring.c
    lstrlib.c
    ltable.c
    ltablib.c
    ltm.c
    lundump.c
    lutf8lib.c
    lvm.c
    lzio.c
)

target_include_directories(lua54 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

if (UNIX)
    target_compile_definitions(lua54 PRIVATE LUA_USE_POSIX)
    target_link_libraries(lua54 PRIVATE m)
endif()
