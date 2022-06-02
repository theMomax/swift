//===--- DataflowDiagnostics.cpp - Emits diagnostics based on SIL analysis ===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTContext.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Stmt.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/SILConstants.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILLocation.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/ConstExpr.h"

using namespace swift;

template<typename...T, typename...U>
static void diagnose(ASTContext &Context, SourceLoc loc, Diag<T...> diag,
              U &&...args) {
  Context.Diags.diagnose(loc,
                         diag, std::forward<U>(args)...);
}

static void diagnoseMissingReturn(const UnreachableInst *UI,
                                  ASTContext &Context) {
  const SILBasicBlock *BB = UI->getParent();
  const SILFunction *F = BB->getParent();
  SILLocation FLoc = F->getLocation();

  Type ResTy;
  BraceStmt *BS;

  if (auto *FD = FLoc.getAsASTNode<FuncDecl>()) {
    ResTy = FD->getResultInterfaceType();
    BS = FD->getBody(/*canSynthesize=*/false);
  } else if (auto *CD = FLoc.getAsASTNode<ConstructorDecl>()) {
    ResTy = CD->getResultInterfaceType();
    BS = FD->getBody();
  } else if (auto *CE = FLoc.getAsASTNode<ClosureExpr>()) {
    ResTy = CE->getResultType();
    BS = CE->getBody();
  } else {
    llvm_unreachable("unhandled case in MissingReturn");
  }

  SILLocation L = UI->getLoc();
  assert(L && ResTy);
  if (!BS->empty()) {
    auto element = BS->getLastElement();
    if (auto expr = element.dyn_cast<Expr *>()) {
      if (expr->getType()->getRValueType()->isEqual(ResTy)) {
        if (FLoc.isASTNode<ClosureExpr>()) {
          Context.Diags.diagnose(expr->getStartLoc(),
                                 diag::missing_return_closure, ResTy);
        } else {
          auto *DC = FLoc.getAsDeclContext();
          assert(DC && DC->getAsDecl() && "not a declaration?");
          Context.Diags.diagnose(expr->getStartLoc(), diag::missing_return_decl,
                                 ResTy, DC->getAsDecl()->getDescriptiveKind());
        }
        Context.Diags
            .diagnose(expr->getStartLoc(), diag::missing_return_last_expr_note)
            .fixItInsert(expr->getStartLoc(), "return ");

        return;
      }
    }
  }

  bool isNoReturnFn = F->isNoReturnFunction(F->getTypeExpansionContext());
  if (FLoc.isASTNode<ClosureExpr>()) {
    auto diagID = isNoReturnFn ? diag::missing_never_call_closure
                               : diag::missing_return_closure;
    diagnose(Context, L.getEndSourceLoc(), diagID, ResTy);
  } else {
    auto *DC = FLoc.getAsDeclContext();
    assert(DC && DC->getAsDecl() && "not a declaration?");
    auto diagID = isNoReturnFn ? diag::missing_never_call_decl
                               : diag::missing_return_decl;
    diagnose(Context, L.getEndSourceLoc(), diagID, ResTy,
             DC->getAsDecl()->getDescriptiveKind());
  }
}

static void diagnoseUnreachable(const SILInstruction *I,
                                ASTContext &Context) {
  if (auto *UI = dyn_cast<UnreachableInst>(I)) {
    SILLocation L = UI->getLoc();

    // Invalid location means that the instruction has been generated by SIL
    // passes, such as DCE. FIXME: we might want to just introduce a separate
    // instruction kind, instead of keeping this invariant.
    //
    // We also do not want to emit diagnostics for code that was
    // transparently inlined. We should have already emitted these
    // diagnostics when we process the callee function prior to
    // inlining it.
    if (!L || L.is<MandatoryInlinedLocation>())
      return;

    // The most common case of getting an unreachable instruction is a
    // missing return statement. In this case, we know that the instruction
    // location will be the enclosing function.
    if (L.isASTNode<AbstractFunctionDecl>() || L.isASTNode<ClosureExpr>()) {
      diagnoseMissingReturn(UI, Context);
      return;
    }

    if (auto *Guard = L.getAsASTNode<GuardStmt>()) {
      diagnose(Context, Guard->getBody()->getEndLoc(),
               diag::guard_body_must_not_fallthrough);
      return;
    }
  }
}

/// Issue diagnostics whenever we see Builtin.static_report(1, ...).
static void diagnoseStaticReports(const SILInstruction *I,
                                  SILModule &M) {

  // Find out if we are dealing with Builtin.staticReport().
  if (auto *BI = dyn_cast<BuiltinInst>(I)) {
    const BuiltinInfo &B = BI->getBuiltinInfo();
    if (B.ID == BuiltinValueKind::StaticReport) {

      // Report diagnostic if the first argument has been folded to '1'.
      OperandValueArrayRef Args = BI->getArguments();
      auto *V = dyn_cast<IntegerLiteralInst>(Args[0]);
      if (!V || V->getValue() != 1)
        return;

      diagnose(M.getASTContext(), I->getLoc().getSourceLoc(),
               diag::static_report_error);
    }
  }
}

