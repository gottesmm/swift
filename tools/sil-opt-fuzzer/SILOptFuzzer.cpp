//===--- SILOptFuzzer.cpp -------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// This is a tool that reads in a specific SIL file and then uses libfuzzer to
/// generate new optimization orderings.
///
//===----------------------------------------------------------------------===//

#include "swift/Strings.h"
#include "swift/Subsystems.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/SILOptions.h"
#include "swift/Basic/LLVMInitialize.h"
#include "swift/Basic/LLVMContext.h"
#include "swift/Frontend/DiagnosticVerifier.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include "swift/SILOptimizer/Analysis/Analysis.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/PassManager.h"
#include "swift/Serialization/SerializedModuleLoader.h"
#include "swift/Serialization/SerializedSILLoader.h"
#include "swift/Serialization/SerializationOptions.h"
#include "swift/IRGen/IRGenPublic.h"
#include "swift/IRGen/IRGenSILPasses.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/YAMLTraits.h"
#include <cstdio>

using namespace swift;

namespace cl = llvm::cl;

static llvm::cl::opt<std::string>
InputFilename(llvm::cl::desc("input file"), llvm::cl::init("-"),
              llvm::cl::Positional);

static llvm::cl::opt<std::string>
OutputFilename("o", llvm::cl::desc("output filename"));

static llvm::cl::list<std::string>
ImportPaths("I", llvm::cl::desc("add a directory to the import search path"));

static llvm::cl::list<std::string>
FrameworkPaths("F", llvm::cl::desc("add a directory to the framework search path"));

static llvm::cl::opt<std::string>
ModuleName("module-name", llvm::cl::desc("The name of the module if processing"
                                         " a module. Necessary for processing "
                                         "stdin."));

static llvm::cl::opt<bool>
EnableResilience("enable-resilience",
                 llvm::cl::desc("Compile the module to export resilient "
                                "interfaces for all public declarations by "
                                "default"));

static llvm::cl::opt<bool>
EnableSILOwnershipOpt("enable-sil-ownership",
                 llvm::cl::desc("Compile the module with sil-ownership initially enabled for all functions"));

static llvm::cl::opt<bool>
EnableSILOpaqueValues("enable-sil-opaque-values",
                      llvm::cl::desc("Compile the module with sil-opaque-values enabled."));

static llvm::cl::opt<bool>
EnableObjCInterop("enable-objc-interop",
                  llvm::cl::desc("Enable Objective-C interoperability."));

static llvm::cl::opt<bool>
DisableObjCInterop("disable-objc-interop",
                   llvm::cl::desc("Disable Objective-C interoperability."));

static llvm::cl::opt<bool>
VerifyExclusivity("enable-verify-exclusivity",
                  llvm::cl::desc("Verify the access markers used to enforce exclusivity."));

namespace {
enum EnforceExclusivityMode {
  Unchecked, // static only
  Checked,   // static and dynamic
  DynamicOnly,
  None
};
} // end anonymous namespace

static cl::opt<EnforceExclusivityMode> EnforceExclusivity(
  "enforce-exclusivity", cl::desc("Enforce law of exclusivity "
                                  "(and support memory access markers)."),
    cl::init(EnforceExclusivityMode::Checked),
    cl::values(clEnumValN(EnforceExclusivityMode::Unchecked, "unchecked",
                          "Static checking only."),
               clEnumValN(EnforceExclusivityMode::Checked, "checked",
                          "Static and dynamic checking."),
               clEnumValN(EnforceExclusivityMode::DynamicOnly, "dynamic-only",
                          "Dynamic checking only."),
               clEnumValN(EnforceExclusivityMode::None, "none",
                          "No exclusivity checking.")));

static llvm::cl::opt<std::string>
ResourceDir("resource-dir",
    llvm::cl::desc("The directory that holds the compiler resource files"));

static llvm::cl::opt<std::string>
SDKPath("sdk", llvm::cl::desc("The path to the SDK for use with the clang "
                              "importer."),
        llvm::cl::init(""));

static llvm::cl::opt<std::string>
Target("target", llvm::cl::desc("target triple"),
       llvm::cl::init(llvm::sys::getDefaultTargetTriple()));

static llvm::cl::opt<unsigned>
AssertConfId("assert-conf-id", llvm::cl::Hidden,
             llvm::cl::init(0));

static llvm::cl::opt<int>
SILInlineThreshold("sil-inline-threshold", llvm::cl::Hidden,
                   llvm::cl::init(-1));

static llvm::cl::opt<bool>
EnableSILVerifyAll("enable-sil-verify-all",
                   llvm::cl::Hidden,
                   llvm::cl::init(true),
                   llvm::cl::desc("Run sil verifications after every pass."));

static llvm::cl::opt<std::string>
ModuleCachePath("module-cache-path", llvm::cl::desc("Clang module cache path"));

