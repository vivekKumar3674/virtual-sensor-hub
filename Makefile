# Top-level Makefile
#   make            build user-space programs (daemon, client, fake sensor)
#   make driver     build the kernel module (needs kernel headers)
#   make test       run the user-space integration test (no kernel module needed)
#   make clean

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2 -pthread
INC      := -Idriver
BUILD    := build

all: $(BUILD)/sensor_daemon $(BUILD)/monitor_client $(BUILD)/fake_sensor

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/sensor_daemon: daemon/main.cpp daemon/sensor.hpp daemon/state.hpp \
        daemon/thread_safe_queue.hpp daemon/logger.hpp daemon/server.hpp \
        driver/vsensor_ioctl.h | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INC) daemon/main.cpp -o $@

$(BUILD)/monitor_client: client/client.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) client/client.cpp -o $@

$(BUILD)/fake_sensor: tests/fake_sensor.cpp driver/vsensor_ioctl.h | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INC) tests/fake_sensor.cpp -o $@

driver:
	$(MAKE) -C driver

test: all
	bash tests/run_tests.sh

clean:
	rm -rf $(BUILD)
	-$(MAKE) -C driver clean 2>/dev/null

.PHONY: all driver test clean
