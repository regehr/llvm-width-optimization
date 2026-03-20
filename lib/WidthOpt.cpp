#include "WidthOpt.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/EquivalenceClasses.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/LazyValueInfo.h"
#include "llvm/Analysis/SimplifyQuery.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Local.h"
#include <cassert>
#include <optional>

using namespace llvm;

namespace widthopt {

AnalysisKey WidthComponentAnalysis::Key;
AnalysisKey WidthPlanAnalysis::Key;

namespace {

enum class ExtKind {
  None,
  ZExt,
  SExt,
};

enum class MinMaxKind {
  None,
  SMin,
  SMax,
  UMin,
  UMax,
};

struct PhiShrinkInfo {
  ExtKind Kind = ExtKind::None;
  unsigned NarrowWidth = 0;
  unsigned WideWidth = 0;
  SmallVector<Instruction *, 8> Producers;
};

struct ExtOperandInfo {
  Value *NarrowValue = nullptr;
  Instruction *Producer = nullptr;
  ExtKind Kind = ExtKind::None;
  unsigned NarrowWidth = 0;
  unsigned WideWidth = 0;
};

IntegerType *getIntegerTy(Value *V) {
  return dyn_cast<IntegerType>(V->getType());
}

bool isIntegerValue(Value *V) {
  return getIntegerTy(V) != nullptr;
}

void unionIfRetargetable(EquivalenceClasses<const Value *> &EC, Value *A,
                         Value *B) {
  if (!isIntegerValue(A) || !isIntegerValue(B))
    return;
  // Fixed argument widths are boundaries for the global search space. Keep
  // them outside retargetable components so widening/narrowing decisions can
  // insert casts at the boundary instead of freezing the whole component.
  if (isa<Argument>(A) || isa<Argument>(B))
    return;
  EC.unionSets(A, B);
}

bool hasFixedIntegerSignature(const CallBase &CB) {
  if (CB.getCalledFunction() == nullptr)
    return true;

  if (CB.isInlineAsm())
    return true;

  const Function *Callee = CB.getCalledFunction();
  if (Callee->isIntrinsic()) {
    Intrinsic::ID ID = Callee->getIntrinsicID();
    switch (ID) {
    case Intrinsic::smax:
    case Intrinsic::smin:
    case Intrinsic::umax:
    case Intrinsic::umin:
      return false;
    default:
      return true;
    }
  }

  return true;
}

InstClass classifyInstruction(Instruction &I) {
  // Width planning only makes sense for integer-typed values. Everything else
  // is outside the search space.
  if (!isIntegerValue(&I))
    return InstClass::Ignore;

  if (isa<PHINode>(I) || isa<FreezeInst>(I))
    return InstClass::FreelyWidthPolymorphic;

  if (auto *SI = dyn_cast<SelectInst>(&I)) {
    if (isIntegerValue(SI->getTrueValue()) && isIntegerValue(SI->getFalseValue()))
      return InstClass::FreelyWidthPolymorphic;
    return InstClass::Ignore;
  }

  if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
    switch (BO->getOpcode()) {
    case Instruction::And:
    case Instruction::Or:
    case Instruction::Xor:
      return InstClass::FreelyWidthPolymorphic;
    default:
      return InstClass::HardAnchor;
    }
  }

  if (auto *Cmp = dyn_cast<ICmpInst>(&I)) {
    (void)Cmp;
    return InstClass::ConditionallyRetargetable;
  }

  if (isa<ZExtInst>(I) || isa<SExtInst>(I) || isa<TruncInst>(I))
    return InstClass::ConditionallyRetargetable;

  if (isa<LoadInst>(I) || isa<AllocaInst>(I) || isa<AtomicRMWInst>(I) ||
      isa<AtomicCmpXchgInst>(I))
    return InstClass::HardAnchor;

  if (isa<PtrToIntInst>(I) || isa<IntToPtrInst>(I))
    return InstClass::HardAnchor;

  if (auto *CB = dyn_cast<CallBase>(&I)) {
    if (hasFixedIntegerSignature(*CB))
      return InstClass::HardAnchor;
    return InstClass::ConditionallyRetargetable;
  }

  return InstClass::HardAnchor;
}

void addEqualWidthConstraints(Instruction &I,
                              EquivalenceClasses<const Value *> &EC) {
  // These instructions are width-polymorphic in the sense that we either want
  // to retarget the whole group together or not at all. The union-find step
  // compresses that equal-width region before any expensive reasoning happens.
  if (!isIntegerValue(&I))
    return;

  if (auto *Phi = dyn_cast<PHINode>(&I)) {
    for (Value *Incoming : Phi->incoming_values())
      unionIfRetargetable(EC, &I, Incoming);
    return;
  }

  if (auto *Fr = dyn_cast<FreezeInst>(&I)) {
    unionIfRetargetable(EC, &I, Fr->getOperand(0));
    return;
  }

  if (auto *Sel = dyn_cast<SelectInst>(&I)) {
    unionIfRetargetable(EC, &I, Sel->getTrueValue());
    unionIfRetargetable(EC, &I, Sel->getFalseValue());
    return;
  }

  if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
    unionIfRetargetable(EC, &I, BO->getOperand(0));
    unionIfRetargetable(EC, &I, BO->getOperand(1));
    return;
  }
}

bool isAnchorValue(const Value *V) {
  if (auto *Arg = dyn_cast<Argument>(V))
    return isa<IntegerType>(Arg->getType());
  return false;
}

bool shouldTrackValue(const Value *V) {
  return isa<IntegerType>(V->getType()) &&
         (isa<Instruction>(V) || isa<Argument>(V));
}

unsigned getValueWidth(const Value *V) {
  assert(isa<IntegerType>(V->getType()) && "Expected integer-typed value");
  return cast<IntegerType>(V->getType())->getBitWidth();
}

std::string formatValue(const Value *V) {
  std::string S;
  raw_string_ostream OS(S);
  if (V->hasName())
    OS << "%" << V->getName();
  else
    V->printAsOperand(OS, false);
  return S;
}

void addCandidateWidth(Component &C, unsigned W) {
  if (W == 0)
    return;
  if (llvm::is_contained(C.CandidateWidths, W))
    return;
  C.CandidateWidths.push_back(W);
}

std::optional<ExtOperandInfo> getExtOperandInfo(Value *V) {
  if (auto *Z = dyn_cast<ZExtInst>(V)) {
    return ExtOperandInfo{Z->getOperand(0), Z, ExtKind::ZExt,
                          getValueWidth(Z->getOperand(0)), getValueWidth(Z)};
  }

  if (auto *S = dyn_cast<SExtInst>(V)) {
    return ExtOperandInfo{S->getOperand(0), S, ExtKind::SExt,
                          getValueWidth(S->getOperand(0)), getValueWidth(S)};
  }

  return std::nullopt;
}

bool canRepresentConstant(ConstantInt &C, ExtKind Kind, unsigned NarrowWidth) {
  APInt Narrow = C.getValue().trunc(NarrowWidth);
  switch (Kind) {
  case ExtKind::ZExt:
    return C.getValue() == Narrow.zext(C.getBitWidth());
  case ExtKind::SExt:
    return C.getValue() == Narrow.sext(C.getBitWidth());
  case ExtKind::None:
    return false;
  }
  llvm_unreachable("Unexpected extension kind");
}

Constant *convertConstantToNarrow(ConstantInt &C, unsigned NarrowWidth) {
  return ConstantInt::get(IntegerType::get(C.getContext(), NarrowWidth),
                          C.getValue().trunc(NarrowWidth));
}

bool areEquivalentValues(Value *A, Value *B) {
  if (A == B)
    return true;

  auto EA = getExtOperandInfo(A);
  auto EB = getExtOperandInfo(B);
  if (!EA || !EB)
    return false;

  return EA->Kind == EB->Kind && EA->NarrowWidth == EB->NarrowWidth &&
         EA->WideWidth == EB->WideWidth && EA->NarrowValue == EB->NarrowValue;
}

bool isEqOrNe(ICmpInst::Predicate Pred) {
  return Pred == ICmpInst::ICMP_EQ || Pred == ICmpInst::ICMP_NE;
}

bool isUnsignedICmp(ICmpInst::Predicate Pred) {
  return ICmpInst::isUnsigned(Pred);
}

bool isSignedICmp(ICmpInst::Predicate Pred) {
  return ICmpInst::isSigned(Pred);
}

unsigned computeShrinkWidth(ICmpInst::Predicate Pred, const ExtOperandInfo &LHS,
                            const ExtOperandInfo &RHS) {
  assert(LHS.Kind != ExtKind::None && RHS.Kind != ExtKind::None &&
         "Shrink rules require explicit extension structure");
  if (LHS.WideWidth != RHS.WideWidth)
    return 0;

  // Same-kind extension pairs are the easy cases: equality is preserved by
  // either extension kind, and signed/unsigned order is preserved by the
  // matching signedness-preserving extension.
  if (LHS.Kind == ExtKind::ZExt && RHS.Kind == ExtKind::ZExt) {
    if (isEqOrNe(Pred) || isUnsignedICmp(Pred))
      return std::max(LHS.NarrowWidth, RHS.NarrowWidth);
    return 0;
  }

  if (LHS.Kind == ExtKind::SExt && RHS.Kind == ExtKind::SExt) {
    if (isEqOrNe(Pred) || isSignedICmp(Pred))
      return std::max(LHS.NarrowWidth, RHS.NarrowWidth);
    return 0;
  }

  if (isEqOrNe(Pred))
    return std::max(LHS.NarrowWidth, RHS.NarrowWidth);

  if (!isSignedICmp(Pred))
    return 0;

  // Mixed signed/unsigned extension pairs need one extra bit on the zero-
  // extended side so the narrowed compare can still distinguish all
  // non-negative values from the negative signed range.
  if (LHS.Kind == ExtKind::SExt && RHS.Kind == ExtKind::ZExt)
    return std::max(LHS.NarrowWidth, RHS.NarrowWidth + 1);

  if (LHS.Kind == ExtKind::ZExt && RHS.Kind == ExtKind::SExt)
    return std::max(LHS.NarrowWidth + 1, RHS.NarrowWidth);

  return 0;
}

