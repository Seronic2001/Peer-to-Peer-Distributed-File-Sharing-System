# Compiler and Flags

CXX = g++

CXXFLAGS = -std=c++17 -Wall -Wextra -g

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

# Source and Object File Definitions

# Automatically find all .cpp source files in each directory.
TRACKER_SRCS = $(wildcard tracker/*.cpp)
CLIENT_SRCS = $(wildcard client/*.cpp)
COMMON_SRCS = $(wildcard common/*.cpp)

# Generate corresponding .o object file names, placing them in the build directory.
TRACKER_OBJS = $(patsubst tracker/%.cpp,$(BUILD_DIR)/tracker/%.o,$(TRACKER_SRCS))
CLIENT_OBJS = $(patsubst client/%.cpp,$(BUILD_DIR)/client/%.o,$(CLIENT_SRCS))
COMMON_OBJS = $(patsubst common/%.cpp,$(BUILD_DIR)/common/%.o,$(COMMON_SRCS))


# Build Rules 

# The 'all' target is the default. 
all: $(TRACKER_EXEC) $(CLIENT_EXEC)

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

# '.PHONY' declares targets that are not actual files.
# This prevents 'make' from getting confused if a file named 'clean' or 'all' exists.
.PHONY: all clean

# The 'clean' target removes the build directory and the final executables.
# Run 'make clean' to clean up your project directory.
clean:
	@echo "==> Cleaning up project..."
	rm -rf $(BUILD_DIR)
	rm -f $(TRACKER_EXEC) $(CLIENT_EXEC)
	@echo "--> Cleanup complete."
