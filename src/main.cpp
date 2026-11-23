#include "ai.hpp"
#include "codegen.hpp"
#include "driver.hpp"
#include "log.hpp"
#include "parser.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <curl/curl.h>
#include <optional>
#include <string>

static void PrintUsage(FILE *Out) {
	fprintf(Out, "usage: jade [-c] [-o <output>] [<file.jade>]\n"
	             "\n"
	             "  <file.jade>   source file to read (default: stdin)\n"
	             "  -o <file>     file to write (default: a.out, or output.o with -c)\n"
	             "  -c            compile to an object file only, don't link\n"
	             "\n"
	             "Entry point: top-level expressions and `def main()` run in order; each result is printed.\n"
	             "  -h, --help    show this help\n");
}

const std::string prompt = "Optimize this LLVM IR without changing its behavior. Reply with only the complete "
                           "modified IR, no explanation and no markdown fences.\n\n";

static std::string ModifyIR(AnthropicClient &AI, const std::string &IR) {
	std::string Reply = AI.ask(prompt + IR, 128000); // rip 😭

	return Reply;
}

/// Quote a path for the shell.
static std::string ShellQuote(const std::string &S) {
	std::string Out = "'";
	for(char C : S) {
		Out += C == '\'' ? std::string("'\\''") : std::string(1, C);
	}
	return Out + "'";
}

/// Link the object into an executable with the system C compiler driver (pulls in libc for printf).
static int LinkExecutable(const std::string &Object, const std::string &Output) {
	Log("link", "cc %s -> %s", Object.c_str(), Output.c_str());
	std::string Cmd = "cc " + ShellQuote(Object) + " -o " + ShellQuote(Output);
	if(std::system(Cmd.c_str()) != 0) {
		Log("link", "failed");
		return 1;
	}
	Log("link", "wrote %s", Output.c_str());
	return 0;
}

int main(int argc, char **argv) {
	const char *InputPath = nullptr;
	const char *OutputPath = nullptr;
	bool CompileOnly = false;

	for(int i = 1; i < argc; ++i) {
		if(!strcmp(argv[i], "-o")) {
			if(++i >= argc) {
				fprintf(stderr, "jade: -o needs a file name\n");
				return 1;
			}
			OutputPath = argv[i];
		} else if(!strcmp(argv[i], "-c")) {
			CompileOnly = true;
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

	if(!OutputPath) {
		OutputPath = CompileOnly ? "output.o" : "a.out";
	}

	std::optional<AnthropicClient> AI;
	if(const char *ApiKey = std::getenv("ANTHROPIC_API_KEY_COLON_THREE")) {
		curl_global_init(CURL_GLOBAL_DEFAULT);
		AI.emplace(ApiKey);
	} else {
		Log("ai", "ANTHROPIC_API_KEY_COLON_THREE not set, skipping");
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

	Log("lex", "reading %s", InputPath ? InputPath : "stdin");
	getNextToken();

	InitializeModuleAndManagers();

	Log("parse", "parsing and generating code");
	MainLoop();

	// Generate the raw LLVM IR, let it be modified, then emit the object from the result.
	std::string RawIR;
	int Status = GenerateIR(RawIR) ? 0 : 1;

	if(Status == 0 && AI) {
		try {
			Log("ai", "asking model to optimize IR");
			RawIR = ModifyIR(*AI, RawIR);
		} catch(const std::exception &error) {
			Log("ai", "failed (%s), using unmodified IR", error.what());
		}
	}

	if(Status == 0) {
		std::string ObjectPath = CompileOnly ? OutputPath : std::string(OutputPath) + ".o";
		Status = EmitObjectFile(ObjectPath.c_str(), RawIR);
		if(Status == 0 && !CompileOnly) {
			Status = LinkExecutable(ObjectPath, OutputPath);
			std::remove(ObjectPath.c_str());
		}
	}

	if(AI) {
		curl_global_cleanup();
	}

	return Status;
}
