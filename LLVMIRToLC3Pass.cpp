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
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Analysis.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
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
//    - Arguments are pushed onto the stack by the caller.
//    - Return values are stored in a dedicated stack slot by the callee.
//    - Prologue saves R7, old R5, and establishes R5 as the frame pointer.
//    - Caller is responsible for popping arguments off the stack post-call.
//
// 4) Safety strategy
//    - If an instruction is unsupported, we abort file generation immediately.
//      Emitting no file is safer than emitting potentially wrong assembly.
//
// -----------------------------------------------------------------------------
static cl::opt<std::string>
    LC3StartAddr("lc3-start-addr",
                 cl::desc("Specify the starting address of LC-3 "
                          "program, default x3000"),
                 cl::value_desc("lc3-start-addr"), cl::init("x3000"));
// Example: `-lc3-start-addr="x3200"` to shift program placement.
// The pass forwards this string verbatim into `.ORIG`.

static cl::opt<std::string>
    LC3StackBase("lc3-stack-base",
                 cl::desc("Specify the base address of the stack in LC-3 "
                          "program, default xFE00"),
                 cl::value_desc("lc3-stack-base"), cl::init("xFE00"));
// Used only when lowering LC3Entry bootstrap sequence:
//   LD R6, STACK_BASE
//   STACK_BASE .FILL <value>

static cl::opt<bool>
    SignedMul("signed-mul",
              cl::desc("Use signed multiplication or not, default false"),
              cl::value_desc("signed-mul"), cl::init(false));
// Controls the lowering variant for `mul`:
// - false: pure repeated-addition assuming non-negative multiplier.
// - true:  sign-normalization prelude before repeated addition.

static cl::opt<bool> NoComment(
    "no-comment",
    cl::desc(
        "Generate pure LC-3 assembly code without any comment, default false"),
    cl::value_desc("no-comment"), cl::init(false));
// Helpful for final submission output where comments are undesirable.
// When false, emitted asm includes:
// - original LLVM IR (prefixed comments)
// - register usage hints
// - function/block metadata

static cl::opt<std::string>
    LC3Entry("lc3-entry",
             cl::desc("Specify the name of entry function of the LC-3 program, "
                      "default LC3_main"),
             cl::value_desc("lc3-entry"), cl::init("LC3_main"));
// Specify the entry function of the program, used when branching to the entry
// of the program:
//   LD R6, STACK_BASE
//   BR <value>

static cl::opt<std::string> LC3GlobalAddr(
    "lc3-global-addr",
    cl::desc("Specify the starting address for global variable data.  "
             "Default: placed after all functions (end of program).  "
             "Example: -lc3-global-addr=x4000"),
    cl::value_desc("lc3-global-addr"), cl::init(""));
// When empty (default), the global data section is emitted right before
// .END so it sits after all code.  When set, a separate .ORIG <addr>
// segment is emitted for globals, placed before the function bodies.

