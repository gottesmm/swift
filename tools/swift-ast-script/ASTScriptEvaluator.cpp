//===--- ASTScriptEvaluator.cpp -------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// AST script evaluation.
///
//===----------------------------------------------------------------------===//

#include "ASTScript.h"
#include "ASTScriptConfiguration.h"

#include "swift/AST/ASTMangler.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Decl.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/NameLookupRequests.h"
#include "swift/Frontend/Frontend.h"

using namespace swift;
using namespace scripting;

namespace {

class ASTScriptWalker : public ASTWalker {
  const ASTScript &Script;
  Optional<unsigned> maxTup;
  unsigned numScannedTups = 0;

public:
  unsigned getNumScannedTups() const { return numScannedTups; }
  Optional<unsigned> getMaxTup() const { return maxTup; }

  ASTScriptWalker(const ASTScript &script)
    : Script(script) {}

  bool walkToDeclPre(Decl *D) override {
    visit(D);
    return true;
  }

  void visitAbstractFunctionDecl(const AbstractFunctionDecl *fn) {
    // Suppress warnings.
    (void)Script;
    bool foundTarget = false;
    for (auto param : *fn->getParameters()) {
      auto paramType = param->getInterfaceType();
      if (!paramType)
        continue;
      if (auto *tup = paramType->getAs<TupleType>()) {
        // Only record tuples that are larger than 0.
        if (tup->getNumElements()) {
          llvm::dbgs() << "Found tuple with eltcount: " << tup->getNumElements() << "\n";
          maxTup = std::max(maxTup.getValueOr(0), tup->getNumElements());
          ++numScannedTups;
          foundTarget = true;
        }
      }
    }

    if (auto *fn2 = dyn_cast<FuncDecl>(fn)) {
      if (auto resultTy = fn2->getResultInterfaceType()) {
        if (auto *tup = resultTy->getAs<TupleType>()) {
          // Only record tuples that are larger than 0.
          if (tup->getNumElements()) {
            llvm::dbgs() << "Found tuple with eltcount: " << tup->getNumElements() << "\n";
            maxTup = std::max(maxTup.getValueOr(0), tup->getNumElements());
            ++numScannedTups;
            foundTarget = true;
          }
        }        
      }
    }

    if (foundTarget) {
      llvm::dbgs() << "Found Target! Source Range:\n";
      fn->getSourceRange().print(llvm::dbgs(), Script.getConfig().getSourceManager());
      llvm::dbgs() << "\nDump:\n";
      fn->print(llvm::dbgs());
      llvm::dbgs() << '\n';
    }
  }

  void visit(const Decl *D) {
    if (auto *fn = dyn_cast<AbstractFunctionDecl>(D))
      return visitAbstractFunctionDecl(fn);
  }
};

}

bool ASTScript::execute() const {
  // Hardcode the actual query we want to execute here.

  auto &ctx = Config.Compiler.getASTContext();
  for (auto p : ctx.getLoadedModules()) {
    if (!p.second->isMainModule())
      continue;
    SmallVector<Decl*, 128> topLevelDecls;
    p.second->getTopLevelDecls(topLevelDecls);
    llvm::dbgs() << "Visiting Module: " << p.first << ". Found " << topLevelDecls.size() << " top level decls.\n";
    ASTScriptWalker walker(*this);
    for (auto decl : topLevelDecls) {
      decl->walk(walker);
    }
    llvm::dbgs() << "Num Tuples Scanned: " << walker.getNumScannedTups() << '\n';
    if (auto val = walker.getMaxTup())
      llvm::dbgs() << "Max Seen Tuple Size: " << *val << '\n';
    return false;
  }

  return false;
}