/// Emit a diagnostic for `poundAssert` builtins whose condition is
/// false or whose condition cannot be evaluated.
static void diagnosePoundAssert(const SILInstruction *I,
                                SILModule &M,
                                ConstExprEvaluator &constantEvaluator) {
  auto *builtinInst = dyn_cast<BuiltinInst>(I);
  if (!builtinInst ||
      builtinInst->getBuiltinKind() != BuiltinValueKind::PoundAssert)
    return;

  SmallVector<SymbolicValue, 1> values;
  constantEvaluator.computeConstantValues({builtinInst->getArguments()[0]},
                                          values);
  SymbolicValue value = values[0];
  if (!value.isConstant()) {
    diagnose(M.getASTContext(), I->getLoc().getSourceLoc(),
             diag::pound_assert_condition_not_constant);

    // If we have more specific information about what went wrong, emit
    // notes.
    if (value.getKind() == SymbolicValue::Unknown)
      value.emitUnknownDiagnosticNotes(builtinInst->getLoc());
    return;
  }
  assert(value.getKind() == SymbolicValue::Integer &&
         "sema prevents non-integer #assert condition");

  APInt intValue = value.getIntegerValue();
  assert(intValue.getBitWidth() == 1 &&
         "sema prevents non-int1 #assert condition");
  if (intValue.isNullValue()) {
    auto *message = cast<StringLiteralInst>(builtinInst->getArguments()[1]);
    StringRef messageValue = message->getValue();
    if (messageValue.empty())
      messageValue = "assertion failed";
    diagnose(M.getASTContext(), I->getLoc().getSourceLoc(),
             diag::pound_assert_failure, messageValue);
    return;
  }
}

static void diagnoseUnspecializedPolymorphicBuiltins(SILInstruction *inst) {
  // We only validate if we are in a non-transparent function.
  if (inst->getFunction()->isTransparent())
    return;

  auto *bi = dyn_cast<BuiltinInst>(inst);
  if (!bi)
    return;

  auto kind = bi->getBuiltinKind();
  if (!kind)
    return;

  if (!isPolymorphicBuiltin(*kind))
    return;

  const auto &builtinInfo = bi->getBuiltinInfo();

  // First that the parameters were acceptable so we can emit a nice error to
  // guide the user.
  for (SILValue value : bi->getOperandValues()) {
    SILType type = value->getType();
    SourceLoc loc;
    if (auto *inst = value->getDefiningInstruction()) {
      loc = inst->getLoc().getSourceLoc();
    } else {
      loc = bi->getLoc().getSourceLoc();
    }

    if (!type.is<BuiltinType>() || !type.isTrivial(*bi->getFunction())) {
      diagnose(bi->getModule().getASTContext(), loc,
               diag::polymorphic_builtin_passed_non_trivial_non_builtin_type,
               type.getASTType());
      return;
    }
  }

  // Ok, we have a valid type for a polymorphic builtin. Make sure we actually
  // have a static overload for this type.
  PolymorphicBuiltinSpecializedOverloadInfo overloadInfo;
  bool ableToMapToStaticOverload = overloadInfo.init(bi);
  (void)ableToMapToStaticOverload;
  assert(ableToMapToStaticOverload);
  if (!overloadInfo.doesOverloadExist()) {
    diagnose(bi->getModule().getASTContext(), bi->getLoc().getSourceLoc(),
             diag::polymorphic_builtin_passed_type_without_static_overload,
             overloadInfo.staticOverloadIdentifier,
             getBuiltinName(builtinInfo.ID),
             overloadInfo.argTypes.front().getASTType());
    return;
  }

  // Otherwise, something happen that we did not understand. This can only
  // happen if we specialize the generic type in the builtin /after/ constant
  // propagation runs at -Onone but before dataflow diagnostics. This is an
  // error in implementation, so we assert.
  llvm_unreachable("Found generic builtin with known static overload that it "
                   "could be transformed to. Did this builtin get its generic "
                   "type specialized /after/ constant propagation?");
}

namespace {
class EmitDFDiagnostics : public SILFunctionTransform {
  ~EmitDFDiagnostics() override {}

  /// The entry point to the transformation.
  void run() override {
    // Don't rerun diagnostics on deserialized functions.
    if (getFunction()->wasDeserializedCanonical())
      return;

    SILModule &M = getFunction()->getModule();
    for (auto &BB : *getFunction()) {
      for (auto &I : BB) {
        diagnoseUnreachable(&I, M.getASTContext());
        diagnoseStaticReports(&I, M);
        diagnoseUnspecializedPolymorphicBuiltins(&I);
      }
    }

    if (M.getASTContext().LangOpts.hasFeature(Feature::StaticAssert)) {
      SymbolicValueBumpAllocator allocator;
      ConstExprEvaluator constantEvaluator(allocator,
                                           getOptions().AssertConfig);
      for (auto &BB : *getFunction())
        for (auto &I : BB)
          diagnosePoundAssert(&I, M, constantEvaluator);
    }
  }
};

} // end anonymous namespace


SILTransform *swift::createEmitDFDiagnostics() {
  return new EmitDFDiagnostics();
}
