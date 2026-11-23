#include "codegen.hpp"
#include "driver.hpp"
#include "parser.hpp"

#include <cstdio>
#include <cstring>

static void PrintUsage(FILE *Out) {
	fprintf(Out, "usage: jade [--jit] [-o <output.o>] [<file.jade>]\n"
	             "\n"
	             "  <file.jade>   source file to read (default: stdin)\n"
	             "  --jit         run the program with the JIT instead of writing an object file\n"
	             "  -o <file>     object file to write when not using --jit (default: output.o)\n"
	             "\n"
	             "Entry point: top-level expressions and `def main()` run in order; each result is printed.\n"
	             "  -h, --help    show this help\n");
}

int main(int argc, char **argv) {
	const char *InputPath = nullptr;
	const char *OutputPath = "output.o";

	for(int i = 1; i < argc; ++i) {
		if(!strcmp(argv[i], "--jit")) {
			JitMode = true;
		} else if(!strcmp(argv[i], "-o")) {
			if(++i >= argc) {
				fprintf(stderr, "jade: -o needs a file name\n");
				return 1;
			}
			OutputPath = argv[i];
		} else if(!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			PrintUsage(stdout);
			return 0;
		} else if(argv[i][0] == '-' && argv[i][1] != '\0') {
			fprintf(stderr, "jade: unknown option '%s'\n", argv[i]);
			PrintUsage(stderr);
			return 1;
		} else if(InputPath) {
			fprintf(stderr, "jade: only one input file is supported\n");
			return 1;
		} else {
			InputPath = argv[i];
		}
	}

	// The lexer reads from stdin, so point stdin at the input file.
	if(InputPath && !freopen(InputPath, "r", stdin)) {
		perror(InputPath);
		return 1;
	}

	// 1 is lowest, >1 == higher precedence.
	BinopPrecedence['='] = 2;
	BinopPrecedence['<'] = 10;
	BinopPrecedence['+'] = 20;
	BinopPrecedence['-'] = 20;
	BinopPrecedence['*'] = 40; // highest.

	// Only show the REPL prompt when interpreting from the terminal.
	bool Prompt = JitMode && !InputPath;

	if(JitMode) {
		InitializeNativeTarget();
		InitializeNativeTargetAsmPrinter();
		InitializeNativeTargetAsmParser();
		TheJIT = ExitOnErr(KaleidoscopeJIT::Create());
	}

	if(Prompt) {
		fprintf(stderr, "ready> ");
	}
	getNextToken();

	InitializeModuleAndManagers();

	MainLoop(Prompt);

	if(JitMode) {
		return 0;
	}
	return EmitObjectFile(OutputPath);
}
