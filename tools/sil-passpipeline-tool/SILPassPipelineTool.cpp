//===--- SILPassPipelineTool.cpp ------------------------------------------===//
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
///
/// \file
///
/// This is a simple tool that is able to dump out a yaml description of one of
/// the current list of pass pipelines and validate hand written pass
/// pipeline. Meant to be used to script on top of sil-opt and swiftc.
///
//===----------------------------------------------------------------------===//

#include "swift/Basic/LLVM.h"
#include "swift/Basic/LLVMInitialize.h"
#include "swift/SILOptimizer/PassManager/PassPipeline.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/CommandLine.h"

using namespace swift;

namespace {

enum class Action {
  Invalid,
  DumpPredefinedPipelines,
  DumpPassKinds,
  VerifyInput,
};

} // end anonymous namespace

namespace llvm {

llvm::raw_ostream &operator<<(llvm::raw_ostream &os, PassPipelineKind Kind) {
  switch (Kind) {
#define PASSPIPELINE(NAME, DESCRIPTION)                                        \
  case PassPipelineKind::NAME:                                                 \
    return os << #NAME;
#include "swift/SILOptimizer/PassManager/PassPipeline.def"
  }
}

} // namespace llvm

static void dumpPredefinedPipelines() {
  // TODO: add options to manipulate this.
  SILOptions Opt;

  SILPassPipelinePlan Plan;

#define PASSPIPELINE(NAME, DESCRIPTION)                                 \
  Plan.appendPipelinePlan(SILPassPipelinePlan::get##NAME##PassPipeline());
#define PASSPIPELINE_WITH_OPTIONS(NAME, DESCRIPTION)                    \
  Plan.appendPipelinePlan(SILPassPipelinePlan::get##NAME##PassPipeline(Opt));
#include "swift/SILOptimizer/PassManager/PassPipeline.def"

  Plan.print(llvm::outs());
}

static void dumpPassKinds() {
#define PASS(ID, TAG, NAME) llvm::outs() << #ID << '\n';
#include "swift/SILOptimizer/PassManager/Passes.def" 
}

static void verifyPipelinePlanParses() {
  auto plan = SILPassPipelinePlan::getPassPipelineFromFile("-");
  if (!plan) {
    llvm::outs() << "Error! Failed to parse pipeline plan!\n";
    exit(1);
  }

  llvm::outs() << "Verified pipeline plan parsed correctly. Dumping!\n";
  plan.getValue().print(llvm::outs());
}

static void printHelp(StringRef name) {
  llvm::outs() << name << " <action>


<action> = dump-predefined-pipelines | dump-pass-kinds | verify-input
dump-predefined-pipelines - Dumps all pipelines used during normal
optimization. Useful as a starting point for a custom pass-pipeline.

dump-pass-kinds - Dump all known pass kinds including utility passes that are
not used in any predefined pipelines.

verify-input - Given a custom pass pipeline file, asserts that the pass pipeline
file is well formed and then echo back the parsed state. This allows for
validation that the compiler was able to properly understand the custom pass
pipeline."
    ;
}

static Action getAction(int argc, char **argv) {
  StringRef progName(argv[0]);

  if (argc != 2) {
    llvm::errs() << "Error! Must have exactly 1 argument!\n";
    printHelp(progName);
    exit(1);
  }

  StringRef actionStr(argv[1]);

  auto parsedAction = llvm::StringSwitch<Action>(actionStr)
    .Case("dump-predefined-pipelines", Action::DumpPredefinedPipelines)
    .Case("dump-pass-kinds", Action::DumpPassKinds)
    .Case("verify-input", Action::VerifyInput)
    .Default(Action::Invalid);

  if (parsedAction == Action::Invalid) {
    llvm::errs() << "Error! Invalid action: '" << actionStr << '\n';
    printHelp(progName);
    exit(1);
  }

  return parsedAction;
}

int main(int argc, char **argv) {
  INITIALIZE_LLVM(argc, argv);

  llvm::cl::ParseCommandLineOptions(argc, argv,
                                    "Swift SIL Pass Pipeline Tool\n");

  Action selectedAction = getAction(argc, argv);

  if (selectedAction == Action::DumpPredefinedPipelines) {
    dumpPredefinedPipelines();
    return 0;
  }

  if (selectedAction == Action::DumpPassKinds) {
    dumpPassKinds();
    return 0;
  }

  assert(selectedAction == Action::VerifyInput);
  verifyPipelinePlanParses();
  return 0;
};
