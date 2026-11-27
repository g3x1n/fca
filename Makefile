CXX ?= c++
OPENSSL_PREFIX ?= $(firstword $(wildcard /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3))
ifneq ($(OPENSSL_PREFIX),)
CPPFLAGS += -I$(OPENSSL_PREFIX)/include
LDFLAGS += -L$(OPENSSL_PREFIX)/lib
endif
CPPFLAGS += -Iinclude
CXXFLAGS ?= -O3 -DNDEBUG
CXXFLAGS += -std=c++20 -Wall -Wextra -Wpedantic
LDLIBS += -lcrypto
ARCH_FLAGS ?= -march=native
BUILD ?= build
LIB_OBJECTS := $(patsubst src/%.cpp,$(BUILD)/%.o,$(wildcard src/algebra.cpp src/protocol.cpp src/local.cpp))
OBJECTS := $(LIB_OBJECTS) $(BUILD)/main.o $(BUILD)/bench.o $(BUILD)/test.o $(BUILD)/figure_experiments.o

.PHONY: all test test-portable figure-experiments clean
all: $(BUILD)/fca $(BUILD)/fca-bench

figure-experiments: $(BUILD)/fca-bench $(BUILD)/fca-figure-experiments

$(BUILD)/fca: $(BUILD)/main.o $(LIB_OBJECTS)
$(BUILD)/fca-bench: $(BUILD)/bench.o $(LIB_OBJECTS)
$(BUILD)/fca-test: $(BUILD)/test.o $(LIB_OBJECTS)
$(BUILD)/fca $(BUILD)/fca-bench $(BUILD)/fca-test:
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/fca-figure-experiments: $(BUILD)/figure_experiments.o $(BUILD)/algebra.o $(BUILD)/protocol.o
	$(CXX) $(LDFLAGS) -pthread $^ $(LDLIBS) -o $@

$(BUILD)/%.o: src/%.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(ARCH_FLAGS) -MMD -MP -c $< -o $@

$(BUILD)/test.o: tests/test.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(ARCH_FLAGS) -MMD -MP -c $< -o $@

$(BUILD)/figure_experiments.o: experiments/figure_experiments.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(ARCH_FLAGS) -pthread -MMD -MP -c $< -o $@

test: $(BUILD)/fca-test $(BUILD)/fca
	$(BUILD)/fca-test
	python3 tests/cli.py $(BUILD)/fca

test-portable:
	$(MAKE) test BUILD=build/portable ARCH_FLAGS=-DFCA_PORTABLE

clean:
	rm -rf $(BUILD)

-include $(OBJECTS:.o=.d)
