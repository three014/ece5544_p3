// ECE/CS 5544 Assignment 3 unifiedpass.cpp

#include <algorithm>
#include <compare>
#include <concepts>
#include <ranges>
#include <string>
#include <vector>

#include <llvm/ADT/BitVector.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/LoopPass.h>
#include <llvm/IR/Analysis.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Pass.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/PassPlugin.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace p3 {
namespace r = std::ranges;
namespace rv = std::ranges::views;

// Concept for constraining the type of a range.
// For example, `range_of<Value *>` is a sized/random access iterator
// of `Value *`.
// `std::vector<T>` should be 100% compatible with this concept.
template <typename U, typename T>
concept sized_random_access_range_of =
    r::sized_range<U> && r::random_access_range<U> &&
    std::same_as<r::range_value_t<U>, T>;

static std::string getBBName(BasicBlock *BB) {
  if (!BB)
    return "null";
  if (!BB->getName().empty())
    return BB->getName().str();

  std::string s;
  raw_string_ostream os(s);
  BB->printAsOperand(os, false);
  return os.str();
}

template <typename T>
void printBitSet(raw_ostream &OS, StringRef label, const BitVector &bits,
                 const std::vector<T> &universe) {
  OS << "  " << label << ": { ";
  bool first = true;
  for (unsigned i = 0; i < bits.size(); ++i) {
    if (!bits.test(i))
      continue;
    if (!first)
      OS << "; ";
    first = false;
    OS << universe[i];
  }
  OS << " }\n";
}

void printBitSet(raw_ostream &OS, StringRef label, const BitVector &bits,
                 const std::vector<Value *> universe) {
  OS << "  " << label << ": { ";
  bool first = true;
  for (unsigned i = 0; i < bits.size(); ++i) {
    if (!bits.test(i))
      continue;
    if (!first)
      OS << "; ";
    first = false;
    universe[i]->printAsOperand(OS, false);
  }
  OS << " }\n";
}

void printBitSet(raw_ostream &OS, StringRef label, const BitVector &bits,
                 const std::vector<BasicBlock *> universe) {
  OS << "  " << label << ": { ";
  bool first = true;
  for (unsigned i = 0; i < bits.size(); ++i) {
    if (!bits.test(i))
      continue;
    if (!first)
      OS << "; ";
    first = false;
    OS << universe[i]->getName();
  }
  OS << " }\n";
}

// Sorts the vector in ascending order and then removes duplicate values.
template <std::three_way_comparable T> void sort_unique(std::vector<T> &v) {
  r::sort(v);
  const auto [begin, end] = r::unique(v);
  v.erase(begin, end);
}

// Creates a forward ordering of basic blocks, where:
// - Successors to a basic block will go after that block
//   if it hasn't been seen already
// - Direct children of a basic block don't neccessarily have
//   an ordering among themselves.
static const std::vector<BasicBlock *> r_post_order(Function &F) {
  std::vector<BasicBlock *> order;
  order.push_back(&F.getEntryBlock());
  for (size_t i = 0; i < order.size(); ++i)
    for (BasicBlock *succ : successors(order[i])) {
      if (r::find(order, succ) == order.end())
        order.push_back(succ);
    }
  return order;
}

// Create a backwards ordering of basic blocks.
static const std::vector<BasicBlock *> post_order(Function &F) {
  std::vector<BasicBlock *> order;
  for (BasicBlock &BB : F) {
    // Rationale: BBs that lead outside the function
    // won't have any successors, so they'll have an
    // empty list, and that's where we'll want to start.
    if (successors(&BB).empty())
      order.push_back(&BB);
  }

  // Find predecessors of the blocks in the list, and add
  // them to the list if they're not already there.
  for (size_t i = 0; i < order.size(); ++i)
    for (BasicBlock *pred : predecessors(order[i]))
      if (r::find(order, pred) == order.end())
        order.push_back(pred);

  return order;
}