Value *materializeAtWidth(IRBuilder<> &B, const ExtOperandInfo &Info,
                          unsigned TargetWidth) {
  assert(TargetWidth >= Info.NarrowWidth && "Cannot shrink below source width");
  assert(Info.Kind != ExtKind::None && "Expected explicit extension kind");

  if (TargetWidth == Info.NarrowWidth)
    return Info.NarrowValue;

  Type *TargetTy = IntegerType::get(B.getContext(), TargetWidth);
  if (Info.Kind == ExtKind::ZExt)
    return B.CreateZExt(Info.NarrowValue, TargetTy);
  if (Info.Kind == ExtKind::SExt)
    return B.CreateSExt(Info.NarrowValue, TargetTy);

  llvm_unreachable("Unexpected extension kind");
}

bool tryShrinkICmp(ICmpInst &Cmp) {
  auto LHSInfo = getExtOperandInfo(Cmp.getOperand(0));
  auto RHSInfo = getExtOperandInfo(Cmp.getOperand(1));
  if (!LHSInfo || !RHSInfo)
    return false;

  unsigned TargetWidth =
      computeShrinkWidth(Cmp.getPredicate(), *LHSInfo, *RHSInfo);
  if (TargetWidth == 0 || TargetWidth >= LHSInfo->WideWidth)
    return false;

  IRBuilder<> B(&Cmp);
  Value *NewLHS = materializeAtWidth(B, *LHSInfo, TargetWidth);
  Value *NewRHS = materializeAtWidth(B, *RHSInfo, TargetWidth);
  auto *NewCmp =
      cast<ICmpInst>(B.CreateICmp(Cmp.getPredicate(), NewLHS, NewRHS));

  SmallVector<Instruction *, 2> DeadRoots;
  if (LHSInfo->Producer != RHSInfo->Producer)
    DeadRoots.push_back(LHSInfo->Producer);
  DeadRoots.push_back(RHSInfo->Producer);

  Cmp.replaceAllUsesWith(NewCmp);
  Cmp.eraseFromParent();

  for (Instruction *I : DeadRoots) {
    if (I != nullptr && I->use_empty())
      RecursivelyDeleteTriviallyDeadInstructions(I);
  }

  return true;
}

bool areHighBitsKnownZero(Value *V, unsigned NarrowWidth, const DataLayout &DL,
                          AssumptionCache *AC, DominatorTree *DT,
                          const Instruction *CxtI) {
  unsigned WideWidth = getValueWidth(V);
  if (NarrowWidth >= WideWidth)
    return true;

  SimplifyQuery SQ(DL, DT, AC, CxtI);
  KnownBits KB = computeKnownBits(V, SQ);
  APInt HighBits = APInt::getHighBitsSet(WideWidth, WideWidth - NarrowWidth);
  return (KB.Zero & HighBits) == HighBits;
}

bool tryWidenTruncEqualityICmp(ICmpInst &Cmp, const DataLayout &DL,
                               AssumptionCache *AC, DominatorTree *DT) {
  if (!isEqOrNe(Cmp.getPredicate()))
    return false;

  auto *LHS = dyn_cast<TruncInst>(Cmp.getOperand(0));
  auto *RHS = dyn_cast<TruncInst>(Cmp.getOperand(1));
  if (!LHS || !RHS)
    return false;

  unsigned NarrowWidth = getValueWidth(LHS);
  if (NarrowWidth != getValueWidth(RHS))
    return false;

  Value *WideLHS = LHS->getOperand(0);
  Value *WideRHS = RHS->getOperand(0);
  if (getValueWidth(WideLHS) != getValueWidth(WideRHS))
    return false;

  if (!areHighBitsKnownZero(WideLHS, NarrowWidth, DL, AC, DT, &Cmp) ||
      !areHighBitsKnownZero(WideRHS, NarrowWidth, DL, AC, DT, &Cmp))
    return false;

  IRBuilder<> B(&Cmp);
  auto *NewCmp =
      cast<ICmpInst>(B.CreateICmp(Cmp.getPredicate(), WideLHS, WideRHS));
  Cmp.replaceAllUsesWith(NewCmp);
  Cmp.eraseFromParent();

  if (LHS->use_empty())
    RecursivelyDeleteTriviallyDeadInstructions(LHS);
  if (RHS->use_empty())
    RecursivelyDeleteTriviallyDeadInstructions(RHS);

  return true;
}

bool tryWidenTruncZeroExtendedICmp(ICmpInst &Cmp, const DataLayout &DL,
                                   AssumptionCache *AC, DominatorTree *DT) {
  if (!isEqOrNe(Cmp.getPredicate()) && !isUnsignedICmp(Cmp.getPredicate()))
    return false;

  // This is intentionally asymmetric. We allow one operand to stay wide when
  // its truncated-away high bits are known zero, then zero-extend the other
  // side to meet it. That captures the common "trunc vs narrow" compare
  // patterns without requiring both operands to share the same shape.
  auto tryOneDirection = [&](unsigned TruncIdx) -> bool {
    auto *Tr = dyn_cast<TruncInst>(Cmp.getOperand(TruncIdx));
    if (!Tr)
      return false;

    Value *Wide = Tr->getOperand(0);
    unsigned NarrowWidth = getValueWidth(Tr);
    unsigned WideWidth = getValueWidth(Wide);
    if (NarrowWidth >= WideWidth)
      return false;

    Value *Other = Cmp.getOperand(1 - TruncIdx);
    if (!isIntegerValue(Other) || getValueWidth(Other) != NarrowWidth)
      return false;

    if (!areHighBitsKnownZero(Wide, NarrowWidth, DL, AC, DT, &Cmp))
      return false;

    IRBuilder<> B(&Cmp);
    Value *WideOther = Other;
    if (WideWidth != NarrowWidth) {
      if (auto OtherExt = getExtOperandInfo(Other)) {
        if (OtherExt->Kind == ExtKind::ZExt &&
            OtherExt->WideWidth == NarrowWidth) {
          WideOther = materializeAtWidth(B, *OtherExt, WideWidth);
        } else {
          WideOther = B.CreateZExt(Other, IntegerType::get(Cmp.getContext(),
                                                          WideWidth));
        }
      } else {
        WideOther =
            B.CreateZExt(Other, IntegerType::get(Cmp.getContext(), WideWidth));
      }
    }
    assert(getValueWidth(WideOther) == WideWidth &&
           "Widened compare operand should match source width");

    Value *NewOps[2] = {Cmp.getOperand(0), Cmp.getOperand(1)};
    NewOps[TruncIdx] = Wide;
    NewOps[1 - TruncIdx] = WideOther;
    auto *NewCmp =
        cast<ICmpInst>(B.CreateICmp(Cmp.getPredicate(), NewOps[0], NewOps[1]));
    NewCmp->setDebugLoc(Cmp.getDebugLoc());
    NewCmp->takeName(&Cmp);
    Cmp.replaceAllUsesWith(NewCmp);
    Cmp.eraseFromParent();

    if (Tr->use_empty())
      RecursivelyDeleteTriviallyDeadInstructions(Tr);
    if (auto *OtherI = dyn_cast<Instruction>(Other))
      if (OtherI->use_empty())
        RecursivelyDeleteTriviallyDeadInstructions(OtherI);

    return true;
  };

  return tryOneDirection(0) || tryOneDirection(1);
}

bool isRelationalLess(ICmpInst::Predicate Pred) {
  return Pred == ICmpInst::ICMP_SLT || Pred == ICmpInst::ICMP_SLE ||
         Pred == ICmpInst::ICMP_ULT || Pred == ICmpInst::ICMP_ULE;
}

bool isRelationalGreater(ICmpInst::Predicate Pred) {
  return Pred == ICmpInst::ICMP_SGT || Pred == ICmpInst::ICMP_SGE ||
         Pred == ICmpInst::ICMP_UGT || Pred == ICmpInst::ICMP_UGE;
}

MinMaxKind getMinMaxKind(ICmpInst::Predicate Pred, bool TrueIsLHS) {
  if (isSignedICmp(Pred)) {
    if (isRelationalLess(Pred))
      return TrueIsLHS ? MinMaxKind::SMin : MinMaxKind::SMax;
    if (isRelationalGreater(Pred))
      return TrueIsLHS ? MinMaxKind::SMax : MinMaxKind::SMin;
    return MinMaxKind::None;
  }

  if (isUnsignedICmp(Pred)) {
    if (isRelationalLess(Pred))
      return TrueIsLHS ? MinMaxKind::UMin : MinMaxKind::UMax;
    if (isRelationalGreater(Pred))
      return TrueIsLHS ? MinMaxKind::UMax : MinMaxKind::UMin;
    return MinMaxKind::None;
  }

  return MinMaxKind::None;
}

Intrinsic::ID getIntrinsicForMinMaxKind(MinMaxKind K) {
  switch (K) {
  case MinMaxKind::SMin:
    return Intrinsic::smin;
  case MinMaxKind::SMax:
    return Intrinsic::smax;
  case MinMaxKind::UMin:
    return Intrinsic::umin;
  case MinMaxKind::UMax:
    return Intrinsic::umax;
  case MinMaxKind::None:
    break;
  }
  llvm_unreachable("Unexpected min/max kind");
}

bool tryConvertSelectToMinMax(SelectInst &Sel) {
  auto *Ty = dyn_cast<IntegerType>(Sel.getType());
  if (!Ty)
    return false;

  auto *Cmp = dyn_cast<ICmpInst>(Sel.getCondition());
  if (!Cmp)
    return false;

  Value *LHS = Cmp->getOperand(0);
  Value *RHS = Cmp->getOperand(1);
  Value *TV = Sel.getTrueValue();
  Value *FV = Sel.getFalseValue();

  if (TV->getType() != Sel.getType() || FV->getType() != Sel.getType())
    return false;
  if (LHS->getType() != Sel.getType() || RHS->getType() != Sel.getType())
    return false;

  bool TrueIsLHS = false;
  bool Matched = false;
  if (areEquivalentValues(TV, LHS) && areEquivalentValues(FV, RHS)) {
    TrueIsLHS = true;
    Matched = true;
  } else if (areEquivalentValues(TV, RHS) && areEquivalentValues(FV, LHS)) {
    TrueIsLHS = false;
    Matched = true;
  }

  if (!Matched)
    return false;

  MinMaxKind K = getMinMaxKind(Cmp->getPredicate(), TrueIsLHS);
  if (K == MinMaxKind::None)
    return false;

  IRBuilder<> B(&Sel);
  Value *MinMax =
      B.CreateBinaryIntrinsic(getIntrinsicForMinMaxKind(K), FV, TV);
  Sel.replaceAllUsesWith(MinMax);
  Sel.eraseFromParent();

  if (Cmp->use_empty())
    RecursivelyDeleteTriviallyDeadInstructions(Cmp);

  return true;
}