static llvm::cl::opt<bool>
EnableSILSortOutput("emit-sorted-sil", llvm::cl::Hidden,
                    llvm::cl::init(false),
                    llvm::cl::desc("Sort Functions, VTables, Globals, "
                                   "WitnessTables by name to ease diffing."));

static llvm::cl::opt<bool>
DisableASTDump("sil-disable-ast-dump", llvm::cl::Hidden,
               llvm::cl::init(false),
               llvm::cl::desc("Do not dump AST."));

static llvm::cl::opt<bool>
PerformWMO("wmo", llvm::cl::desc("Enable whole-module optimizations"));

static llvm::cl::opt<bool>
AssumeUnqualifiedOwnershipWhenParsing(
    "assume-parsing-unqualified-ownership-sil", llvm::cl::Hidden, llvm::cl::init(false),
    llvm::cl::desc("Assume all parsed functions have unqualified ownership"));

static llvm::cl::opt<bool> DisableGuaranteedNormalArguments(
    "disable-guaranteed-normal-arguments", llvm::cl::Hidden,
    llvm::cl::init(false),
    llvm::cl::desc("Assume that the input module was compiled with "
                   "-disable-guaranteed-normal-arguments enabled"));

// This function isn't referenced outside its translation unit, but it
// can't use the "static" keyword because its address is used for
// getMainExecutable (since some platforms don't support taking the
// address of main, and some platforms can't implement getMainExecutable
// without being given the address of a function in the main executable).
void anchorForGetMainExecutable() {}

/// A global that is only written to on startup.
Optional<CompilerInvocation> Invocation = None;
/// A global that is only written to on startup.
std::unique_ptr<llvm::MemoryBuffer> SourceMemoryBuffer = nullptr;
/// A global that is only written to on startup.
Optional<serialization::ExtendedValidationInfo> ExtendedInfo = None;


static std::unique_ptr<llvm::MemoryBuffer> getBufferCopy() {
  // This should be safe since we never write to the SourceMemoryBuffer except
  // in the initialization function.
  return MemoryBuffer::getMemBufferCopy(SourceMemoryBuffer->getBuffer());
}

static void runPasses(SILModule &mod, const uint8_t *data, size_t size) {
  std::vector<PassKind> passes;

  // Take log2 of the max value + 1. Then slurp off that many bytes from data
  // until we run out of size. That is how we quickly parse that data rather
  // than performing modulus in a loop.
  unsigned numBits =
    unsigned(llvm::PowerOf2Ceil(uint64_t(PassKind::AllPasses_Last) + 1));
  assert(numBits <= 8 && "For now we only support up to 8 bytes worth of passes. This can be extended with a little smarter handling to slurp data from multiple data entries");
  unsigned mask = numBits - 1;

  while (size) {
    // If we slurp bytes that are too big for us to represent data, then just
    // continue. This is just for a quick implementation. This can be improved
    // with time to speed up iteration by throwing away less test cases. Since
    // we are only reading in bytes of entropy for each pass, it shouldn't be
    // expensive that we are wasteful here.
    unsigned bits = *data & mask;
    if (bits >= PassKind::AllPasses_Last)
      continue;

    // Otherwise, add it to the passes to run.
    passes.push_back(PassKind(bits));
    ++data;
    --size;
  }

  // Ok, we know have our pass list. Lets run them and see if anything crashes!
  SILPassManager pm(mod);
  pm.executePassPipelinePlan(
      SILPassPipelinePlan::getPassPipelineForKinds(passes));
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  CompilerInstance compilerInstance;
  PrintingDiagnosticConsumer printDiags;
  compilerInstance.addDiagnosticConsumer(&printDiags);

  if (compilerInstance.setup(Invocation.get()))
    return 1;

  compilerInstance.performSema();

  // If parsing produced an error, don't run any passes.
  if (compilerInstance.getASTContext().hadError())
    return 1;

  // Load the SIL if we have a module. We have to do this after SILParse
  // creating the unfortunate double if statement.
  if (invocation.hasSerializedAST()) {
    assert(!compilerInstance.hasSILModule() &&
           "performSema() should not create a SILModule.");
    compilerInstance.setSILModule(SILModule::createEmptyModule(
        compilerInstance.getMainModule(), compilerInstance.getSILOptions(), PerformWMO));
    std::unique_ptr<SerializedSILLoader> SL = SerializedSILLoader::create(
        compilerInstance.getASTContext(), compilerInstance.getSILModule(), nullptr);

    if (extendedInfo.isSIB() || DisableSILLinking)
      SL->getAllForModule(compilerInstance.getMainModule()->getName(), nullptr);
    else
      SL->getAll();
  }

  assert(compilerInstance.getSILModule());
  compilerInstance.getSILModule()->setSerializeSILAction([]{});

  runPasses(compilerInstance.getSILModule(), data, size);

  bool hadError = compilerInstance.getASTContext().hadError();
  (void)hadError;
  return 0;
}

