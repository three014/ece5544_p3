// ECE/CS 5544 Assignment 3 unifiedpass.cpp

#include "llvm/IR/InstrTypes.h"
#include <algorithm>
#include <compare>
#include <concepts>
#include <ranges>
#include <string>
#include <vector>

#include <llvm/ADT/BitVector.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Pass.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/PassPlugin.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace {

// Concept for constraining the type of a range.
// For example, `range_of<Value *>` is a forward iterator of `Value *`.
// `std::vector<T>` should be 100% compatible with this concept.
template <typename U, typename T>
concept range_of =
    std::ranges::sized_range<U> && std::ranges::random_access_range<U> &&
    std::same_as<std::ranges::range_value_t<U>, T>;

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
  std::ranges::sort(v);
  const auto [begin, end] = std::ranges::unique(v);
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
      auto x = std::ranges::find(order, succ);
      if (std::ranges::find(order, succ) == order.end())
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
      if (std::ranges::find(order, pred) == order.end())
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

template <std::three_way_comparable T, range_of<T> R> struct BitSetHelper {
  R &universe;

  // Sets the bit in `bitset` that matches `value`'s position in `universe`
  // to `true`. Does nothing if `value` is not present in `universe`.
  void set_if_exists(BitVector &bitset, T value) {
    const auto it = std::ranges::find(universe, value);
    if (it != universe.cend() && *it == value)
      bitset.set(it - universe.cbegin());
  }

  // Performs the meet operator (union) on `in_or_outs`.
  // `universe_size` tells us how large the final bitvector
  // should be.
  BitVector meet_union(const range_of<BitVector> auto in_or_outs) {
    BitVector output(universe.size());
    for (const auto &first : in_or_outs | std::views::take(1))
      output = first;
    for (const auto &bv : in_or_outs | std::views::drop(1))
      output |= bv;
    return output;
  }

  // Performs the meet operator (intersection) on `in_or_outs`.
  // `universe_size` tells us how large the final bitvector
  // should be.
  BitVector meet_intersect(const range_of<BitVector> auto in_or_outs) {
    BitVector output(universe.size());
    for (const auto &first : in_or_outs | std::views::take(1))
      output = first;
    for (const auto &next : in_or_outs | std::views::drop(1))
      output &= next;
    return output;
  }
};

// -------------------- Faint Analysis ---------------------------

struct FaintPass : FunctionPass {
  static char ID;
  FaintPass() : FunctionPass(ID) {}

  struct BlockState {
    BitVector in, out;
  };

  // Does LLVM create a new instance of FaintPass
  // per-function, or does it reuse the same instance?
  // If so, universe will get clobbered.
  DenseMap<BasicBlock *, BlockState> st;

  // LHS variables
  // DenseMap<Function *, std::vector<Value *>> universe;
  std::vector<Value *> universe;

  struct Helper : BitSetHelper<Value *, std::vector<Value *>> {
    BitVector top() { return BitVector(universe.size(), true); }

    BitVector gen(Instruction &I) {
      BitVector gen(universe.size());
      const auto values = I.operand_values();
      const bool is_assignment = isa<BinaryOperator>(&I);
      const bool is_lhs_used_in_def =
          std::ranges::find(values, &I) != values.end();
      if (is_assignment && !is_lhs_used_in_def)
        set_if_exists(gen, dyn_cast<Value>(&I));
      return gen;
    }

    BitVector const_kill(Instruction &I) {
      BitVector kill(universe.size());
      const bool is_assignment = isa<BinaryOperator>(&I);
      if (is_assignment)
        for (Value *V : I.operand_values())
          set_if_exists(kill, V);
      return kill;
    }

    BitVector dep_kill(Instruction &I, const BitVector &out) {
      BitVector kill(universe.size());
      const BitVector &faint = out;
      const bool is_assignment = isa<BinaryOperator>(&I);
      const bool is_lhs_faint = [&]() {
        const auto it = std::ranges::find(universe, &I);
        if (it != universe.cend()) {
          size_t idx = it - universe.cbegin();
          return faint.size() > idx && faint[idx];
        }
        return false;
      }();

      if (is_assignment && is_lhs_faint)
        for (Value *V : I.operand_values())
          set_if_exists(kill, V);

      return kill;
    }

