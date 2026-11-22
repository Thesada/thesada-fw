#!/usr/bin/env bash
# Every read path that can return config values runs through the mask policy
# (config_mask_policy.h): CLI, chunked MQTT read, HTTP routes, Lua.

set -euo pipefail
cd "$(cd "$(dirname "$0")/.." && pwd)"

SHELL_CPP=lib/thesada-core/src/Shell.cpp
MQTT_CPP=lib/thesada-core/src/MQTTClient.cpp
HTTP_CPP=lib/thesada-mod-httpserver/src/HttpServer.cpp
LUA_CPP=lib/thesada-mod-scriptengine/src/ScriptEngine.cpp
status=0

# need <file> <start line text> <end regex> <wanted text>: the block from the
# first line holding start up to the next line matching end holds wanted.
need() {
  if [ ! -f "$1" ]; then
    printf 'FAIL  %s not found\n' "$1" >&2
    status=1
    return
  fi
  if awk -v start="$2" -v endre="$3" -v want="$4" '
      !on && index($0, start) { on = 1; next }
      on && $0 ~ endre { exit }
      on && index($0, want) { found = 1; exit }
      END { exit !found }' "$1"; then
    printf 'ok    %s  %s  %s\n' "$1" "$2" "$4"
  else
    printf 'FAIL  %s  %s does not call %s\n' "$1" "$2" "$4" >&2
    status=1
  fi
}

FN_END='^}'
ROUTE_END='^  (server[.]on[(]|AsyncCallbackWebHandler[*])'
need "$SHELL_CPP" 'static void cmd_config_get(' "$FN_END" 'configMaskedNodeText('
need "$SHELL_CPP" 'static void cmd_config_dump(' "$FN_END" 'configMaskedNodeText('
need "$SHELL_CPP" 'static void cmd_cat(' "$FN_END" 'Config::readFileMasked('
need "$SHELL_CPP" 'static void cmd_config_set(' "$FN_END" 'configSetKeepsSecret('
need "$MQTT_CPP" 'if (strcmp(cmd, "fs.cat") == 0' '^    // cert[.]set' 'Config::readFileMasked('
need "$HTTP_CPP" 'server.on("/api/config", HTTP_GET' "$ROUTE_END" '_sendMaskedOrReply('
need "$HTTP_CPP" 'server.on("/api/file", HTTP_GET' "$ROUTE_END" '_sendMaskedOrReply('
need "$HTTP_CPP" 'server.on("/api/backup", HTTP_POST' "$ROUTE_END" 'Config::readFileMasked('
need "$HTTP_CPP" 'static size_t _configWriteOrReply(' "$FN_END" 'Config::keepStoredSecrets('
need "$LUA_CPP" 'static int lua_config_get(' "$FN_END" 'configPathIsSecret('
need "$LUA_CPP" 'static int lua_config_get(' "$FN_END" 'configMaskedNodeText('

# The live doc is never serialized straight into a reply.
if grep -nE 'serializeJson(Pretty)?\((Config::get\(\)|cfg|current)[,)]' "$SHELL_CPP" "$LUA_CPP" >&2; then
  printf 'FAIL  config serialized without the mask (above)\n' >&2
  status=1
fi

exit "$status"
