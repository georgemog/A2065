CC  = gcc
CXX = g++
AR  = ar

# Cross-compile for MiSTer ARMv7
CROSS_CXX = arm-none-linux-gnueabihf-g++
CROSS_CC  = arm-none-linux-gnueabihf-gcc

CXXFLAGS_COMMON = -std=c++11 -Wall -Wextra -O2 -I include
CFLAGS_COMMON   = -std=c99   -Wall -Wextra -O2 -I include

CXXFLAGS_NATIVE = $(CXXFLAGS_COMMON)
CXXFLAGS_ARM    = $(CXXFLAGS_COMMON) \
                  -march=armv7-a -mtune=cortex-a9 \
                  -mfloat-abi=hard -mfpu=neon

LDFLAGS = -lpthread

BUILD_NATIVE = build/native
BUILD_ARM    = build/arm

SRCS = src/mac.cpp \
       src/registers.cpp \
       src/rings.cpp \
       src/ethernet.cpp \
       src/bridge.cpp \
       src/main.cpp

OBJS_NATIVE = $(patsubst src/%.cpp, $(BUILD_NATIVE)/%.o, $(SRCS))
OBJS_ARM    = $(patsubst src/%.cpp, $(BUILD_ARM)/%.o, $(SRCS))

TEST_SRCS = src/mac_test.cpp \
            src/csr_test.cpp \
            src/rings_test.cpp

.PHONY: all native arm test clean

all: native

native: $(BUILD_NATIVE)/a2065d

arm: $(BUILD_ARM)/a2065d

$(BUILD_NATIVE)/a2065d: $(OBJS_NATIVE)
	@mkdir -p $(BUILD_NATIVE)
	$(CXX) $(CXXFLAGS_NATIVE) $^ -o $@ $(LDFLAGS)

$(BUILD_ARM)/a2065d: $(OBJS_ARM)
	@mkdir -p $(BUILD_ARM)
	$(CROSS_CXX) $(CXXFLAGS_ARM) $^ -o $@ $(LDFLAGS)

$(BUILD_NATIVE)/%.o: src/%.cpp
	@mkdir -p $(BUILD_NATIVE)
	$(CXX) $(CXXFLAGS_NATIVE) -c $< -o $@

$(BUILD_ARM)/%.o: src/%.cpp
	@mkdir -p $(BUILD_ARM)
	$(CROSS_CXX) $(CXXFLAGS_ARM) -c $< -o $@

test: $(BUILD_NATIVE)/mac_test $(BUILD_NATIVE)/csr_test $(BUILD_NATIVE)/rings_test
	$(BUILD_NATIVE)/mac_test
	$(BUILD_NATIVE)/csr_test
	$(BUILD_NATIVE)/rings_test

$(BUILD_NATIVE)/mac_test: src/mac.cpp src/mac_test.cpp
	@mkdir -p $(BUILD_NATIVE)
	$(CXX) $(CXXFLAGS_NATIVE) $^ -o $@

$(BUILD_NATIVE)/csr_test: src/registers.cpp src/csr_test.cpp
	@mkdir -p $(BUILD_NATIVE)
	$(CXX) $(CXXFLAGS_NATIVE) $^ -o $@

$(BUILD_NATIVE)/rings_test: src/mac.cpp src/registers.cpp src/rings.cpp src/rings_test.cpp
	@mkdir -p $(BUILD_NATIVE)
	$(CXX) $(CXXFLAGS_NATIVE) $^ -o $@

clean:
	rm -rf build/

deploy: arm
	@echo "Deploying to MiSTer..."
	scp $(BUILD_ARM)/a2065d root@mister:/usr/local/bin/a2065d
	ssh root@mister 'chmod +x /usr/local/bin/a2065d'
	@echo "Deploy complete."