    BitVector transfer(Instruction &I, const BitVector &out) {
      BitVector in = out;
      BitVector _gen = gen(I);
      BitVector _kill = const_kill(I) |= dep_kill(I, out);
      return in.reset(_kill) |= _gen;
    }

    BitVector transfer(BasicBlock &BB, const BitVector &out) {
      BitVector in = out;
      for (Instruction &I : *BB.getReverseIterator()) {
        BitVector &out = in;
        in = transfer(I, out);
      }
      return in;
    }
  };

  bool runOnFunction(Function &F) {
    auto to_value = [](Instruction &I) { return cast<Value>(&I); };
    auto lhs = F | std::views::join | std::views::transform(to_value);
    universe.assign(lhs.begin(), lhs.end()); // Clobbers all prev results
    sort_unique(universe);

    const std::vector<BasicBlock *> order = post_order(F);
    Helper helper{universe};

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
    return false;
  }

  void getAnalysisUsage(AnalysisUsage &AU) { AU.setPreservesAll(); }
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
struct DominatorsPass : FunctionPass {
  static char ID;
  DominatorsPass() : FunctionPass(ID) {}

  struct BlockState {
    BitVector in, out;
  };

  struct Helper : BitSetHelper<BasicBlock *, std::vector<BasicBlock *>> {
    BitVector top() { return BitVector(universe.size(), true); }
    BitVector bottom() { return BitVector(universe.size(), false); }
  };

  bool runOnFunction(Function &F) {
    outs() << "=== ";
    F.printAsOperand(outs(), false);
    outs() << " ===\n";

    auto get_ptr = [](BasicBlock &BB) { return &BB; };
    auto blocks = F | std::views::transform(get_ptr);
    std::vector<BasicBlock *> universe{blocks.begin(), blocks.end()};
    sort_unique(universe);

    const std::vector<BasicBlock *> order = r_post_order(F);
    Helper helper{universe};

    DenseMap<BasicBlock *, BlockState> st;
    bool changed = true;
    while (changed) {
      changed = false;
      for (BasicBlock *BB : order) {
        std::vector<BitVector> predOuts;
        if (BB == &F.getEntryBlock()) {
          BitVector boundary = helper.bottom();
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
        auto it = std::ranges::find(universe, BB);
        if (it != universe.end() && *it == BB)
          newOut.set(it - universe.begin());

        if (newOut != st[BB].out) {
          st[BB].out = newOut;
          changed = true;
        }
      }
    }

    for (BasicBlock *BB : order) {
      BB->printAsOperand(outs(), false);
      auto &out = st[BB].out;
      if (out.empty())
        outs() << " has no dominators\n";

      outs() << " is dominated by " << universe[0]->getName();
      for (size_t i = 1; i < out.size(); ++i) {
        if (!out[i])
          continue;
        outs() << ", " << universe[i]->getName();
      }

      outs() << ".\n";
    }

    return false;
  }

  void getAnalysisUsage(AnalysisUsage &AU) const { AU.setPreservesAll(); }
};

struct DeadCodeEliminationPass : FunctionPass {
  static char ID;

  DeadCodeEliminationPass() : FunctionPass(ID) {}

  void getAnalysisUsage(AnalysisUsage &AU) const { AU.setPreservesCFG(); }

  bool runOnFunction(Function &F) { return false; }
};

} // namespace

char DominatorsPass::ID = 0;
static RegisterPass<DominatorsPass> tmp1("dominators", "Dominators", false,
                                         true);

char FaintPass::ID = 1;
static RegisterPass<FaintPass> tmp2("faint", "Faint Analysis", false, true);

char DeadCodeEliminationPass::ID = 2;
static RegisterPass<DeadCodeEliminationPass>
    tmp3("dead-code-elimination", "Dead Code Elimination", false, false);

