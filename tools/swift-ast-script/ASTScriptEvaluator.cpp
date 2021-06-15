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

public:
  ASTScriptWalker(const ASTScript &script)
    : Script(script) {}

  bool walkToDeclPre(Decl *D) override {
    visit(D);
    return true;
  }

  void visit(const Decl *D) {
    auto fn = dyn_cast<AbstractFunctionDecl>(D);
    if (!fn) return;

    // Suppress warnings.
    (void) Script;

    for (auto param : *fn->getParameters()) {
      // The parameter must have function type.
      auto paramType = param->getInterfaceType();
      auto paramFnType = paramType->getAs<FunctionType>();
      if (!paramFnType) continue;

      int maxTup = -1;
      for (auto param : paramFnType->getParams()) {
        if (auto tup = param.getPlainType()->getAs<TupleType>())  {
          maxTup = std::max(maxTup, int(tup->getNumElements()));
        }
        if (auto tup = param.getParameterType(false, &fn->getASTContext())->getAs<TupleType>())  {
          maxTup = std::max(maxTup, int(tup->getNumElements()));
        }
      }

      // Print the function.
      if (maxTup >= 0)
        printDecl(fn, maxTup);
    }
  }

  void printDecl(const ValueDecl *decl, int maxTup) {
    // FIXME: there's got to be some better way to print an exact reference
    // to a declaration, including its context.
    printDecl(llvm::outs(), decl);
    llvm::outs() << ". MaxTup: " << maxTup << '\n';
  }

  void printDecl(llvm::raw_ostream &out, const ValueDecl *decl) {
    if (auto accessor = dyn_cast<AccessorDecl>(decl)) {
      printDecl(out, accessor->getStorage());
      out << ".(accessor)";
    } else {
      printDeclContext(out, decl->getDeclContext());
    }
  }

  void printDeclContext(llvm::raw_ostream &out, const DeclContext *dc) {
    if (!dc) return;
    if (auto module = dyn_cast<ModuleDecl>(dc)) {
      out << module->getName() << ".";
    } else if (auto extension = dyn_cast<ExtensionDecl>(dc)) {
      auto *extended = extension->getExtendedNominal();
      if (extended) {
        printDecl(out, extended);
        out << ".";
      }
    } else if (auto decl = dyn_cast_or_null<ValueDecl>(dc->getAsDecl())) {
      printDecl(out, decl);
      out << ".";
    } else {
      printDeclContext(out, dc->getParent());
    }
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
  }

  return false;
}
