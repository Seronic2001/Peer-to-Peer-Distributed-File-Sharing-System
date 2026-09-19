# Compiler and Flags

CXX = g++

# Base warning set shared by every build mode.
WARNFLAGS = -Wall -Wextra

# ---------------------------------------------------------------------------
# Build modes
#
#   make            (debug, default) -g, P2P_LOG_LEVEL=LOG_DEBUG
#                                     full protocol/peer chatter enabled
#   make release    -O2, P2P_LOG_LEVEL=LOG_INFO
#                                     only necessary outputs; debug chatter
#                                     is compiled out entirely
#   make debug      explicit alias for the default
# ---------------------------------------------------------------------------

DEBUG_FLAGS = -std=c++17 $(WARNFLAGS) -g -DP2P_LOG_LEVEL=LOG_DEBUG
RELEASE_FLAGS = -std=c++17 $(WARNFLAGS) -O2 -DP2P_LOG_LEVEL=LOG_INFO

# Active mode (overridden per-target below; default = debug).
CXXFLAGS = $(DEBUG_FLAGS)

# Flags for the linker:
# -lpthread: Link against the POSIX threads library for concurrency.
# -lcrypto: Link against the OpenSSL crypto library for SHA1 hashing.
LDFLAGS = -lpthread -lcrypto

# Include paths for header files. This allows tracker/ and client/ to find common/ headers.
INCLUDE = -I./common

# Directory and Executable Definitions

# Define the build directory for all compiled object files.
BUILD_DIR = build
# Define the final executable names and locations.
TRACKER_EXEC = tracker/tracker
CLIENT_EXEC = client/client

# Test framework configuration.
TEST_BUILD_DIR = tests/build
TEST_SAN_BUILD_DIR = tests/build_san
TEST_INCLUDES = -I./common -I./tracker -I./tracker/include -I./client \
                -I./client/include -I./tests
SAN_FLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer

# Object files needed per test binary (reuses the main build's objects).
TEST_PROTOCOL_OBJS = $(BUILD_DIR)/common/protocol.o
TEST_HASHING_OBJS = $(BUILD_DIR)/common/hashing.o
TEST_TRACKER_OBJS = $(BUILD_DIR)/tracker/tracker_state.o
TEST_DOWNLOAD_OBJS = $(BUILD_DIR)/client/piece_selector.o

TEST_BINS = $(TEST_BUILD_DIR)/test_protocol \
            $(TEST_BUILD_DIR)/test_hashing \
            $(TEST_BUILD_DIR)/test_tracker_state \
            $(TEST_BUILD_DIR)/test_download_logic

TEST_SAN_BINS = $(TEST_SAN_BUILD_DIR)/test_protocol \
                $(TEST_SAN_BUILD_DIR)/test_hashing \
                $(TEST_SAN_BUILD_DIR)/test_tracker_state \
                $(TEST_SAN_BUILD_DIR)/test_download_logic

# Source and Object File Definitions

