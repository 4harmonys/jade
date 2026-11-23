#pragma once

#include "ast.hpp"
#include <map>
#include <memory>

std::unique_ptr<FunctionAST> ParseDefinition();
int getNextToken();
std::unique_ptr<PrototypeAST> ParseExtern();
std::unique_ptr<FunctionAST> ParseTopLevelExpr();
int GetCurTok();

/// BinopPrecedence - This holds the precedence for each binary operator that is
/// defined.
extern std::map<char, int> BinopPrecedence;
