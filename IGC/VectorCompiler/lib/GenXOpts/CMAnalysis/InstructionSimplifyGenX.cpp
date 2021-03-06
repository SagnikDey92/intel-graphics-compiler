/*===================== begin_copyright_notice ==================================

Copyright (c) 2017 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


======================= end_copyright_notice ==================================*/
//
// This file defines a routine for simplifying a GenX intrinsic call to a
// constant or one of the operands. This is for cases where not all operands
// are constant; the constant operand cases are handled in ConstantFoldGenX.cpp.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "genx-simplify"

#include "llvmWrapper/Analysis/CallGraph.h"
#include "llvmWrapper/IR/CallSite.h"
#include "llvmWrapper/IR/Instructions.h"

#include "vc/GenXOpts/GenXAnalysis.h"
#include "vc/GenXOpts/GenXOpts.h"
#include "vc/GenXOpts/Utils/CMRegion.h"

#include <llvm/GenXIntrinsics/GenXIntrinsics.h>

#include <llvm/Analysis/InstructionSimplify.h>
#include <llvm/Analysis/PostDominators.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/InitializePasses.h>
#include <llvm/Pass.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/CommandLine.h>
#include "Probe/Assertion.h"
#include "llvmWrapper/Support/TypeSize.h"

using namespace llvm;

static cl::opt<bool>
    GenXEnablePeepholes("genx-peepholes", cl::init(true), cl::Hidden,
                        cl::desc("apply additional peephole optimizations"));

// isWriteWithUndefInput - checks whether provided \p Inst is a write
// intrinsic (currently wrregion, wrpredregion) and it's input value
// (new value) is undef (undef value is written into a vector).
static bool isWriteWithUndefInput(const Instruction &Inst) {
  switch (GenXIntrinsic::getAnyIntrinsicID(&Inst)) {
  default:
    return false;
  case GenXIntrinsic::genx_wrregioni:
  case GenXIntrinsic::genx_wrregionf:
    return isa<UndefValue>(
        Inst.getOperand(GenXIntrinsic::GenXRegion::NewValueOperandNum));
  case GenXIntrinsic::genx_wrpredregion:
    return isa<UndefValue>(Inst.getOperand(WrPredRegionOperand::NewValue));
  }
}

static Value &getWriteOldValueOperand(Instruction &Inst) {
  switch (GenXIntrinsic::getAnyIntrinsicID(&Inst)) {
  default:
    IGC_ASSERT_EXIT_MESSAGE(0, "wrong argument: write region intrinsics are expected");
  case GenXIntrinsic::genx_wrregioni:
  case GenXIntrinsic::genx_wrregionf:
    return *Inst.getOperand(GenXIntrinsic::GenXRegion::OldValueOperandNum);
  case GenXIntrinsic::genx_wrpredregion:
    return *Inst.getOperand(WrPredRegionOperand::OldValue);
  }
}

// processWriteWithUndefInput - removes provided \p Inst, replaces its uses
// with the old value. If this replacement produced new context (write
// intrinsic's input value was  replaced with undef), those writes are put into
// \p ToProcess output iterator.
template <typename OutIter>
void processWriteWithUndefInput(Instruction &Inst, OutIter ToProcess) {
  IGC_ASSERT(isWriteWithUndefInput(Inst) &&
         "wrong argument: write intrinsic with undef input was expected");
  auto *OldVal = &getWriteOldValueOperand(Inst);
  Inst.replaceAllUsesWith(OldVal);
  // As a result of operand promotion we can get new suitable instructions.
  // Using additional copy_if instead of make_filter_range as workaround,
  // because user_iterator returns pointer instead of reference.
  std::vector<User *> UsersToProcess;
  std::copy_if(Inst.user_begin(), Inst.user_end(),
               std::back_inserter(UsersToProcess), [](User *Usr) {
                 return isa<Instruction>(Usr) &&
                        isWriteWithUndefInput(*cast<Instruction>(Usr));
               });
  std::transform(UsersToProcess.begin(), UsersToProcess.end(), ToProcess,
                 [](User *Usr) { return cast<Instruction>(Usr); });
  Inst.eraseFromParent();
}

