# Makefile for ECE/CS 5544 Assignment 3 - Unified LLVM Pass
# LLVM 21 / New Pass Manager

CXX   := clang++
OPT   := opt
CLANG := clang
LLI   := lli

LLVM_CXXFLAGS := $(shell llvm-config --cxxflags)
CXXFLAGS      := $(LLVM_CXXFLAGS) -fno-rtti -fPIC -O2 -std=c++20
TEST_FLAGS    := -fno-discard-value-names -Xclang -disable-O0-optnone -O0 -emit-llvm 

TARGET := unifiedpass.so
SRC    := unifiedpass.cpp

# Shorthand: load our plugin
PLUGIN := -load-pass-plugin=./$(TARGET)

.PHONY: all clean test-dom test-dce test-licm profile-dce profile-licm

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -shared -o $@ $<

clean:
	rm -f $(TARGET) \
	      tests/Dominators/*.bc tests/Dominators/*.ll \
	      tests/DCE/*.bc tests/DCE/*.ll \
	      tests/LICM/*.bc tests/LICM/*.ll

%.bc: %.c
	$(CLANG) -O0 -emit-llvm -c -o $@ $<
	$(OPT) -passes=mem2reg -o $@ $@

# -------------------------------------------------------
#  DOMINATORS
# -------------------------------------------------------
test-dom: $(TARGET) tests/Dominators/dom_test1.bc tests/Dominators/dom_test2.bc
	@echo "=== Dominators Pass - Test 1 ==="
	$(OPT) $(PLUGIN) -passes=dominators -S -o /dev/null \
	    < tests/Dominators/dom_test1.bc
	@echo ""
	@echo "=== Dominators Pass - Test 2 ==="
	$(OPT) $(PLUGIN) -passes=dominators -S -o /dev/null \
	    < tests/Dominators/dom_test2.bc

tests/Dominators/dom_test1.bc: tests/Dominators/dom_test1.c
	$(CLANG) $(TEST_FLAGS) -c -o $@ $<
	$(OPT) -passes=mem2reg -o $@ $@

tests/Dominators/dom_test2.bc: tests/Dominators/dom_test2.c
	$(CLANG) $(TEST_FLAGS) -c -o $@ $<
	$(OPT) -passes=mem2reg -o $@ $@

# -------------------------------------------------------
#  DCE
# -------------------------------------------------------
test-dce: $(TARGET) tests/DCE/dce_test1.bc tests/DCE/dce_test2.bc
	@echo "=== DCE Pass - Test 1 ==="
	$(OPT) $(PLUGIN) -passes=dead-code-elimination -S -o /dev/null \
	    < tests/DCE/dce_test1.bc
	@echo ""
	@echo "=== DCE Pass - Test 2 ==="
	$(OPT) $(PLUGIN) -passes=dead-code-elimination -S -o /dev/null \
	    < tests/DCE/dce_test2.bc

tests/DCE/dce_test1.bc: tests/DCE/dce_test1.c
	$(CLANG) $(TEST_FLAGS) -c -o $@ $<
	$(OPT) -passes=mem2reg -o $@ $@

tests/DCE/dce_test2.bc: tests/DCE/dce_test2.c
	$(CLANG) $(TEST_FLAGS) -c -o $@ $<
	$(OPT) -passes=mem2reg -o $@ $@

# -------------------------------------------------------
#  LICM
# -------------------------------------------------------
test-licm: $(TARGET) \
           tests/LICM/licm_test1.bc \
           tests/LICM/licm_test2.bc \
           tests/LICM/licm_test3.bc
	@echo "=== LICM Pass - Test 1 (basic hoist) ==="
	$(OPT) $(PLUGIN) \
	    -passes='loop-simplify,loop-invariant-code-motion' \
	    -S -o tests/LICM/licm_test1_opt.ll \
	    < tests/LICM/licm_test1.bc
	$(OPT) -passes=licm \
		-S -o tests/LICM/licm_test1_llvm.ll \
		< tests/LICM/licm_test1.bc
	diff tests/LICM/licm_test1_llvm.ll tests/LICM/licm_test1_opt.ll || true
	@echo ""
	@echo "=== LICM Pass - Test 2 (nested loops) ==="
	$(OPT) $(PLUGIN) \
	    -passes='loop-simplify,loop-invariant-code-motion' \
	    -S -o tests/LICM/licm_test2_opt.ll \
	    < tests/LICM/licm_test2.bc
	$(OPT) -passes=licm \
		-S -o tests/LICM/licm_test2_llvm.ll \
		< tests/LICM/licm_test2.bc
	diff tests/LICM/licm_test2_llvm.ll tests/LICM/licm_test2_opt.ll || true
	@echo ""
	@echo "=== LICM Pass - Test 3 (zero-iteration / landing pad) ==="
	$(OPT) $(PLUGIN) \
	    -passes='loop-simplify,loop-invariant-code-motion' \
	    -S -o tests/LICM/licm_test3_opt.ll \
	    < tests/LICM/licm_test3.bc
	$(OPT) -passes=licm \
		-S -o tests/LICM/licm_test3_llvm.ll \
		< tests/LICM/licm_test3.bc
	diff tests/LICM/licm_test3_llvm.ll tests/LICM/licm_test3_opt.ll || true

tests/LICM/licm_test1.bc: tests/LICM/licm_test1.c
	$(CLANG) $(TEST_FLAGS) -c -o $@ $<
	$(OPT) -passes=mem2reg -o $@ $@

tests/LICM/licm_test2.bc: tests/LICM/licm_test2.c
	$(CLANG) $(TEST_FLAGS) -c -o $@ $<
	$(OPT) -passes=mem2reg -o $@ $@

tests/LICM/licm_test3.bc: tests/LICM/licm_test3.c
	$(CLANG) $(TEST_FLAGS) -c -o $@ $<
	$(OPT) -passes=mem2reg -o $@ $@

# -------------------------------------------------------
#  Profiling: dynamic instruction count
# -------------------------------------------------------
profile-dce: $(TARGET) tests/DCE/dce_profile.bc
	@echo "--- DCE: instruction count BEFORE ---"
	$(LLI) -stats -force-interpreter tests/DCE/dce_profile.bc 2>&1 | \
	    grep "dynamic instructions"
	$(OPT) $(PLUGIN) -passes=dead-code-elimination \
	    -o tests/DCE/dce_profile_opt.bc < tests/DCE/dce_profile.bc
	@echo "--- DCE: instruction count AFTER ---"
	$(LLI) -stats -force-interpreter tests/DCE/dce_profile_opt.bc 2>&1 | \
	    grep "dynamic instructions"

tests/DCE/dce_profile.bc: tests/DCE/dce_test1.c
	$(CLANG) -O0 -emit-llvm -c -o $@ $<

profile-licm: $(TARGET) tests/LICM/licm_test1.bc
	@echo "--- LICM: instruction count BEFORE ---"
	$(LLI) -stats -force-interpreter tests/LICM/licm_test1.bc 2>&1 | \
	    grep "dynamic instructions"
	$(OPT) $(PLUGIN) \
	    -passes='loop-simplify,loop-invariant-code-motion' \
	    -o tests/LICM/licm_test1_opt.bc < tests/LICM/licm_test1.bc
	@echo "--- LICM: instruction count AFTER ---"
	$(LLI) -stats -force-interpreter tests/LICM/licm_test1_opt.bc 2>&1 | \
	    grep "dynamic instructions"
