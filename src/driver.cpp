#include "src/codegen.hpp"
#include "src/log.hpp"
#include "src/parser.hpp"
#include "src/token.hpp"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <system_error>
#include <vector>

using namespace llvm;

/// Names of the functions generated for top-level expressions .
static std::vector<std::string> TopLevelFns;

static void HandleDefinition() {
	if(auto FnAST = ParseDefinition()) {
		Log("parse", "function definition");
		if(auto *FnIR = FnAST->codegen()) {
			Log("codegen", "function '%s'", FnIR->getName().str().c_str());
		}
	} else {
		// Skip token for error recovery.
		getNextToken();
	}
}

static void HandleExtern() {
	if(auto ProtoAST = ParseExtern()) {
		Log("parse", "extern '%s'", ProtoAST->getName().c_str());
		if(ProtoAST->codegen()) {
			Log("codegen", "extern '%s'", ProtoAST->getName().c_str());
			FunctionProtos[ProtoAST->getName()] = std::move(ProtoAST);
		}
	} else {
		// Skip token for error recovery.
		getNextToken();
	}
}

static void HandleTopLevelExpression() {
	// Evaluate a top-level expression into an anonymous function.
	if(auto FnAST = ParseTopLevelExpr()) {
		Log("parse", "top-level expression");
		Function *FnIR = FnAST->codegen();
		if(FnIR) {
			// Give each top-level expression a unique name; EmitMain() calls them in order.
			std::string Name = "__anon_expr" + std::to_string(TopLevelFns.size());
			FnIR->setName(Name);
			TopLevelFns.push_back(Name);
			Log("codegen", "top-level expression as '%s'", Name.c_str());
		}
	} else {
		// Skip token for error recovery.
		getNextToken();
	}
}

/// top ::= definition | external | expression | ';'
void MainLoop() {
	while(true) {
		switch(GetCurTok()) {
			case tok_eof:
				return;
			case ';': // ignore top-level semicolons.
				getNextToken();
				break;
			case tok_fn:
				HandleDefinition();
				break;
			case tok_extern:
				HandleExtern();
				break;
			default:
				HandleTopLevelExpression();
				break;
		}
	}
}

/// EmitMain - Generate the native `int main()` entry point. It evaluates every
/// top-level expression in order, then calls the user's `def main()` if there is one,
/// printing each result. Does nothing if there is neither, so a file of only
/// definitions still compiles to a plain library object.
static bool EmitMain() {
	std::vector<std::string> Entries = TopLevelFns;

	// A jade `main` returns a double, so it can't be the native entry point itself.
	// Rename it and call it from the generated one.
	if(Function *UserMain = TheModule->getFunction("main")) {
		if(UserMain->arg_size() != 0) {
			errs() << "Error: 'main' must take no arguments\n";
			return false;
		}
		UserMain->setName("jade_main");
		Entries.push_back("jade_main");
	}
	if(Entries.empty()) {
		return true;
	}

	Type *I32 = Type::getInt32Ty(*TheContext);
	Function *Main =
	    Function::Create(FunctionType::get(I32, false), Function::ExternalLinkage, "main", TheModule.get());
	Builder->SetInsertPoint(BasicBlock::Create(*TheContext, "entry", Main));

	FunctionCallee Printf = TheModule->getOrInsertFunction(
	    "printf", FunctionType::get(I32, {PointerType::getUnqual(*TheContext)}, /*isVarArg=*/true));
	Value *Fmt = Builder->CreateGlobalString("%f\n", "fmt");

	for(const std::string &Name : Entries) {
		Value *Result = Builder->CreateCall(TheModule->getFunction(Name));
		Builder->CreateCall(Printf, {Fmt, Result});
	}
	Builder->CreateRet(ConstantInt::get(I32, 0));

	return !verifyFunction(*Main, &errs());
}

/// GenerateIR - Finish TheModule (adds the native `main`) and return its raw LLVM IR.
/// Call this once, before EmitObjectFile, which compiles that same module.
bool GenerateIR(std::string &IR) {
	Log("codegen", "generating native main");
	if(!EmitMain()) {
		return false;
	}
	raw_string_ostream IRStream(IR);
	TheModule->print(IRStream, nullptr);
	return true;
}

/// EmitObjectFile - Parse IR (possibly modified since GenerateIR) back into a module,
/// compile that module for the host target and write it to Filename.
/// Returns 0 on success, nonzero on failure.
int EmitObjectFile(const char *Filename, const std::string &IR) {
	Log("emit", "parsing IR (%zu chars)", IR.size());
	SMDiagnostic Diag;
	std::unique_ptr<Module> Parsed = parseAssemblyString(IR, Diag, *TheContext);
	if(!Parsed) {
		Diag.print("jade", errs());
		return 1;
	}
	if(verifyModule(*Parsed, &errs())) {
		return 1;
	}
	TheModule = std::move(Parsed);
	Log("emit", "IR verified");

	// Initialize the target registry etc.
	InitializeAllTargetInfos();
	InitializeAllTargets();
	InitializeAllTargetMCs();
	InitializeAllAsmParsers();
	InitializeAllAsmPrinters();

	auto TargetTriple = sys::getDefaultTargetTriple();
	TheModule->setTargetTriple(Triple(TargetTriple));

	std::string Error;
	auto Target = TargetRegistry::lookupTarget(TheModule->getTargetTriple(), Error);

	// Print an error and exit if we couldn't find the requested target.
	if(!Target) {
		errs() << Error;
		return 1;
	}

	auto CPU = "generic";
	auto Features = "";

	TargetOptions opt;
	auto TheTargetMachine = Target->createTargetMachine(Triple(TargetTriple), CPU, Features, opt, Reloc::PIC_);

	TheModule->setDataLayout(TheTargetMachine->getTargetTriple().computeDataLayout());

	std::error_code EC;
	raw_fd_ostream dest(Filename, EC, sys::fs::OF_None);

	if(EC) {
		errs() << "Could not open file: " << EC.message() << "\n";
		return 1;
	}

	legacy::PassManager pass;
	auto FileType = CodeGenFileType::ObjectFile;

	if(TheTargetMachine->addPassesToEmitFile(pass, dest, nullptr, FileType)) {
		errs() << "TheTargetMachine can't emit a file of this type\n";
		return 1;
	}

	Log("emit", "compiling for %s", TargetTriple.c_str());
	pass.run(*TheModule);
	dest.flush();

	Log("emit", "wrote %s", Filename);
	return 0;
}

//===----------------------------------------------------------------------===//
// "Library" functions that can be "extern'd" from user code.
//===----------------------------------------------------------------------===//

#ifdef _WIN32
#define DLLEXPORT __declspec(dllexport)
#else
#define DLLEXPORT
#endif

/// putchard - putchar that takes a double and returns 0.
extern "C" DLLEXPORT double putchard(double X) {
	fputc((char)X, stderr);
	return 0;
}

/// printd - printf that takes a double prints it as "%f\n", returning 0.
extern "C" DLLEXPORT double printd(double X) {
	fprintf(stderr, "%f\n", X);
	return 0;
}
