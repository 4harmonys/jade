
#include "codegen.hpp"
#include "driver.hpp"
#include "parser.hpp"
#include "src/driver.hpp"

#include <cstdio>

int main(void) {
	InitializeNativeTarget();
	InitializeNativeTargetAsmPrinter();
	InitializeNativeTargetAsmParser();

	// 1 is lowest, >1 == higher precedence.
	BinopPrecedence['='] = 2;
	BinopPrecedence['<'] = 10;
	BinopPrecedence['+'] = 20;
	BinopPrecedence['-'] = 20;
	BinopPrecedence['*'] = 40; // highest.

	// Prime the first token.
	fprintf(stderr, "ready> ");
	getNextToken();

	TheJIT = ExitOnErr(KaleidoscopeJIT::Create());

	InitializeModuleAndManagers();

	// Run the main "interpreter loop" now.
	MainLoop();
}