# Automatically find all .cpp source files in each directory.
TRACKER_SRCS = $(wildcard tracker/*.cpp)
CLIENT_SRCS = $(wildcard client/*.cpp)
COMMON_SRCS = $(wildcard common/*.cpp)

# Generate corresponding .o object file names, placing them in the build directory.
# Objects are rooted at $(OBJ_BASE), which the release target points at a
# separate tree; otherwise release objects would collide with debug ones.
OBJ_BASE ?= $(BUILD_DIR)
TRACKER_OBJS = $(patsubst tracker/%.cpp,$(OBJ_BASE)/tracker/%.o,$(TRACKER_SRCS))
CLIENT_OBJS = $(patsubst client/%.cpp,$(OBJ_BASE)/client/%.o,$(CLIENT_SRCS))
COMMON_OBJS = $(patsubst common/%.cpp,$(OBJ_BASE)/common/%.o,$(COMMON_SRCS))


# Build Rules 

# The 'all' target is the default (debug build with full logging).
all: debug

debug: $(TRACKER_EXEC) $(CLIENT_EXEC)

# Release build: optimized, quiet logging. OBJ_BASE is switched so release
# objects live in build_release/ and never collide with debug artifacts.
# $(TRACKER_EXEC)/$(CLIENT_EXEC) are immediate-expanded variables, so the
# target names recompute with the overridden OBJ_ROOT too.
OBJ_ROOT = $(OBJ_BASE)

REL_OBJ_BASE = $(BUILD_DIR)_release
REL_TRACKER_EXEC = tracker/tracker-release
REL_CLIENT_EXEC = client/client-release
REL_TRACKER_OBJS = $(patsubst tracker/%.cpp,$(REL_OBJ_BASE)/tracker/%.o,$(TRACKER_SRCS))
REL_CLIENT_OBJS = $(patsubst client/%.cpp,$(REL_OBJ_BASE)/client/%.o,$(CLIENT_SRCS))
REL_COMMON_OBJS = $(patsubst common/%.cpp,$(REL_OBJ_BASE)/common/%.o,$(COMMON_SRCS))

release: $(REL_TRACKER_EXEC) $(REL_CLIENT_EXEC)
	@echo "--> Release binaries built (quiet logging, -O2)."

$(REL_TRACKER_EXEC): $(REL_TRACKER_OBJS) $(REL_COMMON_OBJS)
	@echo "==> Linking tracker (release)..."
	$(CXX) $(RELEASE_FLAGS) -o $@ $^ $(LDFLAGS)

$(REL_CLIENT_EXEC): $(REL_CLIENT_OBJS) $(REL_COMMON_OBJS)
	@echo "==> Linking client (release)..."
	$(CXX) $(RELEASE_FLAGS) -o $@ $^ $(LDFLAGS)

$(REL_OBJ_BASE)/tracker/%.o: tracker/%.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling (release) $<..."
	$(CXX) $(RELEASE_FLAGS) $(INCLUDE) -c -o $@ $<

$(REL_OBJ_BASE)/client/%.o: client/%.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling (release) $<..."
	$(CXX) $(RELEASE_FLAGS) $(INCLUDE) -c -o $@ $<

$(REL_OBJ_BASE)/common/%.o: common/%.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling (release) $<..."
	$(CXX) $(RELEASE_FLAGS) $(INCLUDE) -c -o $@ $<

# ---------------------------------------------------------------------------
# Test targets
#
#   make test            Build and run the unit test suite.
#   make test_sanitize   Same suite under AddressSanitizer + UBSan.
#
# Each tests/test_*.cpp contains its own main() and links only the objects
# it exercises, so a failure isolates the offending module.
# ---------------------------------------------------------------------------

.PHONY: test test_sanitize

test: $(TEST_BINS)
	@echo "==> Running unit tests..."
	@fail=0; \
	for bin in $(TEST_BINS); do \
	  echo "--> $$bin"; \
	  timeout 60 ./$$bin || fail=$$((fail + 1)); \
	done; \
	if [ $$fail -eq 0 ]; then \
	  echo "--> All test binaries passed."; \
	else \
	  echo "--> $$fail test binary(ies) FAILED (or timed out)."; exit 1; \
	fi

test_sanitize: $(TEST_SAN_BINS)
	@echo "==> Running unit tests under ASan + UBSan..."
	@fail=0; \
	for bin in $(TEST_SAN_BINS); do \
	  echo "--> $$bin"; \
	  ASAN_OPTIONS=detect_leaks=1 timeout 120 ./$$bin || fail=$$((fail + 1)); \
	done; \
	if [ $$fail -eq 0 ]; then \
	  echo "--> Sanitized test run passed (no memory errors, no leaks)."; \
	else \
	  echo "--> $$fail sanitized test binary(ies) FAILED."; exit 1; \
	fi

# Normal-build test binaries.
$(TEST_BUILD_DIR)/test_protocol: tests/test_protocol.cpp $(TEST_PROTOCOL_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(TEST_INCLUDES) -o $@ $< $(TEST_PROTOCOL_OBJS) $(LDFLAGS)

$(TEST_BUILD_DIR)/test_hashing: tests/test_hashing.cpp $(TEST_HASHING_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(TEST_INCLUDES) -o $@ $< $(TEST_HASHING_OBJS) $(LDFLAGS)

$(TEST_BUILD_DIR)/test_tracker_state: tests/test_tracker_state.cpp $(TEST_TRACKER_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(TEST_INCLUDES) -o $@ $< $(TEST_TRACKER_OBJS) $(LDFLAGS)

$(TEST_BUILD_DIR)/test_download_logic: tests/test_download_logic.cpp $(TEST_DOWNLOAD_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(TEST_INCLUDES) -o $@ $< $(TEST_DOWNLOAD_OBJS) $(LDFLAGS)

# Pattern rule for test translation units (normal build).
$(TEST_BUILD_DIR)/%.o: tests/%.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) $(TEST_INCLUDES) -c -o $@ $<

# Sanitized variants: every object recompiled with ASan + UBSan into its own
# tree so it never contaminates the normal build artifacts.
$(TEST_SAN_BUILD_DIR)/test_%: tests/test_%.cpp $(TEST_SAN_BUILD_DIR)/common/protocol.o $(TEST_SAN_BUILD_DIR)/common/hashing.o $(TEST_SAN_BUILD_DIR)/tracker/tracker_state.o $(TEST_SAN_BUILD_DIR)/client/piece_selector.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(SAN_FLAGS) $(TEST_INCLUDES) -o $@ $< \
	  $(TEST_SAN_BUILD_DIR)/common/protocol.o \
	  $(TEST_SAN_BUILD_DIR)/common/hashing.o \
	  $(TEST_SAN_BUILD_DIR)/tracker/tracker_state.o \
	  $(TEST_SAN_BUILD_DIR)/client/piece_selector.o $(LDFLAGS)

$(TEST_SAN_BUILD_DIR)/%.o: tests/%.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling (sanitized) $<..."
	$(CXX) $(CXXFLAGS) $(SAN_FLAGS) $(TEST_INCLUDES) -c -o $@ $<

$(TEST_SAN_BUILD_DIR)/common/%.o: common/%.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling (sanitized) $<..."
	$(CXX) $(CXXFLAGS) $(SAN_FLAGS) $(INCLUDE) -c -o $@ $<

$(TEST_SAN_BUILD_DIR)/tracker/%.o: tracker/%.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling (sanitized) $<..."
	$(CXX) $(CXXFLAGS) $(SAN_FLAGS) $(INCLUDE) -c -o $@ $<

$(TEST_SAN_BUILD_DIR)/client/%.o: client/%.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling (sanitized) $<..."
	$(CXX) $(CXXFLAGS) $(SAN_FLAGS) $(INCLUDE) -c -o $@ $<

# Rule to create the tracker executable.
$(TRACKER_EXEC): $(TRACKER_OBJS) $(COMMON_OBJS)
	@echo "==> Linking tracker..."
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "--> Tracker built successfully: '$@'"

# Rule to create the client executable.
$(CLIENT_EXEC): $(CLIENT_OBJS) $(COMMON_OBJS)
	@echo "==> Linking client..."
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "--> Client built successfully: '$@'"

# Pattern rule to compile .cpp files from the 'tracker' directory into the build directory.
$(BUILD_DIR)/tracker/%.o: tracker/%.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) $(INCLUDE) -c -o $@ $<

# Pattern rule to compile .cpp files from the 'client' directory into the build directory.
$(BUILD_DIR)/client/%.o: client/%.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) $(INCLUDE) -c -o $@ $<

# Pattern rule to compile .cpp files from the 'common' directory into the build directory.
$(BUILD_DIR)/common/%.o: common/%.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) $(INCLUDE) -c -o $@ $<


# Housekeeping Rules

# Keep intermediate object files (pattern-rule prerequisites are otherwise
# deleted by make after linking, forcing avoidable rebuilds).
.SECONDARY:

# '.PHONY' declares targets that are not actual files.
# This prevents 'make' from getting confused if a file named 'clean' or 'all' exists.
.PHONY: all clean test test_sanitize integration check debug release

# End-to-end test: real trackers, real clients, real transfers.
integration: all
	@echo "==> Running integration tests..."
	@bash tests/integration_test.sh

# Everything: unit tests, sanitized unit tests, end-to-end.
check: test test_sanitize integration

# The 'clean' target removes the build directory and the final executables.
# Run 'make clean' to clean up your project directory.
clean:
	@echo "==> Cleaning up project..."
	rm -rf $(BUILD_DIR) $(BUILD_DIR)_release $(TEST_BUILD_DIR) $(TEST_SAN_BUILD_DIR)
	rm -f $(TRACKER_EXEC) $(CLIENT_EXEC) \
	      tracker/tracker-release client/client-release
	@echo "--> Cleanup complete."
