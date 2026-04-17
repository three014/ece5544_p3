CXX          = clang++
CXXFLAGS     = -rdynamic $(shell llvm-config --cxxflags) -fPIC -g -std=c++20
LDFLAGS      = $(shell llvm-config --ldflags | tr '\n' ' ') -Wl,--exclude-libs,ALL
BUILDDIR     = build
DEPDIR       = $(BUILDDIR)/.deps
DEPFLAGS     = -MT $@ -MMD -MP -MF $(DEPDIR)/$*.d

TESTS             = available constantprop liveness reaching dce dominators licm
OPTIMIZER_SOURCES = unifiedpass.cpp
OPTIMIZER_LIBS    = $(OPTIMIZER_SOURCES:%.cpp=$(BUILDDIR)/%.so)
TESTS_PRE         = $(TESTS:%=$(BUILDDIR)/tests/%-m2r.ll)
TESTS_OUT         = $(TESTS:%=$(BUILDDIR)/tests/%-opt.ll)
DEPFILES          = $(OPTIMIZER_SOURCES:%.cpp=$(DEPDIR)/%.d)

.PHONY: all clean tests
.SECONDARY:

all: $(OPTIMIZER_LIBS)
tests: $(TESTS_PRE) $(TESTS_OUT)

clean:
	rm -rf $(BUILDDIR)

$(BUILDDIR)/%.o: %.cpp $(DEPDIR)/%.d | $(DEPDIR) $(BUILDDIR)
	$(CXX) $(DEPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILDDIR)/%.so: $(BUILDDIR)/%.o
	$(CXX) -shared $^ -o $@ $(LDFLAGS)

$(BUILDDIR)/tests/%-opt.bc: tests/%-test-m2r.bc $(OPTIMIZER_LIBS) | $(BUILDDIR)/tests
	opt -bugpoint-enable-legacy-pm=1 $(OPTIMIZER_LIBS:%=-load-pass-plugin=%) -passes='$*' $< -o $@

$(BUILDDIR)/tests/%-opt.ll: $(BUILDDIR)/tests/%-opt.bc
	llvm-dis $< -o $@

$(BUILDDIR)/tests/%-m2r.ll: tests/%-test-m2r.bc | $(BUILDDIR)/tests
	llvm-dis $< -o $@

$(DEPDIR) $(BUILDDIR) $(BUILDDIR)/tests:
	@mkdir -p $@

$(DEPFILES):
-include $(wildcard $(DEPFILES))
