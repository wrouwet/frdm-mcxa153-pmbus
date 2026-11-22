# Build, test, flash and monitor the PMBus target for the NXP FRDM-MCXA153.
# Run `make help` for usage.

REPO := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
WS   := $(abspath $(REPO)/..)

BOARD  ?= frdm_mcxa153
APP    ?= $(REPO)/app
PORT   ?= /dev/ttyACM0
RUNNER ?= linkserver
BUILD_DIR := $(REPO)/build/$(BOARD)

# NXP's LinkServer installer puts the flash tool here without adding it to PATH.
LINKSERVER_DIR ?= /usr/local/LinkServer
export PATH := $(WS)/.venv/bin:$(LINKSERVER_DIR):$(PATH)
export ZEPHYR_BASE := $(WS)/zephyr
export ZEPHYR_SDK_INSTALL_DIR := $(WS)/.tools/zephyr-sdk-1.0.1

HOST_CC     ?= gcc
HOST_CFLAGS := -std=c11 -Wall -Wextra -Werror -Wshadow -g -fsanitize=address,undefined
HOST_TEST   := $(REPO)/build/host/test_pmbus
PMBUS_SRCS  := $(wildcard $(REPO)/lib/pmbus/src/*.c)

.DEFAULT_GOAL := help
.PHONY: help setup build flash monitor menuconfig test sim-lib pytest clean

help:
	@echo "First-time setup:"
	@echo "  sudo scripts/install-host-deps.sh   Install Ubuntu packages and udev rule"
	@echo "  make setup                          Install Python, west, Zephyr, toolchain"
	@echo
	@echo "Daily use:"
	@echo "  make test                           Host unit tests, then the device suite on the simulator"
	@echo "  make pytest PMBUS_TRANSPORT=...     The device suite on sim, hub:<port> or linux:<bus>"
	@echo "  make build                          Build the firmware"
	@echo "  make flash                          Build and flash [RUNNER=linkserver|pyocd]"
	@echo "  make monitor                        Open the shell [PORT=$(PORT)]"
	@echo "  make menuconfig                     Edit the Kconfig for a build"
	@echo "  make clean                          Delete all build output"

setup:
	$(REPO)/scripts/bootstrap.sh

build:
	cd $(WS) && west build -b $(BOARD) -d $(BUILD_DIR) $(APP)

flash: build
	cd $(WS) && west flash -d $(BUILD_DIR) --runner $(RUNNER)

monitor:
	picocom -b 115200 $(PORT)

menuconfig:
	cd $(WS) && west build -d $(BUILD_DIR) -t menuconfig

$(HOST_TEST): $(PMBUS_SRCS) $(REPO)/tests/host/test_pmbus.c $(wildcard $(REPO)/lib/pmbus/include/pmbus/*.h)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) -I$(REPO)/lib/pmbus/include $(PMBUS_SRCS) $(REPO)/tests/host/test_pmbus.c -o $@

SIM_LIB := $(REPO)/build/host/libpmbus_sim.so
PMBUS_TRANSPORT ?= sim

$(SIM_LIB): $(PMBUS_SRCS) $(REPO)/tests/sim/sim_shim.c $(wildcard $(REPO)/lib/pmbus/include/pmbus/*.h)
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O1 -g -shared -fPIC -I$(REPO)/lib/pmbus/include \
		$(PMBUS_SRCS) $(REPO)/tests/sim/sim_shim.c -o $@

sim-lib: $(SIM_LIB)

pytest: $(if $(filter sim%,$(PMBUS_TRANSPORT)),$(SIM_LIB))
	PMBUS_TRANSPORT=$(PMBUS_TRANSPORT) python3 -m pytest -q $(REPO)/tests/hw

test: $(HOST_TEST) $(SIM_LIB)
	$(HOST_TEST)
	$(MAKE) --no-print-directory pytest PMBUS_TRANSPORT=sim

clean:
	rm -rf $(REPO)/build
