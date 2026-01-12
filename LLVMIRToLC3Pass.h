#ifndef LLVMIRTOLC3PASS_H
#define LLVMIRTOLC3PASS_H

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/PassManager.h"

namespace llvm {

class LLVMIRToLC3Pass : public PassInfoMixin<LLVMIRToLC3Pass> {
public:
  // The run() method is the entry point.
  // For a Module pass, change Function& to Module&.
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);

  // Add this method:
  static bool isRequired() { return true; }
};

} // namespace llvm

#endif // LLVMIRTOLC3PASS_H