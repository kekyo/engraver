CC ?= gcc
AR ?= ar
RM ?= rm -f
MKDIR_P ?= mkdir -p

CFLAGS ?= -std=c99 -Wall -Wextra -pedantic -O2
CPPFLAGS := -Ilibengraver/include -Ilibengraver/vendor/yyjson
PICFLAGS := -fPIC
LDFLAGS ?=

BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
LIB_OBJ := $(OBJ_DIR)/libengraver/engraver.o $(OBJ_DIR)/libengraver/yyjson.o
CLI_OBJ := $(OBJ_DIR)/engraver/main.o
TEST_OBJ := $(OBJ_DIR)/tests/test_all.o

.PHONY: all clean test

all: $(BUILD_DIR)/libengraver.a $(BUILD_DIR)/libengraver.so $(BUILD_DIR)/engraver

$(BUILD_DIR):
	$(MKDIR_P) $(BUILD_DIR)

$(OBJ_DIR)/libengraver $(OBJ_DIR)/engraver $(OBJ_DIR)/tests $(BUILD_DIR)/tests:
	$(MKDIR_P) $@

$(OBJ_DIR)/libengraver/engraver.o: libengraver/src/engraver.c libengraver/include/engraver.h | $(OBJ_DIR)/libengraver
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PICFLAGS) -c $< -o $@

$(OBJ_DIR)/libengraver/yyjson.o: libengraver/vendor/yyjson/yyjson.c libengraver/vendor/yyjson/yyjson.h | $(OBJ_DIR)/libengraver
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PICFLAGS) -DYYJSON_DISABLE_NON_STANDARD=1 -c $< -o $@

$(BUILD_DIR)/libengraver.a: $(LIB_OBJ) | $(BUILD_DIR)
	$(AR) rcs $@ $(LIB_OBJ)

$(BUILD_DIR)/libengraver.so: $(LIB_OBJ) | $(BUILD_DIR)
	$(CC) -shared -o $@ $(LIB_OBJ) $(LDFLAGS)

$(OBJ_DIR)/engraver/main.o: engraver/main.c libengraver/include/engraver.h | $(OBJ_DIR)/engraver
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/engraver: $(CLI_OBJ) $(BUILD_DIR)/libengraver.a | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(CLI_OBJ) $(BUILD_DIR)/libengraver.a $(LDFLAGS)

$(OBJ_DIR)/tests/test_all.o: tests/test_all.c libengraver/include/engraver.h | $(OBJ_DIR)/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/tests/test_all: $(TEST_OBJ) $(BUILD_DIR)/libengraver.a | $(BUILD_DIR)/tests
	$(CC) $(CFLAGS) -o $@ $(TEST_OBJ) $(BUILD_DIR)/libengraver.a $(LDFLAGS)

test: all $(BUILD_DIR)/tests/test_all
	$(BUILD_DIR)/tests/test_all

clean:
	$(RM) -r $(BUILD_DIR)