bool tryShrinkPhiOfExts(PHINode &Phi) {
  auto *WideTy = dyn_cast<IntegerType>(Phi.getType());
  if (!WideTy)
    return false;

  PhiShrinkInfo Info;
  bool SawExt = false;

  SmallVector<Value *, 8> NarrowIncomingValues;
  SmallVector<BasicBlock *, 8> IncomingBlocks;
  NarrowIncomingValues.reserve(Phi.getNumIncomingValues());
  IncomingBlocks.reserve(Phi.getNumIncomingValues());

  for (unsigned I = 0, E = Phi.getNumIncomingValues(); I != E; ++I) {
    Value *Incoming = Phi.getIncomingValue(I);
    IncomingBlocks.push_back(Phi.getIncomingBlock(I));

    if (auto Ext = getExtOperandInfo(Incoming)) {
      if (Ext->WideWidth != WideTy->getBitWidth())
        return false;

      if (!SawExt) {
        SawExt = true;
        Info.Kind = Ext->Kind;
        Info.NarrowWidth = Ext->NarrowWidth;
        Info.WideWidth = Ext->WideWidth;
      } else if (Info.Kind != Ext->Kind || Info.NarrowWidth != Ext->NarrowWidth ||
                 Info.WideWidth != Ext->WideWidth) {
        return false;
      }

      NarrowIncomingValues.push_back(Ext->NarrowValue);
      Info.Producers.push_back(Ext->Producer);
      continue;
    }

    auto *CI = dyn_cast<ConstantInt>(Incoming);
    if (!CI)
      return false;

    if (!SawExt)
      return false;

    if (!canRepresentConstant(*CI, Info.Kind, Info.NarrowWidth))
      return false;

    NarrowIncomingValues.push_back(convertConstantToNarrow(*CI, Info.NarrowWidth));
  }

  if (!SawExt)
    return false;

  auto *NarrowTy = IntegerType::get(Phi.getContext(), Info.NarrowWidth);
  auto *NarrowPhi = PHINode::Create(NarrowTy, Phi.getNumIncomingValues(),
                                    Phi.getName() + ".narrow",
                                    Phi.getIterator());
  for (unsigned I = 0, E = NarrowIncomingValues.size(); I != E; ++I)
    NarrowPhi->addIncoming(NarrowIncomingValues[I], IncomingBlocks[I]);

  Instruction *InsertPt = &*Phi.getParent()->getFirstInsertionPt();
  IRBuilder<> B(InsertPt);
  Instruction *Wide = nullptr;
  if (Info.Kind == ExtKind::ZExt)
    Wide = cast<Instruction>(B.CreateZExt(NarrowPhi, WideTy, Phi.getName()));
  else
    Wide = cast<Instruction>(B.CreateSExt(NarrowPhi, WideTy, Phi.getName()));

  Phi.replaceAllUsesWith(Wide);
  Phi.eraseFromParent();

  for (Instruction *Producer : Info.Producers)
    if (Producer != nullptr && Producer->use_empty())
      RecursivelyDeleteTriviallyDeadInstructions(Producer);

  return true;
}

bool tryShrinkSelectOfExts(SelectInst &Sel) {
  auto *WideTy = dyn_cast<IntegerType>(Sel.getType());
  if (!WideTy)
    return false;

  Value *TV = Sel.getTrueValue();
  Value *FV = Sel.getFalseValue();

  auto TrueExt = getExtOperandInfo(TV);
  auto FalseExt = getExtOperandInfo(FV);
  auto *TrueC = dyn_cast<ConstantInt>(TV);
  auto *FalseC = dyn_cast<ConstantInt>(FV);

  if (!TrueExt && !FalseExt)
    return false;
  if ((TrueExt == std::nullopt && !TrueC) || (FalseExt == std::nullopt && !FalseC))
    return false;

  PhiShrinkInfo Info;
  if (TrueExt) {
    Info.Kind = TrueExt->Kind;
    Info.NarrowWidth = TrueExt->NarrowWidth;
    Info.WideWidth = TrueExt->WideWidth;
  } else {
    Info.Kind = FalseExt->Kind;
    Info.NarrowWidth = FalseExt->NarrowWidth;
    Info.WideWidth = FalseExt->WideWidth;
  }

  if (Info.WideWidth != WideTy->getBitWidth())
    return false;

  auto validateExt = [&](const std::optional<ExtOperandInfo> &Ext) {
    return Ext && Ext->Kind == Info.Kind && Ext->NarrowWidth == Info.NarrowWidth &&
           Ext->WideWidth == Info.WideWidth;
  };

  if (TrueExt && !validateExt(TrueExt))
    return false;
  if (FalseExt && !validateExt(FalseExt))
    return false;

  if (TrueC && !canRepresentConstant(*TrueC, Info.Kind, Info.NarrowWidth))
    return false;
  if (FalseC && !canRepresentConstant(*FalseC, Info.Kind, Info.NarrowWidth))
    return false;

  Value *NarrowTV = TrueExt ? TrueExt->NarrowValue
                            : convertConstantToNarrow(*TrueC, Info.NarrowWidth);
  Value *NarrowFV = FalseExt ? FalseExt->NarrowValue
                             : convertConstantToNarrow(*FalseC, Info.NarrowWidth);

  IRBuilder<> B(&Sel);
  auto *NarrowSel =
      cast<SelectInst>(B.CreateSelect(Sel.getCondition(), NarrowTV, NarrowFV,
                                      Sel.getName() + ".narrow"));
  Instruction *Wide = nullptr;
  if (Info.Kind == ExtKind::ZExt)
    Wide = cast<Instruction>(B.CreateZExt(NarrowSel, WideTy, Sel.getName()));
  else
    Wide = cast<Instruction>(B.CreateSExt(NarrowSel, WideTy, Sel.getName()));

  Sel.replaceAllUsesWith(Wide);
  Sel.eraseFromParent();

  if (TrueExt && TrueExt->Producer->use_empty())
    RecursivelyDeleteTriviallyDeadInstructions(TrueExt->Producer);
  if (FalseExt && FalseExt->Producer->use_empty())
    RecursivelyDeleteTriviallyDeadInstructions(FalseExt->Producer);

  return true;
}

bool tryConvertSExtToNonNegZExt(SExtInst &Ext, LazyValueInfo &LVI) {
  const Use &Base = Ext.getOperandUse(0);
  if (!LVI.getConstantRangeAtUse(Base, /*UndefAllowed=*/false).isAllNonNegative())
    return false;

  // Once the operand is known non-negative at this use, sign extension and
  // zero extension agree. Mark the replacement non-negative as well so later
  // folds can continue to exploit that fact.
  auto *ZExt = CastInst::CreateZExtOrBitCast(Base, Ext.getType(), "",
                                             Ext.getIterator());
  ZExt->takeName(&Ext);
  ZExt->setDebugLoc(Ext.getDebugLoc());
  ZExt->setNonNeg();
  Ext.replaceAllUsesWith(ZExt);
  Ext.eraseFromParent();
  return true;
}

bool tryFoldAndOfSExtToZExt(BinaryOperator &And) {
  ConstantInt *Mask = dyn_cast<ConstantInt>(And.getOperand(0));
  SExtInst *Ext = dyn_cast<SExtInst>(And.getOperand(1));
  if (!Mask || !Ext) {
    Mask = dyn_cast<ConstantInt>(And.getOperand(1));
    Ext = dyn_cast<SExtInst>(And.getOperand(0));
  }
  if (!Mask || !Ext)
    return false;

  // Leave shared sign-extensions to the whole-value conversion path so we
  // preserve a single widened value across all compatible uses.
  if (!Ext->hasOneUse())
    return false;

  unsigned SrcWidth = getValueWidth(Ext->getOperand(0));
  unsigned WideWidth = getValueWidth(Ext);
  assert(Mask->getBitWidth() == WideWidth &&
         "And mask should match operand width");

  APInt DemandedMask = APInt::getLowBitsSet(WideWidth, SrcWidth);
  if ((Mask->getValue() & ~DemandedMask) != 0)
    return false;

  IRBuilder<> B(&And);
  auto *ZExt = CastInst::CreateZExtOrBitCast(Ext->getOperand(0), Ext->getType(),
                                             "", Ext->getIterator());
  ZExt->setDebugLoc(Ext->getDebugLoc());
  ZExt->takeName(Ext);
  ZExt->setNonNeg();
  And.replaceUsesOfWith(Ext, ZExt);
  if (Ext->use_empty())
    Ext->eraseFromParent();
  return true;
}

bool sextUseAllowsZExt(User &U, SExtInst &Ext) {
  if (auto *BO = dyn_cast<BinaryOperator>(&U)) {
    switch (BO->getOpcode()) {
    case Instruction::And:
      if (auto *Mask = dyn_cast<ConstantInt>(BO->getOperand(0) == &Ext
                                                 ? BO->getOperand(1)
                                                 : BO->getOperand(0))) {
        unsigned SrcWidth = getValueWidth(Ext.getOperand(0));
        unsigned WideWidth = getValueWidth(&Ext);
        APInt DemandedMask = APInt::getLowBitsSet(WideWidth, SrcWidth);
        return (Mask->getValue() & ~DemandedMask) == 0;
      }
      return false;
    case Instruction::LShr:
      return BO->getOperand(0) == &Ext;
    default:
      return false;
    }
  }

  return false;
}

bool tryConvertWholeSExtToZExt(SExtInst &Ext) {
  if (Ext.use_empty())
    return false;

  // This is the shared-value variant of the masked-use fold above. Only weaken
  // the defining sext when every use is compatible with zero-extension
  // semantics; otherwise keep the single shared sext.
  for (User *U : Ext.users())
    if (!sextUseAllowsZExt(*U, Ext))
      return false;

  auto *ZExt = CastInst::CreateZExtOrBitCast(Ext.getOperand(0), Ext.getType(),
                                             "", Ext.getIterator());
  ZExt->setDebugLoc(Ext.getDebugLoc());
  ZExt->takeName(&Ext);
  ZExt->setNonNeg();
  Ext.replaceAllUsesWith(ZExt);
  Ext.eraseFromParent();
  return true;
}

