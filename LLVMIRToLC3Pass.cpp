#include "LLVMIRToLC3Pass.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/Analysis.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"
#include <string>
#include <system_error>

using namespace llvm;

static cl::opt<std::string>
    LC3StartAddrArg("lc3-start-addr",
                    cl::desc("Specify the starting address of the LC-3 "
                             "assembly file, default x3000"),
                    cl::value_desc("lc3-start-addr"), cl::init("x3000"));

int getID(Value *Val, DenseMap<Value *, int> &Map, int &Counter) {
  if (Map.count(Val) == 0) {
    Map[Val] = ++Counter;
  }
  return Map[Val];
}

void addImmidiate(Value *Val, int ValID, raw_string_ostream &Buffer,
                  DenseMap<Value *, int> &Flag) {
  if (auto *ConstInt = dyn_cast<ConstantInt>(Val)) {
    if (Flag.count(Val) == 1) {
      return;
    }
    Flag[Val] = 1;
    int value = ConstInt->getSExtValue();
    Buffer << "VALUE_" << ValID << "\n"
           << "\t.FILL\t#" << value << "\n";
  }
}

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

void addString(Value *Val, int ValID, raw_string_ostream &Buffer,
               DenseMap<Value *, int> &Flag) {
  auto Str = getString(Val);
  if (Str.size() > 0) {
    if (Flag.count(Val) == 1) {
      return;
    }
    Flag[Val] = 1;
    Buffer << "VALUE_" << ValID << "\n"
           << "\t.STRINGZ\t\"" << Str << "\"\n";
  }
}

void LC3Add(ToolOutputFile &Out, int ResID, int AID, int BID) {
  Out.os() << "\tLD\t\tR1, VALUE_" << AID << "\n"
           << "\tLD\t\tR2, VALUE_" << BID << "\n"
           << "\tADD\t\tR1, R1, R2\n"
           << "\tST\t\tR1, VALUE_" << ResID << "\n";
}

void LC3And(ToolOutputFile &Out, int ResID, int AID, int BID) {
  Out.os() << "\tLD\t\tR1, VALUE_" << AID << "\n"
           << "\tLD\t\tR2, VALUE_" << BID << "\n"
           << "\tAND\t\tR1, R1, R2\n"
           << "\tST\t\tR1, VALUE_" << ResID << "\n";
}

void LC3Sub(ToolOutputFile &Out, int ResID, int AID, int BID) {
  Out.os() << "\tLD\t\tR1, VALUE_" << AID << "\n"
           << "\tLD\t\tR2, VALUE_" << BID << "\n"
           << "\tNOT\t\tR2, R2\n"
           << "\tADD\t\tR2, R2, #1\n"
           << "\tADD\t\tR1, R1, R2\n"
           << "\tST\t\tR1, VALUE_" << ResID << "\n";
}

void LC3Shl(ToolOutputFile &Out, int &TempLabelCounter, int ResID, int AID,
            int BID) {
  Out.os() << "\tLD\t\tR1, VALUE_" << AID << "\n"
           << "\tLD\t\tR2, VALUE_" << BID << "\n"
           << "TEMPLABEL_" << ++TempLabelCounter << "\n"
           << "\tADD\t\tR1, R1, R1\n"
           << "\tADD\t\tR2, R2, #-1\n"
           << "\tBRp\t\tTEMPLABEL_" << TempLabelCounter << "\n"
           << "\tST\t\tR1, VALUE_" << ResID << "\n";
}

void LC3Mul(ToolOutputFile &Out, int &TempLabelCounter, int ResID, int AID,
            int BID) {
  Out.os() << "\tAND\t\tR3, R3, #0\n"
           << "\tLD\t\tR1, VALUE_" << AID << "\n"
           << "\tLD\t\tR2, VALUE_" << BID << "\n"
           << "\tBRzp\tTEMPLABEL_" << ++TempLabelCounter << "\n"
           << "\tNOT\t\tR1, R1\n"
           << "\tADD\t\tR1, R1, #1\n"
           << "\tNOT\t\tR2, R2\n"
           << "\tADD\t\tR2, R2, #1\n"
           << "TEMPLABEL_" << TempLabelCounter << "\n"
           << "\tBRz\t\tTEMPLABEL_" << ++TempLabelCounter << "\n"
           << "\tADD\t\tR3, R3, R1\n"
           << "\tADD\t\tR2, R2, #-1\n"
           << "\tBR\t\tTEMPLABEL_" << TempLabelCounter - 1 << "\n"
           << "TEMPLABEL_" << TempLabelCounter << "\n"
           << "\tST\t\tR3, VALUE_" << ResID << "\n";
}