bool llvm::simplifyWritesWithUndefInput(Function &F) {
  using WorkListT = std::vector<Instruction *>;
  WorkListT WorkList;
  auto WorkListRange =
      make_filter_range(instructions(F), [](const Instruction &Inst) {
        return isWriteWithUndefInput(Inst);
      });
  llvm::transform(WorkListRange, std::back_inserter(WorkList),
                  [](Instruction &Inst) { return &Inst; });
  bool Modified = !WorkList.empty();
  while (!WorkList.empty()) {
    WorkListT CurrentWorkList = std::move(WorkList);
    WorkList = WorkListT{};
    auto WorkListInserter = std::back_inserter(WorkList);
    std::for_each(CurrentWorkList.begin(), CurrentWorkList.end(),
                  [WorkListInserter](Instruction *Inst) {
                    processWriteWithUndefInput(*Inst, WorkListInserter);
                  });
  }
  return Modified;
}

/***********************************************************************
 * SimplifyGenXIntrinsic : given a GenX intrinsic and a set of arguments,
 * see if we can fold the result.
 *
 * ConstantFoldingGenX.cpp handles pure constant folding cases. This code
 * only handles cases where not all operands are constant, but we can do
 * some folding anyway.
 *
 * If this call could not be simplified, returns null.
 */
