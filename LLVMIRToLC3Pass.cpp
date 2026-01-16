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
#include <llvm/IR/Value.h>
#include <string>
#include <system_error>

using namespace llvm;

static cl::opt<std::string>
    LC3StartAddrArg("lc3-start-addr",
                    cl::desc("Specify the starting address of the LC-3 "
                             "assembly file, default x3000"),
                    cl::value_desc("lc3-start-addr"), cl::init("x3000"));

static cl::opt<bool>
    SignedMul("signed-mul",
              cl::desc("use signed multiplication or not, default false"),
              cl::value_desc("signed-mul"), cl::init(false));

int getID(Value *Val, DenseMap<Value *, int> &Map, int &Counter) {
  if (Map.count(Val) == 0) {
    Map[Val] = ++Counter;
  }
  return Map[Val];
}

bool addImmidiate(Value *Val, int ValID, raw_string_ostream &Buffer,
                  DenseMap<Value *, bool> &Flag) {
  if (auto *ConstInt = dyn_cast<ConstantInt>(Val)) {
    if (!Flag.count(Val)) {
      Flag[Val] = true;
      int value = ConstInt->getSExtValue();
      Buffer << "VALUE_" << ValID << "\n"
             << "\t.FILL\t#" << value << "\n";
    }
    return true;
  }
  return false;
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

bool addString(Value *Val, int ValID, raw_string_ostream &Buffer,
               DenseMap<Value *, bool> &Flag) {
  auto Str = getString(Val);
  if (Str.size() > 0) {
    if (!Flag.count(Val)) {
      Flag[Val] = true;
      Buffer << "VALUE_" << ValID << "\n"
             << "\t.STRINGZ\t\"" << Str << "\"\n";
    }
    return true;
  }
  return false;
}

auto ParseError(Instruction &I) {
  errs() << "Unsupported Instruction: ";
  I.print(errs());
  errs() << "\nNo File Generated\n";
  return PreservedAnalyses::none();
}

PreservedAnalyses LLVMIRToLC3Pass::run(Module &M, ModuleAnalysisManager &MAM) {
  StringRef SourceFileName = M.getSourceFileName();
  std::string TargetFileName = sys::path::stem(SourceFileName).str() + ".asm";

  std::error_code EC;
  ToolOutputFile Out(TargetFileName, EC, sys::fs::OF_None);
  std::string AllocateBuffer, InstBuffer;
  raw_string_ostream AllocateBufferStream(AllocateBuffer),
      InstBufferStream(InstBuffer);

  if (EC) {
    errs() << "Error: " << EC.message() << "\n";
    return PreservedAnalyses::all();
  }

  Out.os() << "; This file is generated automatically by ir-to-lc3 pass.\n"
           << "\n"
           << "\t.ORIG\t" << LC3StartAddrArg << "\n"
           << "\n";

  DenseMap<Value *, int> BBIDMap, ValueIDMap;
  DenseMap<Function *, int> FuncIDMap;
  DenseMap<Value *, bool> AllocatedFlag;
  int BBIDCounter = 0;
  int TempLabelCounter = 0;
  int ValueIDCounter = 0;

  for (auto &F : M) {

    InstBufferStream << "; function " << F.getName() << "\n";

    bool isFirstBB = true;
    for (auto &BB : F) {
      int BBID = getID(&BB, BBIDMap, BBIDCounter);
      InstBufferStream << "LABEL_" << BBID << "\n";

      if (isFirstBB) {
        if (F.getName() == "main") {
          Out.os() << "\tBR\t\tLABEL_" << BBID << "\n";
        }
        FuncIDMap[&F] = BBID;
        if (int argSize = F.arg_size()) {
          for (int i = 0; i < argSize; i++) {
            Value *Arg = F.getArg(i);
            int ArgID = getID(Arg, ValueIDMap, ValueIDCounter);
            InstBufferStream << "\tST\t\tR" << i << ", VALUE_" << ArgID << "\n";
          }
        }
        isFirstBB = false;
      }

      for (auto &I : BB) {
        InstBufferStream << ";";
        I.print(InstBufferStream);
        InstBufferStream << "\n";
        if (auto *BinI = dyn_cast<BinaryOperator>(&I)) {
          int ResID = getID(&I, ValueIDMap, ValueIDCounter);

          Value *A = BinI->getOperand(0);
          int AID = getID(A, ValueIDMap, ValueIDCounter);
          addImmidiate(A, AID, AllocateBufferStream, AllocatedFlag);

          Value *B = BinI->getOperand(1);
          int BID = getID(B, ValueIDMap, ValueIDCounter);
          addImmidiate(B, BID, AllocateBufferStream, AllocatedFlag);

          if (BinI->getOpcode() == Instruction::Add) {
            InstBufferStream << "\tLD\t\tR1, VALUE_" << AID << "\n"
                             << "\tLD\t\tR2, VALUE_" << BID << "\n"
                             << "\tADD\t\tR1, R1, R2\n"
                             << "\tST\t\tR1, VALUE_" << ResID << "\n";
          } else if (BinI->getOpcode() == Instruction::Sub) {
            InstBufferStream << "\tLD\t\tR1, VALUE_" << AID << "\n"
                             << "\tLD\t\tR2, VALUE_" << BID << "\n"
                             << "\tNOT\t\tR2, R2\n"
                             << "\tADD\t\tR2, R2, #1\n"
                             << "\tADD\t\tR1, R1, R2\n"
                             << "\tST\t\tR1, VALUE_" << ResID << "\n";
          } else if (BinI->getOpcode() == Instruction::And) {
            InstBufferStream << "\tLD\t\tR1, VALUE_" << AID << "\n"
                             << "\tLD\t\tR2, VALUE_" << BID << "\n"
                             << "\tAND\t\tR1, R1, R2\n"
                             << "\tST\t\tR1, VALUE_" << ResID << "\n";
          } else if (BinI->getOpcode() == Instruction::Shl) {
            InstBufferStream << "\tLD\t\tR1, VALUE_" << AID << "\n"
                             << "\tLD\t\tR2, VALUE_" << BID << "\n"
                             << "TEMPLABEL_" << ++TempLabelCounter << "\n"
                             << "\tADD\t\tR1, R1, R1\n"
                             << "\tADD\t\tR2, R2, #-1\n"
                             << "\tBRp\t\tTEMPLABEL_" << TempLabelCounter
                             << "\n"
                             << "\tST\t\tR1, VALUE_" << ResID << "\n";
          } else if (BinI->getOpcode() == Instruction::Mul) {
            InstBufferStream << "\tAND\t\tR3, R3, #0\n"
                             << "\tLD\t\tR1, VALUE_" << AID << "\n"
                             << "\tLD\t\tR2, VALUE_" << BID << "\n";
            if (SignedMul) {
              InstBufferStream << "\tBRzp\tTEMPLABEL_" << TempLabelCounter + 1
                               << "\n"
                               << "\tNOT\t\tR1, R1\n"
                               << "\tADD\t\tR1, R1, #1\n"
                               << "\tNOT\t\tR2, R2\n"
                               << "\tADD\t\tR2, R2, #1\n";
            }
            InstBufferStream
                << "TEMPLABEL_" << ++TempLabelCounter << "\n"
                << "\tBRz\t\tTEMPLABEL_" << ++TempLabelCounter << "\n"
                << "\tADD\t\tR3, R3, R1\n"
                << "\tADD\t\tR2, R2, #-1\n"
                << "\tBR\t\tTEMPLABEL_" << TempLabelCounter - 1 << "\n"
                << "TEMPLABEL_" << TempLabelCounter << "\n"
                << "\tST\t\tR3, VALUE_" << ResID << "\n";
          } else {
            return ParseError(I);
          }
        } else if (auto *LoadI = dyn_cast<LoadInst>(&I)) {
          int ResID = getID(&I, ValueIDMap, ValueIDCounter);

          Value *Op = LoadI->getPointerOperand();
          int OpID = getID(Op, ValueIDMap, ValueIDCounter);

          InstBufferStream << "\tLD\t\tR1, VALUE_" << OpID << "\n"
                           << "\tST\t\tR1, VALUE_" << ResID << "\n";
        } else if (auto *StoreI = dyn_cast<StoreInst>(&I)) {
          Value *Val = StoreI->getValueOperand();
          int ValID = getID(Val, ValueIDMap, ValueIDCounter);
          addImmidiate(Val, ValID, AllocateBufferStream, AllocatedFlag);

          Value *Ptr = StoreI->getPointerOperand();
          int PtrID = getID(Ptr, ValueIDMap, ValueIDCounter);

          InstBufferStream << "\tLD\t\tR1, VALUE_" << ValID << "\n"
                           << "\tST\t\tR1, VALUE_" << PtrID << "\n";
        } else if (auto *BranchI = dyn_cast<BranchInst>(&I)) {
          InstBufferStream << "\tLEA\t\tR6, LABEL_" << BBID << "\n";
          if (BranchI->isUnconditional()) {
            Value *Suc = BranchI->getSuccessor(0);
            int SucID = getID(Suc, BBIDMap, BBIDCounter);

            InstBufferStream << "\tBR\t\tLABEL_" << SucID << "\n";
          } else {
            Value *Con = BranchI->getCondition();
            int ConID = getID(Con, ValueIDMap, ValueIDCounter);

            Value *IfTrue = BranchI->getSuccessor(0);
            int IfTrueID = getID(IfTrue, BBIDMap, BBIDCounter);

            Value *IfFalse = BranchI->getSuccessor(1);
            int IfFalseID = getID(IfFalse, BBIDMap, BBIDCounter);

            InstBufferStream << "\tLD\t\tR1, VALUE_" << ConID << "\n"
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

          InstBufferStream << "\tAND\t\tR3, R3, #0\n"
                           << "\tLD\t\tR1, VALUE_" << AID << "\n"
                           << "\tLD\t\tR2, VALUE_" << BID << "\n"
                           << "\tNOT\t\tR2, R2\n"
                           << "\tADD\t\tR2, R2, #1\n"
                           << "\tADD\t\tR1, R1, R2\n";

          switch (ICmpI->getPredicate()) {
          case CmpInst::ICMP_EQ:
            InstBufferStream << "\tBRnp\tTEMPLABEL_" << ++TempLabelCounter
                             << "\n";
            break;
          case CmpInst::ICMP_NE:
            InstBufferStream << "\tBRz\t\tTEMPLABEL_" << ++TempLabelCounter
                             << "\n";
            break;
          case CmpInst::ICMP_SGT:
            InstBufferStream << "\tBRnz\tTEMPLABEL_" << ++TempLabelCounter
                             << "\n";
            break;
          case CmpInst::ICMP_SGE:
            InstBufferStream << "\tBRn\t\tTEMPLABEL_" << ++TempLabelCounter
                             << "\n";
            break;
          case CmpInst::ICMP_SLT:
            InstBufferStream << "\tBRzp\tTEMPLABEL_" << ++TempLabelCounter
                             << "\n";
            break;
          case CmpInst::ICMP_SLE:
            InstBufferStream << "\tBRp\t\tTEMPLABEL_" << ++TempLabelCounter
                             << "\n";
            break;
          case CmpInst::ICMP_UGT:
            InstBufferStream << "\tBRnz\tTEMPLABEL_" << ++TempLabelCounter
                             << "\n";
            break;
          case CmpInst::ICMP_UGE:
            InstBufferStream << "\tBRn\t\tTEMPLABEL_" << ++TempLabelCounter
                             << "\n";
            break;
          case CmpInst::ICMP_ULT:
            InstBufferStream << "\tBRzp\tTEMPLABEL_" << ++TempLabelCounter
                             << "\n";
            break;
          case CmpInst::ICMP_ULE:
            InstBufferStream << "\tBRp\t\tTEMPLABEL_" << ++TempLabelCounter
                             << "\n";
            break;
          default:
            return ParseError(I);
          }

          InstBufferStream << "\tADD\t\tR3, R3, #1\n"
                           << "TEMPLABEL_" << TempLabelCounter << "\n"
                           << "\tST\t\tR3, VALUE_" << ResID << "\n";
        } else if (auto *CallI = dyn_cast<CallInst>(&I)) {
          if (Function *Func = CallI->getCalledFunction()) {
            if (Func->getName() == "printStrImm") {
              if (CallI->arg_size() == 1) {
                Value *Str = CallI->getArgOperand(0);
                int StrID = getID(Str, ValueIDMap, ValueIDCounter);
                addString(Str, StrID, AllocateBufferStream, AllocatedFlag);

                InstBufferStream << "\tLEA\t\tR0, VALUE_" << StrID << "\n"
                                 << "\tPUTS\n";
              } else {
                return ParseError(I);
              }
            } else if (Func->getName() == "printStrAddr") {
              if (CallI->arg_size() == 1) {
                Value *Addr = CallI->getArgOperand(0);
                int AddrID = getID(Addr, ValueIDMap, ValueIDCounter);
                addImmidiate(Addr, AddrID, AllocateBufferStream, AllocatedFlag);

                InstBufferStream << "\tLD\t\tR0, VALUE_" << AddrID << "\n"
                                 << "\tPUTS\n";
              } else {
                return ParseError(I);
              }
            } else if (Func->getName() == "printCharAddr") {
              if (CallI->arg_size() == 1) {
                Value *Addr = CallI->getArgOperand(0);
                int AddrID = getID(Addr, ValueIDMap, ValueIDCounter);
                addImmidiate(Addr, AddrID, AllocateBufferStream, AllocatedFlag);

                InstBufferStream << "\tLDI\t\tR0, VALUE_" << AddrID << "\n"
                                 << "\tOUT\n";
              } else {
                return ParseError(I);
              }
            } else if (Func->getName() == "printCharImm") {
              if (CallI->arg_size() == 1) {
                Value *Char = CallI->getArgOperand(0);
                int CharID = getID(Char, ValueIDMap, ValueIDCounter);
                addImmidiate(Char, CharID, AllocateBufferStream, AllocatedFlag);

                InstBufferStream << "\tLD\t\tR0, VALUE_" << CharID << "\n"
                                 << "\tOUT\n";
              } else {
                return ParseError(I);
              }
            } else if (Func->getName() == "integrateLC3Asm") {
              if (CallI->arg_size() == 1) {
                Value *Str = CallI->getArgOperand(0);
                StringRef Content = getString(Str);
                if (Content != "") {
                  InstBufferStream << Content << "\n";
                } else {
                  return ParseError(I);
                }
              } else {
                return ParseError(I);
              }
            } else if (Func->getName() == "loadLabel") {
              if (CallI->arg_size() == 1) {
                int DesID = getID(&I, ValueIDMap, ValueIDCounter);

                Value *Str = CallI->getArgOperand(0);
                StringRef Label = getString(Str);

                if (Label != "") {
                  InstBufferStream << "\tLD\t\tR1, " << Label << "\n"
                                   << "\tST\t\tR1, VALUE_" << DesID << "\n";
                } else {
                  return ParseError(I);
                }
              } else {
                return ParseError(I);
              }
            } else if (Func->getName() == "loadAddr") {
              if (CallI->arg_size() == 1) {
                int DesID = getID(&I, ValueIDMap, ValueIDCounter);

                Value *Addr = CallI->getArgOperand(0);
                int AddrID = getID(Addr, ValueIDMap, ValueIDCounter);
                addImmidiate(Addr, AddrID, AllocateBufferStream, AllocatedFlag);

                InstBufferStream << "\tLDI\t\tR1, VALUE_" << AddrID << "\n"
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
                  InstBufferStream << "\tLD\t\tR1, VALUE_" << SrcID << "\n"
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

                InstBufferStream << "\tLD\t\tR1, VALUE_" << SrcID << "\n"
                                 << "\tSTI\t\tR1, VALUE_" << AddrID << "\n";
              } else {
                return ParseError(I);
              }
            } else if (Func->getName() == "readLabelAddr") {
              if (CallI->arg_size() == 1) {
                int DesID = getID(&I, ValueIDMap, ValueIDCounter);

                Value *Str = CallI->getArgOperand(0);
                StringRef Label = getString(Str);

                if (Label != "") {
                  InstBufferStream << "\tLEA\t\tR1, " << Label << "\n"
                                   << "\tST\t\tR1, VALUE_" << DesID << "\n";
                } else {
                  return ParseError(I);
                }
              } else {
                return ParseError(I);
              }
            } else if (CallI->arg_size() <= 5 && FuncIDMap.count(Func)) {
              int FuncID = FuncIDMap[Func];
              if (int ArgSize = CallI->arg_size()) {
                InstBufferStream << "; ArgSize: " << ArgSize << "\n";
                for (int i = 0; i < ArgSize; i++) {
                  Value *Arg = CallI->getArgOperand(i);
                  int ArgID = getID(Arg, ValueIDMap, ValueIDCounter);
                  addImmidiate(Arg, ArgID, AllocateBufferStream, AllocatedFlag);
                  InstBufferStream << "\tLD\t\tR" << i << ", VALUE_" << ArgID
                                   << "\n";
                }
              }
              InstBufferStream << "\tJSR\t\tLABEL_" << FuncID << "\n";
              if (!CallI->getType()->isVoidTy()) {
                int ResID = getID(&I, ValueIDMap, ValueIDCounter);
                InstBufferStream << "\tST\t\tR0, VALUE_" << ResID << "\n";
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

          InstBufferStream << "\tNOT\t\tR2, R6\n"
                           << "\tADD\t\tR2, R2, #1\n";
          int ArgSize = PHIN->getNumIncomingValues();
          int EndLableID = TempLabelCounter + ArgSize;
          for (unsigned int i = 0; i < ArgSize; ++i) {
            Value *Val = PHIN->getIncomingValue(i);
            int ValID = getID(Val, ValueIDMap, ValueIDCounter);
            addImmidiate(Val, ValID, AllocateBufferStream, AllocatedFlag);

            BasicBlock *SrcBB = PHIN->getIncomingBlock(i);
            int SrcBBID = getID(SrcBB, BBIDMap, BBIDCounter);

            InstBufferStream << "\tLEA\t\tR1, LABEL_" << SrcBBID << "\n"
                             << "\tADD\t\tR1, R1, R2\n"
                             << "\tBRnp\tTEMPLABEL_" << ++TempLabelCounter
                             << "\n"
                             << "\tLD\t\tR1, VALUE_" << ValID << "\n"
                             << "\tST\t\tR1, VALUE_" << ResID << "\n";
            if (i < ArgSize - 1) {
              InstBufferStream << "\tBR\t\tTEMPLABEL_" << EndLableID << "\n";
            }
            InstBufferStream << "TEMPLABEL_" << TempLabelCounter << "\n";
          }
        } else if (auto *RetI = dyn_cast<ReturnInst>(&I)) {
          if (Value *Val = RetI->getReturnValue()) {
            int ValID = getID(Val, ValueIDMap, ValueIDCounter);
            addImmidiate(Val, ValID, AllocateBufferStream, AllocatedFlag);
            InstBufferStream << "\tLD\t\tR0, VALUE_" << ValID << "\n";
          }
          if (F.getName() != "main") {
            InstBufferStream << "\tRET\n";
          } else {
            InstBufferStream << "\tHALT\n";
          }
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

          InstBufferStream << "\tLD\t\tR2, VALUE_" << IfTrueID << "\n"
                           << "\tLD\t\tR1, VALUE_" << CondID << "\n"
                           << "\tBRp\t\tTEMPLABEL_" << ++TempLabelCounter
                           << "\n"
                           << "\tLD\t\tR2, VALUE_" << IfFalseID << "\n"
                           << "TEMPLABEL_" << TempLabelCounter << "\n"
                           << "\tST\t\tR2, VALUE_" << ResID << "\n";
        } else {
          return ParseError(I);
        }
      }
      InstBufferStream << "\n";
    }
  }

  for (auto [Val, ValID] : ValueIDMap) {
    if (AllocatedFlag.count(Val) == 0) {
      AllocateBufferStream << "VALUE_" << ValID << "\n"
                           << "\t.BLKW\t1\n";
    }
  }

  Out.os() << InstBufferStream.str() << AllocateBufferStream.str()
           << "\n\t.END";

  Out.keep();

  errs() << "One file generated: " << TargetFileName << "\n";

  return PreservedAnalyses::none();
}

extern "C" LLVM_ATTRIBUTE_WEAK ::PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION,
          "LLVMIRToLC3Pass", // Plugin Name
          "v0.1",            // Plugin Version
          [](PassBuilder &PB) {
            // Register the callback to parse the pass name in command line
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &FPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "llvm-ir-to-lc3-pass") {
                    FPM.addPass(LLVMIRToLC3Pass());
                    return true;
                  }
                  return false;
                });
          }};
}