std::string getShortValueName(const Value *V) {
  if (!V)
    return "(null)";
  if (V->hasName())
    return "%" + V->getName().str();
  if (const auto *C = dyn_cast<ConstantInt>(V))
    return std::to_string(C->getSExtValue());
  std::string S;
  raw_string_ostream OS(S);
  V->printAsOperand(OS, false);
  return S;
}

// Helper struct for representing values as booleans in a bitset.
// To use, create a new struct that inherits this struct
// and add your own methods that need access to the universe.
template <std::three_way_comparable T, sized_random_access_range_of<T> R>
struct BitSetHelper {
  // Must contain all the possible values of
  // the DFA's domain (within reason).
  const R &universe;

  BitVector all() const { return BitVector(universe.size(), true); }

  BitVector none() const { return BitVector(universe.size(), false); }

  // Sets the bit in `bitset` that matches `value`'s position in `universe`
  // to `true`. Does nothing if `value` is not present in `universe`.
  // Assumptions: bitset.size() == universe.size()
  bool set_if_exists(BitVector &bitset, T value) {
    const auto it = r::find(universe, value);
    if (it != universe.cend() && *it == value) {
      bitset.set(it - universe.cbegin());
      return true;
    }
    return false;
  }

  // Performs the meet operator (union) on `in_or_outs`.
  BitVector
  meet_union(const sized_random_access_range_of<BitVector> auto in_or_outs) {
    BitVector output(universe.size());
    for (const BitVector &first : in_or_outs | rv::take(1))
      output = first;
    for (const BitVector &next : in_or_outs | rv::drop(1))
      output |= next;
    return output;
  }

  // Performs the meet operator (intersection) on `in_or_outs`.
  BitVector meet_intersect(
      const sized_random_access_range_of<BitVector> auto in_or_outs) {
    BitVector output(universe.size());
    for (const BitVector &first : in_or_outs | rv::take(1))
      output = first;
    for (const BitVector &next : in_or_outs | rv::drop(1))
      output &= next;
    return output;
  }

  // Assumptions: bitset.size() == universe.size()
  r::range auto set_values(const BitVector &bitset) {
    if (bitset.size() != universe.size()) {
      // return std::views::empty<T>;
    }

    for (auto x : bitset.set_bits()) {
    }

    // return std::views::transform(bitset.set_bits(),
    //            [this](unsigned int idx) { return universe[idx]; });
    // return std::views::empty<T>;
  }

  // Assumptions: bitset.size() == universe.size()
  bool is_set(const BitVector &bitset, T value) const {
    const auto it = r::find(universe, value);
    if (it != universe.cend() && *it == value)
      return bitset.test(it - universe.cbegin());
    return false;
  }
};

// -------------------- Faint Analysis ---------------------------

struct FaintAnalysis : public AnalysisInfoMixin<FaintAnalysis> {
  LLVM_ABI static AnalysisKey Key;
  struct BlockState {
    BitVector in, out;
  };

  struct FaintInfo {
    DenseMap<BasicBlock *, BlockState> st;
    // LHS variables
    std::vector<Instruction *> universe;
  };
  using Result = FaintInfo;

  struct Helper : BitSetHelper<Instruction *, std::vector<Instruction *>> {
    BitVector top() const { return all(); }
    BitVector bottom() const { return none(); }

    BitVector gen(Instruction &I) {
      BitVector gen = bottom();
      const auto operands = I.operand_values();
      const bool is_lhs_used_in_def = r::find(operands, &I) != operands.end();
      if (!is_lhs_used_in_def) {
        set_if_exists(gen, &I);
      }
      return gen;
    }

    BitVector kill(Instruction &I, const BitVector &out) {
      BitVector kill = bottom();
      const BitVector &faint = out;
      auto is_lhs_faint = [&](Instruction *I) {
        const auto it = r::find(universe, I);
        if (it != universe.cend()) {
          size_t idx = it - universe.cbegin();
          return faint[idx];
        }
        return false;
      };

      if (!is_lhs_faint(&I))
        for (Value *V : I.operand_values()) {
          set_if_exists(kill, dyn_cast<Instruction>(V));
        }

      return kill;
    }

