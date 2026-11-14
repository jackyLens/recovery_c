CC      ?= cc
AR      ?= ar
CFLAGS  ?= -std=c99 -Wall -Wextra -O2
CPPFLAGS = -Iinclude
LDLIBS   = -lm

PYREF   ?= ../recovery_v1
PYTHON  ?= python3

BUILD = build
LIB   = $(BUILD)/librecovery.a
SRC   = $(wildcard src/*.c)
OBJ   = $(SRC:src/%.c=$(BUILD)/%.o)

EXAMPLE = $(BUILD)/night_example
T_UNIT  = $(BUILD)/test_recovery
T_PAR   = $(BUILD)/test_parity
CASES   = tests/data/parity_cases.json

.PHONY: all lib example test parity parity-data compare-example clean

all: lib example $(T_UNIT) $(T_PAR)

lib: $(LIB)

example: $(EXAMPLE)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c include/recovery.h src/rc_internal.h | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(LIB): $(OBJ)
	$(AR) rcs $@ $^

$(EXAMPLE): examples/night_example.c examples/json.c examples/json.h $(LIB)
	$(CC) $(CFLAGS) $(CPPFLAGS) examples/night_example.c examples/json.c $(LIB) $(LDLIBS) -o $@

$(T_UNIT): tests/test_recovery.c tests/synth.c tests/synth.h $(LIB)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Isrc tests/test_recovery.c tests/synth.c $(LIB) $(LDLIBS) -o $@

$(T_PAR): tests/test_parity.c examples/json.c examples/json.h $(LIB)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Iexamples tests/test_parity.c examples/json.c $(LIB) $(LDLIBS) -o $@

# 单元测试 + 与 Python 版的一致性比对（用仓库里已导出的用例，不需要 Python）
test: $(T_UNIT) $(T_PAR)
	$(T_UNIT)
	$(T_PAR) $(CASES)

parity: $(T_PAR)
	$(T_PAR) $(CASES)

# 用 Python 参考实现重新导出一致性用例（需要 numpy）
parity-data:
	$(PYTHON) tools/export_parity_cases.py $(PYREF) $(CASES)

# 示例输入分别过 Python 和 C，输出逐字节比较
compare-example: $(EXAMPLE)
	$(PYTHON) $(PYREF)/examples/run_example.py > $(BUILD)/example_py.json
	$(EXAMPLE) $(PYREF)/examples/night_input_example.json > $(BUILD)/example_c.json
	diff $(BUILD)/example_py.json $(BUILD)/example_c.json && echo "example: C 与 Python 输出一致"

clean:
	rm -rf $(BUILD)