unsigned getUnsignedRangeWidth(const Use &OperandUse, LazyValueInfo &LVI) {
  ConstantRange CR = LVI.getConstantRangeAtUse(OperandUse, /*UndefAllowed=*/false);
  if (CR.isFullSet())
    return 0;

  APInt UMax = CR.getUnsignedMax();
  unsigned Bits = UMax.getActiveBits();
  return std::max(1u, Bits);
}

bool tryNarrowUDivWithRange(BinaryOperator &BO, LazyValueInfo &LVI) {
  if (BO.getOpcode() != Instruction::UDiv)
    return false;

  unsigned OrigWidth = getValueWidth(&BO);
  unsigned LHSWidth = getUnsignedRangeWidth(BO.getOperandUse(0), LVI);
  unsigned RHSWidth = getUnsignedRangeWidth(BO.getOperandUse(1), LVI);
  if (LHSWidth == 0 || RHSWidth == 0)
    return false;

  unsigned TargetWidth = std::max(LHSWidth, RHSWidth);
  if (TargetWidth >= OrigWidth)
    return false;

  // For unsigned division, proving both operands fit in a smaller width is
  // enough to run the division there and zero-extend the quotient back.
  IRBuilder<> B(&BO);
  auto *TargetTy = IntegerType::get(BO.getContext(), TargetWidth);
  Value *NarrowLHS = B.CreateTrunc(BO.getOperand(0), TargetTy);
  Value *NarrowRHS = B.CreateTrunc(BO.getOperand(1), TargetTy);
  auto *NarrowDiv = cast<Instruction>(
      B.CreateUDiv(NarrowLHS, NarrowRHS, BO.getName() + ".narrow"));
  NarrowDiv->setDebugLoc(BO.getDebugLoc());
  auto *WideDiv = cast<Instruction>(B.CreateZExt(NarrowDiv, BO.getType(), BO.getName()));
  WideDiv->setDebugLoc(BO.getDebugLoc());

  BO.replaceAllUsesWith(WideDiv);
  BO.eraseFromParent();
  return true;
}

Value *findExistingZExtToWidth(Value *Src, unsigned TargetWidth) {
  for (User *U : Src->users()) {
    auto *Z = dyn_cast<ZExtInst>(U);
    if (!Z)
      continue;
    if (getValueWidth(Z) == TargetWidth)
      return Z;
  }
  return nullptr;
}

bool canWidenAddOperandWithoutOverflow(const ExtOperandInfo &ExtInfo,
                                       ConstantInt &C) {
  if (ExtInfo.Kind != ExtKind::ZExt)
    return false;

  // Require the narrow operand's maximal zero-extended value plus the constant
  // to stay within the intermediate width. That lets us bypass the narrower
  // add without changing modulo arithmetic.
  unsigned MidWidth = ExtInfo.WideWidth;
  APInt Max = APInt::getLowBitsSet(MidWidth, ExtInfo.NarrowWidth).zext(MidWidth);
  APInt Sum = Max + C.getValue().zextOrTrunc(MidWidth);
  return !Sum.ult(Max);
}

bool tryWidenAddThroughZExt(BinaryOperator &BO) {
  if (BO.getOpcode() != Instruction::Add || !BO.hasOneUse())
    return false;

  auto *WideZ = dyn_cast<ZExtInst>(*BO.user_begin());
  if (!WideZ)
    return false;

  unsigned WideWidth = getValueWidth(WideZ);
  unsigned MidWidth = getValueWidth(&BO);
  if (WideWidth <= MidWidth)
    return false;
  if (BO.hasNoUnsignedWrap() || BO.hasNoSignedWrap())
    return false;

  // This is a narrow local widening pattern: if one operand already comes from
  // a zext and the result is immediately zext'ed again, try to reuse an
  // existing wider path and do the add there instead.
  auto trySide = [&](unsigned ExtIdx, unsigned OtherIdx) -> bool {
    auto ExtInfo = getExtOperandInfo(BO.getOperand(ExtIdx));
    auto *C = dyn_cast<ConstantInt>(BO.getOperand(OtherIdx));
    if (!ExtInfo || !C)
      return false;
    if (!canWidenAddOperandWithoutOverflow(*ExtInfo, *C))
      return false;

    Value *WideBase =
        findExistingZExtToWidth(ExtInfo->NarrowValue, WideWidth);
    IRBuilder<> B(WideZ);
    if (!WideBase)
      WideBase = B.CreateZExt(ExtInfo->NarrowValue,
                              IntegerType::get(BO.getContext(), WideWidth));

    Value *WideC =
        ConstantInt::get(IntegerType::get(BO.getContext(), WideWidth),
                         C->getValue().zextOrTrunc(WideWidth));
    auto *WideAdd = cast<Instruction>(
        B.CreateAdd(WideBase, WideC, BO.getName() + ".wide"));
    WideAdd->setDebugLoc(BO.getDebugLoc());
    WideZ->replaceAllUsesWith(WideAdd);
    WideZ->eraseFromParent();

    if (BO.use_empty())
      RecursivelyDeleteTriviallyDeadInstructions(&BO);
    return true;
  };

  return trySide(0, 1) || trySide(1, 0);
}

bool tryFoldZExtOfTruncToMask(ZExtInst &Ext) {
  auto *Tr = dyn_cast<TruncInst>(Ext.getOperand(0));
  if (!Tr)
    return false;

  Value *Src = Tr->getOperand(0);
  assert(isIntegerValue(Src) && "Expected integer source for trunc");
  unsigned SrcWidth = getValueWidth(Src);
  unsigned NarrowWidth = getValueWidth(Tr);
  unsigned WideWidth = getValueWidth(&Ext);
  if (NarrowWidth >= WideWidth)
    return false;

  IRBuilder<> B(&Ext);

  // Materialize the mask in the most convenient width we can without
  // reintroducing the original narrow type. When the original source is at
  // least as wide as the zext result, work directly at the result width.
  // Otherwise keep the source width, mask there, and extend once at the end.
  Value *Masked = nullptr;
  if (SrcWidth >= WideWidth) {
    Value *Base = Src;
    if (SrcWidth != WideWidth)
      Base = B.CreateTrunc(Src, IntegerType::get(Ext.getContext(), WideWidth));
    APInt Mask = APInt::getLowBitsSet(WideWidth, NarrowWidth);
    Masked = B.CreateAnd(Base, ConstantInt::get(Base->getType(), Mask));
  } else {
    APInt Mask = APInt::getLowBitsSet(SrcWidth, NarrowWidth);
    Value *Narrowed = B.CreateAnd(Src, ConstantInt::get(Src->getType(), Mask));
    Masked = B.CreateZExt(Narrowed, Ext.getType());
  }

  auto *NewI = cast<Instruction>(Masked);
  NewI->setDebugLoc(Ext.getDebugLoc());
  NewI->takeName(&Ext);
  Ext.replaceAllUsesWith(NewI);
  Ext.eraseFromParent();

  if (Tr->use_empty())
    RecursivelyDeleteTriviallyDeadInstructions(Tr);

  return true;
}

bool tryShrinkTruncOfAdd(TruncInst &Tr) {
  auto *BO = dyn_cast<BinaryOperator>(Tr.getOperand(0));
  if (!BO || BO->getOpcode() != Instruction::Add || !BO->hasOneUse())
    return false;

  unsigned TargetWidth = getValueWidth(&Tr);
  unsigned SourceWidth = getValueWidth(BO);
  if (TargetWidth >= SourceWidth)
    return false;

  IRBuilder<> B(&Tr);
  auto *TargetTy = IntegerType::get(Tr.getContext(), TargetWidth);
  auto materializeOperand = [&](Value *V) -> Value * {
    if (!isIntegerValue(V))
      return nullptr;
    unsigned W = getValueWidth(V);
    if (W == TargetWidth)
      return V;
    if (auto Ext = getExtOperandInfo(V))
      if (Ext->NarrowWidth == TargetWidth)
        return Ext->NarrowValue;
    if (auto *C = dyn_cast<ConstantInt>(V))
      return ConstantInt::get(TargetTy, C->getValue().trunc(TargetWidth));
    if (W > TargetWidth)
      return B.CreateTrunc(V, TargetTy);
    return B.CreateZExt(V, TargetTy);
  };

  Value *LHS = materializeOperand(BO->getOperand(0));
  Value *RHS = materializeOperand(BO->getOperand(1));
  if (!LHS || !RHS)
    return false;

  auto *NewAdd = cast<Instruction>(B.CreateAdd(LHS, RHS, Tr.getName()));
  NewAdd->setDebugLoc(Tr.getDebugLoc());
  Tr.replaceAllUsesWith(NewAdd);
  Tr.eraseFromParent();

  if (BO->use_empty())
    RecursivelyDeleteTriviallyDeadInstructions(BO);

  return true;
}

Value *materializeTruncRootedValueAtWidth(Value *V, unsigned TargetWidth,
                                          Instruction *InsertBefore,
                                          DenseMap<Value *, Value *> *Cache =
                                              nullptr) {
  // This helper is intentionally tiny. It rebuilds only the narrow patterns we
  // currently know how to prove under a final truncation, and otherwise lets
  // the caller give up instead of speculating about general arithmetic.
  if (!isIntegerValue(V))
    return nullptr;

  if (Cache)
    if (Value *Cached = Cache->lookup(V))
      return Cached;

  unsigned Width = getValueWidth(V);
  auto *TargetTy = IntegerType::get(V->getContext(), TargetWidth);
  if (Width == TargetWidth) {
    if (Cache)
      (*Cache)[V] = V;
    return V;
  }

  if (auto Ext = getExtOperandInfo(V)) {
    if (TargetWidth < Ext->NarrowWidth || TargetWidth > Ext->WideWidth)
      return nullptr;
    IRBuilder<> B(InsertBefore);
    Value *Result = materializeAtWidth(B, *Ext, TargetWidth);
    if (Cache && Result)
      (*Cache)[V] = Result;
    return Result;
  }

  if (auto *C = dyn_cast<ConstantInt>(V)) {
    Value *Result = ConstantInt::get(TargetTy, C->getValue().trunc(TargetWidth));
    if (Cache)
      (*Cache)[V] = Result;
    return Result;
  }

  if (auto *BO = dyn_cast<BinaryOperator>(V)) {
    if (BO->getOpcode() != Instruction::Sub)
      return nullptr;
    auto *Zero = dyn_cast<ConstantInt>(BO->getOperand(0));
    if (!Zero || !Zero->isZero())
      return nullptr;
    Value *NarrowRHS =
        materializeTruncRootedValueAtWidth(BO->getOperand(1), TargetWidth,
                                           InsertBefore, Cache);
    if (!NarrowRHS)
      return nullptr;
    IRBuilder<> B(InsertBefore);
    Value *Result = B.CreateSub(ConstantInt::get(TargetTy, 0), NarrowRHS);
    if (Cache && Result)
      (*Cache)[V] = Result;
    return Result;
  }

  if (Width > TargetWidth) {
    IRBuilder<> B(InsertBefore);
    Value *Result = B.CreateTrunc(V, TargetTy);
    if (Cache && Result)
      (*Cache)[V] = Result;
    return Result;
  }

  return nullptr;
}

