# Host build of the Lua fork for [env:native-lua]: drop its Arduino wrapper
# and the two command-line mains, keep the Lua core and libraries.
Import("env")


def skip(env, node):
    return None


for pattern in ("*/Arduino-Lua/src/LuaWrapper.cpp",
                "*/Arduino-Lua/src/lua/lua.c",
                "*/Arduino-Lua/src/lua/luac.c"):
    env.AddBuildMiddleware(skip, pattern)
