# Host simulator for the reTerminal E1002 dashboard.
#
#   make            build sim/build/simulate
#   make fixtures   regenerate sim/fixtures/*.json
#   make run        render the typical-day fixture
#   make check      render every fixture and fail on any panel-level problem
#   make clean      remove build output and rendered PNGs

SIM_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
ROOT := $(SIM_DIR)..
VENDOR := $(SIM_DIR)vendor
BUILD := $(SIM_DIR)build
OUT := $(SIM_DIR)out

CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter

INCLUDES := \
  -I$(SIM_DIR)shim \
  -I$(VENDOR) \
  -I$(VENDOR)/Adafruit_GFX \
  -I$(SIM_DIR)

# ArduinoJson must not try to pull in the real Arduino core; the shim only
# covers what Adafruit_GFX needs.
# ARDUINO must be set on the command line, not in the shim: Adafruit_GFX.h tests
# it to choose between Arduino.h and the pre-1.0 WProgram.h before including
# either.
DEFINES := \
  -DARDUINO=10819 \
  -DARDUINOJSON_ENABLE_ARDUINO_STRING=0 \
  -DARDUINOJSON_ENABLE_ARDUINO_STREAM=0 \
  -DARDUINOJSON_ENABLE_ARDUINO_PRINT=0 \
  -DARDUINOJSON_ENABLE_PROGMEM=0

# strlcpy is a BSD extension: present on macOS, and on glibc only since 2.38.
HAVE_STRLCPY := $(shell printf '\#include <string.h>\nint main(){char d[4];strlcpy(d,"x",4);return 0;}' \
  | $(CXX) -x c++ -o /dev/null - 2>/dev/null && echo yes || echo no)
ifeq ($(HAVE_STRLCPY),no)
DEFINES += -DSIM_PROVIDE_STRLCPY
endif

SOURCES := \
  $(SIM_DIR)simulate.cpp \
  $(SIM_DIR)appearance.cpp \
  $(SIM_DIR)png.cpp \
  $(SIM_DIR)shim/base64.cpp \
  $(VENDOR)/Adafruit_GFX/Adafruit_GFX.cpp

TARGET := $(BUILD)/simulate

FIXTURES := $(wildcard $(SIM_DIR)fixtures/*.json)

.PHONY: all deps fixtures run check clean

all: $(TARGET)

deps: $(VENDOR)/ArduinoJson.h

$(VENDOR)/ArduinoJson.h:
	@sh $(SIM_DIR)fetch-deps.sh

$(TARGET): deps $(SOURCES) $(SIM_DIR)panel.h $(SIM_DIR)appearance.h $(ROOT)/src/dashboard_view.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(DEFINES) $(SOURCES) -lz -o $@

fixtures:
	@node $(SIM_DIR)make-fixtures.mjs

run: $(TARGET)
	@mkdir -p $(OUT)
	@$(TARGET) $(SIM_DIR)fixtures/typical-day.json --out $(OUT)/typical-day

check: $(TARGET)
	@mkdir -p $(OUT)
	@node $(SIM_DIR)check.mjs

clean:
	@rm -rf $(BUILD) $(OUT)