    BitVector transfer(Instruction &I, const BitVector &out) {
      BitVector in = out;
      return in.reset(kill(I, out)) |= gen(I);
    }

    BitVector transfer(BasicBlock &BB, const BitVector &out) {
      BitVector in = out;
      for (Instruction &I : *BB.getReverseIterator()) {
        const BitVector &out = in;
        in = transfer(I, out);
      }
      return in;
    }
  };

  Result run(Function &F, FunctionAnalysisManager &FAM) {
    auto non_void = [](Instruction &I) { return !I.getType()->isVoidTy(); };
    auto to_ptr = [](Instruction &I) { return &I; };
    auto lhs = F | rv::join | rv::filter(non_void) | rv::transform(to_ptr);
    std::vector<Instruction *> universe{lhs.begin(), lhs.end()};
    sort_unique(universe);

    // Reverse direction
    const std::vector<BasicBlock *> order = post_order(F);
    Helper helper{universe};

    // Set up state for all basic blocks
    DenseMap<BasicBlock *, BlockState> st;
    for (BasicBlock *BB : order) {
      st[BB] = std::move(BlockState{helper.top(), helper.top()});
    }

    bool changed = true;
    while (changed) {
      changed = false;
      for (BasicBlock *BB : order) {
        std::vector<BitVector> succIns;
        for (BasicBlock *succ : successors(BB))
          succIns.push_back(st[succ].in);

        if (succIns.empty())
          succIns.push_back(helper.top()); // = T

        st[BB].out = helper.meet_intersect(succIns);
        BitVector in = helper.transfer(*BB, st[BB].out);
        if (in != st[BB].in) {
          changed = true;
          st[BB].in = in;
        }
      }
    }

    return {st, universe};
  }
};

/**
 * Dominators as a Data Flow Analysis problem:
 *
 * Direction: forward
 * Values: basic blocks
 * Meet: intersect
 * Top (T): all basic blocks
 * Bottom: {}
 * Boundary cond for entry node: OUT[entry] = {entry}
 * Init for internal nodes: OUT[B] = T
 * Transfer: OUT[B] = IN[B] union B
 */
struct DomAnalysis : AnalysisInfoMixin<DomAnalysis> {
  LLVM_ABI static AnalysisKey Key;

  struct BlockState {
    BitVector in, out;
  };

  struct Helper : BitSetHelper<BasicBlock *, std::vector<BasicBlock *>> {
    BitVector top() const { return all(); }
    BitVector bottom() const { return none(); }
  };

  struct DomInfo {
    DenseMap<BasicBlock *, BlockState> st;
    std::vector<BasicBlock *> universe;

    void print_loop_dominators(Loop *L) {
      for (BasicBlock *BB : L->getBlocks()) {
        errs() << getBBName(BB);
        const auto &dom = st[BB].out;
        if (dom.none()) {
          errs() << " is dominated by entry\n";
          continue;
        }
        bool is_first = true;
        errs() << " is dominated by ";
        for (size_t i = 0; i < dom.size(); ++i) {
          if (!dom[i])
            continue;
          if (is_first) {
            errs() << getBBName(universe[i]);
            is_first = false;
          } else
            errs() << ", " << getBBName(universe[i]);
        }
        errs() << "\n";
      }
    }
  };
  using Result = DomInfo;