extern "C" LLVM_ATTRIBUTE_USED int LLVMFuzzerInitialize(
    int *argc, char ***argv) {
  PROGRAM_START(argc, argv);
  INITIALIZE_LLVM();

  CompilerInvocation Invocation;

  Invocation.setMainExecutablePath(
      llvm::sys::fs::getMainExecutable(argv[0],
          reinterpret_cast<void *>(&anchorForGetMainExecutable)));

  // Give the context the list of search paths to use for modules.
  Invocation.setImportSearchPaths(ImportPaths);
  std::vector<SearchPathOptions::FrameworkSearchPath> FramePaths;
  for (const auto &path : FrameworkPaths) {
    FramePaths.push_back({path, /*isSystem=*/false});
  }
  Invocation.setFrameworkSearchPaths(FramePaths);
  // Set the SDK path and target if given.
  if (SDKPath.getNumOccurrences() == 0) {
    const char *SDKROOT = getenv("SDKROOT");
    if (SDKROOT)
      SDKPath = SDKROOT;
  }
  if (!SDKPath.empty())
    Invocation.setSDKPath(SDKPath);
  if (!Target.empty())
    Invocation.setTargetTriple(Target);
  if (!ResourceDir.empty())
    Invocation.setRuntimeResourcePath(ResourceDir);
  Invocation.getFrontendOptions().EnableResilience = EnableResilience;
  // Set the module cache path. If not passed in we use the default swift module
  // cache.
  Invocation.getClangImporterOptions().ModuleCachePath = ModuleCachePath;
  Invocation.setParseStdlib();
  Invocation.getLangOptions().DisableAvailabilityChecking = true;
  Invocation.getLangOptions().EnableAccessControl = false;
  Invocation.getLangOptions().EnableObjCAttrRequiresFoundation = false;
  Invocation.getLangOptions().EnableObjCInterop =
    EnableObjCInterop ? true :
    DisableObjCInterop ? false : llvm::Triple(Target).isOSDarwin();

  Invocation.getLangOptions().EnableSILOpaqueValues = EnableSILOpaqueValues;

  Invocation.getLangOptions().OptimizationRemarkPassedPattern =
      createOptRemarkRegex(PassRemarksPassed);
  Invocation.getLangOptions().OptimizationRemarkMissedPattern =
      createOptRemarkRegex(PassRemarksMissed);

  // Setup the SIL Options.
  SILOptions &SILOpts = Invocation.getSILOptions();
  SILOpts.InlineThreshold = SILInlineThreshold;
  SILOpts.VerifyAll = EnableSILVerifyAll;
  SILOpts.RemoveRuntimeAsserts = RemoveRuntimeAsserts;
  SILOpts.AssertConfig = AssertConfId;
  SILOpts.OptMode = OptimizationMode::ForSpeed;
  SILOpts.EnableSILOwnership = EnableSILOwnershipOpt;
  SILOpts.AssumeUnqualifiedOwnershipWhenParsing =
    AssumeUnqualifiedOwnershipWhenParsing;
  if (EnforceExclusivity.getNumOccurrences() != 0) {
    switch (EnforceExclusivity) {
    case EnforceExclusivityMode::Unchecked:
      // This option is analogous to the -Ounchecked optimization setting.
      // It will disable dynamic checking but still diagnose statically.
      SILOpts.EnforceExclusivityStatic = true;
      SILOpts.EnforceExclusivityDynamic = false;
      break;
    case EnforceExclusivityMode::Checked:
      SILOpts.EnforceExclusivityStatic = true;
      SILOpts.EnforceExclusivityDynamic = true;
      break;
    case EnforceExclusivityMode::DynamicOnly:
      // This option is intended for staging purposes. The intent is that
      // it will eventually be removed.
      SILOpts.EnforceExclusivityStatic = false;
      SILOpts.EnforceExclusivityDynamic = true;
      break;
    case EnforceExclusivityMode::None:
      // This option is for staging purposes.
      SILOpts.EnforceExclusivityStatic = false;
      SILOpts.EnforceExclusivityDynamic = false;
      break;
    }
  }

  serialization::ExtendedValidationInfo extendedInfo;
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> FileBufOrErr =
      Invocation.setUpInputForSILTool(InputFilename, ModuleName,
                                      /*alwaysSetModuleToMain*/ false,
                                      /*bePrimary*/ !PerformWMO, extendedInfo);
  if (!FileBufOrErr) {
    fprintf(stderr, "Error! Failed to open file: %s\n", InputFilename.c_str());
    exit(-1);
  }

  // Get memory for our own purposes just to be really safe.
  SourceMemoryBuffer = MemoryBuffer::getMemBufferCopy(FileBufOrErr->getBuffer());
  ExtendedInfo = extendedInfo;

  return 0;
}
