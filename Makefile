CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Iinclude -fPIC
LDFLAGS ?=

BUILD_DIR := build
LIB_NAME := libjsonpath.so

SRC := src/json.cpp src/jsonpath.cpp
OBJ := $(SRC:src/%.cpp=$(BUILD_DIR)/%.o)

TEST_BIN := $(BUILD_DIR)/jsonpath_tests
TEST_SRC := tests/test_main.cpp tests/jsonpath_tests.cpp

all: $(BUILD_DIR)/$(LIB_NAME)

test: $(TEST_BIN)
	./$(TEST_BIN)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: src/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/$(LIB_NAME): $(OBJ)
	$(CXX) -shared -o $@ $^

$(TEST_BIN): $(OBJ) $(TEST_SRC) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -pthread $(TEST_SRC) $(OBJ) -lgtest -o $@

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all test clean
