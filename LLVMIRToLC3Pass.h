#ifndef LLVMIRTOLC3PASS_H
#define LLVMIRTOLC3PASS_H

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/PassManager.h"

namespace llvm {

/// Module-level lowering pass that translates LLVM IR into LC-3 assembly.
///
/// The implementation (in `LLVMIRToLC3Pass.cpp`) performs:
/// 1) small IR canonicalization rewrites that simplify code generation, and
/// 2) direct instruction emission into a `.asm` file.
///
/// This pass is written as an out-of-tree plugin pass and is intended to be
/// invoked through the new pass manager pipeline parser.
///
/// Design notes:
/// - The pass is effectively a whole-module *codegen* stage, not an optimizer.
/// - It assumes a simple LC-3 calling convention implemented entirely by this
///   pass (register arguments + explicit prologue/epilogue).
/// - LLVM SSA values are assigned synthetic frame offsets, so lowering can
///   "materialize" any value by loading from `[R5 + offset]`.
/// - Unsupported IR forms intentionally fail fast to avoid emitting assembly
///   that is silently wrong.
class LLVMIRToLC3Pass : public PassInfoMixin<LLVMIRToLC3Pass> {
public:
  /// Entry point of the pass.
  ///
  /// \param M   Input module to translate.
  /// \param MAM Module analysis manager (currently unused by this pass).
  /// \return    Preservation result; the pass currently reports `none()`
  ///            because it mutates IR during canonicalization and emits output.
  ///
  /// Behavior contract:
  /// - On success: writes exactly one `.asm` file named from module source
  /// stem.
  /// - On unsupported instruction / structural mismatch: prints diagnostics and
  ///   aborts generation by returning `PreservedAnalyses::none()`.
  /// - The method currently does not consume analyses from `MAM`, but keeps the
  ///   parameter to match new-pass-manager signatures.
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);

  /// Mark this pass as required so optimization levels that skip optional
  /// passes will still execute it when explicitly requested in the pipeline.
  ///
  /// This is important for plugin-style codegen passes, because users may run
  /// pipelines that otherwise prune non-required passes at low optimization
  /// levels or in custom pass pipelines.
  static bool isRequired() { return true; }
};

} // namespace llvm

#endif // LLVMIRTOLC3PASS_H