bool tryShrinkTruncOfSelect(TruncInst &Tr) {
  auto *Sel = dyn_cast<SelectInst>(Tr.getOperand(0));
  if (!Sel || !Sel->hasOneUse())
    return false;

  unsigned TargetWidth = getValueWidth(&Tr);
  unsigned SourceWidth = getValueWidth(Sel);
  if (TargetWidth >= SourceWidth)
    return false;

  // Materialize each arm at the truncated width first, then rebuild the select
  // directly in that type. The small cache keeps shared arm structure shared
  // after rewriting.
  DenseMap<Value *, Value *> Cache;
  Value *NarrowTV =
      materializeTruncRootedValueAtWidth(Sel->getTrueValue(), TargetWidth, &Tr,
                                         &Cache);
  Value *NarrowFV = materializeTruncRootedValueAtWidth(Sel->getFalseValue(),
                                                       TargetWidth, &Tr, &Cache);
  if (!NarrowTV || !NarrowFV)
    return false;

  IRBuilder<> B(&Tr);
  auto *NarrowSel = cast<SelectInst>(
      B.CreateSelect(Sel->getCondition(), NarrowTV, NarrowFV, Tr.getName()));
  NarrowSel->setDebugLoc(Tr.getDebugLoc());
  Tr.replaceAllUsesWith(NarrowSel);
  Tr.eraseFromParent();

  if (Sel->use_empty())
    RecursivelyDeleteTriviallyDeadInstructions(Sel);

  return true;
}

bool tryShrinkTruncOfShiftRecurrence(TruncInst &Tr) {
  auto *Shl = dyn_cast<BinaryOperator>(Tr.getOperand(0));
  if (!Shl || Shl->getOpcode() != Instruction::Shl)
    return false;

  auto *Phi = dyn_cast<PHINode>(Shl->getOperand(0));
  auto *AmtC = dyn_cast<ConstantInt>(Shl->getOperand(1));
  if (!Phi || !AmtC || Phi->getParent() != Shl->getParent())
    return false;
  if (Phi->getNumIncomingValues() != 2)
    return false;

  BasicBlock *LoopBB = Phi->getParent();
  int BackedgeIdx = -1;
  int InitIdx = -1;
  for (unsigned I = 0; I != 2; ++I) {
    if (Phi->getIncomingBlock(I) == LoopBB)
      BackedgeIdx = I;
    else
      InitIdx = I;
  }
  if (BackedgeIdx < 0 || InitIdx < 0)
    return false;
  if (Phi->getIncomingValue(BackedgeIdx) != Shl)
    return false;

  // Keep this first loop rewrite narrow and explicit: one self-recurrence with
  // a constant shift amount, rooted at a final truncation.
  if (!Phi->hasOneUse())
    return false;

  unsigned TargetWidth = getValueWidth(&Tr);
  unsigned SourceWidth = getValueWidth(Shl);
  if (TargetWidth >= SourceWidth)
    return false;

  BasicBlock *InitBB = Phi->getIncomingBlock(InitIdx);
  Value *Init = Phi->getIncomingValue(InitIdx);
  Value *NarrowInit =
      materializeTruncRootedValueAtWidth(Init, TargetWidth,
                                         InitBB->getTerminator());
  if (!NarrowInit)
    return false;

  auto *TargetTy = IntegerType::get(Tr.getContext(), TargetWidth);
  Constant *NarrowAmt = ConstantInt::get(TargetTy, AmtC->getValue().trunc(TargetWidth));

  auto *NarrowPhi = PHINode::Create(TargetTy, 2, Phi->getName() + ".narrow",
                                    Phi->getIterator());
  NarrowPhi->setDebugLoc(Phi->getDebugLoc());
  NarrowPhi->addIncoming(NarrowInit, InitBB);

  IRBuilder<> B(Shl);
  auto *NarrowShl =
      cast<Instruction>(B.CreateShl(NarrowPhi, NarrowAmt, Shl->getName() + ".narrow"));
  NarrowShl->setDebugLoc(Shl->getDebugLoc());
  NarrowPhi->addIncoming(NarrowShl, LoopBB);

  Tr.replaceAllUsesWith(NarrowShl);
  Tr.eraseFromParent();
  return true;
}

bool tryPushFreezeThroughExt(FreezeInst &FI) {
  // Canonicalize freeze(ext x) into ext(freeze x) for simple integer casts.
  // This follows the safe direction used by InstCombine: the cast only
  // propagates poison from its operand, so freezing the narrow operand is
  // sufficient to stop poison without inventing a wider arbitrary value.
  auto *Cast = dyn_cast<CastInst>(FI.getOperand(0));
  if (!Cast || !Cast->hasOneUse())
    return false;

  if (!isa<ZExtInst>(Cast) && !isa<SExtInst>(Cast) && !isa<TruncInst>(Cast))
    return false;

  Value *Src = Cast->getOperand(0);
  assert(isIntegerValue(Src) && isIntegerValue(&FI) &&
         "Freeze-through-cast expects integer values");

  IRBuilder<> B(Cast);
  auto *FrozenSrc = cast<FreezeInst>(
      B.CreateFreeze(Src, Src->hasName() ? Src->getName() + ".fr" : ""));

  Instruction *NewCast = CastInst::Create(Cast->getOpcode(), FrozenSrc,
                                          FI.getType(), "",
                                          FI.getIterator());
  NewCast->setDebugLoc(FI.getDebugLoc());
  NewCast->takeName(&FI);
  FI.replaceAllUsesWith(NewCast);
  FI.eraseFromParent();

  if (Cast->use_empty())
    RecursivelyDeleteTriviallyDeadInstructions(Cast);

  return true;
}

void inferCandidatesFromInstruction(Instruction &I, AnalysisResult &R) {
  // Candidate widths are intentionally cheap and local. This is not a legality
  // proof; it is only a way to seed the planner with widths that are already
  // suggested by ext/trunc structure in the current IR.
  if (auto *Ext = dyn_cast<ZExtInst>(&I)) {
    unsigned SrcW = getValueWidth(Ext->getOperand(0));
    unsigned DstW = getValueWidth(Ext);
    if (SrcW == 1 || DstW == 1)
      return;
    unsigned DstID = R.ValueToComponent.lookup(Ext);
    unsigned SrcID = R.ValueToComponent.lookup(Ext->getOperand(0));
    addCandidateWidth(R.Components[DstID], SrcW);
    addCandidateWidth(R.Components[SrcID], DstW);
    return;
  }

  if (auto *Ext = dyn_cast<SExtInst>(&I)) {
    unsigned SrcW = getValueWidth(Ext->getOperand(0));
    unsigned DstW = getValueWidth(Ext);
    if (SrcW == 1 || DstW == 1)
      return;
    unsigned DstID = R.ValueToComponent.lookup(Ext);
    unsigned SrcID = R.ValueToComponent.lookup(Ext->getOperand(0));
    addCandidateWidth(R.Components[DstID], SrcW);
    addCandidateWidth(R.Components[SrcID], DstW);
    return;
  }

  if (auto *Tr = dyn_cast<TruncInst>(&I)) {
    unsigned SrcW = getValueWidth(Tr->getOperand(0));
    unsigned DstW = getValueWidth(Tr);
    if (SrcW == 1 || DstW == 1)
      return;
    unsigned DstID = R.ValueToComponent.lookup(Tr);
    unsigned SrcID = R.ValueToComponent.lookup(Tr->getOperand(0));
    addCandidateWidth(R.Components[DstID], SrcW);
    addCandidateWidth(R.Components[SrcID], DstW);
    return;
  }

  if (auto *Phi = dyn_cast<PHINode>(&I)) {
    unsigned ThisID = R.ValueToComponent.lookup(Phi);
    for (Value *Incoming : Phi->incoming_values()) {
      auto Ext = getExtOperandInfo(Incoming);
      if (!Ext)
        continue;
      if (Ext->WideWidth != getValueWidth(Phi))
        continue;
      addCandidateWidth(R.Components[ThisID], Ext->NarrowWidth);
    }
    return;
  }

  if (auto *Sel = dyn_cast<SelectInst>(&I)) {
    unsigned ThisID = R.ValueToComponent.lookup(Sel);
    for (Value *Arm : {Sel->getTrueValue(), Sel->getFalseValue()}) {
      auto Ext = getExtOperandInfo(Arm);
      if (!Ext)
        continue;
      if (Ext->WideWidth != getValueWidth(Sel))
        continue;
      addCandidateWidth(R.Components[ThisID], Ext->NarrowWidth);
    }
    return;
  }
}

Constant *extendConstant(ConstantInt &C, ExtKind Kind, unsigned TargetWidth) {
  APInt V = C.getValue();
  APInt NewV =
      Kind == ExtKind::ZExt ? V.zext(TargetWidth) : V.sext(TargetWidth);
  return ConstantInt::get(IntegerType::get(C.getContext(), TargetWidth), NewV);
}

Value *materializeValueAtWidth(Value *V, ExtKind Kind, unsigned TargetWidth,
                               Instruction *InsertBefore) {
  unsigned CurrentWidth = getValueWidth(V);
  if (CurrentWidth == TargetWidth)
    return V;

  auto *TargetTy = IntegerType::get(V->getContext(), TargetWidth);
  if (auto *C = dyn_cast<ConstantInt>(V)) {
    if (TargetWidth < CurrentWidth)
      return ConstantInt::get(TargetTy, C->getValue().trunc(TargetWidth));
    return extendConstant(*C, Kind, TargetWidth);
  }
  if (isa<UndefValue>(V))
    return UndefValue::get(TargetTy);
  if (isa<PoisonValue>(V))
    return PoisonValue::get(TargetTy);

  IRBuilder<> B(InsertBefore);
  if (TargetWidth < CurrentWidth)
    return B.CreateTrunc(V, TargetTy);
  assert(Kind != ExtKind::None &&
         "Need an extension kind when materializing a wider value");
  switch (Kind) {
  case ExtKind::ZExt:
    return B.CreateZExt(V, TargetTy);
  case ExtKind::SExt:
    return B.CreateSExt(V, TargetTy);
  case ExtKind::None:
    break;
  }
  llvm_unreachable("Unexpected extension kind");
}

