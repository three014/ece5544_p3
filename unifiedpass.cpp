// ECE/CS 5544 Assignment 2 starter unifiedpass.cpp
// Lean starter: buildable scaffolds, minimal solved logic.

#include <algorithm>
#include <cstdint>
#include <span>
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
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/PassPlugin.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace {

BitVector meet_union(const std::span<const BitVector> in_or_outs) {
  if (in_or_outs.empty())
    return {};
  BitVector output = in_or_outs[0];
  for (size_t i = 1; i < in_or_outs.size(); ++i)
    output |= in_or_outs[i];
  return output;
}

BitVector meet_intersect(const std::span<const BitVector> in_or_outs) {
  if (in_or_outs.empty())
    return {};
  BitVector output = in_or_outs[0];
  for (size_t i = 1; i < in_or_outs.size(); ++i)
    output &= in_or_outs[i];
  return output;
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

struct AvailablePass : PassInfoMixin<AvailablePass> {
  struct BlockState {
    BitVector in;
    BitVector out;
    BitVector gen;
    BitVector kill;
  };

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

  static BitVector meet(const std::vector<BitVector> &outs) {
    return meet_intersect(outs);
  }

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

    // Create universal set of expressions from function F
    std::vector<Expr> universe;
    for (auto &BB : F) {
      for (auto &I : BB) {
        if (auto *BO = dyn_cast<BinaryOperator>(&I))
          universe.push_back(Expr::fromBO(*BO));
      }
    }

    // Remove redundant expressions
    std::sort(universe.begin(), universe.end());
    universe.erase(std::unique(universe.begin(), universe.end()),
                   universe.end());

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

  static BitVector top(const std::vector<Value *> &domain) {
    return BitVector(domain.size(), true);
  }

  static BitVector bottom(const std::vector<Value *> &domain) {
    return BitVector(domain.size(), false);
  }

  // Meet: OUT[B] = union IN[S], S all successors of B
  static BitVector meet(const std::vector<BitVector> &ins) {
    return meet_union(ins);
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
    std::vector<Value *> universe;
    for (auto &BB : F) {
      for (auto &I : BB) {
        if (!I.getType()->isVoidTy()) {
          universe.push_back(&I);
          for (auto *V : I.operand_values()) {
            if (!isa<ConstantData>(V) && !V->getType()->isVoidTy()) {
              universe.push_back(V);
            }
          }
        }
      }
    }

    // Remove redundant expressions
    std::sort(universe.begin(), universe.end());
    universe.erase(std::unique(universe.begin(), universe.end()),
                   universe.end());

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
    DenseMap<const BasicBlock *, BlockState> st;
    for (const BasicBlock *BB : order) {
      BlockState bs;
      bs.in = bottom(universe);
      bs.def = bottom(universe);
      bs.out = bottom(universe);
      bs.use = bottom(universe);
      for (const Instruction &I : *BB) {
        for (const auto *V : I.operand_values()) {
          const auto it = std::find(universe.cbegin(), universe.cend(), V);
          if (it != universe.cend()) {
            bool def = bs.def[it - universe.cbegin()];
            bool use = bs.use[it - universe.cbegin()];
            bs.use[it - universe.cbegin()] = !def || use;
          }
        }

        auto it = std::find(universe.cbegin(), universe.cend(), &I);
        if (it != universe.cend()) {
          bs.def.set(it - universe.begin());
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
          succIns.push_back(bottom(universe));
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
      printBitSet(outs(), "use", st[BB].use, universe);
      printBitSet(outs(), "def", st[BB].def, universe);
      printBitSet(outs(), "IN", st[BB].in, universe);
      printBitSet(outs(), "OUT", st[BB].out, universe);
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
  static BitVector meet(const std::vector<BitVector> &ins) {
    return meet_union(ins);
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

} // namespace

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "UnifiedPass", "v0.3-starter",
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &FPM,
                   ArrayRef<PassBuilder::PipelineElement>) -> bool {
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
