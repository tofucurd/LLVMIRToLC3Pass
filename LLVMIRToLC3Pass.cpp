#include "LLVMIRToLC3Pass.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/ToolOutputFile.h"
#include <algorithm>
#include <cstdint>
#include <llvm/ADT/APInt.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>
#include <string>

using namespace llvm;

// -----------------------------------------------------------------------------
// High-level lowering model
// -----------------------------------------------------------------------------
// This pass translates LLVM IR into textual LC-3 assembly with a deliberately
// small and explicit runtime model:
//
// 1) Value model
//    - Most LLVM values are treated as "word-sized integers".
//    - SSA values are spilled to synthetic stack slots indexed from R5.
//    - Constants/strings are emitted as local literal labels VALUE_<id>.
//
// 2) Control-flow model
//    - Each LLVM basic block receives a stable generated assembly label.
//    - Branches/jumps target those labels directly.
//    - PHI nodes use R7 to identify predecessor label when selecting incoming
//      values.
//
// 3) Calling model (implemented by this pass)
//    - Up to five arguments are passed in R0..R4.
//    - Return values are produced in R0.
//    - Prologue saves registers and establishes R5 frame pointer.
//
// 4) Safety strategy
//    - If an instruction is unsupported, we abort file generation immediately.
//      Emitting no file is safer than emitting potentially wrong assembly.
//
// -----------------------------------------------------------------------------
// Pass configuration flags
// -----------------------------------------------------------------------------
// These options are exposed through `opt`/`clang` when the plugin is loaded.
// They control assembler layout details and optional codegen behavior.
static cl::opt<std::string>
    LC3StartAddrArg("lc3-start-addr",
                    cl::desc("Specify the starting address of LC-3 "
                             "assembly file, default x3000"),
                    cl::value_desc("lc3-start-addr"), cl::init("x3000"));
// Example: `-lc3-start-addr="x3200"` to shift program placement.
// The pass forwards this string verbatim into `.ORIG`.

static cl::opt<std::string>
    LC3StackBaseArg("lc3-stack-base",
                    cl::desc("Specify the base address of the stack in LC-3 "
                             "assembly file, default xFE00"),
                    cl::value_desc("lc3-stack-base"), cl::init("xFE00"));
// Used only when lowering `LC3_main` bootstrap sequence:
//   LD R6, STACK_BASE
//   STACK_BASE .FILL <value>

static cl::opt<bool>
    SignedMul("signed-mul",
              cl::desc("Use signed multiplication or not, default false"),
              cl::value_desc("signed-mul"), cl::init(false));
// Controls the lowering variant for `mul`:
// - false: pure repeated-addition assuming non-negative multiplier.
// - true:  sign-normalization prelude before repeated addition.

static cl::opt<bool>
    NoComment("no-comment",
              cl::desc("Generate pure LC-3 assembly code without any comment"),
              cl::value_desc("no-comment"), cl::init(false));
// Helpful for final submission output where comments are undesirable.
// When false, emitted asm includes:
// - original LLVM IR (prefixed comments)
// - register usage hints
// - function/block metadata

// Assign a stable 1-based integer id for a Value within a local map.
// The pass uses these ids for stack-slot indexing and constant labels.
//
// Why 1-based:
// - `0` is used by helper callers as a sentinel for "not applicable"
//   (for example `addImmidiate` returns 0 for non-constants).
int getIndex(Value *Val, DenseMap<Value *, int> &Map, int &Counter) {
  if (Map.count(Val) == 0) {
    Map[Val] = ++Counter;
  }
  return Map[Val];
}

// Convert a basic block operand name into an assembler-friendly label suffix:
// - strip leading '%' produced by LLVM printing
// - replace '.' with '_' to avoid assembler parsing issues
//
// Example:
//   LLVM operand `%entry.1` -> label suffix `entry_1`
std::string getBBName(Value *Val) {
  std::string Buffer;
  raw_string_ostream BufferStream(Buffer);
  Val->printAsOperand(BufferStream);
  std::string &Name = BufferStream.str();
  auto LastPos = Name.rfind('%');
  Name = Name.substr(LastPos + 1);
  std::replace(Name.begin(), Name.end(), '.', '_');
  return Name;
}

// Assign a stable textual label to a basic block.
// Format: <function>_<sanitized-bb-name>_<monotonic-id>
//
// The monotonic suffix ensures uniqueness even when functions contain multiple
// blocks that sanitize to identical names.
std::string getIndex(BasicBlock *BB, DenseMap<Value *, std::string> &Map,
                     int &Counter) {
  if (Map.count(BB) == 0) {
    Map[BB] = BB->getParent()->getName().str() + "_" + getBBName(BB) + "_" +
              std::to_string(++Counter);
  }
  return Map[BB];
}

// If Val is an integer constant, append a VALUE_<id> literal into this basic
// block's constant pool and return its id. Returns 0 for non-constant values.
//
// Note: i1 constants are treated as unsigned so `true` stays 1 (not -1).
//
// Dedup behavior:
// - `ImmFlag` prevents duplicate VALUE_<id> definitions *within one BB*.
// - `ImmMap` + global counter stabilize identity for reused constants.
int addImmidiate(Value *Val, raw_string_ostream &ImmBuffer,
                 DenseMap<Value *, bool> &ImmFlag,
                 DenseMap<Value *, int> &ImmMap, int &ImmCounter) {
  if (auto *ConstInt = dyn_cast<ConstantInt>(Val)) {
    int ValID = getIndex(Val, ImmMap, ImmCounter);
    if (!ImmFlag.count(Val)) {
      ImmFlag[Val] = true;
      int value = ConstInt->getSExtValue();
      if (ConstInt->getType()->isIntegerTy(1)) {
        value = ConstInt->getZExtValue();
      }
      ImmBuffer << "VALUE_" << ValID << "\n"
                << "\t.FILL\t#" << value << "\n";
    }
    return ValID;
  }
  return 0;
}

// Extract a string payload from a global constant, if any.
// The pass trims trailing '\0' so callers can use it for labels or `.STRINGZ`.
//
// Typical LLVM form handled:
//   @.str = private unnamed_addr constant [N x i8] c"...\00"
StringRef getString(Value *Val) {
  if (auto *GlobalVal = dyn_cast<GlobalVariable>(Val)) {
    if (GlobalVal->hasInitializer()) {
      Constant *Init = GlobalVal->getInitializer();
      if (auto *CDA = dyn_cast<ConstantDataArray>(Init)) {
        return CDA->getAsString().rtrim('\0');
      }
    }
  }
  return StringRef("").rtrim('\0');
}

// Emit `.STRINGZ` constants into the current basic block constant pool.
// Returns VALUE_<id> on success, or 0 if Val does not contain string data.
//
// This helper is used both for direct string output (PUTS) and for helper
// intrinsics that expect textual labels/inline asm payloads.
int addString(Value *Val, raw_string_ostream &ImmBuffer,
              DenseMap<Value *, bool> &ImmFlag, DenseMap<Value *, int> &ImmMap,
              int &ImmCounter) {
  auto Str = getString(Val);
  if (Str.size() > 0) {
    int ValID = getIndex(Val, ImmMap, ImmCounter);
    if (!ImmFlag.count(Val)) {
      ImmFlag[Val] = true;
      ImmBuffer << "VALUE_" << ValID << "\n"
                << "\t.STRINGZ\t\"" << Str << "\"\n";
    }
    return ValID;
  }
  return 0;
}

// Uniform unsupported-instruction handler. The pass fails fast so we do not
// generate partial/incorrect assembly for constructs we cannot lower safely.
//
// Returning `none()` here intentionally indicates the pass changed/attempted
// work and that no analyses should be assumed preserved.
auto UnsupportInst(Instruction &I) {
  errs() << "Unsupported Instruction:\n";
  I.print(errs());
  errs() << "\nNo File Generated\n";
  return PreservedAnalyses::none();
}