Value *llvm::SimplifyGenXIntrinsic(unsigned IID, Type *RetTy, Use *ArgBegin,
                                   Use *ArgEnd, const DataLayout &DL) {
  switch (IID) {
    case GenXIntrinsic::genx_rdregioni:
    case GenXIntrinsic::genx_rdregionf:
      // Identity rdregion can be simplified to its "old value" input.
      if (RetTy
          == ArgBegin[GenXIntrinsic::GenXRegion::OldValueOperandNum]->getType()) {
        unsigned NumElements = cast<VectorType>(RetTy)->getNumElements();
        unsigned Width = cast<ConstantInt>(
              ArgBegin[GenXIntrinsic::GenXRegion::RdWidthOperandNum])
            ->getZExtValue();
        auto IndexV = dyn_cast<Constant>(
          ArgBegin[GenXIntrinsic::GenXRegion::RdIndexOperandNum]);
        if (!IndexV)
          return nullptr;
        unsigned Index = 0;
        if (!isa<VectorType>(IndexV->getType()))
          Index = dyn_cast<ConstantInt>(IndexV)->getZExtValue() /
                  (DL.getTypeSizeInBits(RetTy->getScalarType()) / 8);
        else
          return nullptr;
        if ((Index == 0 || Index >= NumElements) &&
            (Width == NumElements || Width == cast<ConstantInt>(ArgBegin[
             GenXIntrinsic::GenXRegion::RdVStrideOperandNum])->getSExtValue()))
          if (NumElements == 1 || cast<ConstantInt>(ArgBegin[
                GenXIntrinsic::GenXRegion::RdStrideOperandNum])->getSExtValue())
            return ArgBegin[GenXIntrinsic::GenXRegion::OldValueOperandNum];
      }
      // rdregion with splatted constant input can be simplified to a constant of
      // the appropriate type, ignoring the possibly variable index.
      if (auto C = dyn_cast<Constant>(
            ArgBegin[GenXIntrinsic::GenXRegion::OldValueOperandNum]))
        if (auto Splat = C->getSplatValue()) {
          if (auto VT = dyn_cast<VectorType>(RetTy))
            return ConstantVector::getSplat(
                IGCLLVM::getElementCount(VT->getNumElements()), Splat);
          return Splat;
        }
      break;
    case GenXIntrinsic::genx_wrregioni:
    case GenXIntrinsic::genx_wrregionf:
      // The wrregion case specifically excludes genx_wrconstregion.
      // Identity wrregion can be simplified to its "new value" input.
      if (RetTy
          == ArgBegin[GenXIntrinsic::GenXRegion::NewValueOperandNum]->getType()) {
        if (auto CMask = dyn_cast<Constant>(ArgBegin[
              GenXIntrinsic::GenXRegion::PredicateOperandNum])) {
          if (CMask->isAllOnesValue()) {
            unsigned NumElements = cast<VectorType>(RetTy)->getNumElements();
            unsigned Width = cast<ConstantInt>(
                  ArgBegin[GenXIntrinsic::GenXRegion::WrWidthOperandNum])
                ->getZExtValue();
            auto IndexV = dyn_cast<Constant>(
              ArgBegin[GenXIntrinsic::GenXRegion::WrIndexOperandNum]);
            if (!IndexV)
              return nullptr;
            unsigned Index = 0;
            if (!isa<VectorType>(IndexV->getType()))
              Index = dyn_cast<ConstantInt>(IndexV)->getZExtValue() /
                      (DL.getTypeSizeInBits(RetTy->getScalarType()) / 8);
            else
              return nullptr;
            if ((Index == 0 || Index >= NumElements) &&
                (Width == NumElements || Width == cast<ConstantInt>(ArgBegin[
                 GenXIntrinsic::GenXRegion::WrVStrideOperandNum])->getSExtValue()))
              if (NumElements == 1 || cast<ConstantInt>(ArgBegin[
                    GenXIntrinsic::GenXRegion::WrStrideOperandNum])->getSExtValue())
                return ArgBegin[GenXIntrinsic::GenXRegion::NewValueOperandNum];
          }
        }
      }
      // Wrregion with constant 0 predicate can be simplified to its "old value"
      // input.
      if (auto CMask = dyn_cast<Constant>(ArgBegin[
            GenXIntrinsic::GenXRegion::PredicateOperandNum]))
        if (CMask->isNullValue())
          return ArgBegin[GenXIntrinsic::GenXRegion::OldValueOperandNum];
      // Wrregion writing a value that has just been read out of the same
      // region in the same vector can be simplified to its "old value" input.
      // This works even if the predicate is not all true.
      if (auto RdR = dyn_cast<CallInst>(ArgBegin[
            GenXIntrinsic::GenXRegion::NewValueOperandNum])) {
        if (auto RdRFunc = RdR->getCalledFunction()) {
          Value *OldVal = ArgBegin[GenXIntrinsic::GenXRegion::OldValueOperandNum];
          if ((GenXIntrinsic::getGenXIntrinsicID(RdRFunc) ==
                   GenXIntrinsic::genx_rdregioni ||
               GenXIntrinsic::getGenXIntrinsicID(RdRFunc) ==
                   GenXIntrinsic::genx_rdregionf) &&
              RdR->getArgOperand(GenXIntrinsic::GenXRegion::OldValueOperandNum)
                == OldVal) {
            // Check the region parameters match between the rdregion and
            // wrregion. There are 4 region parameters: vstride, width, stride,
            // index.
            bool CanSimplify = true;
            for (unsigned i = 0; i != 4; ++i) {
              if (ArgBegin[GenXIntrinsic::GenXRegion::WrVStrideOperandNum + i]
                  != RdR->getArgOperand(
                    GenXIntrinsic::GenXRegion::RdVStrideOperandNum + i)) {
                CanSimplify = false;
                break;
              }
            }
            if (CanSimplify)
              return OldVal;
          }
        }
      }
      break;
  }
  return nullptr;
}

/***********************************************************************
 * SimplifyGenX : given a GenX related instruction, see if we can fold
 * the result.
 *
 * ConstantFoldingGenX.cpp handles pure constant folding cases. This code
 * also handles cases where not all operands are constant.
 *
 * If this instruction could not be simplified, returns null.
 */
