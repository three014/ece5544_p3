// ECE/CS 5544 Assignment 3 unifiedpass.cpp

#include "llvm/IR/InstrTypes.h"
#include <algorithm>
#include <compare>
#include <concepts>
#include <cstdint>
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
    std::ranges::range<U> && std::same_as<std::ranges::range_value_t<U>, T>;

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

BitVector meet_union(const range_of<BitVector> auto in_or_outs,
                     size_t universe_size) {
  BitVector output(universe_size);
  for (const auto &first : in_or_outs | std::views::take(1))
    output = first;
  for (const auto &bv : in_or_outs | std::views::drop(1))
    output |= bv;
  return output;
}

BitVector meet_intersect(const range_of<BitVector> auto in_or_outs,
                         size_t universe_size) {
  BitVector output(universe_size);
  for (const auto &first : in_or_outs | std::views::take(1))
    output = first;
  for (const auto &next : in_or_outs | std::views::drop(1))
    output &= next;
  return output;
}

template <std::three_way_comparable T> void sort_unique(std::vector<T> &v) {
  std::ranges::sort(v);
  const auto [begin, end] = std::ranges::unique(v);
  v.erase(begin, end);
}

const std::vector<BasicBlock *> r_post_order(Function &F) {
  // Create a forward ordering of basic blocks, where:
  // - Successors to a basic block will go after that block
  //   if it hasn't been seen already
  // - Direct children of a basic block don't neccessarily have
  //   an ordering among themselves.
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

const std::vector<BasicBlock *> post_order(Function &F) {
  // Create a backwards ordering of basic blocks.
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

template <std::three_way_comparable T>
static void set_if_exists(range_of<T> auto universe, BitVector &bitset,
                          T value) {
  const auto it = std::ranges::find(universe, value);
  if (it != universe.cend() && *it == value)
    bitset.set(it - universe.cbegin());
}

// -------------------- Faint Analysis ---------------------------

struct FaintPass : FunctionPass {
  static char ID;
  FaintPass() : FunctionPass(ID) {}

  struct BlockState {
    BitVector in, out;
  };

  DenseMap<BasicBlock *, BlockState> st;

  // LHS variables
  std::vector<Value *> universe;

  BitVector gen(Instruction &I) {
    BitVector gen(universe.size());
    const auto values = I.operand_values();
    const bool is_assignment = isa<BinaryOperator>(&I);
    const bool is_lhs_used_in_def =
        std::ranges::find(values, &I) != values.end();
    if (is_assignment && !is_lhs_used_in_def)
      set_if_exists(universe, gen, dyn_cast<Value>(&I));
    return gen;
  }

  BitVector const_kill(Instruction &I) {
    BitVector kill(universe.size());
    const bool is_assignment = isa<BinaryOperator>(&I);
    if (is_assignment)
      for (Value *V : I.operand_values())
        set_if_exists(universe, kill, V);
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
        set_if_exists(universe, kill, V);

    return kill;
  }

  // OUT -> IN
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

  bool runOnFunction(Function &F) {
    auto to_value = [](Instruction &I) { return cast<Value>(&I); };
    auto lhs = F | std::views::join | std::views::transform(to_value);
    universe.assign(lhs.begin(), lhs.end());
    sort_unique(universe);

    const std::vector<BasicBlock *> order = post_order(F);

    bool changed = true;
    while (changed) {
      changed = false;
      for (BasicBlock *BB : order) {
        std::vector<BitVector> succIns;
        for (BasicBlock *succ : successors(BB))
          succIns.push_back(st[succ].in);

        if (succIns.empty())
          succIns.push_back(BitVector(universe.size(), true)); // = T

        st[BB].out = meet_intersect(succIns, universe.size());
        BitVector in = transfer(*BB, st[BB].out);
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

// -------------------- Available Expressions --------------------

struct Expr {
  Instruction::BinaryOps opcode;
  Value *lhs;
  Value *rhs;
  auto operator<=>(const Expr &) const = default;

  static Expr fromBO(const BinaryOperator &BO) {
    return {BO.getOpcode(), BO.getOperand(0), BO.getOperand(1)};
  }
};

raw_ostream &operator<<(raw_ostream &OS, const Expr &E) {
  OS << getShortValueName(E.lhs) << " ";
  switch (E.opcode) {
  case Instruction::Add:
    OS << "+";
    break;
  case Instruction::Sub:
    OS << "-";
    break;
  case Instruction::Mul:
    OS << "*";
    break;
  case Instruction::SDiv:
  case Instruction::UDiv:
    OS << "/";
    break;
  default:
    OS << "(op)";
    break;
  }
  OS << " " << getShortValueName(E.rhs);
  return OS;
}

struct AvailablePass : PassInfoMixin<AvailablePass> {
  struct BlockState {
    BitVector in;
    BitVector out;
    BitVector gen;
    BitVector kill;
  };

  std::vector<Expr> universe;

  static BitVector top(const std::vector<Expr> &universe) {
    return BitVector(universe.size(), true);
  }

  static BitVector bottom(const std::vector<Expr> &universe) {
    return BitVector(universe.size(), false);
  }

  static BitVector gen(BasicBlock &BB, std::vector<Expr> &universe) {
    BitVector gen = BitVector(universe.size(), false);
    // If expression exists in universe, it has been generated.
    for (Instruction &I : BB) {
      if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
        Expr e = Expr::fromBO(*BO);
        auto it = std::lower_bound(universe.begin(), universe.end(), e);
        if (it != universe.end() && *it == e) {
          gen.set(static_cast<unsigned>(it - universe.begin()));
        }
      }
    }

    return gen;
  }

  static BitVector kill(BasicBlock &BB, std::vector<Expr> &universe) {
    BitVector kill = BitVector(universe.size(), false);
    // If the result of instruction I was used as
    // an operand in another expression, then
    // that other expression is killed
    // (expression was redefined).
    for (Instruction &I : BB) {
      if (!I.getType()->isVoidTy()) {
        for (size_t i = 0; i < universe.size(); i++) {
          if (universe[i].lhs == &I || universe[i].rhs == &I) {
            kill.set(i);
          }
        }
      }
    }

    return kill;
  }

  BitVector meet(const std::vector<BitVector> &outs) {
    return meet_intersect(outs, universe.size());
  }

  BitVector transfer(const BitVector &in, const BitVector &gen,
                     const BitVector &kill) {
    BitVector out = in;
    out.reset(kill);
    out |= gen;
    return out;
  }

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &) {
    outs() << "=== ";
    F.printAsOperand(outs(), false);
    outs() << " ===\n";

    // Create universal set of expressions from function F
    auto isa_bo = [](auto &I) { return isa<BinaryOperator>(&I); };
    auto into_expr = [](auto &I) {
      return Expr::fromBO(*cast<BinaryOperator>(&I));
    };
    auto exprs = F | std::views::join | std::views::filter(isa_bo) |
                 std::views::transform(into_expr);
    universe.assign(exprs.begin(), exprs.end());

    // Remove redundant expressions
    sort_unique(universe);

    // Create a forward ordering of basic blocks, where:
    // - Successors to a basic block will go after that block
    //   if it hasn't been seen already
    // - Direct children of a basic block don't neccessarily have
    //   an ordering among themselves.
    std::vector<BasicBlock *> order;
    order.push_back(&F.getEntryBlock());
    for (size_t i = 0; i < order.size(); ++i) {
      for (BasicBlock *succ : successors(order[i])) {
        if (std::find(order.begin(), order.end(), succ) == order.end())
          order.push_back(succ);
      }
    }

    DenseMap<const BasicBlock *, BlockState> st;
    for (BasicBlock *BB : order) {
      BlockState bs;
      bs.in = bottom(universe);
      bs.out = top(universe);
      bs.gen = gen(*BB, universe);
      bs.kill = kill(*BB, universe).reset(bs.gen);
      st[BB] = bs;
    }

    // Run the actual iterative algorithm
    bool changed = true;
    while (changed) {
      changed = false;
      for (BasicBlock *BB : order) {
        std::vector<BitVector> predOuts;
        if (BB == &F.getEntryBlock())
          predOuts.push_back(BitVector(universe.size(), false));
        for (BasicBlock *pred : predecessors(BB))
          predOuts.push_back(st[pred].out);
        if (predOuts.empty())
          predOuts.push_back(BitVector(universe.size(), false));

        st[BB].in = meet(predOuts);

        BitVector newOut = transfer(st[BB].in, st[BB].gen, st[BB].kill);
        if (newOut != st[BB].out) {
          st[BB].out = newOut;
          changed = true;
        }
      }
    }

    for (BasicBlock *BB : order) {
      outs() << "BB: ";
      BB->printAsOperand(outs(), false);
      outs() << "\n";
      printBitSet(outs(), "gen", st[BB].gen, universe);
      printBitSet(outs(), "kill", st[BB].kill, universe);
      printBitSet(outs(), "IN", st[BB].in, universe);
      printBitSet(outs(), "OUT", st[BB].out, universe);
    }
    return PreservedAnalyses::all();
  }
};

// -------------------- Liveness --------------------

struct LivenessPass : PassInfoMixin<LivenessPass> {
  struct BlockState {
    BitVector in;
    BitVector out;
    BitVector use;
    BitVector def;
  };

  DenseMap<const BasicBlock *, BlockState> st;
  std::vector<Value *> universe;

  static BitVector top(const std::vector<Value *> &domain) {
    return BitVector(domain.size(), true);
  }

  static BitVector bottom(const std::vector<Value *> &domain) {
    return BitVector(domain.size(), false);
  }

  // Meet: OUT[B] = union IN[S], S all successors of B
  BitVector meet(const std::vector<BitVector> &ins) {
    return meet_union(ins, universe.size());
  }

  // Transfer: IN[B] = USE[B] union (OUT[B] - DEF[B])
  static BitVector transfer(const BitVector &use, const BitVector &def,
                            const BitVector &out) {
    BitVector tmpUse = use;
    BitVector tmpOut = out;
    BitVector in = tmpUse |= tmpOut.reset(def);
    return in;
  }

  // Use: Variable was used in a basic block without being defined first.
  // Def: Variable was defined in a basic block
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &) {
    outs() << "=== ";
    F.printAsOperand(outs(), false);
    outs() << " ===\n";

    // Domain: Set of variables in function F,
    // including the variables as operands.
    std::vector<Value *> tmp;
    for (auto &BB : F) {
      for (auto &I : BB) {
        if (!I.getType()->isVoidTy()) {
          tmp.push_back(&I);
          for (auto *V : I.operand_values()) {
            if (!isa<ConstantData>(V) && !V->getType()->isVoidTy()) {
              tmp.push_back(V);
            }
          }
        }
      }
    }
    universe.assign(tmp.begin(), tmp.end());

    // Remove redundant expressions
    sort_unique(tmp);

    std::vector<const BasicBlock *> order;
    for (const BasicBlock &BB : F) {
      // Rationale: BBs that lead outside the function
      // won't have any successors, so they'll have an
      // empty list, and that's where we'll want to start.
      if (successors(&BB).empty()) {
        order.push_back(&BB);
      }
    }

    // Find predecessors of the blocks in the list, and add
    // them to the list if they're not already there.
    for (size_t i = 0; i < order.size(); i++) {
      for (const BasicBlock *pred : predecessors(order[i])) {
        if (std::find(order.begin(), order.end(), pred) == order.end()) {
          order.push_back(pred);
        }
      }
    }

    // Intialize state
    for (const BasicBlock *BB : order) {
      BlockState bs;
      bs.in = bottom(tmp);
      bs.def = bottom(tmp);
      bs.out = bottom(tmp);
      bs.use = bottom(tmp);
      for (const Instruction &I : *BB) {
        for (const auto *V : I.operand_values()) {
          const auto it = std::find(tmp.cbegin(), tmp.cend(), V);
          if (it != tmp.cend()) {
            bool def = bs.def[it - tmp.cbegin()];
            bool use = bs.use[it - tmp.cbegin()];
            bs.use[it - tmp.cbegin()] = !def || use;
          }
        }

        auto it = std::find(tmp.cbegin(), tmp.cend(), &I);
        if (it != tmp.cend()) {
          bs.def.set(it - tmp.begin());
        }
      }
      st[BB] = bs;
    }

    bool changed = true;
    while (changed) {
      changed = false;
      for (const BasicBlock *BB : order) {
        std::vector<BitVector> succIns;
        for (const BasicBlock *succ : successors(BB)) {
          succIns.push_back(st[succ].in);
        }
        if (succIns.empty()) {
          succIns.push_back(bottom(tmp));
        }

        st[BB].out = meet(succIns);
        BitVector newIn = transfer(st[BB].use, st[BB].def, st[BB].out);
        if (newIn != st[BB].in) {
          st[BB].in = newIn;
          changed = true;
        }
      }
    }

    for (const BasicBlock *BB : order) {
      outs() << "BB: ";
      BB->printAsOperand(outs(), false);
      outs() << "\n";
      printBitSet(outs(), "use", st[BB].use, tmp);
      printBitSet(outs(), "def", st[BB].def, tmp);
      printBitSet(outs(), "IN", st[BB].in, tmp);
      printBitSet(outs(), "OUT", st[BB].out, tmp);
    }
    return PreservedAnalyses::all();
  }
};

// -------------------- Reaching Definitions (forward, union)
// --------------------
struct ReachingPass : PassInfoMixin<ReachingPass> {
  struct BlockState {
    BitVector in, out, gen, kill;
  };

  // Meet: union -- a definition reaches if it arrives via ANY predecessor path.
  static BitVector meet(const std::vector<BitVector> &ins,
                        size_t universe_size) {
    return meet_union(ins, universe_size);
  }

  // Transfer: OUT = gen union (IN - KILL)
  static BitVector transfer(const BitVector &in, const BitVector &gen,
                            const BitVector &kill) {
    BitVector out = in;
    out.reset(kill);
    out |= gen;
    return out;
  }

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &) {
    outs() << "=== ";
    F.printAsOperand(outs(), false);
    outs() << " ===\n";

    // Domain: every instruction that produces a non-void result.
    // In SSA, each value is defined exactly once -> kill is always empty.
    std::vector<Value *> universe;
    for (auto &BB : F)
      for (auto &I : BB)
        if (!I.getType()->isVoidTy())
          universe.push_back(&I);

    // Forward BFS order from entry block.
    std::vector<BasicBlock *> order;
    order.push_back(&F.getEntryBlock());
    for (size_t i = 0; i < order.size(); ++i)
      for (BasicBlock *succ : successors(order[i]))
        if (std::find(order.begin(), order.end(), succ) == order.end())
          order.push_back(succ);

    DenseMap<const BasicBlock *, BlockState> st;
    for (BasicBlock *BB : order) {
      BlockState bs;
      bs.in = BitVector(universe.size(), false);
      bs.out = BitVector(universe.size(), false);
      bs.gen = BitVector(universe.size(), false);
      bs.kill = BitVector(universe.size(), false); // always empty in SSA

      for (Instruction &I : *BB) {
        if (I.getType()->isVoidTy())
          continue;
        auto it = std::find(universe.begin(), universe.end(),
                            static_cast<Value *>(&I));
        if (it != universe.end())
          bs.gen.set(it - universe.begin());
      }
      st[BB] = bs;
    }

    bool changed = true;
    while (changed) {
      changed = false;
      for (BasicBlock *BB : order) {
        std::vector<BitVector> predOuts;
        for (BasicBlock *pred : predecessors(BB))
          predOuts.push_back(st[pred].out);
        // Entry block: IN = empty
        if (predOuts.empty())
          predOuts.push_back(BitVector(universe.size(), false));

        st[BB].in = meet(predOuts, universe.size());
        BitVector newOut = transfer(st[BB].in, st[BB].gen, st[BB].kill);
        if (newOut != st[BB].out) {
          st[BB].out = newOut;
          changed = true;
        }
      }
    }

    for (BasicBlock *BB : order) {
      outs() << "BB: ";
      BB->printAsOperand(outs(), false);
      outs() << "\n";
      // Re-use the Value* printer already in scope
      auto printVals = [&](StringRef label, const BitVector &bits) {
        outs() << " " << label << ": {";
        bool first = true;
        for (size_t i = 0; i < bits.size(); ++i) {
          if (!bits.test(i))
            continue;
          if (!first)
            outs() << "; ";
          first = false;

          universe[i]->printAsOperand(outs(), false);
        }
        outs() << "}\n";
      };
      printVals("gen", st[BB].gen);
      printVals("kill", st[BB].kill);
      printVals("IN", st[BB].in);
      printVals("OUT", st[BB].out);
    }

    return PreservedAnalyses::all();
  }
};

// -------------------- Constant Propagation 3-point --------------------

struct ConstantPropPass : PassInfoMixin<ConstantPropPass> {
  enum class Kind { Top, Const, Bottom }; // Top=unknown, Bottom=NAC

  struct LVal {
    Kind kind = Kind::Top;
    int64_t c = 0;
    static LVal top() { return {Kind::Top, 0}; }
    static LVal constant(int64_t v) { return {Kind::Const, v}; }
    static LVal bottom() { return {Kind::Bottom, 0}; }
    bool operator==(const LVal &o) const { return kind == o.kind && c == o.c; }
    bool operator!=(const LVal &o) const { return !(*this == o); }
  };

  using CPState = DenseMap<const Value *, LVal>;
  struct BlockState {
    CPState in;
    CPState out;
  };

  static LVal meetVal(LVal a, LVal b) {
    if (a.kind == Kind::Top)
      return b;
    if (b.kind == Kind::Top)
      return a;
    if (a.kind == Kind::Bottom || b.kind == Kind::Bottom)
      return LVal::bottom();
    return (a.c == b.c) ? a : LVal::bottom();
  }

  static LVal evalValue(const Value *V, const CPState &st) {
    if (const auto *CI = dyn_cast<ConstantInt>(V))
      return LVal::constant(CI->getSExtValue());
    auto it = st.find(V);
    if (it == st.end())
      return LVal::top();
    return it->second;
  }

  static LVal evalBinary(const BinaryOperator &BO, const CPState &st) {
    LVal l = evalValue(BO.getOperand(0), st);
    LVal r = evalValue(BO.getOperand(1), st);

    // NAC propagates immediately
    if (l.kind == Kind::Bottom || r.kind == Kind::Bottom)
      return LVal::bottom();
    // Unknown operand -> unknown result
    if (l.kind != Kind::Const || r.kind != Kind::Const)
      return LVal::top();

    // Both constants: fold
    switch (BO.getOpcode()) {
    case Instruction::Add:
      return LVal::constant(l.c + r.c);
    case Instruction::Sub:
      return LVal::constant(l.c - r.c);
    case Instruction::Mul:
      return LVal::constant(l.c * r.c);
    case Instruction::SDiv:
      if (r.c == 0)
        return LVal::bottom();
      return LVal::constant(l.c / r.c);
    case Instruction::UDiv:
      if (r.c == 0)
        return LVal::bottom();
      return LVal::constant((uint64_t)l.c / (uint64_t)r.c);
    case Instruction::SRem:
      if (r.c == 0)
        return LVal::bottom();
      return LVal::constant(l.c % r.c);
    case Instruction::And:
      return LVal::constant(l.c & r.c);
    case Instruction::Or:
      return LVal::constant(l.c | r.c);
    case Instruction::Xor:
      return LVal::constant(l.c ^ r.c);
    case Instruction::Shl:
      return LVal::constant(l.c << r.c);
    case Instruction::AShr:
      return LVal::constant(l.c >> r.c);
    default:
      return LVal::bottom();
    }
  }

  static LVal evalPhi(const PHINode &Phi,
                      const DenseMap<const BasicBlock *, BlockState> &states) {
    LVal result = LVal::top();
    for (unsigned i = 0; i < Phi.getNumIncomingValues(); ++i) {
      const BasicBlock *predBB = Phi.getIncomingBlock(i);
      const Value *V = Phi.getIncomingValue(i);

      LVal incoming;
      if (const auto *CI = dyn_cast<ConstantInt>(V)) {
        incoming = LVal::constant(CI->getSExtValue());
      } else {
        auto stateIt = states.find(predBB);
        if (stateIt == states.end()) {
          incoming = LVal::top();
        } else {
          auto valIt = stateIt->second.out.find(V);
          incoming = (valIt == stateIt->second.out.end()) ? LVal::top()
                                                          : valIt->second;
        }
      }
      result = meetVal(result, incoming);
    }
    return result;
  }

  static CPState
  transferBlock(BasicBlock &BB, const CPState &in,
                const DenseMap<const BasicBlock *, BlockState> &states) {
    CPState out = in;
    for (Instruction &I : BB) {
      if (I.getType()->isVoidTy())
        continue;
      if (auto *P = dyn_cast<PHINode>(&I)) {
        out[&I] = evalPhi(*P, states);
      } else if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
        out[&I] = evalBinary(*BO, out);
      } else {
        // Conservative: unhandled instructions -> NAC
        out[&I] = LVal::bottom();
      }
    }
    return out;
  }

  static bool sameState(const CPState &a, const CPState &b,
                        const std::vector<const Value *> &domain) {
    for (const Value *V : domain)
      if (a.lookup(V) != b.lookup(V))
        return false;
    return true;
  }

  static void printState(raw_ostream &OS, StringRef label, const CPState &st,
                         const std::vector<const Value *> &domain,
                         bool showTop = true) {
    OS << "  " << label << ": { ";
    bool first = true;
    for (const Value *V : domain) {
      LVal v = st.lookup(V);
      if (!showTop && v.kind == Kind::Top)
        continue;
      if (!first)
        OS << "; ";
      first = false;
      V->printAsOperand(OS, false);
      if (v.kind == Kind::Const)
        OS << " = " << v.c;
      else if (v.kind == Kind::Bottom)
        OS << " = NAC";
      else
        OS << " = TOP";
    }
    OS << " }\n";
  }

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &) {
    outs() << "=== ";
    F.printAsOperand(outs(), false);
    outs() << " ===\n";

    // Domain: function args first, then all non-void instructions
    std::vector<const Value *> domain;
    for (auto &arg : F.args())
      domain.push_back(&arg);
    for (auto &BB : F)
      for (auto &I : BB)
        if (!I.getType()->isVoidTy())
          domain.push_back(&I);

    // BFS order
    std::vector<BasicBlock *> order;
    order.push_back(&F.getEntryBlock());
    for (size_t i = 0; i < order.size(); ++i)
      for (BasicBlock *succ : successors(order[i]))
        if (std::find(order.begin(), order.end(), succ) == order.end())
          order.push_back(succ);

    // Initialise all blocks to TOP
    DenseMap<const BasicBlock *, BlockState> st;
    for (BasicBlock *BB : order) {
      BlockState bs;
      for (const Value *V : domain) {
        bs.in[V] = LVal::top();
        bs.out[V] = LVal::top();
      }
      st[BB] = std::move(bs);
    }

    // Fixed-point iteration
    bool changed = true;
    while (changed) {
      changed = false;
      for (BasicBlock *BB : order) {
        CPState newIn;
        for (const Value *V : domain)
          newIn[V] = LVal::top();

        bool hasPred = false;
        for (BasicBlock *pred : predecessors(BB)) {
          hasPred = true;
          for (const Value *V : domain)
            newIn[V] = meetVal(newIn.lookup(V), st[pred].out.lookup(V));
        }
        // Entry block has no predecessors: keep everything TOP
        if (!hasPred)
          for (const Value *V : domain)
            newIn[V] = LVal::top();

        st[BB].in = newIn;
        CPState newOut = transferBlock(*BB, newIn, st);
        if (!sameState(st[BB].out, newOut, domain)) {
          st[BB].out = std::move(newOut);
          changed = true;
        }
      }
    }

    for (BasicBlock *BB : order) {
      outs() << "BB: ";
      BB->printAsOperand(outs(), false);
      outs() << "\n";
      printState(outs(), "IN", st[BB].in, domain, true);
      printState(outs(), "OUT", st[BB].out, domain, true);
    }
    return PreservedAnalyses::all();
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
struct DominatorsPass : FunctionPass {
  static char ID;
  DominatorsPass() : FunctionPass(ID) {}

  struct BlockState {
    BitVector in, out;
  };

  BitVector top(size_t size) { return BitVector(size, true); }

  bool runOnFunction(Function &F) {
    outs() << "=== ";
    F.printAsOperand(outs(), false);
    outs() << " ===\n";

    auto get_ptr = [](BasicBlock &BB) { return &BB; };
    auto blocks = F | std::views::transform(get_ptr);
    std::vector<BasicBlock *> universe{blocks.begin(), blocks.end()};
    sort_unique(universe);

    const std::vector<BasicBlock *> order = r_post_order(F);

    DenseMap<BasicBlock *, BlockState> st;
    bool changed = true;
    while (changed) {
      changed = false;
      for (BasicBlock *BB : order) {
        std::vector<BitVector> predOuts;
        if (BB == &F.getEntryBlock()) {
          BitVector boundary = BitVector(universe.size());
          boundary.set(0); // OUT[B] = {entry}
          predOuts.push_back(boundary);
        }
        for (BasicBlock *pred : predecessors(BB))
          predOuts.push_back(st[pred].out);
        if (predOuts.empty())
          predOuts.push_back(top(universe.size()));

        st[BB].in = meet_intersect(predOuts, universe.size());

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

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "UnifiedPass", "v0.4", [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &FPM,
                   ArrayRef<PassBuilder::PipelineElement>) -> bool {
                  if (Name == "dominators") {
                    FPM.addPass(DominatorsPass());
                    return true;
                  }
                  if (Name == "available") {
                    FPM.addPass(AvailablePass());
                    return true;
                  }
                  if (Name == "liveness") {
                    FPM.addPass(LivenessPass());
                    return true;
                  }
                  if (Name == "reaching") {
                    FPM.addPass(ReachingPass());
                    return true;
                  }
                  if (Name == "constantprop") {
                    FPM.addPass(ConstantPropPass());
                    return true;
                  }
                  return false;
                });
          }};
}