auto ParseError(Instruction &I) {
  errs() << " ";
  I.print(errs());
  errs() << "\nNo File Generated\n";
  return PreservedAnalyses::all();
}

PreservedAnalyses LLVMIRToLC3Pass::run(Function &F,
                                       FunctionAnalysisManager &AM) {
  Module *Mod = F.getParent();
  StringRef SourceFileName = Mod->getSourceFileName();
  std::string TargetFileName = sys::path::stem(SourceFileName).str() + ".asm";

  std::error_code EC;
  ToolOutputFile Out(TargetFileName, EC, sys::fs::OF_None);
  std::string Buffer;
  raw_string_ostream AllocateBufferStream(Buffer);

  if (EC) {
    errs() << "Error: " << EC.message() << "\n";
    return PreservedAnalyses::all();
  }

  Out.os() << "; This file is generated automatically by ir-to-lc3 pass.\n"
           << "\n"
           << "\t.ORIG\t" << LC3StartAddrArg << "\n"
           << "\n";

  Out.os() << "; function " << F.getName() << "\n";

  DenseMap<Value *, int> ValueIDMap, AllocatedFlag, BBIDMap;
  int ValueIDCounter = 0;
  int BBIDCounter = 0, TempLabelCounter = 0;

  for (auto &BB : F) {
    int BBID = getID(&BB, BBIDMap, BBIDCounter);
    Out.os() << "LABEL_" << BBID << "\n";
    Instruction &FirstI = BB.front();
    for (auto &I : BB) {
      Out.os() << ";";
      I.print(Out.os());
      Out.os() << "\n";
      if (auto *BinI = dyn_cast<BinaryOperator>(&I)) {
        int ResID = getID(&I, ValueIDMap, ValueIDCounter);

        Value *A = BinI->getOperand(0);
        int AID = getID(A, ValueIDMap, ValueIDCounter);
        addImmidiate(A, AID, AllocateBufferStream, AllocatedFlag);

        Value *B = BinI->getOperand(1);
        int BID = getID(B, ValueIDMap, ValueIDCounter);
        addImmidiate(B, BID, AllocateBufferStream, AllocatedFlag);

        if (BinI->getOpcode() == Instruction::Add) {
          LC3Add(Out, ResID, AID, BID);
        } else if (BinI->getOpcode() == Instruction::Sub) {
          LC3Sub(Out, ResID, AID, BID);
        } else if (BinI->getOpcode() == Instruction::And) {
          LC3And(Out, ResID, AID, BID);
        } else if (BinI->getOpcode() == Instruction::Shl) {
          LC3Shl(Out, TempLabelCounter, ResID, AID, BID);
        } else if (BinI->getOpcode() == Instruction::Mul) {
          LC3Mul(Out, TempLabelCounter, ResID, AID, BID);
        } else {
          return ParseError(I);
        }
      } else if (auto *LoadI = dyn_cast<LoadInst>(&I)) {
        int ResID = getID(&I, ValueIDMap, ValueIDCounter);

        Value *Op = LoadI->getPointerOperand();
        int OpID = getID(Op, ValueIDMap, ValueIDCounter);

        Out.os() << "\tLD\t\tR1, VALUE_" << OpID << "\n"
                 << "\tST\t\tR1, VALUE_" << ResID << "\n";
      } else if (auto *StoreI = dyn_cast<StoreInst>(&I)) {
        Value *Val = StoreI->getValueOperand();
        int ValID = getID(Val, ValueIDMap, ValueIDCounter);
        addImmidiate(Val, ValID, AllocateBufferStream, AllocatedFlag);

        Value *Ptr = StoreI->getPointerOperand();
        int PtrID = getID(Ptr, ValueIDMap, ValueIDCounter);

        Out.os() << "\tLD\t\tR1, VALUE_" << ValID << "\n"
                 << "\tST\t\tR1, VALUE_" << PtrID << "\n";
      } else if (auto *BranchI = dyn_cast<BranchInst>(&I)) {
        Out.os() << "\tLEA\t\tR6, LABEL_" << BBID << "\n";
        if (BranchI->isUnconditional()) {
          Value *Suc = BranchI->getSuccessor(0);
          int SucID = getID(Suc, BBIDMap, BBIDCounter);

          Out.os() << "\tBR\t\tLABEL_" << SucID << "\n";
        } else {
          Value *Con = BranchI->getCondition();
          int ConID = getID(Con, ValueIDMap, ValueIDCounter);

          Value *IfTrue = BranchI->getSuccessor(0);
          int IfTrueID = getID(IfTrue, BBIDMap, BBIDCounter);

          Value *IfFalse = BranchI->getSuccessor(1);
          int IfFalseID = getID(IfFalse, BBIDMap, BBIDCounter);

          Out.os() << "\tLD\t\tR0, VALUE_" << ConID << "\n"
                   << "\tBRz\t\tLABEL_" << IfFalseID << "\n"
                   << "\tBR\t\tLABEL_" << IfTrueID << "\n";
        }
      } else if (auto *ICmpI = dyn_cast<ICmpInst>(&I)) {
        int ResID = getID(&I, ValueIDMap, ValueIDCounter);

        Value *A = ICmpI->getOperand(0);
        int AID = getID(A, ValueIDMap, ValueIDCounter);
        addImmidiate(A, AID, AllocateBufferStream, AllocatedFlag);

        Value *B = ICmpI->getOperand(1);
        int BID = getID(B, ValueIDMap, ValueIDCounter);
        addImmidiate(B, BID, AllocateBufferStream, AllocatedFlag);

        Out.os() << "\tAND\t\tR3, R3, #0\n"
                 << "\tLD\t\tR1, VALUE_" << AID << "\n"
                 << "\tLD\t\tR2, VALUE_" << BID << "\n"
                 << "\tNOT\t\tR2, R2\n"
                 << "\tADD\t\tR2, R2, #1\n"
                 << "\tADD\t\tR1, R1, R2\n";

        switch (ICmpI->getPredicate()) {
        case CmpInst::ICMP_EQ:
          Out.os() << "\tBRnp\tTEMPLABEL_" << ++TempLabelCounter << "\n";
          break;
        case CmpInst::ICMP_NE:
          Out.os() << "\tBRz\t\tTEMPLABEL_" << ++TempLabelCounter << "\n";
          break;
        case CmpInst::ICMP_SGT:
          Out.os() << "\tBRnz\tTEMPLABEL_" << ++TempLabelCounter << "\n";
          break;
        case CmpInst::ICMP_SGE:
          Out.os() << "\tBRn\t\tTEMPLABEL_" << ++TempLabelCounter << "\n";
          break;
        case CmpInst::ICMP_SLT:
          Out.os() << "\tBRzp\tTEMPLABEL_" << ++TempLabelCounter << "\n";
          break;
        case CmpInst::ICMP_SLE:
          Out.os() << "\tBRp\t\tTEMPLABEL_" << ++TempLabelCounter << "\n";
          break;
        default:
          return ParseError(I);
          break;
        }

        Out.os() << "\tADD\t\tR3, R3, #1\n"
                 << "TEMPLABEL_" << TempLabelCounter << "\n"
                 << "\tST\t\tR3, VALUE_" << ResID << "\n";
      } else if (auto *CallI = dyn_cast<CallInst>(&I)) {
        if (Function *Func = CallI->getCalledFunction()) {
          if (Func->getName() == "printStrImm") {
            if (CallI->arg_size() == 1) {
              Value *Str = CallI->getArgOperand(0);
              int StrID = getID(Str, ValueIDMap, ValueIDCounter);
              addString(Str, StrID, AllocateBufferStream, AllocatedFlag);

              Out.os() << "\tLEA\t\tR0, VALUE_" << StrID << "\n"
                       << "\tPUTS\n";
            } else {
              return ParseError(I);
            }
          } else if (Func->getName() == "printStrAddr") {
            if (CallI->arg_size() == 1) {
              Value *Addr = CallI->getArgOperand(0);
              int AddrID = getID(Addr, ValueIDMap, ValueIDCounter);
              addImmidiate(Addr, AddrID, AllocateBufferStream, AllocatedFlag);

              Out.os() << "\tLD\t\tR0, VALUE_" << AddrID << "\n"
                       << "\tPUTS\n";
            } else {
              return ParseError(I);
            }
          } else if (Func->getName() == "printCharAddr") {
            if (CallI->arg_size() == 1) {
              Value *Addr = CallI->getArgOperand(0);
              int AddrID = getID(Addr, ValueIDMap, ValueIDCounter);
              addImmidiate(Addr, AddrID, AllocateBufferStream, AllocatedFlag);

              Out.os() << "\tLDI\t\tR0, VALUE_" << AddrID << "\n"
                       << "\tOUT\n";
            } else {
              return ParseError(I);
            }
          } else if (Func->getName() == "printCharImm") {
            if (CallI->arg_size() == 1) {
              Value *Char = CallI->getArgOperand(0);
              int CharID = getID(Char, ValueIDMap, ValueIDCounter);
              addImmidiate(Char, CharID, AllocateBufferStream, AllocatedFlag);

              Out.os() << "\tLD\t\tR0, VALUE_" << CharID << "\n"
                       << "\tOUT\n";
            } else {
              return ParseError(I);
            }
          } else if (Func->getName() == "integrateLC3Asm") {
            if (CallI->arg_size() == 1) {
              Value *Str = CallI->getArgOperand(0);
              StringRef Content = getString(Str);
              if (Content != "") {
                Out.os() << Content << "\n";
              } else {
                return ParseError(I);
              }
            } else {
              return ParseError(I);
            }
          } else if (Func->getName() == "loadLabel") {
            if (CallI->arg_size() == 2) {
              Value *Des = CallI->getArgOperand(0);
              int DesID = getID(Des, ValueIDMap, ValueIDCounter);

              Value *Str = CallI->getArgOperand(1);
              StringRef Label = getString(Str);

              if (Label != "") {
                Out.os() << "\tLD\t\tR1, " << Label << "\n"
                         << "\tST\t\tR1, VALUE_" << DesID << "\n";
              } else {
                return ParseError(I);
              }
            } else {
              return ParseError(I);
            }
          } else if (Func->getName() == "loadAddr") {
            if (CallI->arg_size() == 2) {
              Value *Des = CallI->getArgOperand(0);
              int DesID = getID(Des, ValueIDMap, ValueIDCounter);

              Value *Addr = CallI->getArgOperand(1);
              int AddrID = getID(Addr, ValueIDMap, ValueIDCounter);
              addImmidiate(Addr, AddrID, AllocateBufferStream, AllocatedFlag);

              Out.os() << "\tLDI\t\tR1, VALUE_" << AddrID << "\n"
                       << "\tST\t\tR1, VALUE_" << DesID << "\n";
            } else {
              return ParseError(I);
            }
          } else if (Func->getName() == "storeLabel") {
            if (CallI->arg_size() == 2) {
              Value *Src = CallI->getArgOperand(0);
              int SrcID = getID(Src, ValueIDMap, ValueIDCounter);

              Value *Str = CallI->getArgOperand(1);
              StringRef Label = getString(Str);

              if (Label != "") {
                Out.os() << "\tLD\t\tR1, VALUE_" << SrcID << "\n"
                         << "\tST\t\tR1, " << Label << "\n";
              } else {
                return ParseError(I);
              }
            } else {
              return ParseError(I);
            }
          } else if (Func->getName() == "storeAddr") {
            if (CallI->arg_size() == 2) {
              Value *Src = CallI->getArgOperand(0);
              int SrcID = getID(Src, ValueIDMap, ValueIDCounter);

              Value *Addr = CallI->getArgOperand(1);
              int AddrID = getID(Addr, ValueIDMap, ValueIDCounter);
              addImmidiate(Addr, AddrID, AllocateBufferStream, AllocatedFlag);

              Out.os() << "\tLD\t\tR1, VALUE_" << SrcID << "\n"
                       << "\tSTI\t\tR1, VALUE_" << AddrID << "\n";
            } else {
              return ParseError(I);
            }
          } else if (Func->getName() == "readLabelAddr") {
            if (CallI->arg_size() == 2) {
              Value *Des = CallI->getArgOperand(0);
              int DesID = getID(Des, ValueIDMap, ValueIDCounter);

              Value *Str = CallI->getArgOperand(1);
              StringRef Label = getString(Str);

              if (Label != "") {
                Out.os() << "\tLEA\t\tR1, " << Label << "\n"
                         << "\tST\t\tR1, VALUE_" << DesID << "\n";
              } else {
                return ParseError(I);
              }
            } else {
              return ParseError(I);
            }
          } else {
            return ParseError(I);
          }
        } else {
          return ParseError(I);
        }
      } else if (auto *AllocaI = dyn_cast<AllocaInst>(&I)) {
        continue;
      } else if (auto *PHIN = dyn_cast<PHINode>(&I)) {
        int ResID = getID(&I, ValueIDMap, ValueIDCounter);

        Out.os() << "\tNOT\t\tR6, R6\n"
                 << "\tADD\t\tR6, R6, #1\n";
        int EndLableID = TempLabelCounter + PHIN->getNumIncomingValues();
        for (unsigned int i = 0; i < PHIN->getNumIncomingValues(); ++i) {
          Value *Val = PHIN->getIncomingValue(i);
          int ValID = getID(Val, ValueIDMap, ValueIDCounter);
          addImmidiate(Val, ValID, AllocateBufferStream, AllocatedFlag);

          BasicBlock *SrcBB = PHIN->getIncomingBlock(i);
          int SrcBBID = getID(SrcBB, BBIDMap, BBIDCounter);

          Out.os() << "\tLEA\t\tR1, LABEL_" << SrcBBID << "\n"
                   << "\tADD\t\tR1, R1, R6\n"
                   << "\tBRnp\tTEMPLABEL_" << ++TempLabelCounter << "\n"
                   << "\tLD\t\tR1, VALUE_" << ValID << "\n"
                   << "\tST\t\tR1, VALUE_" << ResID << "\n"
                   << "\tBR\t\tTEMPLABEL_" << EndLableID << "\n"
                   << "TEMPLABEL_" << TempLabelCounter << "\n";
        }
      } else if (auto *RetI = dyn_cast<ReturnInst>(&I)) {
        continue;
      } else if (I.getOpcode() == Instruction::ZExt ||
                 I.getOpcode() == Instruction::SExt ||
                 I.getOpcode() == Instruction::Trunc) {
        continue;
      } else if (auto *SelI = dyn_cast<SelectInst>(&I)) {
        int ResID = getID(&I, ValueIDMap, ValueIDCounter);

        Value *Cond = SelI->getCondition();
        int CondID = getID(Cond, ValueIDMap, ValueIDCounter);

        Value *IfTrue = SelI->getTrueValue();
        int IfTrueID = getID(IfTrue, ValueIDMap, ValueIDCounter);
        addImmidiate(IfTrue, IfTrueID, AllocateBufferStream, AllocatedFlag);

        Value *IfFalse = SelI->getFalseValue();
        int IfFalseID = getID(IfFalse, ValueIDMap, ValueIDCounter);
        addImmidiate(IfFalse, IfFalseID, AllocateBufferStream, AllocatedFlag);

        Out.os() << "\tLD\t\tR2, VALUE_" << IfTrueID << "\n"
                 << "\tLD\t\tR1, VALUE_" << CondID << "\n"
                 << "\tBRp\t\tTEMPLABEL_" << ++TempLabelCounter << "\n"
                 << "\tLD\t\tR2, VALUE_" << IfFalseID << "\n"
                 << "TEMPLABEL_" << TempLabelCounter << "\n"
                 << "\tST\t\tR2, VALUE_" << ResID << "\n";
      } else {
        return ParseError(I);
      }
    }
    Out.os() << "\n";
  }

  for (auto [Val, ValID] : ValueIDMap) {
    if (AllocatedFlag.count(Val) == 0) {
      AllocateBufferStream << "VALUE_" << ValID << "\n"
                           << "\t.BLKW\t1\n";
    }
  }

  Out.os() << "\tHALT\n" << AllocateBufferStream.str() << "\n\t.END";

  Out.keep();

  errs() << "One file generated: " << TargetFileName << "\n";

  return PreservedAnalyses::all();
}

extern "C" LLVM_ATTRIBUTE_WEAK ::PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION,
          "LLVMIRToLC3Pass", // Plugin Name
          "v0.1",            // Plugin Version
          [](PassBuilder &PB) {
            // Register the callback to parse the pass name in command line
            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &FPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "llvm-ir-to-lc3-pass") {
                    FPM.addPass(LLVMIRToLC3Pass());
                    return true;
                  }
                  return false;
                });
          }};
}