// Assign a stable integer id for a Value within a local map.
// The pass uses these ids for stack-slot indexing and constant labels.
//
// Function arguments are assigned specific negative IDs (e.g., -4, -5)
// to directly map them to their incoming stack offsets relative to R5.
// Regular SSA values are assigned monotonic 0-based counters (which are
// offset by +1 when used for labels).
int getIndex(Value *Val, DenseMap<Value *, int> &Map, int &Counter) {
  if (Map.count(Val) == 0) {
    if (auto *Arg = dyn_cast<Argument>(Val)) {
      unsigned Index = Arg->getArgNo();
      Map[Val] = -4 - Index;
    } else {
      Map[Val] = Counter++;
    }
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
              std::to_string(Counter++);
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
    int ValID = getIndex(Val, ImmMap, ImmCounter) + 1;
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
// Returns empty for non-i8 arrays so integer arrays are not mistaken for
// strings.
//
// Typical LLVM form handled:
//   @.str = private unnamed_addr constant [N x i8] c"...\00"
StringRef getString(Value *Val) {
  if (auto *GlobalVal = dyn_cast<GlobalVariable>(Val)) {
    if (GlobalVal->hasInitializer()) {
      Constant *Init = GlobalVal->getInitializer();
      if (auto *CDA = dyn_cast<ConstantDataArray>(Init)) {
        if (CDA->isString())
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
    int ValID = getIndex(Val, ImmMap, ImmCounter) + 1;
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
auto UnsupportedInst(Instruction &I) {
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


// Load a Value into R0.  Handles constants, strings, and frame slots.
static void emitLoadToR0(Value *Val, raw_string_ostream &OS,
                         raw_string_ostream &ImmBuffer,
                         DenseMap<Value *, bool> &ImmFlag,
                         DenseMap<Value *, int> &ImmMap, int &ImmCounter,
                         DenseMap<Value *, int> &ValueOffsetMap,
                         int &ValueOffsetCounter) {
  if (int ID = addImmidiate(Val, ImmBuffer, ImmFlag, ImmMap, ImmCounter))
    OS << "\tLD\t\tR0, VALUE_" << ID << "\n";
  else if (int StrID =
               addString(Val, ImmBuffer, ImmFlag, ImmMap, ImmCounter))
    OS << "\tLEA\t\tR0, VALUE_" << StrID << "\n";
  else {
    int Off = -getIndex(Val, ValueOffsetMap, ValueOffsetCounter);
    OS << "\tLDR\t\tR0, R5, #" << Off << "\n";
  }
}

// Load a Value into R1.
static void emitLoadToR1(Value *Val, raw_string_ostream &OS,
                         raw_string_ostream &ImmBuffer,
                         DenseMap<Value *, bool> &ImmFlag,
                         DenseMap<Value *, int> &ImmMap, int &ImmCounter,
                         DenseMap<Value *, int> &ValueOffsetMap,
                         int &ValueOffsetCounter) {
  if (int ID = addImmidiate(Val, ImmBuffer, ImmFlag, ImmMap, ImmCounter))
    OS << "\tLD\t\tR1, VALUE_" << ID << "\n";
  else {
    int Off = -getIndex(Val, ValueOffsetMap, ValueOffsetCounter);
    OS << "\tLDR\t\tR1, R5, #" << Off << "\n";
  }
}

// Bundles mutable state shared across the lowering pipeline so individual
// helper functions receive a single reference instead of a long parameter
// list.
struct LoweringState {
  ToolOutputFile &Out;
  raw_string_ostream &InstBufferStream;

  DenseMap<Value *, std::string> BBNameMap;
  DenseMap<Function *, std::string> FuncLabelMap;
  int BBNameCounter = 0;
  int ImmIDCounter = 0;
  int TempLabelCounter = 0;
  bool hasEntry = false;

  DenseMap<GlobalVariable *, int> GlobalWordOffsets;
  int GlobalWordCounter = 0;
  DenseSet<Value *> RealPointers;

  // Buffered global data section — emitted at end of program (or at
  // a user-specified address) instead of inline after the bootstrap.
  std::string GlobalDataBuffer;

  Module &M;
  const DataLayout &DL;

  LoweringState(ToolOutputFile &Out_, raw_string_ostream &IBS_, Module &M_)
      : Out(Out_), InstBufferStream(IBS_), M(M_), DL(M_.getDataLayout()) {}
};

// Forward declarations of extracted helpers.
static void scanGlobals(LoweringState &S);
static void emitBootstrapAndGlobals(LoweringState &S);
static void canonicalizeBlock(BasicBlock &BB, Type *WordTy);

// ---------------------------------------------------------------------------
// Centralised constant pool helpers
// ---------------------------------------------------------------------------
enum LC3Const : int {
  C_MSB_TOGGLE = 0x8000, // toggle bit 15 (unsigned-cmp / ashr)
  C_CHAR_MINUS = 45,     // '-'
  C_CHAR_ZERO  = 48,     // '0'
  C_10         = 10,
};

// Return a VALUE_<id> for an integer literal, registering it in the
// per-block constant pool on first use.
static int getConstID(LoweringState &S, raw_string_ostream &ImmBuffer,
                      DenseMap<Value *, bool> &ImmFlag,
                      DenseMap<Value *, int> &ImmMap, int Value) {
  auto *C = ConstantInt::get(Type::getInt32Ty(S.M.getContext()), Value);
  return addImmidiate(C, ImmBuffer, ImmFlag, ImmMap, S.ImmIDCounter);
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
  LoweringState S(Out, InstBufferStream, M);
  scanGlobals(S);

  if (!NoComment) {
    S.Out.os() << ";\tThis file is generated automatically by ir-to-lc3 pass.\n"
             << "\n"
             << ";\tR6 : stack pointer\n"
             << ";\tR5 : frame pointer\n"
             << "\n";
  }
  S.Out.os() << "\t.ORIG\t" << LC3StartAddr << "\n";

  // Pre-scan: assign a preliminary label to every defined function so
  // that forward references (e.g. call before definition) resolve.
  for (auto &F : M) {
    if (F.isIntrinsic() || F.isDeclaration())
      continue;
    std::string Label = F.getName().str() + "_entry";
    S.FuncLabelMap[&F] = Label;
  }

  // Visit each defined function and lower it independently.
  for (auto &F : S.M) {
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

    // Mark pointer-typed arguments as real pointers so that load/store
    // through them performs true memory dereference.
    for (auto &Arg : F.args()) {
      if (Arg.getType()->isPointerTy())
        S.RealPointers.insert(&Arg);
    }

    bool isFirstBB = true;
    for (auto &BB : F) {
      // Generate/fetch deterministic label for this basic block.
      std::string BBName = getIndex(&BB, S.BBNameMap, S.BBNameCounter);

      if (isFirstBB) {
        if (FuncName == LC3Entry)
          emitBootstrapAndGlobals(S);
        S.FuncLabelMap[&F] = BBName;
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

      // IR canonicalization (intrinsics, constants, control flow).
      canonicalizeBlock(BB, WordTy);

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
                                     S.ImmIDCounter)) {
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
                                     S.ImmIDCounter)) {
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
            FuncInstBufferStream << "SHL_LOOP_" << ++S.TempLabelCounter << "\n"
                                 << "\tADD\t\tR1, R1, R1\n"
                                 << "\tADD\t\tR2, R2, #-1\n"
                                 << "\tBRp\t\tSHL_LOOP_" << S.TempLabelCounter
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
                                   << S.TempLabelCounter + 1 << "\n"
                                   << "\tNOT\t\tR1, R1\n"
                                   << "\tADD\t\tR1, R1, #1\n"
                                   << "\tNOT\t\tR2, R2\n"
                                   << "\tADD\t\tR2, R2, #1\n";
            }
            FuncInstBufferStream << "MUL_LOOP_" << ++S.TempLabelCounter
                                 << "\n"
                                 // Exit when counter (R2) reaches zero.
                                 << "\tBRz\t\tMUL_END_" << S.TempLabelCounter
                                 << "\n"
                                 // Accumulate result += multiplicand.
                                 << "\tADD\t\tR3, R3, R1\n"
                                 // Decrement multiplier/counter.
                                 << "\tADD\t\tR2, R2, #-1\n"
                                 << "\tBR\t\tMUL_LOOP_" << S.TempLabelCounter
                                 << "\n"
                                 << "MUL_END_" << S.TempLabelCounter << "\n"
                                 << "\tSTR\t\tR3, R5, #" << ResOff << "\n";
            break;
          case Instruction::UDiv:
            // Unsigned division by repeated subtraction:
            // increment quotient (R3) while dividend remains non-negative.
            FuncInstBufferStream
                << "UDIV_LOOP_" << ++S.TempLabelCounter
                << "\n"
                // If R1 <= 0, stop attempting subtraction.
                << "\tBRnz\tUDIV_END_" << S.TempLabelCounter
                << "\n"
                // quotient++
                << "\tADD\t\tR3, R3, #1\n"
                // dividend -= divisor (R2 already negated above)
                << "\tADD\t\tR1, R1, R2\n"
                << "\tBR\t\tUDIV_LOOP_" << S.TempLabelCounter << "\n"
                << "UDIV_END_" << S.TempLabelCounter
                << "\n"
                // Correct overshoot when loop exits after crossing below zero.
                << "\tBRz\t\tUDIV_POST_" << S.TempLabelCounter << "\n"
                << "\tADD\t\tR3, R3, #-1\n"
                << "UDIV_POST_" << S.TempLabelCounter << "\n"
                << "\tSTR\t\tR3, R5, #" << ResOff << "\n";
            break;
          case Instruction::URem:
            // Unsigned remainder via repeated subtraction, storing final
            // residue.
            FuncInstBufferStream
                << "UREM_LOOP_" << ++S.TempLabelCounter
                << "\n"
                // Stop when subtraction would cross below zero.
                << "\tBRnz\tUREM_END_" << S.TempLabelCounter
                << "\n"
                // dividend += (-divisor)
                << "\tADD\t\tR1, R1, R3\n"
                << "\tBR\t\tUREM_LOOP_" << S.TempLabelCounter << "\n"
                << "UREM_END_" << S.TempLabelCounter
                << "\n"
                // If we crossed below zero, add divisor back once.
                << "\tBRz\t\tUREM_POST_" << S.TempLabelCounter << "\n"
                << "\tADD\t\tR1, R1, R2\n"
                << "UREM_POST_" << S.TempLabelCounter << "\n"
                << "\tSTR\t\tR1, R5, #" << ResOff << "\n";
            break;
          case Instruction::LShr:
            // R2: counter, R1: result, R0: temporary register
            // R3: source mask, R4: destiny mask (saved/restored because R4
            //     is now the global data base pointer)
            // This fallback executes only when earlier canonicalization leaves
            // an LShr. It shifts bits right one position per outer iteration.
            FuncInstBufferStream
                << "\tADD\t\tR6, R6, #-1\n"
                << "\tSTR\t\tR4, R6, #0\n"
                << "LSHR_OUT_LOOP_" << ++S.TempLabelCounter << "\n"
                << "\tAND\t\tR0, R0, #0\n"
                << "\tAND\t\tR3, R3, #0\n"
                << "\tADD\t\tR3, R3, #2\n"
                << "\tAND\t\tR4, R4, #0\n"
                << "\tADD\t\tR4, R4, #1\n"
                << "LSHR_IN_LOOP_" << S.TempLabelCounter
                << "\n"
                // Clear destination bit before testing corresponding source
                // bit.
                << "\tNOT\t\tR4, R4\n"
                << "\tAND\t\tR1, R1, R4\n"
                << "\tNOT\t\tR4, R4\n"
                << "\tAND\t\tR0, R1, R3\n"
                << "\tBRz\t\tLSHR_SKIP_" << S.TempLabelCounter
                << "\n"
                // If source bit is set, copy to shifted destination position.
                << "\tADD\t\tR1, R1, R4\n"
                << "LSHR_SKIP_" << S.TempLabelCounter
                << "\n"
                // Advance masks to next bit.
                << "\tADD\t\tR3, R3, R3\n"
                << "\tADD\t\tR4, R4, R4\n"
                << "\tBRnp\tLSHR_IN_LOOP_" << S.TempLabelCounter << "\n"
                << "\tADD\t\tR2, R2, #-1\n"
                << "\tBRp\t\tLSHR_OUT_LOOP_" << S.TempLabelCounter << "\n"
                << "\tSTR\t\tR1, R5, #" << ResOff << "\n"
                << "\tLDR\t\tR4, R6, #0\n"
                << "\tADD\t\tR6, R6, #1\n";
            break;
          case Instruction::AShr:
            // Arithmetic shift right: same mask-based inner loop as
            // LShr, but the sign bit (bit 15) is preserved across each
            // one-bit shift so it propagates right by N positions.
            //
            // The 0x8000 constant is emitted into the constant pool
            // via addImmidiate; its VALUE_<id> label is captured
            // before we start generating the instruction stream.
            {
              int ID8000 = getConstID(S, ImmBufferStream, ImmFlag,
                                      ImmIDMap, C_MSB_TOGGLE);

              FuncInstBufferStream
                  // Save R4 (global base) on stack.
                  << "\tADD\t\tR6, R6, #-1\n"
                  << "\tSTR\t\tR4, R6, #0\n"
                  // Save original sign bit onto stack.
                  << "\tLD\t\tR0, VALUE_" << ID8000 << "\n"
                  << "\tAND\t\tR0, R1, R0\n"
                  << "\tADD\t\tR6, R6, #-1\n"
                  << "\tSTR\t\tR0, R6, #0\n"
                  << "ASHR_OUT_LOOP_"
                  << ++S.TempLabelCounter << "\n"
                  // Inner loop: same bit-walk as LShr.
                  << "\tAND\t\tR0, R0, #0\n"
                  << "\tAND\t\tR3, R3, #0\n"
                  << "\tADD\t\tR3, R3, #2\n"
                  << "\tAND\t\tR4, R4, #0\n"
                  << "\tADD\t\tR4, R4, #1\n"
                  << "ASHR_IN_LOOP_"
                  << S.TempLabelCounter << "\n"
                  << "\tNOT\t\tR4, R4\n"
                  << "\tAND\t\tR1, R1, R4\n"
                  << "\tNOT\t\tR4, R4\n"
                  << "\tAND\t\tR0, R1, R3\n"
                  << "\tBRz\t\tASHR_SKIP_"
                  << S.TempLabelCounter << "\n"
                  << "\tADD\t\tR1, R1, R4\n"
                  << "ASHR_SKIP_"
                  << S.TempLabelCounter << "\n"
                  << "\tADD\t\tR3, R3, R3\n"
                  << "\tADD\t\tR4, R4, R4\n"
                  << "\tBRnp\tASHR_IN_LOOP_"
                  << S.TempLabelCounter << "\n"
                  // Restore sign bit (the inner loop's last
                  // iteration cleared bit 15).
                  << "\tLDR\t\tR0, R6, #0\n"
                  << "\tADD\t\tR1, R1, R0\n"
                  << "\tADD\t\tR2, R2, #-1\n"
                  << "\tBRp\t\tASHR_OUT_LOOP_"
                  << S.TempLabelCounter << "\n"
                  // Pop saved sign and R4.
                  << "\tADD\t\tR6, R6, #1\n"
                  << "\tLDR\t\tR4, R6, #0\n"
                  << "\tADD\t\tR6, R6, #1\n"
                  << "\tSTR\t\tR1, R5, #" << ResOff
                  << "\n";
              ++S.TempLabelCounter;
            }
            break;
          default:
            return UnsupportedInst(I);
          }
        } else if (auto *LoadI = dyn_cast<LoadInst>(&I)) {
          int ResOff = -getIndex(&I, ValueOffsetMap, ValueOffsetCounter);
          Value *PtrOp = LoadI->getPointerOperand();

          if (auto *GV = dyn_cast<GlobalVariable>(PtrOp)) {
            // Global: load via R4 + word offset.
            int GOff = S.GlobalWordOffsets.lookup(GV);
            if (GOff <= 31) {
              FuncInstBufferStream << "\tLDR\t\tR1, R4, #" << GOff
                                   << "\n";
            } else {
              // Offset too large for immediate; compute address first.
              FuncInstBufferStream << "\tADD\t\tR1, R4, #0\n";
              int Rem = GOff;
              while (Rem > 15) {
                FuncInstBufferStream << "\tADD\t\tR1, R1, #15\n";
                Rem -= 15;
              }
              if (Rem > 0)
                FuncInstBufferStream << "\tADD\t\tR1, R1, #" << Rem
                                     << "\n";
              FuncInstBufferStream << "\tLDR\t\tR1, R1, #0\n";
            }
            FuncInstBufferStream << "\tSTR\t\tR1, R5, #" << ResOff
                                 << "\n";
          } else if (S.RealPointers.count(PtrOp)) {
            // Real pointer: load address from frame, then dereference.
            int PtrOff =
                -getIndex(PtrOp, ValueOffsetMap, ValueOffsetCounter);
            FuncInstBufferStream
                << "\tLDR\t\tR1, R5, #" << PtrOff << "\n"
                << "\tLDR\t\tR1, R1, #0\n"
                << "\tSTR\t\tR1, R5, #" << ResOff << "\n";
          } else {
            // Regular SSA: frame-slot copy (existing behavior).
            int OpOff =
                -getIndex(PtrOp, ValueOffsetMap, ValueOffsetCounter);
            FuncInstBufferStream
                << "\tLDR\t\tR1, R5, #" << OpOff << "\n"
                << "\tSTR\t\tR1, R5, #" << ResOff << "\n";
          }
          // If the loaded value is a pointer, mark it so subsequent
          // loads/stores through it dereference correctly.
          if (LoadI->getType()->isPointerTy())
            S.RealPointers.insert(&I);
        } else if (auto *StoreI = dyn_cast<StoreInst>(&I)) {
          // Load value-to-store into R1.
          Value *Val = StoreI->getValueOperand();
          if (int ValID = addImmidiate(Val, ImmBufferStream, ImmFlag, ImmIDMap,
                                       S.ImmIDCounter)) {
            FuncInstBufferStream << "\tLD\t\tR1, VALUE_" << ValID << "\n";
          } else {
            int ValOff = -getIndex(Val, ValueOffsetMap, ValueOffsetCounter);
            FuncInstBufferStream << "\tLDR\t\tR1, R5, #" << ValOff << "\n";
          }

          Value *PtrOp = StoreI->getPointerOperand();

          if (auto *GV = dyn_cast<GlobalVariable>(PtrOp)) {
            // Global: store via R4 + word offset.
            int GOff = S.GlobalWordOffsets.lookup(GV);
            if (GOff <= 31) {
              FuncInstBufferStream << "\tSTR\t\tR1, R4, #" << GOff
                                   << "\n";
            } else {
              // Offset too large for immediate; compute address first.
              FuncInstBufferStream << "\tADD\t\tR2, R4, #0\n";
              int Rem = GOff;
              while (Rem > 15) {
                FuncInstBufferStream << "\tADD\t\tR2, R2, #15\n";
                Rem -= 15;
              }
              if (Rem > 0)
                FuncInstBufferStream << "\tADD\t\tR2, R2, #" << Rem
                                     << "\n";
              FuncInstBufferStream << "\tSTR\t\tR1, R2, #0\n";
            }
          } else if (S.RealPointers.count(PtrOp)) {
            // Real pointer: load target address into R2, store through it.
            int PtrOff =
                -getIndex(PtrOp, ValueOffsetMap, ValueOffsetCounter);
            FuncInstBufferStream
                << "\tLDR\t\tR2, R5, #" << PtrOff << "\n"
                << "\tSTR\t\tR1, R2, #0\n";
          } else {
            // Regular SSA: frame-slot copy (existing behavior).
            int PtrOff =
                -getIndex(PtrOp, ValueOffsetMap, ValueOffsetCounter);
            FuncInstBufferStream << "\tSTR\t\tR1, R5, #" << PtrOff
                                 << "\n";
          }
        } else if (auto *BranchI = dyn_cast<BranchInst>(&I)) {
          // Save current label in R7 so PHI lowering can infer predecessor
          // path.
          FuncInstBufferStream << "\tLEA\t\tR7, " << BBName << "\n";
          if (BranchI->isUnconditional()) {
            BasicBlock *SucBB = BranchI->getSuccessor(0);
            std::string SucBBName = getIndex(SucBB, S.BBNameMap, S.BBNameCounter);

            FuncInstBufferStream << "\tBR\t\t" << SucBBName << "\n";
          } else {
            // Conditional branch convention: zero => false path, non-zero =>
            // true.
            Value *Con = BranchI->getCondition();
            if (int ConID = addImmidiate(Con, ImmBufferStream, ImmFlag,
                                         ImmIDMap, S.ImmIDCounter)) {
              FuncInstBufferStream << "\tLD\t\tR1, VALUE_" << ConID << "\n";
            } else {
              int ConOff = -getIndex(Con, ValueOffsetMap, ValueOffsetCounter);
              FuncInstBufferStream << "\tLDR\t\tR1, R5, #" << ConOff << "\n";
            }

            BasicBlock *IfTrueBB = BranchI->getSuccessor(0);
            std::string IfTrueBBName =
                getIndex(IfTrueBB, S.BBNameMap, S.BBNameCounter);

            BasicBlock *IfFalseBB = BranchI->getSuccessor(1);
            std::string IfFalseBBName =
                getIndex(IfFalseBB, S.BBNameMap, S.BBNameCounter);

            FuncInstBufferStream << "\tBRz\t\t" << IfFalseBBName << "\n"
                                 << "\tBR\t\t" << IfTrueBBName << "\n";
          }
        } else if (auto *ICmpI = dyn_cast<ICmpInst>(&I)) {
          // Comparison lowers to:
          // 1) R1 <- A - B  (after optional MSB-toggle for unsigned)
          // 2) branch on condition-code
          // 3) materialize boolean 0/1 in R3
          //
          // Unsigned comparisons use the technique:
          //   A <_u B  iff  (A XOR 0x8000) <_s (B XOR 0x8000)
          // toggling the MSB reduces unsigned comparison to signed comparison.
          auto Pred = ICmpI->getPredicate();
          bool isUnsigned = ICmpI->isUnsigned();

          FuncInstBufferStream << "\tAND\t\tR3, R3, #0\n";

          int ResOff = -getIndex(&I, ValueOffsetMap, ValueOffsetCounter);

          // Emit the MSB-toggle constant (0x8000) into the constant pool once
          // per block when needed.  ADD with 0x8000 toggles bit 15 because
          // modulo-2^16 addition flips the sign bit.
          int MSBToggleID = 0;
          if (isUnsigned) {
            MSBToggleID = getConstID(S, ImmBufferStream, ImmFlag,
                                     ImmIDMap, C_MSB_TOGGLE);
          }

          // --- operand A ---
          Value *A = ICmpI->getOperand(0);
          if (int AID = addImmidiate(A, ImmBufferStream, ImmFlag, ImmIDMap,
                                     S.ImmIDCounter)) {
            FuncInstBufferStream << "\tLD\t\tR1, VALUE_" << AID << "\n";
          } else {
            int AOff = -getIndex(A, ValueOffsetMap, ValueOffsetCounter);
            FuncInstBufferStream << "\tLDR\t\tR1, R5, #" << AOff << "\n";
          }

          if (isUnsigned) {
            FuncInstBufferStream << "\tLD\t\tR0, VALUE_" << MSBToggleID << "\n"
                                 << "\tADD\t\tR1, R1, R0\n";
          }

          // --- operand B ---
          Value *B = ICmpI->getOperand(1);
          if (int BID = addImmidiate(B, ImmBufferStream, ImmFlag, ImmIDMap,
                                     S.ImmIDCounter)) {
            FuncInstBufferStream << "\tLD\t\tR2, VALUE_" << BID << "\n";
            if (isUnsigned) {
              // Constant was not negated in canonicalization pass #2.
              // Toggle MSB then negate to form -(C XOR 0x8000).
              FuncInstBufferStream << "\tADD\t\tR2, R2, R0\n"
                                   << "\tNOT\t\tR2, R2\n"
                                   << "\tADD\t\tR2, R2, #1\n";
            }
          } else {
            int BOff = -getIndex(B, ValueOffsetMap, ValueOffsetCounter);
            FuncInstBufferStream << "\tLDR\t\tR2, R5, #" << BOff << "\n";
            if (isUnsigned) {
              // Toggle MSB before negating.
              FuncInstBufferStream << "\tADD\t\tR2, R2, R0\n";
            }
            FuncInstBufferStream << "\tNOT\t\tR2, R2\n"
                                 << "\tADD\t\tR2, R2, #1\n";
          }

          FuncInstBufferStream << "\tADD\t\tR1, R1, R2\n";

          switch (Pred) {
          case CmpInst::ICMP_EQ:
            // Branch away when non-zero => equal is false.
            FuncInstBufferStream << "\tBRnp\tICMP_END_" << ++S.TempLabelCounter
                                 << "\n";
            break;
          case CmpInst::ICMP_NE:
            // Branch away when zero => not-equal is false.
            FuncInstBufferStream << "\tBRz\t\tICMP_END_" << ++S.TempLabelCounter
                                 << "\n";
            break;
          case CmpInst::ICMP_SGT:
          case CmpInst::ICMP_UGT:
            // Greater-than true only for positive result.
            FuncInstBufferStream << "\tBRnz\tICMP_END_" << ++S.TempLabelCounter
                                 << "\n";
            break;
          case CmpInst::ICMP_SGE:
          case CmpInst::ICMP_UGE:
            // >= true for zero or positive.
            FuncInstBufferStream << "\tBRn\t\tICMP_END_" << ++S.TempLabelCounter
                                 << "\n";
            break;
          case CmpInst::ICMP_SLT:
          case CmpInst::ICMP_ULT:
            // Less-than true only for negative result.
            FuncInstBufferStream << "\tBRzp\tICMP_END_" << ++S.TempLabelCounter
                                 << "\n";
            break;
          case CmpInst::ICMP_SLE:
          case CmpInst::ICMP_ULE:
            // <= true for zero or negative.
            FuncInstBufferStream << "\tBRp\t\tICMP_END_" << ++S.TempLabelCounter
                                 << "\n";
            break;
          default:
            return UnsupportedInst(I);
          }

          FuncInstBufferStream << "\tADD\t\tR3, R3, #1\n"
                               << "ICMP_END_" << S.TempLabelCounter << "\n"
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
                                          ImmIDMap, S.ImmIDCounter)) {
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
                return UnsupportedInst(I);
              }
            } else if (Func->getName() == "printStrAddr") {
              // printStrAddr(ptr) -> load pointer to R0, then PUTS.
              // Contract: pointer must address a memory region terminated by 0.
              if (CallI->arg_size() == 1) {
                Value *Addr = CallI->getArgOperand(0);
                if (int AddrID = addImmidiate(Addr, ImmBufferStream, ImmFlag,
                                              ImmIDMap, S.ImmIDCounter)) {
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
                return UnsupportedInst(I);
              }
            } else if (Func->getName() == "printCharAddr") {
              // printCharAddr(ptr) -> load byte/word at ptr, then OUT.
              // Uses R1 as temporary pointer register and R0 as OUT operand.
              if (CallI->arg_size() == 1) {
                Value *Addr = CallI->getArgOperand(0);
                if (int AddrID = addImmidiate(Addr, ImmBufferStream, ImmFlag,
                                              ImmIDMap, S.ImmIDCounter)) {
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
                return UnsupportedInst(I);
              }
            } else if (Func->getName() == "printChar") {
              // printChar(ch) -> move char to R0, then OUT.
              // No masking is done here; caller/frontend should provide char in
              // LC-3 compatible range if strict behavior is required.
              if (CallI->arg_size() == 1) {
                Value *Char = CallI->getArgOperand(0);
                if (int CharID = addImmidiate(Char, ImmBufferStream, ImmFlag,
                                              ImmIDMap, S.ImmIDCounter)) {
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
                return UnsupportedInst(I);
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
                  return UnsupportedInst(I);
                }
              } else {
                return UnsupportedInst(I);
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
                  return UnsupportedInst(I);
                }
              } else {
                return UnsupportedInst(I);
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
                                              ImmIDMap, S.ImmIDCounter)) {
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
                return UnsupportedInst(I);
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
                  return UnsupportedInst(I);
                }
              } else {
                return UnsupportedInst(I);
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
                                              ImmIDMap, S.ImmIDCounter)) {
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
                return UnsupportedInst(I);
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
                  return UnsupportedInst(I);
                }
              } else {
                return UnsupportedInst(I);
              }
            } else if (Func->getName() == "printf") {
              // Compile-time format-string lowering for printf.
              // Supports %d (signed int), %s (string), %c (char).
              // Requires a constant format string; otherwise aborts.
              Value *Fmt = CallI->getArgOperand(0);
              StringRef FmtStr = getString(Fmt);
              if (FmtStr.empty())
                return UnsupportedInst(I);

              int argIdx = 1;
              size_t pos = 0;
              while (pos < FmtStr.size()) {
                if (FmtStr[pos] == '%' && pos + 1 < FmtStr.size()) {
                  pos++;
                  switch (FmtStr[pos]) {
                  case 'd': {
                    // Emit integer-print routine.
                    // Load argument into R1.
                    Value *Arg = CallI->getArgOperand(argIdx++);
                    emitLoadToR1(Arg, FuncInstBufferStream, ImmBufferStream,
                                 ImmFlag, ImmIDMap, S.ImmIDCounter,
                                 ValueOffsetMap, ValueOffsetCounter);
                    // Emit minus-sign constant into pool.
                    int MinusID = getConstID(S, ImmBufferStream, ImmFlag,
                                            ImmIDMap, C_CHAR_MINUS);
                    int ZeroID = getConstID(S, ImmBufferStream, ImmFlag,
                                            ImmIDMap, C_CHAR_ZERO);

                    FuncInstBufferStream
                        << "\tADD\t\tR1, R1, #0\n"
                        << "\tBRzp\t\tPRINTF_POS_"
                        << S.TempLabelCounter << "\n"
                        << "\tLD\t\tR0, VALUE_" << MinusID << "\n"
                        << "\tOUT\n"
                        << "\tNOT\t\tR1, R1\n"
                        << "\tADD\t\tR1, R1, #1\n"
                        << "PRINTF_POS_" << S.TempLabelCounter << "\n"
                        << "\tAND\t\tR0, R0, #0\n"
                        << "\tADD\t\tR6, R6, #-1\n"
                        << "\tSTR\t\tR0, R6, #0\n"
                        << "PRINTF_DIV_" << S.TempLabelCounter
                        << "\n"
                        << "\tAND\t\tR3, R3, #0\n"
                        << "\tADD\t\tR2, R1, #0\n"
                        << "PRINTF_DIV_INNER_"
                        << S.TempLabelCounter << "\n"
                        << "\tADD\t\tR2, R2, #-10\n"
                        << "\tBRn\t\tPRINTF_DIV_DONE_"
                        << S.TempLabelCounter << "\n"
                        << "\tADD\t\tR3, R3, #1\n"
                        << "\tBR\t\tPRINTF_DIV_INNER_"
                        << S.TempLabelCounter << "\n"
                        << "PRINTF_DIV_DONE_"
                        << S.TempLabelCounter << "\n"
                        << "\tADD\t\tR2, R2, #10\n"
                        << "\tLD\t\tR0, VALUE_" << ZeroID
                        << "\n"
                        << "\tADD\t\tR0, R0, R2\n"
                        << "\tADD\t\tR6, R6, #-1\n"
                        << "\tSTR\t\tR0, R6, #0\n"
                        << "\tADD\t\tR1, R3, #0\n"
                        << "\tBRp\t\tPRINTF_DIV_"
                        << S.TempLabelCounter << "\n"
                        << "PRINTF_OUT_" << S.TempLabelCounter
                        << "\n"
                        << "\tLDR\t\tR0, R6, #0\n"
                        << "\tADD\t\tR6, R6, #1\n"
                        << "\tADD\t\tR0, R0, #0\n"
                        << "\tBRz\t\tPRINTF_DONE_"
                        << S.TempLabelCounter << "\n"
                        << "\tOUT\n"
                        << "\tBR\t\tPRINTF_OUT_"
                        << S.TempLabelCounter << "\n"
                        << "PRINTF_DONE_"
                        << S.TempLabelCounter << "\n";
                    ++S.TempLabelCounter;
                    break;
                  }
                  case 's': {
                    // Emit PUTS for string pointer argument.
                    Value *Arg = CallI->getArgOperand(argIdx++);
                    emitLoadToR0(Arg, FuncInstBufferStream, ImmBufferStream,
                                 ImmFlag, ImmIDMap, S.ImmIDCounter,
                                 ValueOffsetMap, ValueOffsetCounter);
                    FuncInstBufferStream << "\tPUTS\n";
                    break;
                  }
                  case 'c': {
                    // Emit OUT for char argument.
                    Value *Arg = CallI->getArgOperand(argIdx++);
                    emitLoadToR0(Arg, FuncInstBufferStream, ImmBufferStream,
                                 ImmFlag, ImmIDMap, S.ImmIDCounter,
                                 ValueOffsetMap, ValueOffsetCounter);
                    FuncInstBufferStream << "\tOUT\n";
                    break;
                  }
                  default:
                    return UnsupportedInst(I);
                  }
                  pos++;
                } else {
                  // Literal text segment: emit .STRINGZ + LEA + PUTS.
                  size_t start = pos;
                  while (pos < FmtStr.size() && FmtStr[pos] != '%')
                    pos++;
                  std::string Lit =
                      FmtStr.slice(start, pos).str();
                  // Emit .STRINGZ into constant pool with a unique
                  // label.
                  int LitID = ++S.TempLabelCounter;
                  ImmBufferStream
                      << "PRINTF_LIT_" << LitID << "\n"
                      << "\t.STRINGZ\t\"" << Lit << "\"\n";
                  FuncInstBufferStream
                      << "\tLEA\t\tR0, PRINTF_LIT_" << LitID
                      << "\n"
                      << "\tPUTS\n";
                }
              }
            } else if (Func->getName() == "sprintf") {
              // Compile-time sprintf: like printf but writes to a buffer.
              // First arg = buffer, second arg = format string.
              Value *Buf = CallI->getArgOperand(0);
              int BufOff =
                  -getIndex(Buf, ValueOffsetMap, ValueOffsetCounter);
              Value *Fmt = CallI->getArgOperand(1);
              StringRef FmtStr = getString(Fmt);
              if (FmtStr.empty())
                return UnsupportedInst(I);

              // Load buffer pointer into R3 (saved/restored around %d).
              FuncInstBufferStream << "\tLDR\t\tR3, R5, #" << BufOff
                                   << "\n";

              int argIdx = 2;
              size_t pos = 0;
              while (pos < FmtStr.size()) {
                if (FmtStr[pos] == '%' && pos + 1 < FmtStr.size()) {
                  pos++;
                  switch (FmtStr[pos]) {
                  case 'd': {
                    Value *Arg = CallI->getArgOperand(argIdx++);
                    emitLoadToR1(Arg, FuncInstBufferStream, ImmBufferStream,
                                 ImmFlag, ImmIDMap, S.ImmIDCounter,
                                 ValueOffsetMap, ValueOffsetCounter);
                    int MinusID = getConstID(S, ImmBufferStream, ImmFlag,
                                            ImmIDMap, C_CHAR_MINUS);
                    int ZeroID = getConstID(S, ImmBufferStream, ImmFlag,
                                            ImmIDMap, C_CHAR_ZERO);

                    // Save R3 (buffer ptr) on stack during division.
                    FuncInstBufferStream
                        << "\tADD\t\tR6, R6, #-1\n"
                        << "\tSTR\t\tR3, R6, #0\n"
                        << "\tADD\t\tR1, R1, #0\n"
                        << "\tBRzp\t\tSPRINTF_POS_"
                        << S.TempLabelCounter << "\n"
                        << "\tLD\t\tR0, VALUE_" << MinusID << "\n"
                        << "\tSTR\t\tR0, R3, #0\n"
                        << "\tADD\t\tR3, R3, #1\n"
                        << "\tNOT\t\tR1, R1\n"
                        << "\tADD\t\tR1, R1, #1\n"
                        << "SPRINTF_POS_" << S.TempLabelCounter
                        << "\n"
                        << "\tAND\t\tR0, R0, #0\n"
                        << "\tADD\t\tR6, R6, #-1\n"
                        << "\tSTR\t\tR0, R6, #0\n"
                        << "SPRINTF_DIV_"
                        << S.TempLabelCounter << "\n"
                        << "\tAND\t\tR3, R3, #0\n"
                        << "\tADD\t\tR2, R1, #0\n"
                        << "SPRINTF_DIV_INNER_"
                        << S.TempLabelCounter << "\n"
                        << "\tADD\t\tR2, R2, #-10\n"
                        << "\tBRn\t\tSPRINTF_DIV_DONE_"
                        << S.TempLabelCounter << "\n"
                        << "\tADD\t\tR3, R3, #1\n"
                        << "\tBR\t\tSPRINTF_DIV_INNER_"
                        << S.TempLabelCounter << "\n"
                        << "SPRINTF_DIV_DONE_"
                        << S.TempLabelCounter << "\n"
                        << "\tADD\t\tR2, R2, #10\n"
                        << "\tLD\t\tR0, VALUE_" << ZeroID
                        << "\n"
                        << "\tADD\t\tR0, R0, R2\n"
                        << "\tADD\t\tR6, R6, #-1\n"
                        << "\tSTR\t\tR0, R6, #0\n"
                        << "\tADD\t\tR1, R3, #0\n"
                        << "\tBRp\t\tSPRINTF_DIV_"
                        << S.TempLabelCounter << "\n"
                        // Restore buffer ptr from stack.
                        << "\tLDR\t\tR3, R6, #2\n"
                        << "SPRINTF_OUT_"
                        << S.TempLabelCounter << "\n"
                        << "\tLDR\t\tR0, R6, #1\n"
                        << "\tADD\t\tR6, R6, #1\n"
                        << "\tADD\t\tR0, R0, #0\n"
                        << "\tBRz\t\tSPRINTF_DONE_"
                        << S.TempLabelCounter << "\n"
                        << "\tSTR\t\tR0, R3, #0\n"
                        << "\tADD\t\tR3, R3, #1\n"
                        << "\tBR\t\tSPRINTF_OUT_"
                        << S.TempLabelCounter << "\n"
                        << "SPRINTF_DONE_"
                        << S.TempLabelCounter << "\n"
                        << "\tADD\t\tR6, R6, #1\n";
                    ++S.TempLabelCounter;
                    break;
                  }
                  case 's': {
                    Value *Arg = CallI->getArgOperand(argIdx++);
                    int SrcOff = -getIndex(Arg, ValueOffsetMap,
                                          ValueOffsetCounter);
                    // Copy string to buffer.
                    FuncInstBufferStream
                        << "\tLDR\t\tR1, R5, #" << SrcOff
                        << "\n"
                        << "SPRINTF_STRCPY_"
                        << S.TempLabelCounter << "\n"
                        << "\tLDR\t\tR0, R1, #0\n"
                        << "\tBRz\t\tSPRINTF_STRDONE_"
                        << S.TempLabelCounter << "\n"
                        << "\tSTR\t\tR0, R3, #0\n"
                        << "\tADD\t\tR1, R1, #1\n"
                        << "\tADD\t\tR3, R3, #1\n"
                        << "\tBR\t\tSPRINTF_STRCPY_"
                        << S.TempLabelCounter << "\n"
                        << "SPRINTF_STRDONE_"
                        << S.TempLabelCounter << "\n";
                    ++S.TempLabelCounter;
                    break;
                  }
                  case 'c': {
                    Value *Arg = CallI->getArgOperand(argIdx++);
                    emitLoadToR0(Arg, FuncInstBufferStream, ImmBufferStream,
                                 ImmFlag, ImmIDMap, S.ImmIDCounter,
                                 ValueOffsetMap, ValueOffsetCounter);
                    FuncInstBufferStream
                        << "\tSTR\t\tR0, R3, #0\n"
                        << "\tADD\t\tR3, R3, #1\n";
                    break;
                  }
                  default:
                    return UnsupportedInst(I);
                  }
                  pos++;
                } else {
                  // Copy literal text to buffer.
                  size_t start = pos;
                  while (pos < FmtStr.size() && FmtStr[pos] != '%')
                    pos++;
                  std::string Lit =
                      FmtStr.slice(start, pos).str();
                  int LitID = ++S.TempLabelCounter;
                  ImmBufferStream
                      << "SPRINTF_LIT_" << LitID << "\n"
                      << "\t.STRINGZ\t\"" << Lit << "\"\n";
                  FuncInstBufferStream
                      << "\tLEA\t\tR1, SPRINTF_LIT_" << LitID
                      << "\n"
                      << "SPRINTF_LITCPY_"
                      << S.TempLabelCounter << "\n"
                      << "\tLDR\t\tR0, R1, #0\n"
                      << "\tBRz\t\tSPRINTF_LITDONE_"
                      << S.TempLabelCounter << "\n"
                      << "\tSTR\t\tR0, R3, #0\n"
                      << "\tADD\t\tR1, R1, #1\n"
                      << "\tADD\t\tR3, R3, #1\n"
                      << "\tBR\t\tSPRINTF_LITCPY_"
                      << S.TempLabelCounter << "\n"
                      << "SPRINTF_LITDONE_"
                      << S.TempLabelCounter << "\n";
                  ++S.TempLabelCounter;
                }
              }
              // Null-terminate the buffer.
              FuncInstBufferStream
                  << "\tAND\t\tR0, R0, #0\n"
                  << "\tSTR\t\tR0, R3, #0\n";
            } else if (Func->getName() == "scanf") {
              // Compile-time format-string lowering for scanf.
              // Supports %d, %s, %c. Stores results through pointer args.
              Value *Fmt = CallI->getArgOperand(0);
              StringRef FmtStr = getString(Fmt);
              if (FmtStr.empty())
                return UnsupportedInst(I);

              int ZeroID = getConstID(S, ImmBufferStream, ImmFlag,
                                      ImmIDMap, C_CHAR_ZERO);
              int MinusID2 = getConstID(S, ImmBufferStream, ImmFlag,
                                        ImmIDMap, C_CHAR_MINUS);

              int argIdx = 1;
              size_t pos = 0;
              while (pos < FmtStr.size()) {
                if (FmtStr[pos] == '%' && pos + 1 < FmtStr.size()) {
                  pos++;
                  switch (FmtStr[pos]) {
                  case 'd': {
                    Value *Arg = CallI->getArgOperand(argIdx++);
                    int PtrOff = -getIndex(Arg, ValueOffsetMap,
                                          ValueOffsetCounter);
                    // R2 = destination pointer, R4 = accumulator
                    // (R4 is saved/restored because it's the global
                    // base pointer).
                    FuncInstBufferStream
                        << "\tLDR\t\tR2, R5, #" << PtrOff
                        << "\n"
                        << "\tADD\t\tR6, R6, #-1\n"
                        << "\tSTR\t\tR4, R6, #0\n"
                        << "\tAND\t\tR4, R4, #0\n"
                        // Skip whitespace.
                        << "SCANF_SKIPWS_"
                        << S.TempLabelCounter << "\n"
                        << "\tGETC\n"
                        << "\tADD\t\tR1, R0, #-16\n"
                        << "\tADD\t\tR1, R1, #-16\n"
                        << "\tBRz\t\tSCANF_SKIPWS_"
                        << S.TempLabelCounter << "\n"
                        << "\tADD\t\tR1, R0, #-13\n"
                        << "\tBRz\t\tSCANF_SKIPWS_"
                        << S.TempLabelCounter << "\n"
                        << "\tADD\t\tR1, R0, #-10\n"
                        << "\tBRz\t\tSCANF_SKIPWS_"
                        << S.TempLabelCounter << "\n"
                        // Check for '-'.
                        << "\tAND\t\tR3, R3, #0\n"
                        << "\tLD\t\tR1, VALUE_" << MinusID2
                        << "\n"
                        << "\tNOT\t\tR1, R1\n"
                        << "\tADD\t\tR1, R1, #1\n"
                        << "\tADD\t\tR1, R0, R1\n"
                        << "\tBRnp\t\tSCANF_DIGIT_"
                        << S.TempLabelCounter << "\n"
                        << "\tADD\t\tR3, R3, #1\n"
                        << "\tGETC\n"
                        // Process digits.  R4 = accumulator, R3 = sign.
                        << "SCANF_DIGIT_"
                        << S.TempLabelCounter << "\n"
                        << "\tLD\t\tR1, VALUE_" << ZeroID
                        << "\n"
                        << "\tNOT\t\tR1, R1\n"
                        << "\tADD\t\tR1, R1, #1\n"
                        << "\tADD\t\tR1, R0, R1\n"
                        << "\tBRn\t\tSCANF_DONE_"
                        << S.TempLabelCounter << "\n"
                        << "\tADD\t\tR1, R1, #-10\n"
                        << "\tBRzp\t\tSCANF_DONE_"
                        << S.TempLabelCounter << "\n"
                        // R4 = R4 * 10 + digit (R1).
                        << "\tADD\t\tR0, R4, #0\n"
                        << "\tADD\t\tR4, R4, R4\n"
                        << "\tADD\t\tR4, R4, R4\n"
                        << "\tADD\t\tR4, R4, R0\n"
                        << "\tADD\t\tR4, R4, R4\n"
                        << "\tADD\t\tR4, R4, R1\n"
                        << "\tGETC\n"
                        << "\tBR\t\tSCANF_DIGIT_"
                        << S.TempLabelCounter << "\n"
                        << "SCANF_DONE_"
                        << S.TempLabelCounter << "\n"
                        // Apply sign.
                        << "\tADD\t\tR3, R3, #0\n"
                        << "\tBRz\t\tSCANF_STORE_"
                        << S.TempLabelCounter << "\n"
                        << "\tNOT\t\tR4, R4\n"
                        << "\tADD\t\tR4, R4, #1\n"
                        << "SCANF_STORE_"
                        << S.TempLabelCounter << "\n"
                        << "\tSTR\t\tR4, R2, #0\n"
                        << "\tLDR\t\tR4, R6, #0\n"
                        << "\tADD\t\tR6, R6, #1\n";
                    ++S.TempLabelCounter;
                    break;
                  }
                  case 's': {
                    Value *Arg = CallI->getArgOperand(argIdx++);
                    int PtrOff = -getIndex(Arg, ValueOffsetMap,
                                          ValueOffsetCounter);
                    // Read string: skip whitespace, read until
                    // whitespace, store with null terminator.
                    FuncInstBufferStream
                        << "\tLDR\t\tR2, R5, #" << PtrOff
                        << "\n"
                        << "SCANF_SSKIP_"
                        << S.TempLabelCounter << "\n"
                        << "\tGETC\n"
                        << "\tADD\t\tR0, R0, #-16\n"
                        << "\tADD\t\tR0, R0, #-16\n"
                        << "\tBRz\t\tSCANF_SSKIP_"
                        << S.TempLabelCounter << "\n"
                        << "\tADD\t\tR0, R0, #-16\n"
                        << "\tBRz\t\tSCANF_SSKIP_"
                        << S.TempLabelCounter << "\n"
                        << "SCANF_SLOOP_"
                        << S.TempLabelCounter << "\n"
                        << "\tSTR\t\tR0, R2, #0\n"
                        << "\tADD\t\tR2, R2, #1\n"
                        << "\tGETC\n"
                        << "\tADD\t\tR1, R0, #0\n"
                        << "\tADD\t\tR1, R1, #-16\n"
                        << "\tADD\t\tR1, R1, #-16\n"
                        << "\tBRz\t\tSCANF_SEND_"
                        << S.TempLabelCounter << "\n"
                        << "\tADD\t\tR1, R0, #0\n"
                        << "\tADD\t\tR1, R1, #-16\n"
                        << "\tBRz\t\tSCANF_SEND_"
                        << S.TempLabelCounter << "\n"
                        << "\tADD\t\tR1, R1, #-16\n"
                        << "\tBRz\t\tSCANF_SEND_"
                        << S.TempLabelCounter << "\n"
                        << "\tBR\t\tSCANF_SLOOP_"
                        << S.TempLabelCounter << "\n"
                        << "SCANF_SEND_"
                        << S.TempLabelCounter << "\n"
                        << "\tAND\t\tR0, R0, #0\n"
                        << "\tSTR\t\tR0, R2, #0\n";
                    ++S.TempLabelCounter;
                    break;
                  }
                  case 'c': {
                    Value *Arg = CallI->getArgOperand(argIdx++);
                    int PtrOff = -getIndex(Arg, ValueOffsetMap,
                                          ValueOffsetCounter);
                    FuncInstBufferStream
                        << "\tGETC\n"
                        << "\tLDR\t\tR2, R5, #" << PtrOff
                        << "\n"
                        << "\tSTR\t\tR0, R2, #0\n";
                    break;
                  }
                  default:
                    return UnsupportedInst(I);
                  }
                  pos++;
                } else {
                  // Skip literal characters in scanf format.
                  // We just consume matching input.
                  while (pos < FmtStr.size() && FmtStr[pos] != '%')
                    pos++;
                }
              }
            } else if (Func->getName() == "sscanf") {
              // sscanf: like scanf but reads from a buffer (arg 0)
              // instead of console.  Format string is arg 1.
              Value *BufSrc = CallI->getArgOperand(0);
              int SrcOff =
                  -getIndex(BufSrc, ValueOffsetMap, ValueOffsetCounter);
              Value *Fmt = CallI->getArgOperand(1);
              StringRef FmtStr = getString(Fmt);
              if (FmtStr.empty())
                return UnsupportedInst(I);

              // Load source pointer into a stack slot for reuse.
              FuncInstBufferStream << "\tADD\t\tR6, R6, #-1\n"
                                   << "\tLDR\t\tR1, R5, #" << SrcOff
                                   << "\n"
                                   << "\tSTR\t\tR1, R6, #0\n";

              int ZeroID3 = getConstID(S, ImmBufferStream, ImmFlag,
                                       ImmIDMap, C_CHAR_ZERO);
              int MinusID3 = getConstID(S, ImmBufferStream, ImmFlag,
                                        ImmIDMap, C_CHAR_MINUS);

              int argIdx = 2;
              size_t pos = 0;
              while (pos < FmtStr.size()) {
                if (FmtStr[pos] == '%' && pos + 1 < FmtStr.size()) {
                  pos++;
                  switch (FmtStr[pos]) {
                  case 'd': {
                    Value *Arg = CallI->getArgOperand(argIdx++);
                    int PtrOff = -getIndex(Arg, ValueOffsetMap,
                                          ValueOffsetCounter);
                    // R2 = destination, R4 = accumulator.
                    FuncInstBufferStream
                        << "\tLDR\t\tR2, R5, #" << PtrOff
                        << "\n"
                        << "\tADD\t\tR6, R6, #-1\n"
                        << "\tSTR\t\tR4, R6, #0\n"
                        << "\tAND\t\tR4, R4, #0\n"
                        // Get char from buffer.
                        << "SSCANF_SKIPWS_"
                        << S.TempLabelCounter << "\n"
                        << "\tLDR\t\tR1, R6, #1\n"
                        << "\tLDR\t\tR0, R1, #0\n"
                        << "\tADD\t\tR1, R1, #1\n"
                        << "\tSTR\t\tR1, R6, #1\n"
                        << "\tADD\t\tR1, R0, #-16\n"
                        << "\tADD\t\tR1, R1, #-16\n"
                        << "\tBRz\t\tSSCANF_SKIPWS_"
                        << S.TempLabelCounter << "\n"
                        << "\tADD\t\tR1, R0, #-13\n"
                        << "\tBRz\t\tSSCANF_SKIPWS_"
                        << S.TempLabelCounter << "\n"
                        << "\tADD\t\tR1, R0, #-10\n"
                        << "\tBRz\t\tSSCANF_SKIPWS_"
                        << S.TempLabelCounter << "\n"
                        // Sign check.
                        << "\tAND\t\tR3, R3, #0\n"
                        << "\tLD\t\tR1, VALUE_" << MinusID3
                        << "\n"
                        << "\tNOT\t\tR1, R1\n"
                        << "\tADD\t\tR1, R1, #1\n"
                        << "\tADD\t\tR1, R0, R1\n"
                        << "\tBRnp\t\tSSCANF_DIGIT_"
                        << S.TempLabelCounter << "\n"
                        << "\tADD\t\tR3, R3, #1\n"
                        // Next char.
                        << "\tLDR\t\tR1, R6, #1\n"
                        << "\tLDR\t\tR0, R1, #0\n"
                        << "\tADD\t\tR1, R1, #1\n"
                        << "\tSTR\t\tR1, R6, #1\n"
                        << "SSCANF_DIGIT_"
                        << S.TempLabelCounter << "\n"
                        << "\tLD\t\tR1, VALUE_" << ZeroID3
                        << "\n"
                        << "\tNOT\t\tR1, R1\n"
                        << "\tADD\t\tR1, R1, #1\n"
                        << "\tADD\t\tR1, R0, R1\n"
                        << "\tBRn\t\tSSCANF_DONE_"
                        << S.TempLabelCounter << "\n"
                        << "\tADD\t\tR1, R1, #-10\n"
                        << "\tBRzp\t\tSSCANF_DONE_"
                        << S.TempLabelCounter << "\n"
                        << "\tADD\t\tR0, R4, #0\n"
                        << "\tADD\t\tR4, R4, R4\n"
                        << "\tADD\t\tR4, R4, R4\n"
                        << "\tADD\t\tR4, R4, R0\n"
                        << "\tADD\t\tR4, R4, R4\n"
                        << "\tADD\t\tR4, R4, R1\n"
                        // Next char.
                        << "\tLDR\t\tR1, R6, #1\n"
                        << "\tLDR\t\tR0, R1, #0\n"
                        << "\tADD\t\tR1, R1, #1\n"
                        << "\tSTR\t\tR1, R6, #1\n"
                        << "\tBR\t\tSSCANF_DIGIT_"
                        << S.TempLabelCounter << "\n"
                        << "SSCANF_DONE_"
                        << S.TempLabelCounter << "\n"
                        << "\tADD\t\tR3, R3, #0\n"
                        << "\tBRz\t\tSSCANF_STORE_"
                        << S.TempLabelCounter << "\n"
                        << "\tNOT\t\tR4, R4\n"
                        << "\tADD\t\tR4, R4, #1\n"
                        << "SSCANF_STORE_"
                        << S.TempLabelCounter << "\n"
                        << "\tSTR\t\tR4, R2, #0\n"
                        << "\tLDR\t\tR4, R6, #0\n"
                        << "\tADD\t\tR6, R6, #1\n";
                    ++S.TempLabelCounter;
                    break;
                  }
                  default:
                    return UnsupportedInst(I);
                  }
                  pos++;
                } else {
                  while (pos < FmtStr.size() && FmtStr[pos] != '%')
                    pos++;
                }
              }
              // Pop source pointer.
              FuncInstBufferStream << "\tADD\t\tR6, R6, #1\n";
            } else if (S.FuncLabelMap.count(Func)) {
              // User function call convention:
              // - arguments are pushed onto the stack
              // - JSR by function name
              // - return value is retrieved from the stack (if non-void)
              // - caller is responsible for popping arguments off the stack
              // Calls to functions outside current module are rejected unless
              // mapped by built-in helper names above.
              StringRef CalledFuncName = Func->getName();
              int ArgSize = 0;
              if ((ArgSize = CallI->arg_size())) {
                for (int i = ArgSize - 1; i >= 0; i--) {
                  FuncInstBufferStream << "\tADD\t\tR6, R6, #-1\n";
                  Value *Arg = CallI->getArgOperand(i);

                  if (auto *GV = dyn_cast<GlobalVariable>(Arg)) {
                    // Push the ADDRESS of the global (R4 + offset).
                    int GOff = S.GlobalWordOffsets.lookup(GV);
                    if (GOff <= 15) {
                      FuncInstBufferStream
                          << "\tADD\t\tR0, R4, #" << GOff << "\n"
                          << "\tSTR\t\tR0, R6, #0\n";
                    } else {
                      FuncInstBufferStream << "\tADD\t\tR0, R4, #0\n";
                      int Rem = GOff;
                      while (Rem > 15) {
                        FuncInstBufferStream
                            << "\tADD\t\tR0, R0, #15\n";
                        Rem -= 15;
                      }
                      if (Rem > 0)
                        FuncInstBufferStream
                            << "\tADD\t\tR0, R0, #" << Rem << "\n";
                      FuncInstBufferStream << "\tSTR\t\tR0, R6, #0\n";
                    }
                  } else if (int ArgID =
                                 addImmidiate(Arg, ImmBufferStream, ImmFlag,
                                              ImmIDMap, S.ImmIDCounter)) {
                    FuncInstBufferStream << "\tLD\t\tR0, VALUE_" << ArgID
                                         << "\n"
                                         << "\tSTR\t\tR0, R6, #0\n";
                  } else {
                    int ArgOff =
                        -getIndex(Arg, ValueOffsetMap, ValueOffsetCounter);
                    FuncInstBufferStream << "\tLDR\t\tR0, R5, #" << ArgOff
                                         << "\n"
                                         << "\tSTR\t\tR0, R6, #0\n";
                  }
                }
              }
              FuncInstBufferStream << "\tJSR\t\t" << CalledFuncName << "\n";
              if (!CallI->getType()->isVoidTy()) {
                FuncInstBufferStream << "\tLDR\t\tR0, R6, #0\n";
                if (int ResID = addImmidiate(&I, ImmBufferStream, ImmFlag,
                                             ImmIDMap, S.ImmIDCounter)) {
                  FuncInstBufferStream << "\tST\t\tR0, VALUE_" << ResID << "\n";
                } else {
                  int ResOff =
                      -getIndex(&I, ValueOffsetMap, ValueOffsetCounter);
                  FuncInstBufferStream << "\tSTR\t\tR0, R5, #" << ResOff
                                       << "\n";
                }
              }
              if (!NoComment) {
                FuncInstBufferStream << ";\tPop return value, arguments\n";
              }
              int PopStep = ArgSize + 1;
              while (PopStep > 15) {
                FuncInstBufferStream << "\tADD\t\tR6, R6, #15\n";
                PopStep -= 15;
              }
              if (PopStep) {
                FuncInstBufferStream << "\tADD\t\tR6, R6, #" << PopStep << "\n";
              }
            } else {
              return UnsupportedInst(I);
            }
          } else {
            return UnsupportedInst(I);
          }
        } else if (auto *AllocaI = dyn_cast<AllocaInst>(&I)) {
          Type *AllocTy = AllocaI->getAllocatedType();

          if (AllocTy->isArrayTy()) {
            // Array alloca: reserve stack space and store the absolute
            // address as the alloca result. Scalar alloca remains virtual.
            ArrayType *ArrTy = cast<ArrayType>(AllocTy);
            uint64_t NumElems = ArrTy->getNumElements();

            int ResOff =
                -getIndex(&I, ValueOffsetMap, ValueOffsetCounter);

            // Reserve N words on the stack (split for ADD imm limit).
            unsigned Remaining = NumElems;
            while (Remaining > 15) {
              FuncInstBufferStream << "\tADD\t\tR6, R6, #-15\n";
              Remaining -= 15;
            }
            if (Remaining > 0) {
              FuncInstBufferStream << "\tADD\t\tR6, R6, #-"
                                   << Remaining << "\n";
            }

            // Store absolute address (R6 after reservation).
            FuncInstBufferStream << "\tADD\t\tR1, R6, #0\n"
                                 << "\tSTR\t\tR1, R5, #" << ResOff
                                 << "\n";

            S.RealPointers.insert(&I);
          } else {
            // Scalar alloca: reserve 1 word on stack, store absolute
            // address so `&var` and `*ptr` work correctly.
            int ResOff =
                -getIndex(&I, ValueOffsetMap, ValueOffsetCounter);
            FuncInstBufferStream << "\tADD\t\tR6, R6, #-1\n"
                                 << "\tADD\t\tR1, R6, #0\n"
                                 << "\tSTR\t\tR1, R5, #" << ResOff
                                 << "\n";
            S.RealPointers.insert(&I);
          }
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
          int EndLableID = S.TempLabelCounter + ArgSize;
          for (unsigned int i = 0; i < ArgSize; ++i) {

            BasicBlock *SrcBB = PHIN->getIncomingBlock(i);
            std::string SrcBBName = getIndex(SrcBB, S.BBNameMap, S.BBNameCounter);

            ++S.TempLabelCounter;
            if (i < ArgSize - 1) {
              // Compare predecessor label with this incoming block label.
              FuncInstBufferStream << "\tLEA\t\tR1, " << SrcBBName << "\n"
                                   << "\tADD\t\tR1, R1, R0\n"
                                   << "\tBRnp\tPHI_NEXT_" << S.TempLabelCounter
                                   << "\n";
            }

            Value *Val = PHIN->getIncomingValue(i);
            if (int ValID = addImmidiate(Val, ImmBufferStream, ImmFlag,
                                         ImmIDMap, S.ImmIDCounter)) {
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
            FuncInstBufferStream << "PHI_NEXT_" << S.TempLabelCounter << "\n";
          }
        } else if (auto *RetI = dyn_cast<ReturnInst>(&I)) {
          // Function epilogue:
          // 1) store return value (if any) into the caller's reserved stack
          // slot [R5+3] 2) pop local variables, restore frame pointer (R5) and
          // return address (R7) 3) RET (HALT is now handled strictly in the
          // bootstrap sequence)
          bool hasRetVal = false;
          if (Value *Val = RetI->getReturnValue()) {
            if (!NoComment) {
              FuncInstBufferStream << ";\tSave return value\n";
            }
            hasRetVal = true;
            if (int ValID = addImmidiate(Val, ImmBufferStream, ImmFlag,
                                         ImmIDMap, S.ImmIDCounter)) {
              FuncInstBufferStream << "\tLD\t\tR0, VALUE_" << ValID << "\n";
            } else {
              int ValOff = -getIndex(Val, ValueOffsetMap, ValueOffsetCounter);
              FuncInstBufferStream << "\tLDR\t\tR0, R5, #" << ValOff << "\n";
            }
            FuncInstBufferStream << "\tSTR\t\tR0, R5, #3\n";
          }
          if (!NoComment) {
            FuncInstBufferStream << ";\tPop local variables, R5, R7\n";
          }
          FuncInstBufferStream << "\tADD\t\tR6, R5, #3\n";
          if (!NoComment) {
            FuncInstBufferStream << ";\tRestore frame pointer\n";
          }
          FuncInstBufferStream << "\tLDR\t\tR5, R6, #-2\n";
          if (!NoComment) {
            FuncInstBufferStream << ";\tRestore R7\n";
          }
          FuncInstBufferStream << "\tLDR\t\tR7, R6, #-1\n";
          if (!NoComment) {
            FuncInstBufferStream << ";\tReturn to caller\n";
          }
          FuncInstBufferStream << "\tRET\n";
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
                                          ImmIDMap, S.ImmIDCounter)) {
            FuncInstBufferStream << "\tLD\t\tR2, VALUE_" << IfTrueID << "\n";
          } else {
            int IfTrueOff =
                -getIndex(IfTrue, ValueOffsetMap, ValueOffsetCounter);
            FuncInstBufferStream << "\tLDR\t\tR2, R5, #" << IfTrueOff << "\n";
          }

          FuncInstBufferStream << "\tLDR\t\tR1, R5, #" << CondOff << "\n"
                               << "\tBRp\t\tSELECT_END_" << ++S.TempLabelCounter
                               << "\n";
          // Fallthrough path loads false value when condition is not positive.

          Value *IfFalse = SelI->getFalseValue();
          if (int IfFalseID = addImmidiate(IfFalse, ImmBufferStream, ImmFlag,
                                           ImmIDMap, S.ImmIDCounter)) {
            FuncInstBufferStream << "\tLD\t\tR2, VALUE_" << IfFalseID << "\n";
          } else {
            int IfFalseOff =
                -getIndex(IfFalse, ValueOffsetMap, ValueOffsetCounter);
            FuncInstBufferStream << "\tLDR\t\tR2, R5, #" << IfFalseOff << "\n";
          }

          FuncInstBufferStream << "SELECT_END_" << S.TempLabelCounter << "\n"
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
              getIndex(DefaultBB, S.BBNameMap, S.BBNameCounter);

          FuncInstBufferStream << "\tLEA\t\tR7, " << BBName << "\n"
                               << "\tLDR\t\tR1, R5, #" << CondOff << "\n";

          bool isFirstCase = true;
          for (auto Case : SwitchI->cases()) {
            ConstantInt *Val = Case.getCaseValue();
            int ValID = addImmidiate(Val, ImmBufferStream, ImmFlag, ImmIDMap,
                                     S.ImmIDCounter);

            BasicBlock *DesBB = Case.getCaseSuccessor();
            std::string DesBBName = getIndex(DesBB, S.BBNameMap, S.BBNameCounter);

            if (Val->getSExtValue() == 0) {
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
        } else if (auto *GEPI = dyn_cast<GetElementPtrInst>(&I)) {
          // GEP lowering: compute base address + element offset.
          int ResOff = -getIndex(&I, ValueOffsetMap, ValueOffsetCounter);
          Value *PtrOp = GEPI->getPointerOperand();

          // Emit base address into R1.
          if (auto *GV = dyn_cast<GlobalVariable>(PtrOp)) {
            int GOff = S.GlobalWordOffsets.lookup(GV);
            // R4 is base of global data; add word offset.
            if (GOff <= 15) {
              FuncInstBufferStream << "\tADD\t\tR1, R4, #" << GOff << "\n";
            } else {
              FuncInstBufferStream << "\tADD\t\tR1, R4, #0\n";
              int Rem = GOff;
              while (Rem > 15) {
                FuncInstBufferStream << "\tADD\t\tR1, R1, #15\n";
                Rem -= 15;
              }
              if (Rem > 0)
                FuncInstBufferStream << "\tADD\t\tR1, R1, #" << Rem
                                     << "\n";
            }
          } else {
            int PtrOff =
                -getIndex(PtrOp, ValueOffsetMap, ValueOffsetCounter);
            FuncInstBufferStream << "\tLDR\t\tR1, R5, #" << PtrOff
                                 << "\n";
          }

          // Accumulate index offsets.  Indices that walk through an
          // aggregate type (ArrayType) are skipped — they are always 0
          // in practice.  Once CurTy reaches a non-aggregate type, the
          // current index is the element offset and is emitted.
          Type *CurTy = GEPI->getSourceElementType();

          for (auto Idx = GEPI->idx_begin(), End = GEPI->idx_end();
               Idx != End; ++Idx) {
            if (auto *ArrTy = dyn_cast<ArrayType>(CurTy)) {
              // Walk through the aggregate — skip this index.
              CurTy = ArrTy->getElementType();
              continue;
            }

            // Non-aggregate type: this index is the element offset.
            unsigned ElemSizeBytes = S.DL.getTypeAllocSize(CurTy);
            unsigned ElemSizeWords = ElemSizeBytes / 4;

            if (auto *ConstIdx = dyn_cast<ConstantInt>(*Idx)) {
              int WordOff =
                  ConstIdx->getSExtValue() * (int)ElemSizeWords;
              int Rem = WordOff > 0 ? WordOff : -WordOff;
              const char *Op = WordOff >= 0 ? "#" : "#-";
              while (Rem > 15) {
                FuncInstBufferStream << "\tADD\t\tR1, R1, " << Op
                                     << "15\n";
                Rem -= 15;
              }
              if (Rem > 0)
                FuncInstBufferStream << "\tADD\t\tR1, R1, " << Op
                                     << Rem << "\n";
            } else {
              int IdxOff =
                  -getIndex(*Idx, ValueOffsetMap, ValueOffsetCounter);
              FuncInstBufferStream << "\tLDR\t\tR2, R5, #" << IdxOff
                                   << "\n";
              if (ElemSizeWords == 2) {
                FuncInstBufferStream << "\tADD\t\tR2, R2, R2\n";
              } else if (ElemSizeWords == 4) {
                FuncInstBufferStream << "\tADD\t\tR2, R2, R2\n"
                                     << "\tADD\t\tR2, R2, R2\n";
              } else if (ElemSizeWords > 4) {
                FuncInstBufferStream
                    << "\tAND\t\tR3, R3, #0\n"
                    << "\tADD\t\tR3, R3, R2\n"
                    << "GEP_MUL_" << ++S.TempLabelCounter
                    << "\n"
                    << "\tADD\t\tR3, R3, R2\n"
                    << "\tADD\t\tR2, R2, #-1\n"
                    << "\tBRp\t\tGEP_MUL_" << S.TempLabelCounter
                    << "\n"
                    << "\tADD\t\tR2, R3, #0\n";
              }
              FuncInstBufferStream << "\tADD\t\tR1, R1, R2\n";
            }
          }

          // Store computed address.
          FuncInstBufferStream << "\tSTR\t\tR1, R5, #" << ResOff
                               << "\n";

          S.RealPointers.insert(&I);
        } else {
          return UnsupportedInst(I);
        }
        // Any instruction that produces a pointer value (load of ptr,
        // select of ptr, call returning ptr, etc.) is a real pointer.
        if (I.getType()->isPointerTy())
          S.RealPointers.insert(&I);
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

    std::string FuncBuffer;
    raw_string_ostream FuncBufferStream(FuncBuffer);

    if (!NoComment) {
      // Summary metadata for quick debugging of generated function frame.
      FuncBufferStream << ";\tfunction " << FuncName << "\n";
      FuncBufferStream << ";\targument count: " << F.arg_size() << "\n";
      FuncBufferStream << ";\tlocal variable count: " << ValueOffsetCounter
                       << "\n";
    }
    // Function symbol + entry basic-block label.
    FuncBufferStream << FuncName << "\n";
    FuncBufferStream << S.FuncLabelMap[&F] << "\n";

    // Prologue: reserve space for return value, save volatile state, and
    // establish frame pointer. Stack layout relative to the new frame pointer
    // (R5):
    //   [R5+4...] incoming arguments (Arg 0 at R5+4, Arg 1 at R5+5, etc.)
    //   [R5+3]    space reserved for return value
    //   [R5+2]    old R7 (return address)
    //   [R5+1]    old R5 (caller's frame pointer)
    // Locals are then allocated at negative offsets from R5.
    if (!NoComment) {
      FuncBufferStream << ";\tSpace for return value, R7, R5\n";
    }
    FuncBufferStream << "\tADD\t\tR6, R6, #-3\n";
    if (!NoComment) {
      FuncBufferStream << ";\tSave R7\n";
    }
    FuncBufferStream << "\tSTR\t\tR7, R6, #1\n";
    if (!NoComment) {
      FuncBufferStream << ";\tSave R5\n";
    }
    FuncBufferStream << "\tSTR\t\tR5, R6, #0\n";
    if (!NoComment) {
      FuncBufferStream << ";\tSet frame pointer\n";
    }
    FuncBufferStream << "\tADD\t\tR5, R6, #-1\n";
    // Reserve frame space for SSA values. Current implementation supports
    // up to 32 virtual locals due to immediate range and simple allocator.
    if (ValueOffsetCounter <= 32) {
      if (ValueOffsetCounter > 16) {
        FuncBufferStream << "\tADD\t\tR6, R6, #-16\n";
        ValueOffsetCounter -= 16;
      }
      if (ValueOffsetCounter) {
        FuncBufferStream << "\tADD\t\tR6, R6, #-" << ValueOffsetCounter << "\n";
      }
    } else {
      errs() << "Too many local variables in function " << FuncName
             << ", count : " << ValueOffsetCounter << "\n"
             << "No file generated.\n";
      return PreservedAnalyses::none();
    }

    FuncBufferStream << FuncInstBufferStream.str();

    if (FuncName == LC3Entry) {
      S.Out.os() << FuncBufferStream.str();
    } else {
      S.InstBufferStream << FuncBufferStream.str();
    }
  }

  // Emit buffered global data section.
  if (!LC3GlobalAddr.empty()) {
    // User-specified address: emit a separate .ORIG segment before
    // any function code so the globals live at a fixed address.
    S.Out.os() << "\n\t.ORIG\t" << LC3GlobalAddr << "\n"
               << S.GlobalDataBuffer;
  }

  // Finalize the module output and materialize the target file.
  S.Out.os() << S.InstBufferStream.str();
  if (LC3GlobalAddr.empty())
    S.Out.os() << S.GlobalDataBuffer; // default: end of program
  S.Out.os() << "\t.END";

  if (!S.hasEntry) {
    errs() << "Entry function \"" << LC3Entry << "\" not found!\n";
    return PreservedAnalyses::none();
  }

  // Commit temporary output to final path only after successful generation.
  S.Out.keep();

  errs() << "One file generated: " << TargetFileName << "\n";

  return PreservedAnalyses::none();
}

