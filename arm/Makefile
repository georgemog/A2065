CC  = gcc
CXX = g++
AR  = ar

# Cross-compile for MiSTer ARMv7
CROSS_PREFIX = /opt/armV7-linux-gcc/bin/arm-none-linux-gnueabihf-
CROSS_CXX = $(CROSS_PREFIX)g++
CROSS_CC  = $(CROSS_PREFIX)gcc

CXXFLAGS_COMMON = -std=c++11 -Wall -Wextra -O2 -I include
CFLAGS_COMMON   = -std=c99   -Wall -Wextra -O2 -I include

CXXFLAGS_NATIVE = $(CXXFLAGS_COMMON)
CXXFLAGS_ARM    = $(CXXFLAGS_COMMON) \
                  -march=armv7-a -mtune=cortex-a9 \
                  -mfloat-abi=hard -mfpu=neon
CXXFLAGS_DDR3   = $(CXXFLAGS_ARM) -DBOARDRAM_REMOTE

LDFLAGS       = -lpthread
LDFLAGS_LINUX = -lrt

BUILD_NATIVE = build/native
BUILD_ARM    = build/arm
BUILD_DDR3   = build/ddr3
BUILD_DOORBELL = build/doorbell

SRCS = src/mac.cpp \
       src/registers.cpp \
       src/rings.cpp \
       src/ethernet.cpp \
       src/bridge.cpp \
       src/bridge_sim.cpp \
       src/crc32.cpp \
       src/main.cpp

SRCS_DDR3 = src/mac.cpp \
            src/registers.cpp \
            src/rings.cpp \
            src/ethernet.cpp \
            src/crc32.cpp \
            src/boardram_remote.cpp \
            src/main_ddr3.cpp

SRCS_DOORBELL = src/mac.cpp \
                src/registers.cpp \
                src/rings.cpp \
                src/ethernet.cpp \
                src/crc32.cpp \
                src/main_doorbell.cpp

OBJS_NATIVE   = $(patsubst src/%.cpp, $(BUILD_NATIVE)/%.o, $(SRCS))
OBJS_ARM      = $(patsubst src/%.cpp, $(BUILD_ARM)/%.o, $(SRCS))
OBJS_DDR3     = $(patsubst src/%.cpp, $(BUILD_DDR3)/%.o, $(SRCS_DDR3))
OBJS_DOORBELL = $(patsubst src/%.cpp, $(BUILD_DOORBELL)/%.o, $(SRCS_DOORBELL))

TEST_SRCS = src/mac_test.cpp \
            src/csr_test.cpp \
            src/rings_test.cpp \
            src/bridge_test.cpp

.PHONY: all native arm ddr3 doorbell test status clean deploy_ddr3 deploy_doorbell boardram_flat_test

all: native

native: $(BUILD_NATIVE)/a2065d

status: $(BUILD_ARM)/bridge_status

arm: $(BUILD_ARM)/a2065d

ddr3: $(BUILD_DDR3)/a2065d_ddr3

doorbell: $(BUILD_DOORBELL)/a2065d_doorbell

$(BUILD_NATIVE)/a2065d: $(OBJS_NATIVE)
	@mkdir -p $(BUILD_NATIVE)
	$(CXX) $(CXXFLAGS_NATIVE) $^ -o $@ $(LDFLAGS)

$(BUILD_ARM)/a2065d: $(OBJS_ARM)
	@mkdir -p $(BUILD_ARM)
	$(CROSS_CXX) $(CXXFLAGS_ARM) $^ -o $@ $(LDFLAGS) $(LDFLAGS_LINUX)

$(BUILD_DDR3)/a2065d_ddr3: $(OBJS_DDR3)
	@mkdir -p $(BUILD_DDR3)
	$(CROSS_CXX) $(CXXFLAGS_DDR3) $^ -o $@ $(LDFLAGS) $(LDFLAGS_LINUX)

$(BUILD_DOORBELL)/a2065d_doorbell: $(OBJS_DOORBELL)
	@mkdir -p $(BUILD_DOORBELL)
	$(CROSS_CXX) $(CXXFLAGS_ARM) $^ -o $@ $(LDFLAGS) $(LDFLAGS_LINUX)