Value *llvm::SimplifyGenX(CallInst *I, const DataLayout &DL) {
  Value *V = IGCLLVM::getCalledValue(I);
  Type *Ty = V->getType();
  if (auto *PTy = dyn_cast<PointerType>(Ty))
    Ty = PTy->getElementType();
  auto *FTy = cast<FunctionType>(Ty);
  auto *F = dyn_cast<Function>(V);
  if (!F)
    return nullptr;

  LLVM_DEBUG(dbgs() << "Trying to simplify " << *I << "\n");
  auto GenXID = GenXIntrinsic::getGenXIntrinsicID(F);
  if (Value *Ret = SimplifyGenXIntrinsic(GenXID, FTy->getReturnType(),
                                         I->arg_begin(), I->arg_end(), DL)) {
    LLVM_DEBUG(dbgs() << "Simplified to " << *Ret << "\n");
    return Ret;
  }

  LLVM_DEBUG(dbgs() << "Failed to simplify, trying to constant fold\n");
  Constant *C = ConstantFoldGenX(I, DL);
  if (C)
    LLVM_DEBUG(dbgs() << "Successfully folded to " << *C << "\n");
  else
    LLVM_DEBUG(dbgs() << "Failed to constant fold instruction\n");
  return C;
}

namespace {
class GenXSimplify : public FunctionPass {
public:
  static char ID;

  GenXSimplify() : FunctionPass(ID) {
    initializeGenXSimplifyPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<DominatorTreeWrapperPass>();
    AU.addPreserved<DominatorTreeWrapperPass>();
  }

private:
  std::vector<CallInst *> WorkSet;

  bool processGenXIntrinsics(Function &F);
};
} // namespace

bool GenXSimplify::runOnFunction(Function &F) {
  const DataLayout &DL = F.getParent()->getDataLayout();
  bool Changed = false;
  for (auto &BB : F) {
    for (auto I = BB.begin(); I != BB.end();) {
      Instruction *Inst = &*I++;
      if (auto *CI = dyn_cast<CallInst>(Inst)) {
        if (GenXIntrinsic::isGenXIntrinsic(CI)) {
          if (Value *V = SimplifyGenX(CI, DL)) {
            CI->replaceAllUsesWith(V);
            CI->eraseFromParent();
            Changed = true;
          }
          continue;
        }
      }

      if (Value *V = SimplifyInstruction(Inst, DL)) {
        Inst->replaceAllUsesWith(V);
        Inst->eraseFromParent();
        Changed = true;
      }
    }
  }
  Changed |= processGenXIntrinsics(F);
  Changed |= simplifyWritesWithUndefInput(F);
  return Changed;
}

bool GenXSimplify::processGenXIntrinsics(Function &F) {

  if (!GenXEnablePeepholes) {
    LLVM_DEBUG(dbgs() << "genx-specific peepholes disabled\n");
    return false;
  }

  bool Changed = false;

  for (Instruction &Inst : instructions(F))
    if (GenXIntrinsic::isGenXIntrinsic(&Inst))
      WorkSet.push_back(cast<CallInst>(&Inst));

  const auto &DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  while (!WorkSet.empty()) {
    auto *CI = WorkSet.back();
    WorkSet.pop_back();

    auto GenXID = GenXIntrinsic::getGenXIntrinsicID(CI);
    switch (GenXID) {
    default:
      (void)CI; // do nothing
    }
  }

  return Changed;
}

char GenXSimplify::ID = 0;
INITIALIZE_PASS_BEGIN(GenXSimplify, "genx-simplify",
                      "simplify genx specific instructions", false, false)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_END(GenXSimplify, "genx-simplify",
                    "simplify genx specific instructions", false, false)

FunctionPass *llvm::createGenXSimplifyPass() { return new GenXSimplify; }