// Render an instruction as text and add a prefix to each line.
// Used for embedding LLVM IR as assembly comments in verbose mode.
//
// Multi-line handling is retained so complex IR prints remain readable.
std::string addPrefixInst(Instruction &I, StringRef Prefix) {
  std::string Buffer;
  raw_string_ostream BufferStream(Buffer);
  I.print(BufferStream);

  StringRef Out(BufferStream.str());
  SmallVector<StringRef, 4> Lines;

  Out.split(Lines, '\n');

  std::string Result;
  raw_string_ostream ResultStream(Result);
  for (StringRef Line : Lines) {
    if (!Line.empty()) {
      ResultStream << Prefix << " " << Line << "\n";
    }
  }
  return ResultStream.str();
}

// Provide register-usage hints for selected lowered instructions.
// These comments are intentionally concise but help debugging generated code.
//
// Only instructions where register roles are non-obvious are annotated.
std::string addRegisterComment(Instruction &I) {
  std::string Buffer;
  raw_string_ostream BufferStream(Buffer);

  if (auto *BinOp = dyn_cast<BinaryOperator>(&I)) {
    auto OpCode = BinOp->getOpcode();
    switch (OpCode) {
    case Instruction::Sub:
      BufferStream << ";\tR1: minuend, result\n"
                   << ";\tR2: -subtrahend\n";
      break;
    case Instruction::UDiv:
      BufferStream << ";\tR1: dividend\n"
                   << ";\tR2: divisor\n"
                   << ";\tR3: iterator, result\n";
      break;
    case Instruction::URem:
      BufferStream << ";\tR1: dividend, result\n"
                   << ";\tR2: divisor\n"
                   << ";\tR3: -divisor\n";
      break;
    default:
      return "";
    }
  } else if (auto *ICmpI = dyn_cast<ICmpInst>(&I)) {
    BufferStream << ";\tR1: left\n"
                 << ";\tR2: right\n"
                 << ";\tR3: result(0:false, 1:true)\n";
  } else if (auto *SwitchI = dyn_cast<SwitchInst>(&I)) {
    BufferStream << ";\tR1: set CC\n" << ";\tR7: save current label\n";
  } else if (auto *BrI = dyn_cast<BranchInst>(&I)) {
    if (BrI->isConditional()) {
      BufferStream << ";\tR1: set CC\n" << ";\tR7: save current label\n";
    } else {
      BufferStream << ";\tR7: save the current label\n";
    }
  } else if (auto *PHIN = dyn_cast<PHINode>(&I)) {
    BufferStream << ";\tR0: -from label\n"
                 << ";\tR1: cond label, result\n";
  }
  return BufferStream.str();
}