$(BUILD_NATIVE)/%.o: src/%.cpp
	@mkdir -p $(BUILD_NATIVE)
	$(CXX) $(CXXFLAGS_NATIVE) -c $< -o $@

$(BUILD_ARM)/%.o: src/%.cpp
	@mkdir -p $(BUILD_ARM)
	$(CROSS_CXX) $(CXXFLAGS_ARM) -c $< -o $@

$(BUILD_DDR3)/%.o: src/%.cpp
	@mkdir -p $(BUILD_DDR3)
	$(CROSS_CXX) $(CXXFLAGS_DDR3) -c $< -o $@

$(BUILD_DOORBELL)/%.o: src/%.cpp
	@mkdir -p $(BUILD_DOORBELL)
	$(CROSS_CXX) $(CXXFLAGS_ARM) -c $< -o $@

test: $(BUILD_NATIVE)/mac_test $(BUILD_NATIVE)/csr_test $(BUILD_NATIVE)/rings_test $(BUILD_NATIVE)/bridge_test $(BUILD_NATIVE)/boardram_flat_test
	$(BUILD_NATIVE)/mac_test
	$(BUILD_NATIVE)/csr_test
	$(BUILD_NATIVE)/rings_test
	$(BUILD_NATIVE)/bridge_test
	$(BUILD_NATIVE)/boardram_flat_test

boardram_flat_test: $(BUILD_NATIVE)/boardram_flat_test
	$(BUILD_NATIVE)/boardram_flat_test

$(BUILD_NATIVE)/boardram_flat_test: src/boardram_flat_test.cpp
	@mkdir -p $(BUILD_NATIVE)
	$(CXX) $(CXXFLAGS_NATIVE) $< -o $@

$(BUILD_NATIVE)/mac_test: src/mac.cpp src/mac_test.cpp
	@mkdir -p $(BUILD_NATIVE)
	$(CXX) $(CXXFLAGS_NATIVE) $^ -o $@

$(BUILD_NATIVE)/csr_test: src/registers.cpp src/csr_test.cpp
	@mkdir -p $(BUILD_NATIVE)
	$(CXX) $(CXXFLAGS_NATIVE) $^ -o $@

$(BUILD_NATIVE)/rings_test: src/mac.cpp src/registers.cpp src/rings.cpp src/crc32.cpp src/rings_test.cpp
	@mkdir -p $(BUILD_NATIVE)
	$(CXX) $(CXXFLAGS_NATIVE) $^ -o $@

$(BUILD_NATIVE)/bridge_test: src/mac.cpp src/registers.cpp src/rings.cpp src/crc32.cpp src/bridge_sim.cpp src/bridge_test.cpp
	@mkdir -p $(BUILD_NATIVE)
	$(CXX) $(CXXFLAGS_NATIVE) $^ -o $@

clean:
	rm -rf build/

deploy: arm
	@echo "Deploying to MiSTer..."
	scp $(BUILD_ARM)/a2065d root@mister:/usr/local/bin/a2065d
	ssh root@mister 'chmod +x /usr/local/bin/a2065d'
	@echo "Deploy complete."

deploy_ddr3: ddr3
	@echo "Deploying DDR3 daemon to MiSTer..."
	scp $(BUILD_DDR3)/a2065d_ddr3 root@mister:/media/fat/trans/a2065d_ddr3
	ssh root@mister 'chmod +x /media/fat/trans/a2065d_ddr3'
	@echo "Deploy complete."

deploy_doorbell: doorbell
	@echo "Deploying doorbell daemon to MiSTer..."
	scp $(BUILD_DOORBELL)/a2065d_doorbell root@mister:/media/fat/trans/a2065d_ddr3
	ssh root@mister 'chmod +x /media/fat/trans/a2065d_ddr3'
	@echo "Deploy complete."

deploy_status: status
	scp $(BUILD_ARM)/bridge_status root@mister:/usr/local/bin/bridge_status

$(BUILD_ARM)/bridge_status: src/bridge_status.cpp
	@mkdir -p $(BUILD_ARM)
	$(CROSS_CXX) $(CXXFLAGS_ARM) $< -o $@
