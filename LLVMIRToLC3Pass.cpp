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
              cl::desc("Use signed multiplication or not, default false"),
              cl::value_desc("signed-mul"), cl::init(false));

static cl::opt<bool>
    NoComment("no-comment",
              cl::desc("Generate pure LC-3 assembly code without any comment"),
              cl::value_desc("no-comment"), cl::init(false));

int getIndex(Value *Val, DenseMap<Value *, int> &Map, int &Counter) {
  if (Map.count(Val) == 0) {
    Map[Val] = ++Counter;
  }
  return Map[Val];
}

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

std::string getIndex(BasicBlock *BB, DenseMap<Value *, std::string> &Map,
                     int &Counter) {
  if (Map.count(BB) == 0) {
    Map[BB] = BB->getParent()->getName().str() + "_" + getBBName(BB) + "_" +
              std::to_string(++Counter);
  }
  return Map[BB];
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

auto UnsupportInst(Instruction &I) {
  errs() << "Unsupported Instruction:\n";
  I.print(errs());
  errs() << "\nNo File Generated\n";
  return PreservedAnalyses::none();
}

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
  StringRef SourceFileName = M.getSourceFileName();
  std::string TargetFileName = sys::path::stem(SourceFileName).str() + ".asm";

  std::error_code EC;
  ToolOutputFile Out(TargetFileName, EC, sys::fs::OF_None);
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

  DenseMap<Value *, std::string> BBNameMap;
  DenseMap<Function *, std::string> FuncLabelMap;
  int BBNameCounter = 0;
  int ImmIDCounter = 0;
  int TempLabelCounter = 0;

  for (auto &F : M) {
    if (F.isIntrinsic() || F.isDeclaration()) {
      continue;
    }

    StringRef FuncName = F.getName();
    std::string FuncInstBuffer;
    raw_string_ostream FuncInstBufferStream(FuncInstBuffer);

    DenseMap<Value *, int> ValueOffsetMap;
    int ValueOffsetCounter = 0;

    bool isFirstBB = true;
    for (auto &BB : F) {
      std::string BBName = getIndex(&BB, BBNameMap, BBNameCounter);

      if (isFirstBB) {
        if (FuncName == "main") {
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

      LLVMContext &Ctx = F.getContext();
      Type *WordTy = Type::getInt32Ty(Ctx);

      for (auto &I : make_early_inc_range(BB)) {
        if (auto *IntrI = dyn_cast<IntrinsicInst>(&I)) {
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

          Value *Cmp = Builder.CreateICmp(Pred, A, B);
          Value *Select = Builder.CreateSelect(Cmp, A, B);

          IntrI->replaceAllUsesWith(Select);
          IntrI->eraseFromParent();
        }
      }

      for (auto &I : make_early_inc_range(BB)) {
        if (auto *ICmpI = dyn_cast<ICmpInst>(&I)) {
          Value *FirVal = ICmpI->getOperand(0);
          if (auto *ConstInt = dyn_cast<ConstantInt>(FirVal)) {
            ICmpI->swapOperands();
          }
          auto Pred = ICmpI->getPredicate();
          Value *A = ICmpI->getOperand(0);
          Value *B = ICmpI->getOperand(1);
          if (auto *ConstInt = dyn_cast<ConstantInt>(B)) {
            IRBuilder<> Builder(ICmpI);

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
                  auto *Zero =
                      ConstantInt::get(WordTy, ConstInt->getSExtValue());
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
          auto OpCode = BinOp->getOpcode();
          Value *A = I.getOperand(0);
          Value *B = I.getOperand(1);
          switch (OpCode) {
          case Instruction::LShr:
            if (auto *ShiftAmt = dyn_cast<ConstantInt>(B)) {
              unsigned BitWidth = A->getType()->getIntegerBitWidth();
              uint64_t ShiftVal = ShiftAmt->getZExtValue();

              if (ShiftVal >= BitWidth) {
                continue;
              }

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

      for (auto &I : BB) {
        if (!NoComment) {
          FuncInstBufferStream << addPrefixInst(I, ";")
                               << addRegisterComment(I);
        }
        if (auto *BinOp = dyn_cast<BinaryOperator>(&I)) {
          auto OpCode = BinOp->getOpcode();
          if (OpCode == Instruction::Mul || OpCode == Instruction::UDiv) {
            FuncInstBufferStream << "\tAND\t\tR3, R3, #0\n";
          }

          int ResOff = -getIndex(&I, ValueOffsetMap, ValueOffsetCounter);

          Value *B = BinOp->getOperand(1);
          if (int BID = addImmidiate(B, ImmBufferStream, ImmFlag, ImmIDMap,
                                     ImmIDCounter)) {
            FuncInstBufferStream << "\tLD\t\tR2, VALUE_" << BID << "\n";
          } else {
            int BOff = -getIndex(B, ValueOffsetMap, ValueOffsetCounter);
            FuncInstBufferStream << "\tLDR\t\tR2, R5, #" << BOff << "\n";
          }
          if (OpCode == Instruction::Sub || OpCode == Instruction::UDiv) {
            FuncInstBufferStream << "\tNOT\t\tR2, R2\n"
                                 << "\tADD\t\tR2, R2, #1\n";
          } else if (OpCode == Instruction::URem) {
            FuncInstBufferStream << "\tNOT\t\tR3, R2\n"
                                 << "\tADD\t\tR3, R3, #1\n";
          }

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
            FuncInstBufferStream << "\tADD\t\tR1, R1, R2\n"
                                 << "\tSTR\t\tR1, R5, #" << ResOff << "\n";
            break;
          case Instruction::Sub:
            FuncInstBufferStream << "\tADD\t\tR1, R1, R2\n"
                                 << "\tSTR\t\tR1, R5, #" << ResOff << "\n";
            break;
          case Instruction::And:
            FuncInstBufferStream << "\tAND\t\tR1, R1, R2\n"
                                 << "\tSTR\t\tR1, R5, #" << ResOff << "\n";
            break;
          case Instruction::Or:
            FuncInstBufferStream << "\tNOT\t\tR1, R1\n"
                                 << "\tNOT\t\tR2, R2\n"
                                 << "\tAND\t\tR1, R1, R2\n"
                                 << "\tNOT\t\tR1, R1\n"
                                 << "\tSTR\t\tR1, R5, #" << ResOff << "\n";
            break;
          case Instruction::Shl:
            FuncInstBufferStream << "SHL_LOOP_" << ++TempLabelCounter << "\n"
                                 << "\tADD\t\tR1, R1, R1\n"
                                 << "\tADD\t\tR2, R2, #-1\n"
                                 << "\tBRp\t\tSHL_LOOP_" << TempLabelCounter
                                 << "\n"
                                 << "\tSTR\t\tR1, R5, #" << ResOff << "\n";
            break;
          case Instruction::Mul:
            if (SignedMul) {
              FuncInstBufferStream << "\tBRzp\tMUL_LOOP_"
                                   << TempLabelCounter + 1 << "\n"
                                   << "\tNOT\t\tR1, R1\n"
                                   << "\tADD\t\tR1, R1, #1\n"
                                   << "\tNOT\t\tR2, R2\n"
                                   << "\tADD\t\tR2, R2, #1\n";
            }
            FuncInstBufferStream
                << "MUL_LOOP_" << ++TempLabelCounter << "\n"
                << "\tBRz\t\tMUL_END_" << TempLabelCounter << "\n"
                << "\tADD\t\tR3, R3, R1\n"
                << "\tADD\t\tR2, R2, #-1\n"
                << "\tBR\t\tMUL_LOOP_" << TempLabelCounter << "\n"
                << "MUL_END_" << TempLabelCounter << "\n"
                << "\tSTR\t\tR3, R5, #" << ResOff << "\n";
            break;
          case Instruction::UDiv:
            FuncInstBufferStream
                << "UDIV_LOOP_" << ++TempLabelCounter << "\n"
                << "\tBRnz\tUDIV_END_" << TempLabelCounter << "\n"
                << "\tADD\t\tR3, R3, #1\n"
                << "\tADD\t\tR1, R1, R2\n"
                << "\tBR\t\tUDIV_LOOP_" << TempLabelCounter << "\n"
                << "UDIV_END_" << TempLabelCounter << "\n"
                << "\tBRz\t\tUDIV_POST_" << TempLabelCounter << "\n"
                << "\tADD\t\tR3, R3, #-1\n"
                << "UDIV_POST_" << TempLabelCounter << "\n"
                << "\tSTR\t\tR3, R5, #" << ResOff << "\n";
            break;
          case Instruction::URem:
            FuncInstBufferStream
                << "UREM_LOOP_" << ++TempLabelCounter << "\n"
                << "\tBRnz\tUREM_END_" << TempLabelCounter << "\n"
                << "\tADD\t\tR1, R1, R3\n"
                << "\tBR\t\tUREM_LOOP_" << TempLabelCounter << "\n"
                << "UREM_END_" << TempLabelCounter << "\n"
                << "\tBRz\t\tUREM_POST_" << TempLabelCounter << "\n"
                << "\tADD\t\tR1, R1, R2\n"
                << "UREM_POST_" << TempLabelCounter << "\n"
                << "\tSTR\t\tR1, R5, #" << ResOff << "\n";
            break;
          case Instruction::LShr:
            // R2: counter, R1: result, R0: temporary register
            // R3: source mask, R4: destiny mask
            FuncInstBufferStream
                << "LSHR_OUT_LOOP_" << ++TempLabelCounter << "\n"
                << "\tAND\t\tR0, R0, #0\n"
                << "\tAND\t\tR3, R3, #0\n"
                << "\tADD\t\tR3, R3, #2\n"
                << "\tAND\t\tR4, R4, #0\n"
                << "\tADD\t\tR4, R4, #1\n"
                << "LSHR_IN_LOOP_" << TempLabelCounter << "\n"
                << "\tNOT\t\tR4, R4\n"
                << "\tAND\t\tR1, R1, R4\n"
                << "\tNOT\t\tR4, R4\n"
                << "\tAND\t\tR0, R1, R3\n"
                << "\tBRz\t\tLSHR_SKIP_" << TempLabelCounter << "\n"
                << "\tADD\t\tR1, R1, R4\n"
                << "LSHR_SKIP_" << TempLabelCounter << "\n"
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
          FuncInstBufferStream << "\tLEA\t\tR7, " << BBName << "\n";
          if (BranchI->isUnconditional()) {
            BasicBlock *SucBB = BranchI->getSuccessor(0);
            std::string SucBBName = getIndex(SucBB, BBNameMap, BBNameCounter);

            FuncInstBufferStream << "\tBR\t\t" << SucBBName << "\n";
          } else {
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
            FuncInstBufferStream << "\tBRnp\tICMP_END_" << ++TempLabelCounter
                                 << "\n";
            break;
          case CmpInst::ICMP_NE:
            FuncInstBufferStream << "\tBRz\t\tICMP_END_" << ++TempLabelCounter
                                 << "\n";
            break;
          case CmpInst::ICMP_SGT:
            FuncInstBufferStream << "\tBRnz\tICMP_END_" << ++TempLabelCounter
                                 << "\n";
            break;
          case CmpInst::ICMP_SGE:
            FuncInstBufferStream << "\tBRn\t\tICMP_END_" << ++TempLabelCounter
                                 << "\n";
            break;
          case CmpInst::ICMP_SLT:
            FuncInstBufferStream << "\tBRzp\tICMP_END_" << ++TempLabelCounter
                                 << "\n";
            break;
          case CmpInst::ICMP_SLE:
            FuncInstBufferStream << "\tBRp\t\tICMP_END_" << ++TempLabelCounter
                                 << "\n";
            break;
          case CmpInst::ICMP_UGT:
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
          if (Function *Func = CallI->getCalledFunction()) {
            if (Func->getName() == "printStr") {
              if (CallI->arg_size() == 1) {
                Value *Str = CallI->getArgOperand(0);

                if (int StrID = addString(Str, ImmBufferStream, ImmFlag,
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
                return UnsupportInst(I);
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
                return UnsupportInst(I);
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
                return UnsupportInst(I);
              }
            } else if (Func->getName() == "printChar") {
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
          continue;
        } else if (auto *PHIN = dyn_cast<PHINode>(&I)) {
          int ResOff = -getIndex(&I, ValueOffsetMap, ValueOffsetCounter);

          FuncInstBufferStream << "\tNOT\t\tR0, R7\n"
                               << "\tADD\t\tR0, R0, #1\n";
          int ArgSize = PHIN->getNumIncomingValues();
          int EndLableID = TempLabelCounter + ArgSize;
          for (unsigned int i = 0; i < ArgSize; ++i) {

            BasicBlock *SrcBB = PHIN->getIncomingBlock(i);
            std::string SrcBBName = getIndex(SrcBB, BBNameMap, BBNameCounter);

            ++TempLabelCounter;
            if (i < ArgSize - 1) {
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
              FuncInstBufferStream << "\tBR\t\tPHI_NEXT_" << EndLableID << "\n";
            }
            FuncInstBufferStream << "PHI_NEXT_" << TempLabelCounter << "\n";
          }
        } else if (auto *RetI = dyn_cast<ReturnInst>(&I)) {
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
          FuncInstBufferStream << "\tADD\t\tR6, R5, #0\n"
                               << "\tLDR\t\tR5, R6, #0\n"
                               << "\tLDR\t\tR7, R6, #1\n"
                               << "\tLDR\t\tR4, R6, #2\n"
                               << "\tLDR\t\tR3, R6, #3\n"
                               << "\tLDR\t\tR2, R6, #4\n"
                               << "\tLDR\t\tR1, R6, #5\n"
                               << "\tADD\t\tR6, R6, #7\n";
          if (!hasRetVal) {
            FuncInstBufferStream << "\tLDR\t\tR0, R6, #-1\n";
          }
          FuncInstBufferStream << "\tRET\n";
          continue;
        } else if (auto *CastI = dyn_cast<CastInst>(&I)) {
          int ResOff = -getIndex(&I, ValueOffsetMap, ValueOffsetCounter);

          Value *Op = CastI->getOperand(0);
          int OpOff = -getIndex(Op, ValueOffsetMap, ValueOffsetCounter);

          FuncInstBufferStream << "\tLDR\t\tR1, R5, #" << OpOff << "\n"
                               << "\tSTR\t\tR1, R5, #" << ResOff << "\n";
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
                               << "\tBRp\t\tSELECT_END_" << ++TempLabelCounter
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

          FuncInstBufferStream << "SELECT_END_" << TempLabelCounter << "\n"
                               << "\tSTR\t\tR2, R5, #" << ResOff << "\n";
        } else if (auto *SwitchI = dyn_cast<SwitchInst>(&I)) {
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
              if (!isFirstCase) {
                FuncInstBufferStream << "\tADD\t\tR1, R1, #0";
              }
              FuncInstBufferStream << "\tBRz\t\t" << DesBBName << "\n";
            } else {
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
        if (!NoComment) {
          FuncInstBufferStream << ";\tconstant section for " << BBName << "\n";
        }
        FuncInstBufferStream << ImmBufferStream.str() << "\n";
      }
    }
    if (!NoComment) {
      InstBufferStream << ";\tfunction " << FuncName << "\n";
      InstBufferStream << ";\targument count: " << F.arg_size() << "\n";
      InstBufferStream << ";\tlocal variable count: " << ValueOffsetCounter
                       << "\n";
    }
    InstBufferStream << FuncName << "\n";
    InstBufferStream << FuncLabelMap[&F] << "\n";
    if (!NoComment) {
      InstBufferStream << ";\tinit R6, R5, save old registers\n";
    }
    InstBufferStream << "\tADD\t\tR6, R6, #-7\n"
                     << "\tSTR\t\tR0, R6, #6\n"
                     << "\tSTR\t\tR1, R6, #5\n"
                     << "\tSTR\t\tR2, R6, #4\n"
                     << "\tSTR\t\tR3, R6, #3\n"
                     << "\tSTR\t\tR4, R6, #2\n"
                     << "\tSTR\t\tR7, R6, #1\n"
                     << "\tSTR\t\tR5, R6, #0\n"
                     << "\tADD\t\tR5, R6, #0\n";
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
    if (int argSize = F.arg_size()) {
      if (!NoComment) {
        InstBufferStream << ";\tstore arguments\n";
      }
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

  return PreservedAnalyses::none();
}

extern "C" LLVM_ATTRIBUTE_WEAK ::PassPluginLibraryInfo llvmGetPassPluginInfo() {
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