unsigned edgeCutCost(unsigned FromWidth, unsigned ToWidth) {
  return FromWidth == ToWidth ? 0u : 1u;
}

unsigned compareAffinityCost(unsigned LHSWidth, unsigned RHSWidth) {
  return LHSWidth == RHSWidth ? 0u : 1u;
}

unsigned scoreWidthChoice(const AnalysisResult &R,
                          ArrayRef<unsigned> ChosenWidths,
                          unsigned ComponentID, unsigned Width) {
  unsigned Score = 0;
  for (const ComponentEdge &E : R.Edges) {
    if (E.From == ComponentID)
      Score += edgeCutCost(Width, ChosenWidths[E.To]);
    else if (E.To == ComponentID)
      Score += edgeCutCost(ChosenWidths[E.From], Width);
  }
  // Compares do not define a common-width component because their result is
  // i1, but they still create pressure for their integer operands to agree on
  // one width. Model that as a symmetric affinity in the planner.
  for (const CompareAffinity &A : R.CompareAffinities) {
    assert(A.LHS < ChosenWidths.size() && A.RHS < ChosenWidths.size() &&
           "Compare affinities should reference valid component IDs");
    if (A.LHS == ComponentID)
      Score += compareAffinityCost(Width, ChosenWidths[A.RHS]);
    else if (A.RHS == ComponentID)
      Score += compareAffinityCost(ChosenWidths[A.LHS], Width);
  }
  return Score;
}

bool preferOrigWidthOnTie(const Component &C) {
  // Single cast nodes are natural boundaries. Preferring their original width
  // on a tie avoids trivial oscillation and gives neighboring wider components
  // a chance to absorb them only when that strictly improves cut cost.
  return C.Instructions.size() == 1 &&
         (isa<ZExtInst>(C.Instructions.front()) ||
          isa<SExtInst>(C.Instructions.front()) ||
          isa<TruncInst>(C.Instructions.front()));
}

bool isBetterWidthChoice(const Component &C, unsigned CandidateWidth,
                         unsigned CandidateScore, unsigned BestWidth,
                         unsigned BestScore) {
  if (CandidateScore != BestScore)
    return CandidateScore < BestScore;
  if (preferOrigWidthOnTie(C)) {
    bool CandidateIsOrig = CandidateWidth == C.OrigWidth;
    bool BestIsOrig = BestWidth == C.OrigWidth;
    if (CandidateIsOrig != BestIsOrig)
      return CandidateIsOrig;
  }
  if (CandidateWidth != BestWidth)
    return CandidateWidth < BestWidth;
  return false;
}

PlanResult computeWidthPlan(const AnalysisResult &R) {
  PlanResult Plan;
  Plan.ChosenWidths.reserve(R.Components.size());
  for (const Component &C : R.Components) {
    assert(C.ID == Plan.ChosenWidths.size() &&
           "Component IDs should be dense and zero-based");
    Plan.ChosenWidths.push_back(C.OrigWidth);
  }

  unsigned MaxRounds = std::max<unsigned>(1, R.Components.size() * 4);
  for (unsigned Round = 0; Round != MaxRounds; ++Round) {
    bool Changed = false;

    for (const Component &C : R.Components) {
      assert(C.ID < Plan.ChosenWidths.size() &&
             "Planner state must cover every component");
      if (C.Fixed)
        continue;

      SmallVector<unsigned, 4> Widths = C.CandidateWidths;
      if (!llvm::is_contained(Widths, C.OrigWidth))
        Widths.push_back(C.OrigWidth);
      llvm::sort(Widths);

      unsigned BestWidth = Plan.ChosenWidths[C.ID];
      unsigned BestScore =
          scoreWidthChoice(R, Plan.ChosenWidths, C.ID, BestWidth);
      for (unsigned Width : Widths) {
        unsigned Score = scoreWidthChoice(R, Plan.ChosenWidths, C.ID, Width);
        if (isBetterWidthChoice(C, Width, Score, BestWidth, BestScore)) {
          BestWidth = Width;
          BestScore = Score;
        }
      }

      if (BestWidth != Plan.ChosenWidths[C.ID]) {
        Plan.ChosenWidths[C.ID] = BestWidth;
        Changed = true;
      }
    }

    if (!Changed)
      break;
  }

  // The current planner is a small local-improvement heuristic over the
  // component graph, not the eventual exact binary solver described in the
  // design document. Keep the reported total cost in the same cost model the
  // chooser used so debug output stays interpretable.
  for (const ComponentEdge &E : R.Edges)
    Plan.TotalCutCost +=
        edgeCutCost(Plan.ChosenWidths[E.From], Plan.ChosenWidths[E.To]);
  for (const CompareAffinity &A : R.CompareAffinities) {
    assert(A.LHS < Plan.ChosenWidths.size() && A.RHS < Plan.ChosenWidths.size() &&
           "Compare affinities should reference valid component IDs");
    Plan.TotalCutCost +=
        compareAffinityCost(Plan.ChosenWidths[A.LHS], Plan.ChosenWidths[A.RHS]);
  }

  return Plan;
}

AnalysisResult computeWidthComponents(Function &F) {
  // Build width components first, before candidate generation or planning.
  // This keeps later reasoning at component granularity instead of raw SSA
  // value granularity.
  EquivalenceClasses<const Value *> EC;
  DenseSet<const Value *> TrackedValues;
  for (Argument &Arg : F.args()) {
    if (!shouldTrackValue(&Arg))
      continue;
    EC.insert(&Arg);
    TrackedValues.insert(&Arg);
  }

  for (Instruction &I : instructions(F)) {
    if (!shouldTrackValue(&I))
      continue;
    EC.insert(&I);
    TrackedValues.insert(&I);

    if (classifyInstruction(I) == InstClass::FreelyWidthPolymorphic)
      addEqualWidthConstraints(I, EC);
  }

  AnalysisResult R;
  DenseMap<const Value *, unsigned> LeaderToID;

  unsigned NextID = 0;
  for (const Value *V : TrackedValues) {
    const Value *Leader = EC.getLeaderValue(V);
    auto It = LeaderToID.find(Leader);
    if (It == LeaderToID.end()) {
      unsigned ID = NextID++;
      LeaderToID[Leader] = ID;
      R.Components.push_back(Component());
      R.Components.back().ID = ID;
      R.Components.back().OrigWidth = getValueWidth(V);
      addCandidateWidth(R.Components.back(), getValueWidth(V));
      It = LeaderToID.find(Leader);
    }

    unsigned ID = It->second;
    R.ValueToComponent[V] = ID;
    assert(ID < R.Components.size() && "Component ID out of range");
    Component &C = R.Components[ID];
    C.OrigWidth = std::max(C.OrigWidth, getValueWidth(V));
    C.Fixed |= isAnchorValue(V);
    // Boolean-producing instructions are outside the current global width
    // search space. We may still track them for analysis/printing, but the
    // planner should not try to widen or narrow them.
    if (getValueWidth(V) == 1)
      C.Fixed = true;
    C.Values.push_back(const_cast<Value *>(V));
    if (auto *I = dyn_cast<Instruction>(const_cast<Value *>(V))) {
      C.Instructions.push_back(I);
      if (classifyInstruction(*I) == InstClass::HardAnchor)
        C.Fixed = true;
    }
  }

  DenseSet<uint64_t> SeenEdges;
  DenseSet<uint64_t> SeenCompareAffinities;
  for (Instruction &I : instructions(F)) {
    // Ordinary cross-component def-use edges become cut candidates in the
    // planner: differing chosen widths here imply a boundary conversion.
    for (Value *Op : I.operands()) {
      if (!shouldTrackValue(Op))
        continue;
      auto FromIt = R.ValueToComponent.find(Op);
      auto ToIt = R.ValueToComponent.find(&I);
      if (FromIt == R.ValueToComponent.end() || ToIt == R.ValueToComponent.end())
        continue;
      if (FromIt->second == ToIt->second)
        continue;
      uint64_t Key = (uint64_t(FromIt->second) << 32) | uint64_t(ToIt->second);
      if (SeenEdges.insert(Key).second)
        R.Edges.push_back(ComponentEdge{FromIt->second, ToIt->second});
    }

    if (auto *Cmp = dyn_cast<ICmpInst>(&I)) {
      // Compare operands do not merge into one component because the result is
      // i1, but the compare still prefers them to agree on width. Record that
      // separately from the ordinary def-use graph.
      Value *LHS = Cmp->getOperand(0);
      Value *RHS = Cmp->getOperand(1);
      if (shouldTrackValue(LHS) && shouldTrackValue(RHS)) {
        auto LHSIt = R.ValueToComponent.find(LHS);
        auto RHSIt = R.ValueToComponent.find(RHS);
        if (LHSIt != R.ValueToComponent.end() &&
            RHSIt != R.ValueToComponent.end() &&
            LHSIt->second != RHSIt->second) {
          unsigned A = std::min(LHSIt->second, RHSIt->second);
          unsigned B = std::max(LHSIt->second, RHSIt->second);
          uint64_t Key = (uint64_t(A) << 32) | uint64_t(B);
          if (SeenCompareAffinities.insert(Key).second)
            R.CompareAffinities.push_back(CompareAffinity{A, B});
        }
      }
    }

    inferCandidatesFromInstruction(I, R);
  }

  return R;
}

struct ExternalExtUser {
  Instruction *User = nullptr;
  Value *OldValue = nullptr;
};

bool canRetargetBoundaryCompare(ICmpInst::Predicate Pred, ExtKind InternalKind) {
  if (isEqOrNe(Pred))
    return InternalKind != ExtKind::None;
  if (isUnsignedICmp(Pred))
    return InternalKind == ExtKind::ZExt;
  if (isSignedICmp(Pred))
    return InternalKind == ExtKind::SExt;
  return false;
}

