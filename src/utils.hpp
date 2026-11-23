#pragma once

/// LogError* - These are little helper functions for error handling.
#include "src/ast.hpp"
#include <cstdio>
#include <memory>
inline std::unique_ptr<ExprAST> LogError(const char *Str) {
	fprintf(stderr, "Error: %s\n", Str);
	return nullptr;
}
inline std::unique_ptr<PrototypeAST> LogErrorP(const char *Str) {
	LogError(Str);
	return nullptr;
}