  Result run(Function &F, FunctionAnalysisManager &FAM) {
    outs() << "=== ";
    F.printAsOperand(outs(), false);
    outs() << " ===\n";

    auto get_ptr = [](BasicBlock &BB) { return &BB; };
    auto blocks = F | rv::transform(get_ptr);
    std::vector<BasicBlock *> universe{blocks.begin(), blocks.end()};
    sort_unique(universe);

    const std::vector<BasicBlock *> order = r_post_order(F);
    Helper helper{universe};

    // Set up state for all basic blocks
    DenseMap<BasicBlock *, BlockState> st;
    for (BasicBlock *BB : order) {
      st[BB] = std::move(BlockState{helper.top(), helper.top()});
    }

    bool changed = true;
    while (changed) {
      changed = false;
      for (BasicBlock *BB : order) {
        std::vector<BitVector> predOuts;
        if (BB == &F.getEntryBlock()) {
          BitVector boundary = helper.bottom();
          assert(boundary.size() > 1 && "The presence of an entry block => "
                                        "BitVector has at least one bit");
          boundary.set(0); // OUT[B] = {entry}
          predOuts.push_back(boundary);
        }
        for (BasicBlock *pred : predecessors(BB))
          predOuts.push_back(st[pred].out);
        if (predOuts.empty())
          predOuts.push_back(helper.top());

        st[BB].in = helper.meet_intersect(predOuts);

        // Transfer
        BitVector newOut = st[BB].in;
        helper.set_if_exists(newOut, BB);

        if (newOut != st[BB].out) {
          st[BB].out = newOut;
          changed = true;
        }
      }
    }

    return {st, universe};
  }
};

struct PrintDominatorsPass : PassInfoMixin<PrintDominatorsPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM) {
    DomAnalysis::DomInfo &DI = FAM.getResult<DomAnalysis>(F);
    LoopInfo &LI = FAM.getResult<LoopAnalysis>(F);

    for (Loop *L : LI) {
      DI.print_loop_dominators(L);
      for (Loop *sub : L->getSubLoops()) {
        DI.print_loop_dominators(sub);
      }
    }

    return PreservedAnalyses::all();
  }

  static bool isRequired() { return true; }
};

struct DCEPass : PassInfoMixin<DCEPass> {
  static bool is_live(const Instruction &I) {
    return I.isTerminator() || I.mayHaveSideEffects() ||
           isa<DbgInfoIntrinsic>(I) || isa<LandingPadInst>(I);
  }

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM) {
    const auto &[state, universe] = FAM.getResult<FaintAnalysis>(F);

    std::vector<Instruction *> to_erase;
    for (const auto &block : state.values())
      for (unsigned int i : block.in.set_bits()) {
        if (!is_live(*universe[i])) {
          to_erase.push_back(universe[i]);
        }
      }
    sort_unique(to_erase);

    for (Instruction *I : to_erase) {
      errs() << "Removing instruction\t" << *I << "\n";
    }

    bool modified = false;
    bool erased = true;
    while (erased) {
      erased = false;
      std::vector<Instruction *> remaining;
      for (Instruction *I : to_erase) {
        if (I->use_empty()) {
          I->eraseFromParent();
          modified = true;
          erased = true;
        } else {
          remaining.push_back(I);
        }
      }

      to_erase = std::move(remaining);
    }

    return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }

  static bool isRequired() { return true; }
};

struct LICMPass : LoopPass {
  bool runOnLoop(Loop *L, LPPassManager &LPM) {
    LPM.getAnalysis<DomAnalysis>();

    return false;
  }
};

struct PrintLICMPass : PassInfoMixin<PrintLICMPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM) {
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &DI = FAM.getResult<DomAnalysis>(F);

    return PreservedAnalyses{}.preserve<LoopAnalysis>();
  }

  static bool isRequired() { return true; }
};

} // namespace p3

// ============================================================
// Plugin Registration
// ============================================================

AnalysisKey p3::FaintAnalysis::Key;
AnalysisKey p3::DomAnalysis::Key;

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "ECE5544Passes", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerAnalysisRegistrationCallback(
                [](FunctionAnalysisManager &FAM) {
                  FAM.registerPass([]() { return p3::FaintAnalysis(); });
                  FAM.registerPass([]() { return p3::DomAnalysis(); });
                });
            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &FPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "dominators") {
                    FPM.addPass(p3::PrintDominatorsPass());
                    return true;
                  }
                  if (Name == "dead-code-elimination") {
                    FPM.addPass(p3::DCEPass());
                    return true;
                  }
                  if (Name == "loop-invariant-code-motion") {
                    FPM.addPass(p3::PrintLICMPass());
                    return true;
                  }
                  return false;
                });
          }};
}