bool tryRetargetExternalICmp(ICmpInst &Cmp, const DenseSet<const Value *> &ComponentValues,
                             const DenseMap<Value *, Value *> &NewValues,
                             ExtKind InternalKind, unsigned OrigWidth,
                             unsigned TargetWidth) {
  if (!canRetargetBoundaryCompare(Cmp.getPredicate(), InternalKind))
    return false;

  // Retarget the compare itself when the widened internal representation
  // already matches the predicate's semantics. This is cheaper than truncating
  // the widened value back to OrigWidth just to rebuild the compare as-is.
  Value *NewOps[2] = {nullptr, nullptr};
  for (unsigned Idx = 0; Idx != 2; ++Idx) {
    Value *Op = Cmp.getOperand(Idx);
    if (ComponentValues.count(Op)) {
      NewOps[Idx] = NewValues.lookup(Op);
      if (!NewOps[Idx])
        return false;
      continue;
    }

    if (!isIntegerValue(Op) || getValueWidth(Op) != OrigWidth)
      return false;
    NewOps[Idx] = materializeValueAtWidth(Op, InternalKind, TargetWidth, &Cmp);
    if (!NewOps[Idx])
      return false;
  }

  IRBuilder<> B(&Cmp);
  auto *NewCmp =
      cast<ICmpInst>(B.CreateICmp(Cmp.getPredicate(), NewOps[0], NewOps[1]));
  NewCmp->setDebugLoc(Cmp.getDebugLoc());
  NewCmp->takeName(&Cmp);
  Cmp.replaceAllUsesWith(NewCmp);
  Cmp.eraseFromParent();
  return true;
}

bool isWidenablePolyInstruction(Instruction &I) {
  // These are the operations we currently know how to rebuild uniformly at a
  // larger width inside one component. Anything else stays outside the generic
  // region widener for now.
  if (isa<PHINode>(I) || isa<SelectInst>(I) || isa<FreezeInst>(I))
    return true;

  auto *BO = dyn_cast<BinaryOperator>(&I);
  if (!BO)
    return false;

  switch (BO->getOpcode()) {
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
    return true;
  default:
    return false;
  }
}

bool tryWidenComponentFromPlan(const Component &C, const AnalysisResult &R,
                               const PlanResult &Plan) {
  assert(C.ID < Plan.ChosenWidths.size() &&
         "Planner state must cover every component");
  if (C.Fixed || C.Instructions.empty())
    return false;

  unsigned TargetWidth = Plan.ChosenWidths[C.ID];
  if (TargetWidth <= C.OrigWidth)
    return false;

  DenseSet<const Value *> ComponentValues;
  for (Value *V : C.Values)
    ComponentValues.insert(V);

  // The generic widener only handles small width-polymorphic regions. As soon
  // as a component contains something we cannot rebuild structurally, we leave
  // it to narrower local folds or future legality work.
  for (Instruction *I : C.Instructions)
    if (!isWidenablePolyInstruction(*I))
      return false;

  bool SawZExtToTarget = false;
  bool SawSExtToTarget = false;
  SmallVector<ExternalExtUser, 8> ExternalUsers;
  for (Value *V : C.Values) {
    for (User *U : V->users()) {
      auto *UserI = dyn_cast<Instruction>(U);
      if (!UserI)
        return false;
      if (ComponentValues.count(UserI))
        continue;

      if (auto *Z = dyn_cast<ZExtInst>(UserI)) {
        if (Z->getOperand(0) != V)
          return false;
        if (getValueWidth(Z) == TargetWidth)
          SawZExtToTarget = true;
      } else if (auto *S = dyn_cast<SExtInst>(UserI)) {
        if (S->getOperand(0) != V)
          return false;
      }

      ExternalUsers.push_back(ExternalExtUser{UserI, V});
    }
  }

  // The current region widener is consumer-driven: it only widens when there
  // is some external pressure at the chosen target width to absorb.
  if (ExternalUsers.empty())
    return false;

  for (const ExternalExtUser &EU : ExternalUsers)
    if (auto *S = dyn_cast<SExtInst>(EU.User))
      if (getValueWidth(S) == TargetWidth)
        SawSExtToTarget = true;

  // Policy choice: prefer a zero-extended internal representation unless all
  // target-width consumers specifically want sign extension. Zero extension is
  // the more neutral choice because we can always reconstruct the original
  // narrow value with a truncation at the boundary and then re-extend it per
  // edge when needed.
  ExtKind InternalKind =
      SawSExtToTarget && !SawZExtToTarget ? ExtKind::SExt : ExtKind::ZExt;

  auto *TargetTy = IntegerType::get(C.Instructions.front()->getContext(),
                                    TargetWidth);
  DenseMap<Value *, Value *> NewValues;

  // PHIs must be created first so that later instructions in the component can
  // refer to the widened region without worrying about cycles.
  for (Instruction *I : C.Instructions) {
    if (auto *Phi = dyn_cast<PHINode>(I)) {
      auto *NewPhi = PHINode::Create(TargetTy, Phi->getNumIncomingValues(), "",
                                     Phi->getIterator());
      NewPhi->setDebugLoc(Phi->getDebugLoc());
      NewPhi->takeName(Phi);
      NewValues[Phi] = NewPhi;
    }
  }

  for (Instruction *I : C.Instructions) {
    auto *Phi = dyn_cast<PHINode>(I);
    if (!Phi)
      continue;
    auto *NewPhi = cast<PHINode>(NewValues[Phi]);
    for (unsigned Idx = 0, E = Phi->getNumIncomingValues(); Idx != E; ++Idx) {
      Value *Incoming = Phi->getIncomingValue(Idx);
      BasicBlock *Pred = Phi->getIncomingBlock(Idx);
      Value *WideIncoming = nullptr;
      if (ComponentValues.count(Incoming))
        WideIncoming = NewValues.lookup(Incoming);
      else
        WideIncoming =
            materializeValueAtWidth(Incoming, InternalKind, TargetWidth,
                                    Pred->getTerminator());
      if (WideIncoming == nullptr)
        return false;
      NewPhi->addIncoming(WideIncoming, Pred);
    }
  }

  unsigned Remaining = 0;
  for (Instruction *I : C.Instructions)
    if (!isa<PHINode>(I))
      ++Remaining;

  while (Remaining != 0) {
    bool Progress = false;
    for (Instruction *I : C.Instructions) {
      if (isa<PHINode>(I) || NewValues.count(I))
        continue;

      if (auto *Sel = dyn_cast<SelectInst>(I)) {
        auto getWideValue = [&](Value *V) -> Value * {
          if (ComponentValues.count(V))
            return NewValues.lookup(V);
          return materializeValueAtWidth(V, InternalKind, TargetWidth, Sel);
        };

        Value *WideTV = getWideValue(Sel->getTrueValue());
        Value *WideFV = getWideValue(Sel->getFalseValue());
        if (!WideTV || !WideFV)
          continue;

        IRBuilder<> B(Sel);
        auto *WideSel = cast<SelectInst>(
            B.CreateSelect(Sel->getCondition(), WideTV, WideFV, ""));
        WideSel->setDebugLoc(Sel->getDebugLoc());
        WideSel->takeName(Sel);
        NewValues[Sel] = WideSel;
        Progress = true;
        --Remaining;
        continue;
      }

      if (auto *Fr = dyn_cast<FreezeInst>(I)) {
        Value *Op = Fr->getOperand(0);
        Value *WideOp = nullptr;
        if (ComponentValues.count(Op))
          WideOp = NewValues.lookup(Op);
        else
          WideOp = materializeValueAtWidth(Op, InternalKind, TargetWidth, Fr);
        if (!WideOp)
          continue;

        IRBuilder<> B(Fr);
        auto *WideFreeze = cast<Instruction>(B.CreateFreeze(WideOp));
        WideFreeze->setDebugLoc(Fr->getDebugLoc());
        WideFreeze->takeName(Fr);
        NewValues[Fr] = WideFreeze;
        Progress = true;
        --Remaining;
        continue;
      }

      if (auto *BO = dyn_cast<BinaryOperator>(I)) {
        auto getWideValue = [&](Value *V) -> Value * {
          if (ComponentValues.count(V))
            return NewValues.lookup(V);
          return materializeValueAtWidth(V, InternalKind, TargetWidth, BO);
        };

        Value *WideLHS = getWideValue(BO->getOperand(0));
        Value *WideRHS = getWideValue(BO->getOperand(1));
        if (!WideLHS || !WideRHS)
          continue;

        IRBuilder<> B(BO);
        auto *WideBO = cast<Instruction>(
            B.CreateBinOp((Instruction::BinaryOps)BO->getOpcode(), WideLHS,
                          WideRHS));
        WideBO->setDebugLoc(BO->getDebugLoc());
        WideBO->takeName(BO);
        NewValues[BO] = WideBO;
        Progress = true;
        --Remaining;
        continue;
      }

      return false;
    }

    // Remaining instructions should form an acyclic use graph once PHIs are
    // removed. If we stop making progress, this component shape is outside the
    // current rebuilder.
    if (!Progress)
      return false;
  }

  // Each outgoing edge gets its own reconstruction policy. Matching wide
  // extension users can disappear entirely; everything else is repaired from
  // the widened value through an explicit boundary cast.
  DenseSet<Instruction *> RetargetedUsers;
  DenseSet<Instruction *> SeenUsers;
  for (const ExternalExtUser &EU : ExternalUsers) {
    if (!SeenUsers.insert(EU.User).second)
      continue;
    auto *Cmp = dyn_cast<ICmpInst>(EU.User);
    if (!Cmp)
      continue;
    if (tryRetargetExternalICmp(*Cmp, ComponentValues, NewValues, InternalKind,
                                C.OrigWidth, TargetWidth))
      RetargetedUsers.insert(EU.User);
  }

  for (const ExternalExtUser &EU : ExternalUsers) {
    if (RetargetedUsers.contains(EU.User))
      continue;

    Value *NewV = NewValues.lookup(EU.OldValue);
    assert(NewV && "Every component value should have a widened replacement");

    if (auto *Z = dyn_cast<ZExtInst>(EU.User)) {
      if (getValueWidth(Z) == TargetWidth && InternalKind == ExtKind::ZExt) {
        Z->replaceAllUsesWith(NewV);
        Z->eraseFromParent();
        continue;
      }
    }

    if (auto *S = dyn_cast<SExtInst>(EU.User)) {
      if (getValueWidth(S) == TargetWidth && InternalKind == ExtKind::SExt) {
        S->replaceAllUsesWith(NewV);
        S->eraseFromParent();
        continue;
      }
    }

    Value *Boundary =
        materializeValueAtWidth(NewV, InternalKind, C.OrigWidth, EU.User);
    assert(getValueWidth(Boundary) == C.OrigWidth &&
           "Boundary repair should recreate the original component width");
    EU.User->replaceUsesOfWith(EU.OldValue, Boundary);
  }

  // At this point all external uses should have been redirected either to the
  // widened region directly or to explicit boundary conversions.
  for (Instruction *I : C.Instructions)
    if (!I->use_empty())
      I->replaceAllUsesWith(PoisonValue::get(I->getType()));
  for (Instruction *I : C.Instructions)
    I->eraseFromParent();

  return true;
}

} // namespace

