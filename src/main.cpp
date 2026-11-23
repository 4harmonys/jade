#include "ai.hpp"
#include "codegen.hpp"
#include "driver.hpp"
#include "log.hpp"
#include "parser.hpp"
#include <cstdio>
#include <cstring>
#include <curl/curl.h>
#include <optional>
#include <string>

static void PrintUsage(FILE *Out) {
	fprintf(Out, "usage: jade [-o <output.o>] [<file.jade>]\n"
	             "\n"
	             "  <file.jade>   source file to read (default: stdin)\n"
	             "  -o <file>     object file to write (default: output.o)\n"
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

int main(int argc, char **argv) {
	const char *InputPath = nullptr;
	const char *OutputPath = "output.o";

	for(int i = 1; i < argc; ++i) {
		if(!strcmp(argv[i], "-o")) {
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
		Status = EmitObjectFile(OutputPath, RawIR);
	}

	if(AI) {
		curl_global_cleanup();
	}

	return Status;
}
