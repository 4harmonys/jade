#include "src/codegen.hpp"
#include "src/parser.hpp"
#include "src/token.hpp"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>

using namespace llvm;

static void HandleDefinition() {
	if(auto FnAST = ParseDefinition()) {
		if(auto *FnIR = FnAST->codegen()) {
			fprintf(stderr, "Read function definition:");
			FnIR->print(errs());
			fprintf(stderr, "\n");
			ExitOnErr(TheJIT->addModule(ThreadSafeModule(std::move(TheModule), std::move(TheContext))));
			InitializeModuleAndManagers();
		}
	} else {
		// Skip token for error recovery.
		getNextToken();
	}
}

static void HandleExtern() {
	if(auto ProtoAST = ParseExtern()) {
		if(auto *FnIR = ProtoAST->codegen()) {
			fprintf(stderr, "Read extern: ");
			FnIR->print(errs());
			fprintf(stderr, "\n");
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
		if(FnAST->codegen()) {
			// Create a ResourceTracker to track JIT'd memory allocated to our
			// anonymous expression -- that way we can free it after executing.
			auto RT = TheJIT->getMainJITDylib().createResourceTracker();

			auto TSM = ThreadSafeModule(std::move(TheModule), std::move(TheContext));
			ExitOnErr(TheJIT->addModule(std::move(TSM), RT));
			InitializeModuleAndManagers();

			// Search the JIT for the __anon_expr symbol.
			auto ExprSymbol = ExitOnErr(TheJIT->lookup("__anon_expr"));

			// Get the symbol's address and cast it to the right type (takes no
			// arguments, returns a double) so we can call it as a native function.
			double (*FP)() = ExprSymbol.getAddress().toPtr<double (*)()>();
			fprintf(stderr, "Evaluated to %f\n", FP());

			// Delete the anonymous expression module from the JIT.
			ExitOnErr(RT->remove());
		}
	} else {
		// Skip token for error recovery.
		getNextToken();
	}
}

/// top ::= definition | external | expression | ';'
void MainLoop() {
	while(true) {
		fprintf(stderr, "ready> ");
		switch(GetCurTok()) {
			case tok_eof:
				return;
			case ';': // ignore top-level semicolons.
				getNextToken();
				break;
			case tok_def:
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