// ============================================================================
// Extracted helpers
// ============================================================================

// Walk module globals and record word offsets for every non-string,
// non-declaration global variable.
static void scanGlobals(LoweringState &S) {
  for (auto &GV : S.M.globals()) {
    if (GV.isDeclaration())
      continue;
    StringRef Str = getString(&GV);
    if (Str.size() > 0)
      continue;

    Type *GVTy = GV.getValueType();
    if (GVTy->isIntegerTy() || GVTy->isArrayTy()) {
      S.GlobalWordOffsets[&GV] = S.GlobalWordCounter;
      unsigned SizeBytes = S.DL.getTypeAllocSize(GVTy);
      S.GlobalWordCounter += SizeBytes / 4;
    }
  }
}

// Emit the bootstrap sequence followed by the global data section.
static void emitBootstrapAndGlobals(LoweringState &S) {
  S.hasEntry = true;

  // Bootstrap: set up SP, global base, call entry, HALT.
  // Use LD (not LEA) to load the global-data pointer so we are not
  // limited to the ±256-word PC-relative range of LEA.
  S.Out.os() << "\tLD\t\tR6, STACK_BASE\n"
             << "\tLD\t\tR4, GLOBAL_DATA_PTR\n"
             << "\tJSR\t\t" << LC3Entry << "\n"
             << "\tHALT\n"
             << "\n"
             << "STACK_BASE\n\t.FILL\t" << LC3StackBase << "\n"
             << "GLOBAL_DATA_PTR\n\t.FILL\tGLOBAL_DATA_START\n"
             << "\n";

  // Buffer the global data section to be emitted at end of program
  // (or at a user-specified address) so it does not sit between the
  // bootstrap and the function bodies.
  std::string Buf;
  raw_string_ostream BufOS(Buf);

  BufOS << "; --- global data section ---\n"
        << "GLOBAL_DATA_START\n";

  for (auto &GV : S.M.globals()) {
    if (GV.isDeclaration())
      continue;
    StringRef Str = getString(&GV);
    if (Str.size() > 0)
      continue;

    Type *GVTy = GV.getValueType();
    Constant *Init = GV.getInitializer();
    std::string Label = "G_" + GV.getName().str();
    BufOS << Label << "\n";

    if (GVTy->isIntegerTy()) {
      if (auto *CI = dyn_cast<ConstantInt>(Init))
        BufOS << "\t.FILL\t#" << CI->getSExtValue() << "\n";
      else
        BufOS << "\t.FILL\t#0\n";
    } else if (GVTy->isArrayTy()) {
      ArrayType *ArrTy = cast<ArrayType>(GVTy);
      uint64_t NumElems = ArrTy->getNumElements();
      if (isa<ConstantAggregateZero>(Init)) {
        BufOS << "\t.BLKW\t#" << NumElems << "\n";
      } else if (auto *CDS = dyn_cast<ConstantDataSequential>(Init)) {
        for (unsigned i = 0; i < CDS->getNumElements(); i++) {
          if (auto *CI = dyn_cast<ConstantInt>(CDS->getElementAsConstant(i)))
            BufOS << "\t.FILL\t#" << CI->getSExtValue() << "\n";
          else
            BufOS << "\t.FILL\t#0\n";
        }
      } else if (auto *CA = dyn_cast<ConstantArray>(Init)) {
        for (unsigned i = 0; i < NumElems; i++) {
          if (auto *CI = dyn_cast<ConstantInt>(CA->getOperand(i)))
            BufOS << "\t.FILL\t#" << CI->getSExtValue() << "\n";
          else
            BufOS << "\t.FILL\t#0\n";
        }
      } else {
        BufOS << "\t.BLKW\t#" << NumElems << "\n";
      }
    } else {
      BufOS << "\t.FILL\t#0\n";
    }
  }
  BufOS << "\n";
  S.GlobalDataBuffer = Buf;
}