PreservedAnalyses LLVMIRToLC3Pass::run(Module &M, ModuleAnalysisManager &MAM) {
  // Currently we do not query analyses, but keep this parameter for new PM API.
  (void)MAM;

  // Derive `<source-stem>.asm` in the current working directory.
  StringRef SourceFileName = M.getSourceFileName();
  std::string TargetFileName = sys::path::stem(SourceFileName).str() + ".asm";

  std::error_code EC;
  // ToolOutputFile guarantees temporary-file semantics until `keep()` is
  // called.
  ToolOutputFile Out(TargetFileName, EC, sys::fs::OF_None);
  // `InstBuffer` collects function bodies and is flushed once at the end.
  std::string InstBuffer;
  raw_string_ostream InstBufferStream(InstBuffer);

  if (EC) {
    errs() << "Error: " << EC.message() << "\n";
    return PreservedAnalyses::none();
  }
  if (!NoComment) {
    Out.os() << ";\tThis file is generated automatically by ir-to-lc3 pass.\n"
             << "\n"
             << ";\tR6 : stack pointer\n"
             << ";\tR5 : frame pointer\n"
             << "\n";
  }
  Out.os() << "\t.ORIG\t" << LC3StartAddrArg << "\n";
  // `.ORIG` anchors program layout; all labels are relative to this segment.

  // Cross-function bookkeeping:
  // - BBNameMap: stable assembly label for each LLVM basic block
  // - FuncLabelMap: entry label for each function (for call lowering)
  DenseMap<Value *, std::string> BBNameMap;
  DenseMap<Function *, std::string> FuncLabelMap;
  // Global counters so all generated labels are unique in the output file.
  int BBNameCounter = 0;
  int ImmIDCounter = 0;
  int TempLabelCounter = 0;

  // Visit each defined function and lower it independently.
  for (auto &F : M) {
    // Skip declarations/intrinsics: they carry no body to lower.
    if (F.isIntrinsic() || F.isDeclaration()) {
      continue;
    }

    StringRef FuncName = F.getName();
    std::string FuncInstBuffer;
    raw_string_ostream FuncInstBufferStream(FuncInstBuffer);

    // Per-function virtual frame map:
    // each SSA value gets a negative offset from R5 (frame pointer).
    DenseMap<Value *, int> ValueOffsetMap;
    int ValueOffsetCounter = 0;

    bool isFirstBB = true;
    for (auto &BB : F) {
      // Generate/fetch deterministic label for this basic block.
      std::string BBName = getIndex(&BB, BBNameMap, BBNameCounter);

      if (isFirstBB) {
        // Program entry bootstrap: initialize stack pointer only once at
        // `LC3_main`, then branch to that function's first lowered block.
        if (FuncName == "LC3_main") {
          // Convention: `LC3_main` is the executable entry symbol.
          // Stack pointer is initialized once here before jumping into function
          // body. Other functions inherit caller-provided R6 state.
          Out.os() << "\tLD\t\tR6, STACK_BASE\n"
                   << "\tBR\t\t" << BBName << "\n"
                   << "\n"
                   << "STACK_BASE\n\t.FILL\t" << LC3StackBaseArg << "\n"
                   << "\n";
        }
        FuncLabelMap[&F] = BBName;
        isFirstBB = false;
      } else {
        FuncInstBufferStream << BBName << "\n";
      }

      DenseMap<Value *, bool> ImmFlag;
      DenseMap<Value *, int> ImmIDMap;
      std::string ImmBuffer;
      raw_string_ostream ImmBufferStream(ImmBuffer);
      // Immediate pool is scoped per-basic-block so constant labels do not
      // accidentally collide semantically across independent blocks.

      // The lowering logic treats scalar values as word-size integers.
      LLVMContext &Ctx = F.getContext();
      Type *WordTy = Type::getInt32Ty(Ctx);

      // ---------------------------------------------------------------------
      // Canonicalization pass #1: lower selected intrinsics into core IR.
      // ---------------------------------------------------------------------
      for (auto &I : make_early_inc_range(BB)) {
        if (auto *IntrI = dyn_cast<IntrinsicInst>(&I)) {
          // Map min/max intrinsics to a compare+select form that downstream
          // emitter already supports. Lifetime intrinsics are no-ops here.
          CmpInst::Predicate Pred;
          switch (IntrI->getIntrinsicID()) {
          case Intrinsic::smin:
            Pred = CmpInst::ICMP_SLT;
            break;
          case Intrinsic::smax:
            Pred = CmpInst::ICMP_SGT;
            break;
          case Intrinsic::umin:
            Pred = CmpInst::ICMP_ULT;
            break;
          case Intrinsic::umax:
            Pred = CmpInst::ICMP_UGT;
            break;
          case Intrinsic::lifetime_start:
            continue;
          case Intrinsic::lifetime_end:
            continue;
          default:
            return UnsupportInst(I);
          }

          IRBuilder<> Builder(IntrI);
          Value *A = IntrI->getArgOperand(0);
          Value *B = IntrI->getArgOperand(1);

          // Example rewrite:
          //   %x = llvm.smax(a, b)
          // becomes:
          //   %cmp = icmp sgt a, b
          //   %x   = select %cmp, a, b
          Value *Cmp = Builder.CreateICmp(Pred, A, B);
          Value *Select = Builder.CreateSelect(Cmp, A, B);

          IntrI->replaceAllUsesWith(Select);
          IntrI->eraseFromParent();
        }
      }

      // ---------------------------------------------------------------------
      // Canonicalization pass #2: normalize immediate handling.
      // - `icmp X, C` becomes `icmp X, -C` to match codegen subtract pattern.
      // - `sub X, C` becomes `add X, -C`.
      // ---------------------------------------------------------------------
      for (auto &I : make_early_inc_range(BB)) {
        if (auto *ICmpI = dyn_cast<ICmpInst>(&I)) {
          // Canonicalize constant to RHS where possible, because emission logic
          // assumes subtract-style `A + (-B)` setup.
          Value *FirVal = ICmpI->getOperand(0);
          if (auto *ConstInt = dyn_cast<ConstantInt>(FirVal)) {
            ICmpI->swapOperands();
          }
          auto Pred = ICmpI->getPredicate();
          Value *A = ICmpI->getOperand(0);
          Value *B = ICmpI->getOperand(1);
          if (auto *ConstInt = dyn_cast<ConstantInt>(B)) {
            IRBuilder<> Builder(ICmpI);

            // Store `-C` directly so compare lowering can reuse addition path.
            auto *NegativeConst =
                ConstantInt::get(WordTy, -ConstInt->getSExtValue());
            auto *II = Builder.CreateICmp(Pred, A, NegativeConst);

            I.replaceAllUsesWith(II);
            I.eraseFromParent();
          }
        } else if (auto *BinOp = dyn_cast<BinaryOperator>(&I)) {
          auto OpCode = BinOp->getOpcode();
          Value *A = BinOp->getOperand(0);
          Value *B = BinOp->getOperand(1);
          switch (OpCode) {
          case Instruction::Sub:
            if (auto *ConstInt = dyn_cast<ConstantInt>(B)) {
              IRBuilder<> Builder(BinOp);

              // Turn `A - C` into `A + (-C)` to reduce emitter special-cases.
              auto *NegativeConst =
                  ConstantInt::get(WordTy, -ConstInt->getSExtValue());
              auto *BI = Builder.CreateAdd(A, NegativeConst);

              I.replaceAllUsesWith(BI);
              I.eraseFromParent();
            }
            break;
          default:
            continue;
          }
        }
      }

      // ---------------------------------------------------------------------
      // Canonicalization pass #3: lower control/data forms not directly mapped
      // by the emitter.
      // - `br (icmp eq/ne X, C)` -> `switch X`
      // - `trunc` -> passthrough (word-machine model)
      // - `lshr X, C` -> `udiv X, (1<<C)` when constant shift
      // - disjoint `or` -> `add`
      // ---------------------------------------------------------------------
      for (auto &I : make_early_inc_range(BB)) {
        if (auto *BrI = dyn_cast<BranchInst>(&I)) {
          if (BrI->isConditional()) {
            Value *Cond = BrI->getCondition();

            if (auto *ICmpI = dyn_cast<ICmpInst>(Cond)) {
              auto Pred = ICmpI->getPredicate();
              if (Pred == CmpInst::ICMP_EQ || Pred == CmpInst::ICMP_NE) {
                // Pattern:
                //   br (icmp eq/ne X, C), T, F
                // rewrites to:
                //   switch X, <default>, [ C -> <other> ]
                //
                // Rationale: this pass has a dedicated `switch` emitter that
                // already handles compare-against-constant cleanly.
                Value *Val = ICmpI->getOperand(0);
                Value *ConVal = ICmpI->getOperand(1);
                if (auto *ConstInt = dyn_cast<ConstantInt>(ConVal)) {
                  IRBuilder<> Builder(BrI);
                  auto *Zero =
                      ConstantInt::get(WordTy, ConstInt->getSExtValue());
                  SwitchInst *SI = Builder.CreateSwitch(
                      Val, BrI->getSuccessor(Pred == CmpInst::ICMP_EQ), 1);
                  SI->addCase(cast<ConstantInt>(Zero),
                              BrI->getSuccessor(Pred == CmpInst::ICMP_NE));

                  BrI->eraseFromParent();
                  // If compare became dead after branch rewrite, delete it.
                  if (ICmpI->use_empty())
                    ICmpI->eraseFromParent();
                }
              }
            }
          }
        } else if (auto *TruncI = dyn_cast<TruncInst>(&I)) {
          // No explicit narrowing operation in this backend model; preserve
          // value identity and drop the cast.
          Value *Source = TruncI->getOperand(0);

          TruncI->replaceAllUsesWith(Source);
          TruncI->eraseFromParent();
        } else if (auto *BinOp = dyn_cast<BinaryOperator>(&I)) {
          auto OpCode = BinOp->getOpcode();
          Value *A = I.getOperand(0);
          Value *B = I.getOperand(1);
          switch (OpCode) {
          case Instruction::LShr:
            if (auto *ShiftAmt = dyn_cast<ConstantInt>(B)) {
              unsigned BitWidth = A->getType()->getIntegerBitWidth();
              uint64_t ShiftVal = ShiftAmt->getZExtValue();

              if (ShiftVal >= BitWidth) {
                // Undefined/edge shift magnitudes are not canonicalized here.
                continue;
              }

              // Replace with unsigned division by power-of-two.
              // This lets lowering reuse the UDiv path.
              APInt DivisorVal = APInt(BitWidth, 1).shl(ShiftVal);
              Constant *Divisor = ConstantInt::get(I.getContext(), DivisorVal);

              IRBuilder<> Builder(&I);
              Value *DivInst = Builder.CreateUDiv(A, Divisor);
              I.replaceAllUsesWith(DivInst);
              I.eraseFromParent();
            }
            break;
          case Instruction::Or:
            if (auto *DisjointOp = dyn_cast<PossiblyDisjointInst>(BinOp)) {
              (void)DisjointOp;
              // For OR ops marked "disjoint", bitwise OR equals addition.
              // Canonicalize to add to simplify codegen.
              IRBuilder<> Builder(&I);

              Value *AddInst = Builder.CreateAdd(A, B);

              I.replaceAllUsesWith(AddInst);
              I.eraseFromParent();
            }
          default:
            continue;
          }
        }
      }

      // ---------------------------------------------------------------------
      // Emission pass: lower each normalized LLVM instruction to LC-3 text.
      // ---------------------------------------------------------------------
      for (auto &I : BB) {
        if (!NoComment) {
          // Inline the source LLVM instruction for traceability.
          FuncInstBufferStream << addPrefixInst(I, ";")
                               << addRegisterComment(I);
        }
        if (auto *BinOp = dyn_cast<BinaryOperator>(&I)) {
          // Generic binary arithmetic/logical lowering.
          auto OpCode = BinOp->getOpcode();
          if (OpCode == Instruction::Mul || OpCode == Instruction::UDiv) {
            // R3 acts as accumulator/counter in iterative algorithms below.
            FuncInstBufferStream << "\tAND\t\tR3, R3, #0\n";
          }

          // Reserve/get stack slot for this instruction result.
          int ResOff = -getIndex(&I, ValueOffsetMap, ValueOffsetCounter);

          // Load RHS into R2 (immediate pool or frame slot).
          Value *B = BinOp->getOperand(1);
          if (int BID = addImmidiate(B, ImmBufferStream, ImmFlag, ImmIDMap,
                                     ImmIDCounter)) {
            FuncInstBufferStream << "\tLD\t\tR2, VALUE_" << BID << "\n";
          } else {
            int BOff = -getIndex(B, ValueOffsetMap, ValueOffsetCounter);
            FuncInstBufferStream << "\tLDR\t\tR2, R5, #" << BOff << "\n";
          }
          if (OpCode == Instruction::Sub || OpCode == Instruction::UDiv) {
            // Convert RHS into two's-complement negative so ADD can subtract.
            FuncInstBufferStream << "\tNOT\t\tR2, R2\n"
                                 << "\tADD\t\tR2, R2, #1\n";
          } else if (OpCode == Instruction::URem) {
            // UREM keeps both divisor and -divisor for loop termination/update.
            FuncInstBufferStream << "\tNOT\t\tR3, R2\n"
                                 << "\tADD\t\tR3, R3, #1\n";
          }

          // Load LHS into R1 (immediate pool or frame slot).
          Value *A = BinOp->getOperand(0);
          if (int AID = addImmidiate(A, ImmBufferStream, ImmFlag, ImmIDMap,
                                     ImmIDCounter)) {
            FuncInstBufferStream << "\tLD\t\tR1, VALUE_" << AID << "\n";
          } else {
            int AOff = -getIndex(A, ValueOffsetMap, ValueOffsetCounter);
            FuncInstBufferStream << "\tLDR\t\tR1, R5, #" << AOff << "\n";
          }
          switch (OpCode) {
          case Instruction::Add:
            // Direct register add.
            FuncInstBufferStream << "\tADD\t\tR1, R1, R2\n"
                                 << "\tSTR\t\tR1, R5, #" << ResOff << "\n";
            break;
          case Instruction::Sub:
            // Subtraction already converted to addition with negated RHS.
            FuncInstBufferStream << "\tADD\t\tR1, R1, R2\n"
                                 << "\tSTR\t\tR1, R5, #" << ResOff << "\n";
            break;
          case Instruction::And:
            // Bitwise AND maps 1:1.
            FuncInstBufferStream << "\tAND\t\tR1, R1, R2\n"
                                 << "\tSTR\t\tR1, R5, #" << ResOff << "\n";
            break;
          case Instruction::Or:
            // Synthesize OR using De Morgan because LC-3 has NOT+AND.
            FuncInstBufferStream << "\tNOT\t\tR1, R1\n"
                                 << "\tNOT\t\tR2, R2\n"
                                 << "\tAND\t\tR1, R1, R2\n"
                                 << "\tNOT\t\tR1, R1\n"
                                 << "\tSTR\t\tR1, R5, #" << ResOff << "\n";
            break;
          case Instruction::Shl:
            // Logical left shift via repeated doubling.
            FuncInstBufferStream << "SHL_LOOP_" << ++TempLabelCounter << "\n"
                                 << "\tADD\t\tR1, R1, R1\n"
                                 << "\tADD\t\tR2, R2, #-1\n"
                                 << "\tBRp\t\tSHL_LOOP_" << TempLabelCounter
                                 << "\n"
                                 << "\tSTR\t\tR1, R5, #" << ResOff << "\n";
            break;
          case Instruction::Mul:
            // Multiply by repeated addition.
            // Optional signed mode normalizes both operands when multiplier <
            // 0.
            if (SignedMul) {
              // Signed path note:
              // This prelude assumes branch condition reflects sign of R2.
              // If R2 is negative, both operands are negated so loop can use
              // a positive counter while preserving final sign.
              FuncInstBufferStream << "\tBRzp\tMUL_LOOP_"
                                   << TempLabelCounter + 1 << "\n"
                                   << "\tNOT\t\tR1, R1\n"
                                   << "\tADD\t\tR1, R1, #1\n"
                                   << "\tNOT\t\tR2, R2\n"
                                   << "\tADD\t\tR2, R2, #1\n";
            }
            FuncInstBufferStream << "MUL_LOOP_" << ++TempLabelCounter
                                 << "\n"
                                 // Exit when counter (R2) reaches zero.
                                 << "\tBRz\t\tMUL_END_" << TempLabelCounter
                                 << "\n"
                                 // Accumulate result += multiplicand.
                                 << "\tADD\t\tR3, R3, R1\n"
                                 // Decrement multiplier/counter.
                                 << "\tADD\t\tR2, R2, #-1\n"
                                 << "\tBR\t\tMUL_LOOP_" << TempLabelCounter
                                 << "\n"
                                 << "MUL_END_" << TempLabelCounter << "\n"
                                 << "\tSTR\t\tR3, R5, #" << ResOff << "\n";
            break;
          case Instruction::UDiv:
            // Unsigned division by repeated subtraction:
            // increment quotient (R3) while dividend remains non-negative.
            FuncInstBufferStream
                << "UDIV_LOOP_" << ++TempLabelCounter
                << "\n"
                // If R1 <= 0, stop attempting subtraction.
                << "\tBRnz\tUDIV_END_" << TempLabelCounter
                << "\n"
                // quotient++
                << "\tADD\t\tR3, R3, #1\n"
                // dividend -= divisor (R2 already negated above)
                << "\tADD\t\tR1, R1, R2\n"
                << "\tBR\t\tUDIV_LOOP_" << TempLabelCounter << "\n"
                << "UDIV_END_" << TempLabelCounter
                << "\n"
                // Correct overshoot when loop exits after crossing below zero.
                << "\tBRz\t\tUDIV_POST_" << TempLabelCounter << "\n"
                << "\tADD\t\tR3, R3, #-1\n"
                << "UDIV_POST_" << TempLabelCounter << "\n"
                << "\tSTR\t\tR3, R5, #" << ResOff << "\n";
            break;
          case Instruction::URem:
            // Unsigned remainder via repeated subtraction, storing final
            // residue.
            FuncInstBufferStream
                << "UREM_LOOP_" << ++TempLabelCounter
                << "\n"
                // Stop when subtraction would cross below zero.
                << "\tBRnz\tUREM_END_" << TempLabelCounter
                << "\n"
                // dividend += (-divisor)
                << "\tADD\t\tR1, R1, R3\n"
                << "\tBR\t\tUREM_LOOP_" << TempLabelCounter << "\n"
                << "UREM_END_" << TempLabelCounter
                << "\n"
                // If we crossed below zero, add divisor back once.
                << "\tBRz\t\tUREM_POST_" << TempLabelCounter << "\n"
                << "\tADD\t\tR1, R1, R2\n"
                << "UREM_POST_" << TempLabelCounter << "\n"
                << "\tSTR\t\tR1, R5, #" << ResOff << "\n";
            break;
          case Instruction::LShr:
            // R2: counter, R1: result, R0: temporary register
            // R3: source mask, R4: destiny mask
            // This fallback executes only when earlier canonicalization leaves
            // an LShr. It shifts bits right one position per outer iteration.
            FuncInstBufferStream
                << "LSHR_OUT_LOOP_" << ++TempLabelCounter << "\n"
                << "\tAND\t\tR0, R0, #0\n"
                << "\tAND\t\tR3, R3, #0\n"
                << "\tADD\t\tR3, R3, #2\n"
                << "\tAND\t\tR4, R4, #0\n"
                << "\tADD\t\tR4, R4, #1\n"
                << "LSHR_IN_LOOP_" << TempLabelCounter
                << "\n"
                // Clear destination bit before testing corresponding source
                // bit.
                << "\tNOT\t\tR4, R4\n"
                << "\tAND\t\tR1, R1, R4\n"
                << "\tNOT\t\tR4, R4\n"
                << "\tAND\t\tR0, R1, R3\n"
                << "\tBRz\t\tLSHR_SKIP_" << TempLabelCounter
                << "\n"
                // If source bit is set, copy to shifted destination position.
                << "\tADD\t\tR1, R1, R4\n"
                << "LSHR_SKIP_" << TempLabelCounter
                << "\n"
                // Advance masks to next bit.
                << "\tADD\t\tR3, R3, R3\n"
                << "\tADD\t\tR4, R4, R4\n"
                << "\tBRnp\tLSHR_IN_LOOP_" << TempLabelCounter << "\n"
                << "\tADD\t\tR2, R2, #-1\n"
                << "\tBRp\t\tLSHR_OUT_LOOP_" << TempLabelCounter << "\n"
                << "\tSTR\t\tR1, R5, #" << ResOff << "\n";
            break;
          default:
            return UnsupportInst(I);
          }
        } else if (auto *LoadI = dyn_cast<LoadInst>(&I)) {
          // Load from an address-like SSA value represented as frame content.
          int ResOff = -getIndex(&I, ValueOffsetMap, ValueOffsetCounter);

          Value *Op = LoadI->getPointerOperand();
          int OpOff = -getIndex(Op, ValueOffsetMap, ValueOffsetCounter);

          FuncInstBufferStream << "\tLDR\t\tR1, R5, #" << OpOff << "\n"
                               << "\tSTR\t\tR1, R5, #" << ResOff << "\n";
        } else if (auto *StoreI = dyn_cast<StoreInst>(&I)) {
          // Store writes a value into another frame slot used as "memory cell".
          Value *Val = StoreI->getValueOperand();
          if (int ValID = addImmidiate(Val, ImmBufferStream, ImmFlag, ImmIDMap,
                                       ImmIDCounter)) {
            FuncInstBufferStream << "\tLD\t\tR1, VALUE_" << ValID << "\n";
          } else {
            int ValOff = -getIndex(Val, ValueOffsetMap, ValueOffsetCounter);
            FuncInstBufferStream << "\tLDR\t\tR1, R5, #" << ValOff << "\n";
          }

          Value *Ptr = StoreI->getPointerOperand();
          int PtrOff = -getIndex(Ptr, ValueOffsetMap, ValueOffsetCounter);

          FuncInstBufferStream << "\tSTR\t\tR1, R5, #" << PtrOff << "\n";
        } else if (auto *BranchI = dyn_cast<BranchInst>(&I)) {
          // Save current label in R7 so PHI lowering can infer predecessor
          // path.
          FuncInstBufferStream << "\tLEA\t\tR7, " << BBName << "\n";
          if (BranchI->isUnconditional()) {
            BasicBlock *SucBB = BranchI->getSuccessor(0);
            std::string SucBBName = getIndex(SucBB, BBNameMap, BBNameCounter);

            FuncInstBufferStream << "\tBR\t\t" << SucBBName << "\n";
          } else {
            // Conditional branch convention: zero => false path, non-zero =>
            // true.
            Value *Con = BranchI->getCondition();
            if (int ConID = addImmidiate(Con, ImmBufferStream, ImmFlag,
                                         ImmIDMap, ImmIDCounter)) {
              FuncInstBufferStream << "\tLD\t\tR1, VALUE_" << ConID << "\n";
            } else {
              int ConOff = -getIndex(Con, ValueOffsetMap, ValueOffsetCounter);
              FuncInstBufferStream << "\tLDR\t\tR1, R5, #" << ConOff << "\n";
            }

            BasicBlock *IfTrueBB = BranchI->getSuccessor(0);
            std::string IfTrueBBName =
                getIndex(IfTrueBB, BBNameMap, BBNameCounter);

            BasicBlock *IfFalseBB = BranchI->getSuccessor(1);
            std::string IfFalseBBName =
                getIndex(IfFalseBB, BBNameMap, BBNameCounter);

            FuncInstBufferStream << "\tBRz\t\t" << IfFalseBBName << "\n"
                                 << "\tBR\t\t" << IfTrueBBName << "\n";
          }
        } else if (auto *ICmpI = dyn_cast<ICmpInst>(&I)) {
          // Comparison lowers to:
          // 1) R1 <- A - B
          // 2) branch on condition-code
          // 3) materialize boolean 0/1 in R3
          FuncInstBufferStream << "\tAND\t\tR3, R3, #0\n";

          int ResOff = -getIndex(&I, ValueOffsetMap, ValueOffsetCounter);

          Value *A = ICmpI->getOperand(0);
          if (int AID = addImmidiate(A, ImmBufferStream, ImmFlag, ImmIDMap,
                                     ImmIDCounter)) {
            FuncInstBufferStream << "\tLD\t\tR1, VALUE_" << AID << "\n";
          } else {
            int AOff = -getIndex(A, ValueOffsetMap, ValueOffsetCounter);
            FuncInstBufferStream << "\tLDR\t\tR1, R5, #" << AOff << "\n";
          }

          Value *B = ICmpI->getOperand(1);
          if (int BID = addImmidiate(B, ImmBufferStream, ImmFlag, ImmIDMap,
                                     ImmIDCounter)) {
            FuncInstBufferStream << "\tLD\t\tR2, VALUE_" << BID << "\n";
          } else {
            int BOff = -getIndex(B, ValueOffsetMap, ValueOffsetCounter);
            FuncInstBufferStream << "\tLDR\t\tR2, R5, #" << BOff << "\n"
                                 << "\tNOT\t\tR2, R2\n"
                                 << "\tADD\t\tR2, R2, #1\n";
          }

          FuncInstBufferStream << "\tADD\t\tR1, R1, R2\n";

          switch (ICmpI->getPredicate()) {
          case CmpInst::ICMP_EQ:
            // Branch away when non-zero => equal is false.
            FuncInstBufferStream << "\tBRnp\tICMP_END_" << ++TempLabelCounter
                                 << "\n";
            break;
          case CmpInst::ICMP_NE:
            // Branch away when zero => not-equal is false.
            FuncInstBufferStream << "\tBRz\t\tICMP_END_" << ++TempLabelCounter
                                 << "\n";
            break;
          case CmpInst::ICMP_SGT:
            // Signed greater-than true only for positive result.
            FuncInstBufferStream << "\tBRnz\tICMP_END_" << ++TempLabelCounter
                                 << "\n";
            break;
          case CmpInst::ICMP_SGE:
            // Signed >= true for zero or positive.
            FuncInstBufferStream << "\tBRn\t\tICMP_END_" << ++TempLabelCounter
                                 << "\n";
            break;
          case CmpInst::ICMP_SLT:
            // Signed < true only for negative result.
            FuncInstBufferStream << "\tBRzp\tICMP_END_" << ++TempLabelCounter
                                 << "\n";
            break;
          case CmpInst::ICMP_SLE:
            // Signed <= true for zero or negative.
            FuncInstBufferStream << "\tBRp\t\tICMP_END_" << ++TempLabelCounter
                                 << "\n";
            break;
          case CmpInst::ICMP_UGT:
            // Unsigned variants currently share signed branch mapping under
            // this simplified integer model.
            FuncInstBufferStream << "\tBRnz\tICMP_END_" << ++TempLabelCounter
                                 << "\n";
            break;
          case CmpInst::ICMP_UGE:
            FuncInstBufferStream << "\tBRn\t\tICMP_END_" << ++TempLabelCounter
                                 << "\n";
            break;
          case CmpInst::ICMP_ULT:
            FuncInstBufferStream << "\tBRzp\tICMP_END_" << ++TempLabelCounter
                                 << "\n";
            break;
          case CmpInst::ICMP_ULE:
            FuncInstBufferStream << "\tBRp\t\tICMP_END_" << ++TempLabelCounter
                                 << "\n";
            break;
          default:
            return UnsupportInst(I);
          }

          FuncInstBufferStream << "\tADD\t\tR3, R3, #1\n"
                               << "ICMP_END_" << TempLabelCounter << "\n"
                               << "\tSTR\t\tR3, R5, #" << ResOff << "\n";
        } else if (auto *CallI = dyn_cast<CallInst>(&I)) {
          // Built-in runtime helpers are identified by function name and
          // lowered directly to LC-3 sequences or pseudo-runtime conventions.
          if (Function *Func = CallI->getCalledFunction()) {
            if (Func->getName() == "printStr") {
              // printStr(<global-string>) -> LEA/PUTS
              // Expected argument: LLVM global string constant or address-like
              // value that resolves to a zero-terminated string.
              if (CallI->arg_size() == 1) {
                Value *Str = CallI->getArgOperand(0);

                if (int StrID = addString(Str, ImmBufferStream, ImmFlag,
                                          ImmIDMap, ImmIDCounter)) {
                  FuncInstBufferStream << "\tLEA\t\tR0, VALUE_" << StrID
                                       << "\n";
                } else {
                  int StrOff =
                      -getIndex(Str, ValueOffsetMap, ValueOffsetCounter);
                  FuncInstBufferStream << "\tLDR\t\tR0, R5, #" << StrOff
                                       << "\n";
                }

                FuncInstBufferStream << "\tPUTS\n";
              } else {
                // Arity mismatch indicates unexpected frontend contract.
                return UnsupportInst(I);
              }
            } else if (Func->getName() == "printStrAddr") {
              // printStrAddr(ptr) -> load pointer to R0, then PUTS.
              // Contract: pointer must address a memory region terminated by 0.
              if (CallI->arg_size() == 1) {
                Value *Addr = CallI->getArgOperand(0);
                if (int AddrID = addImmidiate(Addr, ImmBufferStream, ImmFlag,
                                              ImmIDMap, ImmIDCounter)) {
                  FuncInstBufferStream << "\tLD\t\tR0, VALUE_" << AddrID
                                       << "\n";
                } else {
                  int AddrOff =
                      -getIndex(Addr, ValueOffsetMap, ValueOffsetCounter);
                  FuncInstBufferStream << "\tLDR\t\tR0, R5, #" << AddrOff
                                       << "\n";
                }

                FuncInstBufferStream << "\tPUTS\n";
              } else {
                return UnsupportInst(I);
              }
            } else if (Func->getName() == "printCharAddr") {
              // printCharAddr(ptr) -> load byte/word at ptr, then OUT.
              // Uses R1 as temporary pointer register and R0 as OUT operand.
              if (CallI->arg_size() == 1) {
                Value *Addr = CallI->getArgOperand(0);
                if (int AddrID = addImmidiate(Addr, ImmBufferStream, ImmFlag,
                                              ImmIDMap, ImmIDCounter)) {
                  FuncInstBufferStream << "\tLD\t\tR1, VALUE_" << AddrID
                                       << "\n";
                } else {
                  int AddrOff =
                      -getIndex(Addr, ValueOffsetMap, ValueOffsetCounter);
                  FuncInstBufferStream << "\tLDR\t\tR1, R5, #" << AddrOff
                                       << "\n";
                }

                FuncInstBufferStream << "\tLDR\t\tR0, R1, #0\n"
                                     << "\tOUT\n";
              } else {
                return UnsupportInst(I);
              }
            } else if (Func->getName() == "printChar") {
              // printChar(ch) -> move char to R0, then OUT.
              // No masking is done here; caller/frontend should provide char in
              // LC-3 compatible range if strict behavior is required.
              if (CallI->arg_size() == 1) {
                Value *Char = CallI->getArgOperand(0);
                if (int CharID = addImmidiate(Char, ImmBufferStream, ImmFlag,
                                              ImmIDMap, ImmIDCounter)) {
                  FuncInstBufferStream << "\tLD\t\tR0, VALUE_" << CharID
                                       << "\n";
                } else {
                  int CharOff =
                      -getIndex(Char, ValueOffsetMap, ValueOffsetCounter);
                  FuncInstBufferStream << "\tLDR\t\tR0, R5, #" << CharOff
                                       << "\n";
                }

                FuncInstBufferStream << "\tOUT\n";
              } else {
                return UnsupportInst(I);
              }
            } else if (Func->getName() == "integrateLC3Asm") {
              // Escape hatch: splice literal LC-3 source from a constant
              // string. This is intentionally raw; emitted text is inserted
              // as-is. Use carefully because it bypasses structural checks.
              if (CallI->arg_size() == 1) {
                Value *Str = CallI->getArgOperand(0);
                StringRef Content = getString(Str);
                if (Content != "") {
                  FuncInstBufferStream << Content << "\n";
                } else {
                  return UnsupportInst(I);
                }
              } else {
                return UnsupportInst(I);
              }
            } else if (Func->getName() == "loadLabel") {
              // loadLabel("LABEL") -> load symbol value into a stack slot.
              // Lowering uses `LD`, so LABEL is expected to reference a literal
              // cell containing the desired value.
              if (CallI->arg_size() == 1) {
                int DesOff = -getIndex(&I, ValueOffsetMap, ValueOffsetCounter);

                Value *Str = CallI->getArgOperand(0);
                StringRef Label = getString(Str);

                if (Label != "") {
                  FuncInstBufferStream << "\tLD\t\tR1, " << Label << "\n"
                                       << "\tSTR\t\tR1, R5, #" << DesOff
                                       << "\n";
                } else {
                  return UnsupportInst(I);
                }
              } else {
                return UnsupportInst(I);
              }
            } else if (Func->getName() == "loadAddr") {
              // loadAddr(ptr) -> dereference pointer-like value once.
              // Semantics:
              //   R1 <- ptr
              //   R1 <- Mem[R1]
              //   store R1 as call result
              if (CallI->arg_size() == 1) {
                int DesOff = -getIndex(&I, ValueOffsetMap, ValueOffsetCounter);

                Value *Addr = CallI->getArgOperand(0);
                if (int AddrID = addImmidiate(Addr, ImmBufferStream, ImmFlag,
                                              ImmIDMap, ImmIDCounter)) {
                  FuncInstBufferStream << "\tLD\t\tR1, VALUE_" << AddrID
                                       << "\n";
                } else {
                  int AddrOff =
                      -getIndex(Addr, ValueOffsetMap, ValueOffsetCounter);
                  FuncInstBufferStream << "\tLDR\t\tR1, R5, #" << AddrOff
                                       << "\n";
                }

                FuncInstBufferStream << "\tLDR\t\tR1, R1, #0\n"
                                     << "\tSTR\t\tR1, R5, #" << DesOff << "\n";
              } else {
                return UnsupportInst(I);
              }
            } else if (Func->getName() == "storeLabel") {
              // storeLabel(src, "LABEL") -> store register/frame value to
              // symbol. Uses direct `ST`, so LABEL should represent an
              // addressable symbol.
              if (CallI->arg_size() == 2) {
                Value *Src = CallI->getArgOperand(0);
                int SrcOff = -getIndex(Src, ValueOffsetMap, ValueOffsetCounter);

                Value *Str = CallI->getArgOperand(1);
                StringRef Label = getString(Str);

                if (Label != "") {
                  FuncInstBufferStream << "\tLDR\t\tR1, R5, #" << SrcOff << "\n"
                                       << "\tST\t\tR1, " << Label << "\n";
                } else {
                  return UnsupportInst(I);
                }
              } else {
                return UnsupportInst(I);
              }
            } else if (Func->getName() == "storeAddr") {
              // storeAddr(src, addr) -> indirect store through pointer/address.
              // For immediate address operand, backend uses `STI` convenience;
              // otherwise it does explicit address load + STR.
              if (CallI->arg_size() == 2) {
                Value *Src = CallI->getArgOperand(0);
                int SrcOff = -getIndex(Src, ValueOffsetMap, ValueOffsetCounter);
                FuncInstBufferStream << "\tLDR\t\tR1, R5, #" << SrcOff << "\n";

                Value *Addr = CallI->getArgOperand(1);
                if (int AddrID = addImmidiate(Addr, ImmBufferStream, ImmFlag,
                                              ImmIDMap, ImmIDCounter)) {
                  FuncInstBufferStream << "\tSTI\t\tR1, VALUE_" << AddrID
                                       << "\n";
                } else {
                  int AddrOff =
                      -getIndex(Addr, ValueOffsetMap, ValueOffsetCounter);
                  FuncInstBufferStream << "\tLDR\t\tR2, R5, #" << AddrOff
                                       << "\n"
                                       << "\tSTR\t\tR1, R2, #0\n";
                }
              } else {
                return UnsupportInst(I);
              }
            } else if (Func->getName() == "readLabelAddr") {
              // readLabelAddr("LABEL") -> take symbol address via LEA.
              // Result is a pointer value placed into this call's stack slot.
              if (CallI->arg_size() == 1) {
                int DesOff = -getIndex(&I, ValueOffsetMap, ValueOffsetCounter);

                Value *Str = CallI->getArgOperand(0);
                StringRef Label = getString(Str);

                if (Label != "") {
                  FuncInstBufferStream << "\tLEA\t\tR1, " << Label << "\n"
                                       << "\tSTR\t\tR1, R5, #" << DesOff
                                       << "\n";
                } else {
                  return UnsupportInst(I);
                }
              } else {
                return UnsupportInst(I);
              }
            } else if (CallI->arg_size() <= 5 && FuncLabelMap.count(Func)) {
              // User function call convention:
              // - first up to five args passed in R0..R4
              // - JSR by function name
              // - return value in R0 (stored if non-void)
              // Calls to functions outside current module are rejected unless
              // mapped by built-in helper names above.
              StringRef CalledFuncName = Func->getName();
              if (int ArgSize = CallI->arg_size()) {
                for (int i = 0; i < ArgSize; i++) {
                  Value *Arg = CallI->getArgOperand(i);
                  if (int ArgID = addImmidiate(Arg, ImmBufferStream, ImmFlag,
                                               ImmIDMap, ImmIDCounter)) {
                    FuncInstBufferStream << "\tLD\t\tR" << i << ", VALUE_"
                                         << ArgID << "\n";
                  } else {
                    int ArgOff =
                        -getIndex(Arg, ValueOffsetMap, ValueOffsetCounter);
                    FuncInstBufferStream << "\tLDR\t\tR" << i << ", R5, #"
                                         << ArgOff << "\n";
                  }
                }
              }
              FuncInstBufferStream << "\tJSR\t\t" << CalledFuncName << "\n";
              if (!CallI->getType()->isVoidTy()) {
                if (int ResID = addImmidiate(&I, ImmBufferStream, ImmFlag,
                                             ImmIDMap, ImmIDCounter)) {
                  FuncInstBufferStream << "\tST\t\tR0, VALUE_" << ResID << "\n";
                } else {
                  int ResOff =
                      -getIndex(&I, ValueOffsetMap, ValueOffsetCounter);
                  FuncInstBufferStream << "\tSTR\t\tR0, R5, #" << ResOff
                                       << "\n";
                }
              }
            } else {
              return UnsupportInst(I);
            }
          } else {
            return UnsupportInst(I);
          }
        } else if (auto *AllocaI = dyn_cast<AllocaInst>(&I)) {
          // Stack slots are virtualized through ValueOffsetMap; explicit alloca
          // no longer needs direct codegen here.
          continue;
        } else if (auto *PHIN = dyn_cast<PHINode>(&I)) {
          // PHI lowering strategy:
          // compare saved predecessor label (R7) against each incoming block
          // label, select matching value, and write to result slot.
          //
          // Mechanism details:
          // - On every branch/switch emission, R7 is loaded with current block
          //   label address via `LEA`.
          // - Here we negate R7 and add candidate source label address.
          // - Non-zero => not the predecessor we came from; continue scanning.
          int ResOff = -getIndex(&I, ValueOffsetMap, ValueOffsetCounter);

          FuncInstBufferStream << "\tNOT\t\tR0, R7\n"
                               << "\tADD\t\tR0, R0, #1\n";
          int ArgSize = PHIN->getNumIncomingValues();
          // Reserve a contiguous id range for PHI_NEXT labels so each incoming
          // edge can jump to a shared end label after successful match.
          int EndLableID = TempLabelCounter + ArgSize;
          for (unsigned int i = 0; i < ArgSize; ++i) {

            BasicBlock *SrcBB = PHIN->getIncomingBlock(i);
            std::string SrcBBName = getIndex(SrcBB, BBNameMap, BBNameCounter);

            ++TempLabelCounter;
            if (i < ArgSize - 1) {
              // Compare predecessor label with this incoming block label.
              FuncInstBufferStream << "\tLEA\t\tR1, " << SrcBBName << "\n"
                                   << "\tADD\t\tR1, R1, R0\n"
                                   << "\tBRnp\tPHI_NEXT_" << TempLabelCounter
                                   << "\n";
            }

            Value *Val = PHIN->getIncomingValue(i);
            if (int ValID = addImmidiate(Val, ImmBufferStream, ImmFlag,
                                         ImmIDMap, ImmIDCounter)) {
              FuncInstBufferStream << "\tLD\t\tR1, VALUE_" << ValID << "\n";
            } else {
              int ValOff = -getIndex(Val, ValueOffsetMap, ValueOffsetCounter);
              FuncInstBufferStream << "\tLDR\t\tR1, R5, #" << ValOff << "\n";
            }

            FuncInstBufferStream << "\tSTR\t\tR1, R5, #" << ResOff << "\n";
            if (i < ArgSize - 1) {
              // Matched one incoming edge; skip remaining candidates.
              FuncInstBufferStream << "\tBR\t\tPHI_NEXT_" << EndLableID << "\n";
            }
            FuncInstBufferStream << "PHI_NEXT_" << TempLabelCounter << "\n";
          }
        } else if (auto *RetI = dyn_cast<ReturnInst>(&I)) {
          // Function epilogue:
          // 1) move return value (if any) to R0
          // 2) restore callee-saved/call-frame registers
          // 3) HALT for entry function, RET otherwise
          bool hasRetVal = false;
          if (Value *Val = RetI->getReturnValue()) {
            hasRetVal = true;
            if (int ValID = addImmidiate(Val, ImmBufferStream, ImmFlag,
                                         ImmIDMap, ImmIDCounter)) {
              FuncInstBufferStream << "\tLD\t\tR0, VALUE_" << ValID << "\n";
            } else {
              int ValOff = -getIndex(Val, ValueOffsetMap, ValueOffsetCounter);
              FuncInstBufferStream << "\tLDR\t\tR0, R5, #" << ValOff << "\n";
            }
          }
          if (!NoComment) {
            FuncInstBufferStream << ";\trestore registers\n";
          }
          // Restore in reverse of prologue save layout:
          // [R5, R7, R4, R3, R2, R1, R0] relative to frame base.
          FuncInstBufferStream << "\tADD\t\tR6, R5, #0\n"
                               << "\tLDR\t\tR5, R6, #0\n"
                               << "\tLDR\t\tR7, R6, #1\n"
                               << "\tLDR\t\tR4, R6, #2\n"
                               << "\tLDR\t\tR3, R6, #3\n"
                               << "\tLDR\t\tR2, R6, #4\n"
                               << "\tLDR\t\tR1, R6, #5\n"
                               << "\tADD\t\tR6, R6, #7\n";
          if (!hasRetVal) {
            // Preserve caller-visible R0 when function returns void.
            FuncInstBufferStream << "\tLDR\t\tR0, R6, #-1\n";
          }
          if (FuncName == "LC3_main") {
            FuncInstBufferStream << "\tHALT\n";
          } else {
            FuncInstBufferStream << "\tRET\n";
          }
          continue;
        } else if (auto *CastI = dyn_cast<CastInst>(&I)) {
          // Integer casts collapse to move/copy in this word-sized model.
          int ResOff = -getIndex(&I, ValueOffsetMap, ValueOffsetCounter);

          Value *Op = CastI->getOperand(0);
          int OpOff = -getIndex(Op, ValueOffsetMap, ValueOffsetCounter);

          FuncInstBufferStream << "\tLDR\t\tR1, R5, #" << OpOff << "\n"
                               << "\tSTR\t\tR1, R5, #" << ResOff << "\n";
        } else if (auto *SelI = dyn_cast<SelectInst>(&I)) {
          // Select(cond, T, F): preload true value, overwrite with false when
          // condition is not positive, then store final selection.
          // Branch condition here treats `<= 0` as false and `> 0` as true.
          int ResOff = -getIndex(&I, ValueOffsetMap, ValueOffsetCounter);

          Value *Cond = SelI->getCondition();
          int CondOff = -getIndex(Cond, ValueOffsetMap, ValueOffsetCounter);

          Value *IfTrue = SelI->getTrueValue();
          if (int IfTrueID = addImmidiate(IfTrue, ImmBufferStream, ImmFlag,
                                          ImmIDMap, ImmIDCounter)) {
            FuncInstBufferStream << "\tLD\t\tR2, VALUE_" << IfTrueID << "\n";
          } else {
            int IfTrueOff =
                -getIndex(IfTrue, ValueOffsetMap, ValueOffsetCounter);
            FuncInstBufferStream << "\tLDR\t\tR2, R5, #" << IfTrueOff << "\n";
          }

          FuncInstBufferStream << "\tLDR\t\tR1, R5, #" << CondOff << "\n"
                               << "\tBRp\t\tSELECT_END_" << ++TempLabelCounter
                               << "\n";
          // Fallthrough path loads false value when condition is not positive.

          Value *IfFalse = SelI->getFalseValue();
          if (int IfFalseID = addImmidiate(IfFalse, ImmBufferStream, ImmFlag,
                                           ImmIDMap, ImmIDCounter)) {
            FuncInstBufferStream << "\tLD\t\tR2, VALUE_" << IfFalseID << "\n";
          } else {
            int IfFalseOff =
                -getIndex(IfFalse, ValueOffsetMap, ValueOffsetCounter);
            FuncInstBufferStream << "\tLDR\t\tR2, R5, #" << IfFalseOff << "\n";
          }

          FuncInstBufferStream << "SELECT_END_" << TempLabelCounter << "\n"
                               << "\tSTR\t\tR2, R5, #" << ResOff << "\n";
        } else if (auto *SwitchI = dyn_cast<SwitchInst>(&I)) {
          // Switch lowering compares condition value against each case
          // constant. If no case matches, branch to default destination block.
          // Convention: R7 stores current block label address for PHI support
          // in successor blocks.
          Value *Cond = SwitchI->getCondition();
          int CondOff = -getIndex(Cond, ValueOffsetMap, ValueOffsetCounter);

          BasicBlock *DefaultBB = SwitchI->getDefaultDest();
          std::string DefaultBBName =
              getIndex(DefaultBB, BBNameMap, BBNameCounter);

          FuncInstBufferStream << "\tLEA\t\tR7, " << BBName << "\n"
                               << "\tLDR\t\tR1, R5, #" << CondOff << "\n";

          bool isFirstCase = true;
          for (auto Case : SwitchI->cases()) {
            ConstantInt *Val = Case.getCaseValue();
            int ValID = addImmidiate(Val, ImmBufferStream, ImmFlag, ImmIDMap,
                                     ImmIDCounter);

            BasicBlock *DesBB = Case.getCaseSuccessor();
            std::string DesBBName = getIndex(DesBB, BBNameMap, BBNameCounter);

            if (dyn_cast<ConstantInt>(Val)->getSExtValue() == 0) {
              // For zero-case we can branch on condition codes directly after
              // touching R1 (or no-op add to refresh CC on non-first case).
              if (!isFirstCase) {
                FuncInstBufferStream << "\tADD\t\tR1, R1, #0";
              }
              FuncInstBufferStream << "\tBRz\t\t" << DesBBName << "\n";
            } else {
              // For non-zero case, add pre-negated constant and branch on zero.
              FuncInstBufferStream << "\tLD\t\tR2, VALUE_" << ValID << "\n"
                                   << "\tADD\t\tR2, R1, R2\n"
                                   << "\tBRz\t\t" << DesBBName << "\n";
            }

            isFirstCase = false;
          }
          FuncInstBufferStream << "\tBR\t\t" << DefaultBBName << "\n";
        } else {
          return UnsupportInst(I);
        }
      }

      FuncInstBufferStream << "\n";
      if (!ImmBuffer.empty()) {
        // Emit per-basic-block literal pool after the block body so generated
        // labels remain close and unique, while avoiding duplicate constants.
        if (!NoComment) {
          FuncInstBufferStream << ";\tconstant section for " << BBName << "\n";
        }
        FuncInstBufferStream << ImmBufferStream.str() << "\n";
      }
    }
    if (!NoComment) {
      // Summary metadata for quick debugging of generated function frame.
      InstBufferStream << ";\tfunction " << FuncName << "\n";
      InstBufferStream << ";\targument count: " << F.arg_size() << "\n";
      InstBufferStream << ";\tlocal variable count: " << ValueOffsetCounter
                       << "\n";
    }
    // Function symbol + entry basic-block label.
    InstBufferStream << FuncName << "\n";
    InstBufferStream << FuncLabelMap[&F] << "\n";
    if (!NoComment) {
      InstBufferStream << ";\tinit R6, R5, save old registers\n";
    }
    // Prologue: save volatile state and establish frame pointer.
    // Stack layout after prologue:
    //   [R5+0] old R5
    //   [R5+1] old R7
    //   [R5+2] old R4
    //   [R5+3] old R3
    //   [R5+4] old R2
    //   [R5+5] old R1
    //   [R5+6] old R0
    // Locals are then allocated at negative offsets from R5.
    InstBufferStream << "\tADD\t\tR6, R6, #-7\n"
                     << "\tSTR\t\tR0, R6, #6\n"
                     << "\tSTR\t\tR1, R6, #5\n"
                     << "\tSTR\t\tR2, R6, #4\n"
                     << "\tSTR\t\tR3, R6, #3\n"
                     << "\tSTR\t\tR4, R6, #2\n"
                     << "\tSTR\t\tR7, R6, #1\n"
                     << "\tSTR\t\tR5, R6, #0\n"
                     << "\tADD\t\tR5, R6, #0\n";
    // Reserve frame space for SSA values. Current implementation supports
    // up to 32 virtual locals due to immediate range and simple allocator.
    if (ValueOffsetCounter <= 32) {
      if (ValueOffsetCounter > 16) {
        InstBufferStream << "\tADD\t\tR6, R6, #-16\n";
        ValueOffsetCounter -= 16;
      }
      if (ValueOffsetCounter) {
        InstBufferStream << "\tADD\t\tR6, R6, #-" << ValueOffsetCounter << "\n";
      }
    } else {
      errs() << "Too many local variables: " << ValueOffsetCounter << "\n"
             << "No file generated.\n";
      return PreservedAnalyses::none();
    }
    // Spill incoming register arguments into their frame slots.
    if (int argSize = F.arg_size()) {
      if (!NoComment) {
        InstBufferStream << ";\tstore arguments\n";
      }
      for (int i = 0; i < argSize; i++) {
        Value *Arg = F.getArg(i);
        // Each argument is treated like a regular SSA value by assigning it a
        // frame slot in ValueOffsetMap.
        int ArgOff = -getIndex(Arg, ValueOffsetMap, ValueOffsetCounter);
        InstBufferStream << "\tSTR\t\tR" << i << ", R5, #" << ArgOff << "\n";
      }
    }

    InstBufferStream << FuncInstBufferStream.str();
  }

  // Finalize the module output and materialize the target file.
  Out.os() << InstBufferStream.str() << "\t.END";

  // Commit temporary output to final path only after successful generation.
  Out.keep();

  errs() << "One file generated: " << TargetFileName << "\n";

  return PreservedAnalyses::none();
}

extern "C" LLVM_ATTRIBUTE_WEAK ::PassPluginLibraryInfo llvmGetPassPluginInfo() {
  // Plugin registration entry point required by the new pass manager.
  // The pipeline name exposed by this plugin is `llvm-ir-to-lc3-pass`.
  return {LLVM_PLUGIN_API_VERSION,
          "LLVMIRToLC3Pass", // Plugin Name
          "v0.3",            // Plugin Version
          [](PassBuilder &PB) {
            // Register the callback to parse the pass name in command line
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "llvm-ir-to-lc3-pass") {
                    MPM.addPass(LLVMIRToLC3Pass());
                    return true;
                  }
                  return false;
                });
          }};
}