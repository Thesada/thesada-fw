# thesada-fw. Bare `make` prints this list and changes nothing.
# No fmt target: the repo has no .clang-format to run.
.DEFAULT_GOAL := help
SHELL := bash

PIO  ?= pio
ENV  ?= esp32-owb
PORT ?=
TEST ?=
LUA  ?= $(shell command -v lua5.3 || command -v lua)
BOARD_ENVS := esp32-owb esp32-owb-rescue esp32-owb-debug esp32-s3-debug esp32-s3-debug-rescue esp32-s3-carrier
BENCH_ENVS := $(filter-out esp32-owb esp32-owb-rescue,$(BOARD_ENVS))
PORT_FLAG  := $(if $(PORT),--upload-port $(PORT),)

##@ General

.PHONY: help
help: ## Show this help
	@awk 'BEGIN {FS = ":.*##"; printf "\nusage: make <target> [ENV=esp32-owb] [PORT=/dev/cu.usbmodemXXXX]\n"} \
	  /^[a-zA-Z0-9_.-]+:.*##/ { printf "  \033[36m%-14s\033[0m %s\n", $$1, $$2 } \
	  /^##@/ { printf "\n\033[1m%s\033[0m\n", substr($$0, 5) }' $(MAKEFILE_LIST)

##@ Setup

.PHONY: setup
setup: setup-py setup-sys setup-hooks ## Everything a clean clone needs: Python tools, cppcheck, clang-tidy, Lua, git hooks

.PHONY: setup-py
setup-py: ## PlatformIO, intelhex and gcovr via pipx when present (NO_PIPX=1 forces pip)
	@if [ -z "$$NO_PIPX" ] && command -v pipx >/dev/null; then \
	  pipx install platformio && pipx upgrade platformio && pipx inject platformio intelhex \
	    && pipx install gcovr && pipx upgrade gcovr; \
	else \
	  python3 -m pip install --upgrade platformio intelhex gcovr; \
	fi

.PHONY: setup-sys
setup-sys: ## cppcheck, clang-tidy and Lua via brew, or apt-get with sudo
	@if command -v brew >/dev/null; then brew install cppcheck llvm lua; \
	elif command -v apt-get >/dev/null; then sudo apt-get update -qq && sudo apt-get install -y -qq cppcheck clang-tidy lua5.3; \
	else echo "setup-sys: install cppcheck, clang-tidy and Lua 5.3+ by hand"; exit 1; fi

.PHONY: setup-hooks
setup-hooks: ## Link the git hooks: invariant ledger, commit-msg lint
	@./scripts/hooks/install.sh

##@ Build

.PHONY: build
build: ## Compile one env (ENV=esp32-owb)
	$(PIO) run -e $(ENV)

.PHONY: build-all
build-all: ## Compile every board env, the CI matrix
	$(PIO) run $(foreach e,$(BOARD_ENVS),-e $(e))

.PHONY: build-minimal
build-minimal: ## esp32-owb with every optional module off; proves core builds alone
	@set -e; cp src/thesada_config.h src/thesada_config.h.bak; \
	  trap 'mv -f src/thesada_config.h.bak src/thesada_config.h' EXIT; \
	  sed -E 's,^([[:space:]]*)#define (ENABLE_(TEMPERATURE|ADS1115|BATTERY|PMU|SD|CELLULAR|TELEGRAM|WEBSERVER|SCRIPTENGINE|IDENTITY)),\1// #define \2,' src/thesada_config.h.bak > src/thesada_config.h; \
	  $(PIO) run -e esp32-owb; \
	  mkdir -p build && cp .pio/build/esp32-owb/firmware.bin build/firmware_minimal.bin

.PHONY: dist
dist: ## What a release ships: every env, the minimal build, build/boards/
	mkdir -p build/boards
	$(PIO) run -e esp32-owb
	cp .pio/build/esp32-owb/firmware.bin build/boards/firmware-owb.bin
	cp build/firmware.json build/boards/firmware-owb.json
	$(PIO) run -e esp32-owb-rescue
	cp .pio/build/esp32-owb-rescue/firmware.bin build/boards/firmware-owb-rescue.bin
	$(PIO) run $(foreach e,$(BENCH_ENVS),-e $(e))
	$(MAKE) build-minimal
	cp build/boards/firmware-owb.bin build/firmware.bin
	cp build/boards/firmware-owb.json build/firmware.json

.PHONY: compiledb
compiledb: ## compile_commands.json for ENV, for clang-tidy and editors
	$(PIO) run -e $(ENV) -t compiledb

##@ Test

.PHONY: test
test: test-native test-lua ## Host-side suites: native unit tests + Lua rules harness

.PHONY: test-native
test-native: ## Unity tests over the pure policy headers, no board (TEST=test_rollback for one suite)
	$(PIO) test -e native $(if $(TEST),-f $(TEST),)

.PHONY: test-lua
test-lua: ## Lua rules harness against the mocked bindings
	@test -n "$(LUA)" || { echo "test-lua: no lua5.3 or lua on PATH - run make setup-sys"; exit 1; }
	$(LUA) tests/lua/run_all.lua

.PHONY: coverage
coverage: ## Per-file line-coverage floors from scripts/coverage-floors.txt
	./scripts/check-coverage.sh

.PHONY: lint
lint: ## cppcheck + clang-tidy on the pure units, then the path-safety and lib-deps gates
	./scripts/static-check.sh
	./scripts/tidy-check.sh
	./scripts/check-path-safety.sh
	./scripts/check-lib-deps.sh

.PHONY: tidy
tidy: ## clang-tidy alone (.clang-tidy), via the native test units
	./scripts/tidy-check.sh

.PHONY: deps
deps: ## Compare pinned libraries against the registries (network)
	python3 scripts/check_deps.py

##@ Device

.PHONY: flash
flash: ## Build and upload ENV (PORT optional)
	$(PIO) run -e $(ENV) -t upload $(PORT_FLAG)

.PHONY: flashfs
flashfs: ## Upload data/ as the LittleFS image
	$(PIO) run -e $(ENV) -t uploadfs $(PORT_FLAG)

.PHONY: monitor
monitor: ## Serial console at 115200
	$(PIO) device monitor $(if $(PORT),-p $(PORT),)

.PHONY: run
run: flash monitor ## Flash ENV, then open the serial console

.PHONY: provision
provision: ## First flash of a new board: upload + seed the AP passphrase (needs PORT)
	@test -n "$(PORT)" || { echo "provision: PORT=/dev/cu.usbmodemXXXX required"; exit 1; }
	./scripts/flash-provision.sh --env $(ENV) --port $(PORT)

##@ Housekeeping

.PHONY: clean
clean: ## Remove build output: .pio, build/, compile_commands.json
	rm -rf .pio build compile_commands.json
