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
                    cl::desc("Specify the starting address of LC-3 "
                             "assembly file, default x3000"),
                    cl::value_desc("lc3-start-addr"), cl::init("x3000"));

static cl::opt<std::string>
    LC3StackBaseArg("lc3-stack-base",
                    cl::desc("Specify the base address of the stack in LC-3 "
                             "assembly file, default xFE00"),
                    cl::value_desc("lc3-stack-base"), cl::init("xFE00"));

static cl::opt<bool>
    SignedMul("signed-mul",
              cl::desc("use signed multiplication or not, default false"),
              cl::value_desc("signed-mul"), cl::init(false));

int getIndex(Value *Val, DenseMap<Value *, int> &Map, int &Counter) {
  if (Map.count(Val) == 0) {
    Map[Val] = ++Counter;
  }
  return Map[Val];
}

int addImmidiate(Value *Val, raw_string_ostream &ImmBuffer,
                 DenseMap<Value *, bool> &ImmFlag,
                 DenseMap<Value *, int> &ImmMap, int &ImmCounter) {
  if (auto *ConstInt = dyn_cast<ConstantInt>(Val)) {
    int ValID = getIndex(Val, ImmMap, ImmCounter);
    if (!ImmFlag.count(Val)) {
      ImmFlag[Val] = true;
      int value = ConstInt->getSExtValue();
      ImmBuffer << "VALUE_" << ValID << "\n"
                << "\t.FILL\t#" << value << "\n";
    }
    return ValID;
  }
  return 0;
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

auto ParseError(Instruction &I) {
  errs() << "Unsupported Instruction: ";
  I.print(errs());
  errs() << "\nNo File Generated\n";
  return PreservedAnalyses::all();
}

PreservedAnalyses LLVMIRToLC3Pass::run(Module &M, ModuleAnalysisManager &MAM) {
  StringRef SourceFileName = M.getSourceFileName();
  std::string TargetFileName = sys::path::stem(SourceFileName).str() + ".asm";

  std::error_code EC;
  ToolOutputFile Out(TargetFileName, EC, sys::fs::OF_None);
  std::string InstBuffer;
  raw_string_ostream InstBufferStream(InstBuffer);

  if (EC) {
    errs() << "Error: " << EC.message() << "\n";
    return PreservedAnalyses::all();
  }

  Out.os() << "; This file is generated automatically by ir-to-lc3 pass.\n"
           << "\n"
           << "; R6 : stack pointer\n"
           << "; R5 : frame pointer\n"
           << "\n"
           << "\t.ORIG\t" << LC3StartAddrArg << "\n"
           << "\tLD\t\tR6, STACK_BASE\n";

  DenseMap<Value *, int> BBIDMap;
  DenseMap<Function *, int> FuncIDMap;
  int BBIDCounter = 0;
  int ImmIDCounter = 0;
  int TempLabelCounter = 0;

  for (auto &F : M) {
    StringRef FuncName = F.getName();
    if (FuncName == "printStrAddr" || FuncName == "printStrImm" ||
        FuncName == "printCharAddr" || FuncName == "printCharImm" ||
        FuncName == "integrateLC3Asm" || FuncName == "loadLabel" ||
        FuncName == "loadAddr" || FuncName == "readLabelAddr" ||
        FuncName == "storeLabel" || FuncName == "storeAddr") {
      continue;
    }

    std::string FuncInstBuffer;
    raw_string_ostream FuncInstBufferStream(FuncInstBuffer);

    DenseMap<Value *, int> ValueOffsetMap;
    int ValueOffsetCounter = 0;

    bool isFirstBB = true;
    for (auto &BB : F) {
      int BBID = getIndex(&BB, BBIDMap, BBIDCounter);

      if (isFirstBB) {
        if (FuncName == "main") {
          Out.os() << "\tBR\t\tLABEL_" << BBID << "\n"
                   << "\n"
                   << "STACK_BASE\n\t.FILL\t" << LC3StackBaseArg << "\n"
                   << "\n";
        }
        FuncIDMap[&F] = BBID;
        isFirstBB = false;
      } else {
        FuncInstBufferStream << ";  ";
        BB.printAsOperand(FuncInstBufferStream);
        FuncInstBufferStream << "\n";
        FuncInstBufferStream << "LABEL_" << BBID << "\n";
      }

      DenseMap<Value *, bool> ImmFlag;
      DenseMap<Value *, int> ImmIDMap;
      std::string ImmBuffer;
      raw_string_ostream ImmBufferStream(ImmBuffer);
      for (auto &I : BB) {
        FuncInstBufferStream << ";";
        I.print(FuncInstBufferStream);
        FuncInstBufferStream << "\n";
        if (auto *BinI = dyn_cast<BinaryOperator>(&I)) {
          if (BinI->getOpcode() == Instruction::Mul) {
            FuncInstBufferStream << "\tAND\t\tR3, R3, #0\n";
          }

          int ResOff = -getIndex(&I, ValueOffsetMap, ValueOffsetCounter);

          Value *A = BinI->getOperand(0);
          if (int AID = addImmidiate(A, ImmBufferStream, ImmFlag, ImmIDMap,
                                     ImmIDCounter)) {
            FuncInstBufferStream << "\tLD\t\tR1, VALUE_" << AID << "\n";
          } else {
            int AOff = -getIndex(A, ValueOffsetMap, ValueOffsetCounter);
            FuncInstBufferStream << "\tLDR\t\tR1, R5, #" << AOff << "\n";
          }

          Value *B = BinI->getOperand(1);
          if (int BID = addImmidiate(B, ImmBufferStream, ImmFlag, ImmIDMap,
                                     ImmIDCounter)) {
            FuncInstBufferStream << "\tLD\t\tR2, VALUE_" << BID << "\n";
          } else {
            int BOff = -getIndex(B, ValueOffsetMap, ValueOffsetCounter);
            FuncInstBufferStream << "\tLDR\t\tR2, R5, #" << BOff << "\n";
          }

          if (BinI->getOpcode() == Instruction::Add) {
            FuncInstBufferStream << "\tADD\t\tR1, R1, R2\n"
                                 << "\tSTR\t\tR1, R5, #" << ResOff << "\n";
          } else if (BinI->getOpcode() == Instruction::Sub) {
            FuncInstBufferStream << "\tNOT\t\tR2, R2\n"
                                 << "\tADD\t\tR2, R2, #1\n"
                                 << "\tADD\t\tR1, R1, R2\n"
                                 << "\tSTR\t\tR1, R5, #" << ResOff << "\n";
          } else if (BinI->getOpcode() == Instruction::And) {
            FuncInstBufferStream << "\tAND\t\tR1, R1, R2\n"
                                 << "\tSTR\t\tR1, R5, #" << ResOff << "\n";
          } else if (BinI->getOpcode() == Instruction::Shl) {
            FuncInstBufferStream << "TEMPLABEL_" << ++TempLabelCounter << "\n"
                                 << "\tADD\t\tR1, R1, R1\n"
                                 << "\tADD\t\tR2, R2, #-1\n"
                                 << "\tBRp\t\tTEMPLABEL_" << TempLabelCounter
                                 << "\n"
                                 << "\tSTR\t\tR1, R5, #" << ResOff << "\n";
          } else if (BinI->getOpcode() == Instruction::Mul) {
            if (SignedMul) {
              FuncInstBufferStream << "\tBRzp\tTEMPLABEL_"
                                   << TempLabelCounter + 1 << "\n"
                                   << "\tNOT\t\tR1, R1\n"
                                   << "\tADD\t\tR1, R1, #1\n"
                                   << "\tNOT\t\tR2, R2\n"
                                   << "\tADD\t\tR2, R2, #1\n";
            }
            FuncInstBufferStream
                << "TEMPLABEL_" << ++TempLabelCounter << "\n"
                << "\tBRz\t\tTEMPLABEL_" << ++TempLabelCounter << "\n"
                << "\tADD\t\tR3, R3, R1\n"
                << "\tADD\t\tR2, R2, #-1\n"
                << "\tBR\t\tTEMPLABEL_" << TempLabelCounter - 1 << "\n"
                << "TEMPLABEL_" << TempLabelCounter << "\n"
                << "\tSTR\t\tR3, R5, #" << ResOff << "\n";
          } else {
            return ParseError(I);
          }
        } else if (auto *LoadI = dyn_cast<LoadInst>(&I)) {
          int ResOff = -getIndex(&I, ValueOffsetMap, ValueOffsetCounter);

          Value *Op = LoadI->getPointerOperand();
          int OpOff = -getIndex(Op, ValueOffsetMap, ValueOffsetCounter);

          FuncInstBufferStream << "\tLDR\t\tR1, R5, #" << OpOff << "\n"
                               << "\tSTR\t\tR1, R5, #" << ResOff << "\n";
        } else if (auto *StoreI = dyn_cast<StoreInst>(&I)) {
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
          FuncInstBufferStream << "\tLEA\t\tR7, LABEL_" << BBID << "\n";
          if (BranchI->isUnconditional()) {
            Value *Suc = BranchI->getSuccessor(0);
            int SucID = getIndex(Suc, BBIDMap, BBIDCounter);

            FuncInstBufferStream << "\tBR\t\tLABEL_" << SucID << "\n";
          } else {
            Value *Con = BranchI->getCondition();
            if (int ConID = addImmidiate(Con, ImmBufferStream, ImmFlag,
                                         ImmIDMap, ImmIDCounter)) {
              FuncInstBufferStream << "\tLD\t\tR1, VALUE_" << ConID << "\n";
            } else {
              int ConOff = -getIndex(Con, ValueOffsetMap, ValueOffsetCounter);
              FuncInstBufferStream << "\tLDR\t\tR1, R5, #" << ConOff << "\n";
            }

            Value *IfTrue = BranchI->getSuccessor(0);
            int IfTrueID = getIndex(IfTrue, BBIDMap, BBIDCounter);

            Value *IfFalse = BranchI->getSuccessor(1);
            int IfFalseID = getIndex(IfFalse, BBIDMap, BBIDCounter);

            FuncInstBufferStream << "\tBRz\t\tLABEL_" << IfFalseID << "\n"
                                 << "\tBR\t\tLABEL_" << IfTrueID << "\n";
          }
        } else if (auto *ICmpI = dyn_cast<ICmpInst>(&I)) {
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
            FuncInstBufferStream << "\tLDR\t\tR2, R5, #" << BOff << "\n";
          }

          FuncInstBufferStream << "\tAND\t\tR3, R3, #0\n"
                               << "\tNOT\t\tR2, R2\n"
                               << "\tADD\t\tR2, R2, #1\n"
                               << "\tADD\t\tR1, R1, R2\n";

          switch (ICmpI->getPredicate()) {
          case CmpInst::ICMP_EQ:
            FuncInstBufferStream << "\tBRnp\tTEMPLABEL_" << ++TempLabelCounter
                                 << "\n";
            break;
          case CmpInst::ICMP_NE:
            FuncInstBufferStream << "\tBRz\t\tTEMPLABEL_" << ++TempLabelCounter
                                 << "\n";
            break;
          case CmpInst::ICMP_SGT:
            FuncInstBufferStream << "\tBRnz\tTEMPLABEL_" << ++TempLabelCounter
                                 << "\n";
            break;
          case CmpInst::ICMP_SGE:
            FuncInstBufferStream << "\tBRn\t\tTEMPLABEL_" << ++TempLabelCounter
                                 << "\n";
            break;
          case CmpInst::ICMP_SLT:
            FuncInstBufferStream << "\tBRzp\tTEMPLABEL_" << ++TempLabelCounter
                                 << "\n";
            break;
          case CmpInst::ICMP_SLE:
            FuncInstBufferStream << "\tBRp\t\tTEMPLABEL_" << ++TempLabelCounter
                                 << "\n";
            break;
          case CmpInst::ICMP_UGT:
            FuncInstBufferStream << "\tBRnz\tTEMPLABEL_" << ++TempLabelCounter
                                 << "\n";
            break;
          case CmpInst::ICMP_UGE:
            FuncInstBufferStream << "\tBRn\t\tTEMPLABEL_" << ++TempLabelCounter
                                 << "\n";
            break;
          case CmpInst::ICMP_ULT:
            FuncInstBufferStream << "\tBRzp\tTEMPLABEL_" << ++TempLabelCounter
                                 << "\n";
            break;
          case CmpInst::ICMP_ULE:
            FuncInstBufferStream << "\tBRp\t\tTEMPLABEL_" << ++TempLabelCounter
                                 << "\n";
            break;
          default:
            return ParseError(I);
          }

          FuncInstBufferStream << "\tADD\t\tR3, R3, #1\n"
                               << "TEMPLABEL_" << TempLabelCounter << "\n"
                               << "\tSTR\t\tR3, R5, #" << ResOff << "\n";
        } else if (auto *CallI = dyn_cast<CallInst>(&I)) {
          if (Function *Func = CallI->getCalledFunction()) {
            if (Func->getName() == "printStrImm") {
              if (CallI->arg_size() == 1) {
                Value *Str = CallI->getArgOperand(0);

                if (int StrID = addImmidiate(Str, ImmBufferStream, ImmFlag,
                                             ImmIDMap, ImmIDCounter)) {
                  FuncInstBufferStream << "\tLEA\t\tR0, VALUE_" << StrID
                                       << "\n";
                } else {
                  int StrOff =
                      -getIndex(Str, ValueOffsetMap, ValueOffsetCounter);
                  FuncInstBufferStream << "\tADD\t\tR0, R5, #" << StrOff
                                       << "\n";
                }

                FuncInstBufferStream << "\tPUTS\n";
              } else {
                return ParseError(I);
              }
            } else if (Func->getName() == "printStrAddr") {
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
                return ParseError(I);
              }
            } else if (Func->getName() == "printCharAddr") {
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
                return ParseError(I);
              }
            } else if (Func->getName() == "printCharImm") {
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
                return ParseError(I);
              }
            } else if (Func->getName() == "integrateLC3Asm") {
              if (CallI->arg_size() == 1) {
                Value *Str = CallI->getArgOperand(0);
                StringRef Content = getString(Str);
                if (Content != "") {
                  FuncInstBufferStream << Content << "\n";
                } else {
                  return ParseError(I);
                }
              } else {
                return ParseError(I);
              }
            } else if (Func->getName() == "loadLabel") {
              if (CallI->arg_size() == 1) {
                int DesOff = -getIndex(&I, ValueOffsetMap, ValueOffsetCounter);

                Value *Str = CallI->getArgOperand(0);
                StringRef Label = getString(Str);

                if (Label != "") {
                  FuncInstBufferStream << "\tLD\t\tR1, " << Label << "\n"
                                       << "\tSTR\t\tR1, R5, #" << DesOff
                                       << "\n";
                } else {
                  return ParseError(I);
                }
              } else {
                return ParseError(I);
              }
            } else if (Func->getName() == "loadAddr") {
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
                return ParseError(I);
              }
            } else if (Func->getName() == "storeLabel") {
              if (CallI->arg_size() == 2) {
                Value *Src = CallI->getArgOperand(0);
                int SrcOff = -getIndex(Src, ValueOffsetMap, ValueOffsetCounter);

                Value *Str = CallI->getArgOperand(1);
                StringRef Label = getString(Str);

                if (Label != "") {
                  FuncInstBufferStream << "\tLDR\t\tR1, R5, #" << SrcOff << "\n"
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
                                       << "\tSTR\t\tR1, R2, #0";
                }
              } else {
                return ParseError(I);
              }
            } else if (Func->getName() == "readLabelAddr") {
              if (CallI->arg_size() == 1) {
                int DesOff = -getIndex(&I, ValueOffsetMap, ValueOffsetCounter);

                Value *Str = CallI->getArgOperand(0);
                StringRef Label = getString(Str);

                if (Label != "") {
                  FuncInstBufferStream << "\tLEA\t\tR1, " << Label << "\n"
                                       << "\tSTR\t\tR1, R5, #" << DesOff
                                       << "\n";
                } else {
                  return ParseError(I);
                }
              } else {
                return ParseError(I);
              }
            } else if (CallI->arg_size() <= 5 && FuncIDMap.count(Func)) {
              int FuncID = FuncIDMap[Func];
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
              FuncInstBufferStream << "\tJSR\t\tLABEL_" << FuncID << "\n";
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
              return ParseError(I);
            }
          } else {
            return ParseError(I);
          }
        } else if (auto *AllocaI = dyn_cast<AllocaInst>(&I)) {
          continue;
        } else if (auto *PHIN = dyn_cast<PHINode>(&I)) {
          int ResOff = -getIndex(&I, ValueOffsetMap, ValueOffsetCounter);

          FuncInstBufferStream << "\tNOT\t\tR2, R7\n"
                               << "\tADD\t\tR2, R2, #1\n";
          int ArgSize = PHIN->getNumIncomingValues();
          int EndLableID = TempLabelCounter + ArgSize;
          for (unsigned int i = 0; i < ArgSize; ++i) {

            BasicBlock *SrcBB = PHIN->getIncomingBlock(i);
            int SrcBBID = getIndex(SrcBB, BBIDMap, BBIDCounter);

            FuncInstBufferStream << "\tLEA\t\tR1, LABEL_" << SrcBBID << "\n"
                                 << "\tADD\t\tR1, R1, R2\n"
                                 << "\tBRnp\tTEMPLABEL_" << ++TempLabelCounter
                                 << "\n";

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
              FuncInstBufferStream << "\tBR\t\tTEMPLABEL_" << EndLableID
                                   << "\n";
            }
            FuncInstBufferStream << "TEMPLABEL_" << TempLabelCounter << "\n";
          }
        } else if (auto *RetI = dyn_cast<ReturnInst>(&I)) {
          if (Value *Val = RetI->getReturnValue()) {
            if (int ValID = addImmidiate(Val, ImmBufferStream, ImmFlag,
                                         ImmIDMap, ImmIDCounter)) {
              FuncInstBufferStream << "\tLD\t\tR0, VALUE_" << ValID << "\n";
            } else {
              int ValOff = -getIndex(Val, ValueOffsetMap, ValueOffsetCounter);
              FuncInstBufferStream << "\tLDR\t\tR0, R5, #" << ValOff << "\n";
            }
          }
          FuncInstBufferStream << "; restore R5, R6, R7\n";
          FuncInstBufferStream << "\tADD\t\tR6, R5, #0\n";
          FuncInstBufferStream << "\tLDR\t\tR5, R6, #0\n"
                               << "\tADD\t\tR6, R6, #1\n";
          FuncInstBufferStream << "\tLDR\t\tR7, R6, #0\n"
                               << "\tADD\t\tR6, R6, #1\n";
          FuncInstBufferStream << "\tRET\n";
          continue;
        } else if (I.getOpcode() == Instruction::ZExt ||
                   I.getOpcode() == Instruction::SExt ||
                   I.getOpcode() == Instruction::Trunc) {
          continue;
        } else if (auto *SelI = dyn_cast<SelectInst>(&I)) {
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
                               << "\tBRp\t\tTEMPLABEL_" << ++TempLabelCounter
                               << "\n";

          Value *IfFalse = SelI->getFalseValue();
          if (int IfFalseID = addImmidiate(IfFalse, ImmBufferStream, ImmFlag,
                                           ImmIDMap, ImmIDCounter)) {
            FuncInstBufferStream << "\tLD\t\tR2, VALUE_" << IfFalseID << "\n";
          } else {
            int IfFalseOff =
                -getIndex(IfFalse, ValueOffsetMap, ValueOffsetCounter);
            FuncInstBufferStream << "\tLDR\t\tR2, R5, #" << IfFalseOff << "\n";
          }

          FuncInstBufferStream << "TEMPLABEL_" << TempLabelCounter << "\n"
                               << "\tSTR\t\tR2, R5, #" << ResOff << "\n";
        } else {
          return ParseError(I);
        }
      }

      FuncInstBufferStream << "\n";
      if (!ImmBuffer.empty()) {
        FuncInstBufferStream << "; static value section for LABEL_" << BBID
                             << "\n"
                             << ImmBufferStream.str() << "\n";
      }
    }

    InstBufferStream << "; function " << FuncName << "\n";
    InstBufferStream << "; local variable count: " << ValueOffsetCounter << "\n";
    InstBufferStream << "LABEL_" << FuncIDMap[&F] << "\n";
    InstBufferStream << "; init R6, R5, store old R5, R7\n";
    InstBufferStream << "\tADD\t\tR6, R6, #-1\n"
                     << "\tSTR\t\tR7, R6, #0\n";
    InstBufferStream << "\tADD\t\tR6, R6, #-1\n"
                     << "\tSTR\t\tR5, R6, #0\n";
    InstBufferStream << "\tADD\t\tR5, R6, #0\n";
    if (ValueOffsetCounter < 32) {
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
      return PreservedAnalyses::all();
    }
    if (int argSize = F.arg_size()) {
      InstBufferStream << "; store arguments\n";
      for (int i = 0; i < argSize; i++) {
        Value *Arg = F.getArg(i);
        int ArgOff = -getIndex(Arg, ValueOffsetMap, ValueOffsetCounter);
        InstBufferStream << "\tSTR\t\tR" << i << ", R5, #" << ArgOff << "\n";
      }
    }

    InstBufferStream << FuncInstBufferStream.str();
  }

  Out.os() << InstBufferStream.str() << "\t.END";

  Out.keep();

  errs() << "One file generated: " << TargetFileName << "\n";

  return PreservedAnalyses::all();
}

extern "C" LLVM_ATTRIBUTE_WEAK ::PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION,
          "LLVMIRToLC3Pass", // Plugin Name
          "v0.2",            // Plugin Version
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