// Three canonicalization passes over a single basic block.
static void canonicalizeBlock(BasicBlock &BB, Type *WordTy) {
  // Pass 1: lower min/max intrinsics to icmp+select.
  for (auto &I : make_early_inc_range(BB)) {
    if (auto *IntrI = dyn_cast<IntrinsicInst>(&I)) {
      CmpInst::Predicate Pred;
      switch (IntrI->getIntrinsicID()) {
      case Intrinsic::smin:  Pred = CmpInst::ICMP_SLT; break;
      case Intrinsic::smax:  Pred = CmpInst::ICMP_SGT; break;
      case Intrinsic::umin:  Pred = CmpInst::ICMP_ULT; break;
      case Intrinsic::umax:  Pred = CmpInst::ICMP_UGT; break;
      case Intrinsic::lifetime_start:
      case Intrinsic::lifetime_end: continue;
      default: return;
      }
      IRBuilder<> Builder(IntrI);
      Value *A = IntrI->getArgOperand(0);
      Value *B = IntrI->getArgOperand(1);
      Value *Cmp = Builder.CreateICmp(Pred, A, B);
      Value *Select = Builder.CreateSelect(Cmp, A, B);
      IntrI->replaceAllUsesWith(Select);
      IntrI->eraseFromParent();
    }
  }

  // Pass 2: normalize icmp / sub constants.
  for (auto &I : make_early_inc_range(BB)) {
    if (auto *ICmpI = dyn_cast<ICmpInst>(&I)) {
      Value *FirVal = ICmpI->getOperand(0);
      if (auto *ConstInt = dyn_cast<ConstantInt>(FirVal))
        ICmpI->swapOperands();
      auto Pred = ICmpI->getPredicate();
      Value *A = ICmpI->getOperand(0);
      Value *B = ICmpI->getOperand(1);
      if (auto *ConstInt = dyn_cast<ConstantInt>(B)) {
        if (!ICmpI->isUnsigned()) {
          IRBuilder<> Builder(ICmpI);
          auto *NegativeConst =
              ConstantInt::get(WordTy, -ConstInt->getSExtValue());
          auto *II = Builder.CreateICmp(Pred, A, NegativeConst);
          I.replaceAllUsesWith(II);
          I.eraseFromParent();
        }
      }
    } else if (auto *BinOp = dyn_cast<BinaryOperator>(&I)) {
      if (BinOp->getOpcode() == Instruction::Sub) {
        Value *A = BinOp->getOperand(0);
        Value *B = BinOp->getOperand(1);
        if (auto *ConstInt = dyn_cast<ConstantInt>(B)) {
          IRBuilder<> Builder(BinOp);
          auto *NegativeConst =
              ConstantInt::get(WordTy, -ConstInt->getSExtValue());
          auto *BI = Builder.CreateAdd(A, NegativeConst);
          I.replaceAllUsesWith(BI);
          I.eraseFromParent();
        }
      }
    }
  }

  // Pass 3: lower br(icmp eq/ne) -> switch, trunc -> passthrough,
  //         constant lshr -> udiv, disjoint or -> add.
  for (auto &I : make_early_inc_range(BB)) {
    if (auto *BrI = dyn_cast<BranchInst>(&I)) {
      if (BrI->isConditional()) {
        Value *Cond = BrI->getCondition();
        if (auto *ICmpI = dyn_cast<ICmpInst>(Cond)) {
          auto Pred = ICmpI->getPredicate();
          if (Pred == CmpInst::ICMP_EQ || Pred == CmpInst::ICMP_NE) {
            Value *Val = ICmpI->getOperand(0);
            Value *ConVal = ICmpI->getOperand(1);
            if (auto *ConstInt = dyn_cast<ConstantInt>(ConVal)) {
              IRBuilder<> Builder(BrI);
              int CaseVal = ConstInt->getSExtValue();
              if (ConstInt->getType()->isIntegerTy(1))
                CaseVal = ConstInt->getZExtValue();
              auto *Zero = ConstantInt::get(Val->getType(), CaseVal);
              SwitchInst *SI = Builder.CreateSwitch(
                  Val, BrI->getSuccessor(Pred == CmpInst::ICMP_EQ), 1);
              SI->addCase(cast<ConstantInt>(Zero),
                          BrI->getSuccessor(Pred == CmpInst::ICMP_NE));
              BrI->eraseFromParent();
              if (ICmpI->use_empty())
                ICmpI->eraseFromParent();
            }
          }
        }
      }
    } else if (auto *TruncI = dyn_cast<TruncInst>(&I)) {
      Value *Source = TruncI->getOperand(0);
      TruncI->replaceAllUsesWith(Source);
      TruncI->eraseFromParent();
    } else if (auto *BinOp = dyn_cast<BinaryOperator>(&I)) {
      switch (BinOp->getOpcode()) {
      case Instruction::LShr:
        if (auto *ShiftAmt = dyn_cast<ConstantInt>(BinOp->getOperand(1))) {
          Value *A = BinOp->getOperand(0);
          unsigned BitWidth = A->getType()->getIntegerBitWidth();
          uint64_t ShiftVal = ShiftAmt->getZExtValue();
          if (ShiftVal >= BitWidth) continue;
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
          if (DisjointOp->isDisjoint()) {
            IRBuilder<> Builder(&I);
            Value *AddInst = Builder.CreateAdd(BinOp->getOperand(0),
                                               BinOp->getOperand(1));
            I.replaceAllUsesWith(AddInst);
            I.eraseFromParent();
          }
        }
        break;
      default:
        continue;
      }
    }
  }
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