WidthComponentAnalysis::Result
WidthComponentAnalysis::run(Function &F, FunctionAnalysisManager &) {
  return computeWidthComponents(F);
}

WidthPlanAnalysis::Result WidthPlanAnalysis::run(Function &F,
                                                 FunctionAnalysisManager &AM) {
  const AnalysisResult &R = AM.getResult<WidthComponentAnalysis>(F);
  return computeWidthPlan(R);
}

PreservedAnalyses WidthComponentPrinter::run(Function &F,
                                             FunctionAnalysisManager &AM) {
  const AnalysisResult &R = AM.getResult<WidthComponentAnalysis>(F);

  OS << "Width components for function '" << F.getName() << "':\n";
  for (const Component &C : R.Components) {
    OS << formatv("  component {0}: width=i{1}, fixed={2}, values={3}\n", C.ID,
                  C.OrigWidth, C.Fixed ? "true" : "false", C.Values.size());
    for (Value *V : C.Values) {
      OS << "    " << formatValue(V);
      if (auto *I = dyn_cast<Instruction>(V)) {
        OS << " [";
        switch (classifyInstruction(*I)) {
        case InstClass::HardAnchor:
          OS << "anchor";
          break;
        case InstClass::FreelyWidthPolymorphic:
          OS << "poly";
          break;
        case InstClass::ConditionallyRetargetable:
          OS << "conditional";
          break;
        case InstClass::Ignore:
          OS << "ignore";
          break;
        }
        OS << "]";
      } else {
        OS << " [arg]";
      }
      OS << "\n";
    }
  }

  return PreservedAnalyses::all();
}

PreservedAnalyses WidthCandidatePrinter::run(Function &F,
                                             FunctionAnalysisManager &AM) {
  const AnalysisResult &R = AM.getResult<WidthComponentAnalysis>(F);

  OS << "Width candidates for function '" << F.getName() << "':\n";
  for (const Component &C : R.Components) {
    SmallVector<unsigned, 4> Widths = C.CandidateWidths;
    llvm::sort(Widths);
    OS << formatv("  component {0}: orig=i{1}, fixed={2}, candidates=",
                  C.ID, C.OrigWidth, C.Fixed ? "true" : "false");
    for (unsigned I = 0, E = Widths.size(); I != E; ++I) {
      if (I)
        OS << ",";
      OS << "i" << Widths[I];
    }
    OS << "\n";
  }

  for (const ComponentEdge &E : R.Edges)
    OS << formatv("  edge {0} -> {1}\n", E.From, E.To);

  return PreservedAnalyses::all();
}

PreservedAnalyses WidthPlanPrinter::run(Function &F,
                                        FunctionAnalysisManager &AM) {
  const AnalysisResult &R = AM.getResult<WidthComponentAnalysis>(F);
  const PlanResult &Plan = AM.getResult<WidthPlanAnalysis>(F);

  OS << "Width plan for function '" << F.getName()
     << "': total-cut-cost=" << Plan.TotalCutCost << "\n";
  for (const Component &C : R.Components) {
    SmallVector<unsigned, 4> Widths = C.CandidateWidths;
    if (!llvm::is_contained(Widths, C.OrigWidth))
      Widths.push_back(C.OrigWidth);
    llvm::sort(Widths);

    OS << formatv("  component {0}: orig=i{1}, fixed={2}, chosen=i{3}, "
                  "candidates=",
                  C.ID, C.OrigWidth, C.Fixed ? "true" : "false",
                  Plan.ChosenWidths[C.ID]);
    for (unsigned I = 0, E = Widths.size(); I != E; ++I) {
      if (I)
        OS << ",";
      OS << "i" << Widths[I];
    }
    OS << "\n";
  }

  for (const CompareAffinity &A : R.CompareAffinities)
    OS << formatv("  compare-affinity {0} <-> {1}\n", A.LHS, A.RHS);

  return PreservedAnalyses::all();
}

PreservedAnalyses WidthOptPass::run(Function &F, FunctionAnalysisManager &AM) {
  LazyValueInfo &LVI = AM.getResult<LazyValueAnalysis>(F);
  AssumptionCache &AC = AM.getResult<AssumptionAnalysis>(F);
  DominatorTree &DT = AM.getResult<DominatorTreeAnalysis>(F);
  const DataLayout &DL = F.getDataLayout();

  SmallVector<ICmpInst *, 16> Worklist;
  SmallVector<SelectInst *, 16> Selects;
  SmallVector<PHINode *, 16> Phis;
  SmallVector<SExtInst *, 16> SExts;
  SmallVector<BinaryOperator *, 16> Adds;
  SmallVector<BinaryOperator *, 16> Ands;
  SmallVector<BinaryOperator *, 16> UDivs;
  SmallVector<ZExtInst *, 16> ZExts;
  SmallVector<TruncInst *, 16> Truncs;
  SmallVector<FreezeInst *, 16> Freezes;
  for (Instruction &I : instructions(F))
    if (auto *Cmp = dyn_cast<ICmpInst>(&I))
      Worklist.push_back(Cmp);
    else if (auto *Sel = dyn_cast<SelectInst>(&I))
      Selects.push_back(Sel);
    else if (auto *Phi = dyn_cast<PHINode>(&I))
      Phis.push_back(Phi);
    else if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
      if (BO->getOpcode() == Instruction::Add)
        Adds.push_back(BO);
      else if (BO->getOpcode() == Instruction::And)
        Ands.push_back(BO);
      else if (BO->getOpcode() == Instruction::UDiv)
        UDivs.push_back(BO);
    } else if (auto *ZExt = dyn_cast<ZExtInst>(&I))
      ZExts.push_back(ZExt);
    else if (auto *SExt = dyn_cast<SExtInst>(&I))
      SExts.push_back(SExt);
    else if (auto *Tr = dyn_cast<TruncInst>(&I))
      Truncs.push_back(Tr);
    else if (auto *FI = dyn_cast<FreezeInst>(&I))
      Freezes.push_back(FI);

  bool Changed = false;

  // Run local rewrites first. They do most of the current narrowing work,
  // simplify the IR seen by component analysis, and expose cleaner widening
  // opportunities for the plan-driven phase at the end of the pass.
  for (BinaryOperator *Add : Adds) {
    if (Add->getParent() == nullptr)
      continue;
    Changed |= tryWidenAddThroughZExt(*Add);
  }

  for (BinaryOperator *And : Ands) {
    if (And->getParent() == nullptr)
      continue;
    Changed |= tryFoldAndOfSExtToZExt(*And);
  }

  for (BinaryOperator *UDiv : UDivs) {
    if (UDiv->getParent() == nullptr)
      continue;
    Changed |= tryNarrowUDivWithRange(*UDiv, LVI);
  }

  for (ZExtInst *ZExt : ZExts) {
    if (ZExt->getParent() == nullptr)
      continue;
    Changed |= tryFoldZExtOfTruncToMask(*ZExt);
  }

  for (TruncInst *Tr : Truncs) {
    if (Tr->getParent() == nullptr)
      continue;
    if (tryShrinkTruncOfShiftRecurrence(*Tr)) {
      Changed = true;
      continue;
    }
    if (tryShrinkTruncOfSelect(*Tr)) {
      Changed = true;
      continue;
    }
    Changed |= tryShrinkTruncOfAdd(*Tr);
  }

  for (SExtInst *SExt : SExts) {
    if (SExt->getParent() == nullptr)
      continue;
    if (tryConvertWholeSExtToZExt(*SExt)) {
      Changed = true;
      continue;
    }
    Changed |= tryConvertSExtToNonNegZExt(*SExt, LVI);
  }

  for (FreezeInst *FI : Freezes) {
    if (FI->getParent() == nullptr)
      continue;
    Changed |= tryPushFreezeThroughExt(*FI);
  }

  for (ICmpInst *Cmp : Worklist) {
    if (Cmp->getParent() == nullptr)
      continue;
    if (tryWidenTruncEqualityICmp(*Cmp, DL, &AC, &DT)) {
      Changed = true;
      continue;
    }
    if (tryWidenTruncZeroExtendedICmp(*Cmp, DL, &AC, &DT)) {
      Changed = true;
      continue;
    }
    Changed |= tryShrinkICmp(*Cmp);
  }

  for (SelectInst *Sel : Selects) {
    if (Sel->getParent() == nullptr)
      continue;
    if (tryConvertSelectToMinMax(*Sel)) {
      Changed = true;
      continue;
    }
    Changed |= tryShrinkSelectOfExts(*Sel);
  }

  for (PHINode *Phi : Phis) {
    if (Phi->getParent() == nullptr)
      continue;
    Changed |= tryShrinkPhiOfExts(*Phi);
  }

  AnalysisResult R = computeWidthComponents(F);
  PlanResult Plan = computeWidthPlan(R);

  // The plan is currently consumed only by widening transforms. Narrowing is
  // still handled by the local folds above.
  for (const Component &C : R.Components)
    Changed |= tryWidenComponentFromPlan(C, R, Plan);

  if (!Changed)
    return PreservedAnalyses::all();
  return PreservedAnalyses::none();
}

PassPluginLibraryInfo getWidthOptPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "WidthOpt", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerAnalysisRegistrationCallback(
                [](FunctionAnalysisManager &FAM) {
                  FAM.registerPass([&] { return WidthComponentAnalysis(); });
                  FAM.registerPass([&] { return WidthPlanAnalysis(); });
                });

            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &FPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "width-opt") {
                    FPM.addPass(WidthOptPass());
                    return true;
                  }
                  if (Name == "print<width-components>") {
                    FPM.addPass(WidthComponentPrinter(errs()));
                    return true;
                  }
                  if (Name == "print<width-candidates>") {
                    FPM.addPass(WidthCandidatePrinter(errs()));
                    return true;
                  }
                  if (Name == "print<width-plan>") {
                    FPM.addPass(WidthPlanPrinter(errs()));
                    return true;
                  }
                  return false;
                });
          }};
}

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return getWidthOptPluginInfo();
}

} // namespace widthopt
