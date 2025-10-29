/*
 *  Slipper: a small driver to run Dyck alias analysis on LLVM IR/bitcode
 *  Based on canary.cpp
 */

#include <llvm/Bitcode/BitcodeWriterPass.h>
#include <llvm/IR/IRPrintingPasses.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/InitializePasses.h>
#include <llvm/Pass.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/ToolOutputFile.h>
#include <llvm/Support/raw_ostream.h>
#include <memory>

#include "DyckAA/DyckAliasAnalysis.h"

using namespace llvm;

static cl::opt<std::string> InputFilename(cl::Positional, cl::desc("<input bitcode or IR file>"), cl::init("-"), cl::value_desc("filename"));

static cl::opt<bool> OutputAssembly("S", cl::desc("Write output as LLVM assembly"), cl::init(false));

static cl::opt<std::string> OutputFilename("o", cl::desc("<output file>"), cl::init(""), cl::value_desc("filename"));

int main(int argc, char **argv) {
    InitLLVM X(argc, argv);

    llvm::EnableDebugBuffering = true;

    sys::PrintStackTraceOnErrorSignal(argv[0]);
    PrettyStackTraceProgram Z(argc, argv);

    PassRegistry &Registry = *PassRegistry::getPassRegistry();
    initializeCore(Registry);
    initializeScalarOpts(Registry);
    initializeIPO(Registry);
    initializeAnalysis(Registry);
    initializeTransformUtils(Registry);
    initializeInstCombine(Registry);
    initializeTarget(Registry);

    cl::ParseCommandLineOptions(argc, argv, "Run Dyck alias analysis on an LLVM bitcode/IR module.\n");

    SMDiagnostic Err;
    LLVMContext Context;
    std::unique_ptr<Module> M = parseIRFile(InputFilename.getValue(), Err, Context);
    if (!M) {
        Err.print(argv[0], errs());
        return 1;
    }

    if (verifyModule(*M, &errs())) {
        errs() << argv[0] << ": error: input module is broken!\n";
        return 1;
    }

    legacy::PassManager Passes;

    // Add the Dyck alias analysis pass
    Passes.add(new DyckAliasAnalysis());

    // If user asked to emit assembly/bitcode, allow printing the module after analysis
    std::unique_ptr<ToolOutputFile> Out;
    if (!OutputFilename.getValue().empty()) {
        std::error_code EC;
        Out = std::make_unique<ToolOutputFile>(OutputFilename, EC, sys::fs::OpenFlags::OF_None);
        if (EC) {
            errs() << EC.message() << '\n';
            return 1;
        }

        if (OutputAssembly.getValue()) {
            Passes.add(createPrintModulePass(Out->os()));
        } else {
            Passes.add(createBitcodeWriterPass(Out->os()));
        }
    }

    Passes.run(*M);

    if (Out)
        Out->keep();

    return 0;
}
