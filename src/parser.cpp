#include "parser.hpp"
#include "src/ast.hpp"
#include "src/lexer.hpp"
#include "src/token.hpp"
#include "utils.hpp"
#include <cctype>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>

static int CurTok;

std::map<char, int> BinopPrecedence;

int GetCurTok() {
	return CurTok;
}

int getNextToken() {
	return CurTok = gettok();
}

/// GetTokPrecedence - Get the precedence of the pending binary operator token.
static int GetTokPrecedence() {
	if(!isascii(CurTok)) {
		return -1;
	}

	// Make sure it's a declared binop.
	int TokPrec = BinopPrecedence[CurTok];
	if(TokPrec <= 0) {
		return -1;
	}
	return TokPrec;
}

static std::unique_ptr<ExprAST> ParseExpression();

/// numberexpr ::= number
static std::unique_ptr<ExprAST> ParseNumberExpr() {
	auto Result = std::make_unique<NumberExprAST>(NumVal);
	getNextToken(); // consume the number
	return std::move(Result);
}

/// parenexpr ::= '(' expression ')'
static std::unique_ptr<ExprAST> ParseParenExpr() {
	getNextToken(); // eat (.
	auto V = ParseExpression();
	if(!V) {
		return nullptr;
	}

	if(CurTok != ')') {
		return LogError("expected ')'");
	}
	getNextToken(); // eat ).
	return V;
}

/// identifierexpr
///   ::= identifier
///   ::= identifier '(' expression* ')'
static std::unique_ptr<ExprAST> ParseIdentifierExpr() {
	std::string IdName = IdentifierStr;

	getNextToken(); // eat identifier.

	if(CurTok != '(') { // Simple variable ref.
		return std::make_unique<VariableExprAST>(IdName);
	}

	// Call.
	getNextToken(); // eat (
	std::vector<std::unique_ptr<ExprAST>> Args;
	if(CurTok != ')') {
		while(true) {
			if(auto Arg = ParseExpression()) {
				Args.push_back(std::move(Arg));
			} else {
				return nullptr;
			}

			if(CurTok == ')') {
				break;
			}

			if(CurTok != ',') {
				return LogError("Expected ')' or ',' in argument list");
			}
			getNextToken();
		}
	}

	// Eat the ')'.
	getNextToken();

	return std::make_unique<CallExprAST>(IdName, std::move(Args));
}

static std::unique_ptr<ExprAST> ParseBlock();

/// ifexpr ::= 'if' expression block 'else' (block | ifexpr)
static std::unique_ptr<ExprAST> ParseIfExpr() {
	getNextToken(); // eat the if.

	auto Cond = ParseExpression();
	if(!Cond) {
		return nullptr;
	}

	auto Then = ParseBlock();
	if(!Then) {
		return nullptr;
	}

	if(CurTok != tok_else) {
		return LogError("expected 'else' (if is an expression and needs both branches)");
	}
	getNextToken(); // eat the else

	std::unique_ptr<ExprAST> Else;
	if(CurTok == tok_if) {
		Else = ParseIfExpr(); // else if
	} else {
		Else = ParseBlock();
	}
	if(!Else) {
		return nullptr;
	}

	return std::make_unique<IfExprAST>(std::move(Cond), std::move(Then), std::move(Else));
}

/// forexpr ::= 'for' identifier 'in' expr '..' expr ('step' expr)? block
/// The range is end-exclusive, like Rust's.
static std::unique_ptr<ExprAST> ParseForExpr() {
	getNextToken(); // eat the for.

	if(CurTok != tok_identifier) {
		return LogError("expected identifier after for");
	}

	std::string IdName = IdentifierStr;
	getNextToken(); // eat identifier.

	if(CurTok != tok_in) {
		return LogError("expected 'in' after for variable");
	}
	getNextToken(); // eat 'in'.

	auto Start = ParseExpression();
	if(!Start) {
		return nullptr;
	}
	if(CurTok != '.') {
		return LogError("expected '..' in for range");
	}
	getNextToken();
	if(CurTok != '.') {
		return LogError("expected '..' in for range");
	}
	getNextToken();

	auto Limit = ParseExpression();
	if(!Limit) {
		return nullptr;
	}
	// The loop runs while `i < limit`.
	auto End = std::make_unique<BinaryExprAST>('<', std::make_unique<VariableExprAST>(IdName), std::move(Limit));

	// The step value is optional.
	std::unique_ptr<ExprAST> Step;
	if(CurTok == tok_identifier && IdentifierStr == "step") {
		getNextToken(); // eat 'step'.
		Step = ParseExpression();
		if(!Step) {
			return nullptr;
		}
	}

	auto Body = ParseBlock();
	if(!Body) {
		return nullptr;
	}

	return std::make_unique<ForExprAST>(IdName, std::move(Start), std::move(End), std::move(Step), std::move(Body));
}

/// blockbody ::= (letstmt | expression ';' | expression)*   (up to, not including, '}')
/// `let` scopes over the rest of the block.
static std::unique_ptr<ExprAST> ParseBlockBody() {
	std::vector<std::unique_ptr<ExprAST>> Exprs;
	bool EndsWithSemi = false;

	while(CurTok != '}') {
		if(CurTok == tok_eof) {
			return LogError("expected '}' before end of file");
		}

		if(CurTok == ';') { // empty statement
			getNextToken();
			continue;
		}

		if(CurTok == tok_let) {
			getNextToken(); // eat let.
			if(CurTok == tok_mut) {
				getNextToken(); // eat mut.
			}
			if(CurTok != tok_identifier) {
				return LogError("expected identifier after let");
			}
			std::string Name = IdentifierStr;
			getNextToken(); // eat identifier.
			if(CurTok == ':') {
				getNextToken();
				if(CurTok != tok_identifier || IdentifierStr != "f64") {
					return LogError("only type 'f64' is supported");
				}
				getNextToken();
			}

			std::unique_ptr<ExprAST> Init;
			if(CurTok == '=') {
				getNextToken(); // eat '='.
				Init = ParseExpression();
				if(!Init) {
					return nullptr;
				}
			}
			if(CurTok != ';') {
				return LogError("expected ';' after let");
			}
			getNextToken(); // eat ';'.

			auto Rest = ParseBlockBody();
			if(!Rest) {
				return nullptr;
			}
			std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> VarNames;
			VarNames.push_back(std::make_pair(Name, std::move(Init)));
			Exprs.push_back(std::make_unique<VarExprAST>(std::move(VarNames), std::move(Rest)));
			return std::make_unique<BlockExprAST>(std::move(Exprs));
		}

		bool Braced = CurTok == tok_if || CurTok == tok_for || CurTok == '{';
		auto E = ParseExpression();
		if(!E) {
			return nullptr;
		}
		Exprs.push_back(std::move(E));

		EndsWithSemi = false;
		if(CurTok == ';') {
			getNextToken();
			EndsWithSemi = true;
		} else if(CurTok != '}' && !Braced) {
			return LogError("expected ';' or '}'");
		}
	}

	// A trailing ';' (or an empty block) makes the block evaluate to 0.
	if(Exprs.empty() || EndsWithSemi) {
		Exprs.push_back(std::make_unique<NumberExprAST>(0.0));
	}
	return std::make_unique<BlockExprAST>(std::move(Exprs));
}

/// block ::= '{' blockbody '}'
static std::unique_ptr<ExprAST> ParseBlock() {
	if(CurTok != '{') {
		return LogError("expected '{'");
	}
	getNextToken(); // eat '{'.

	auto Body = ParseBlockBody();
	if(!Body) {
		return nullptr;
	}

	if(CurTok != '}') {
		return LogError("expected '}'");
	}
	getNextToken(); // eat '}'.
	return Body;
}

/// primary
///   ::= identifierexpr
///   ::= numberexpr
///   ::= parenexpr
static std::unique_ptr<ExprAST> ParsePrimary() {
	switch(CurTok) {
		default:
			return LogError("unknown token when expecting an expression");
		case tok_identifier:
			return ParseIdentifierExpr();
		case tok_number:
			return ParseNumberExpr();
		case '(':
			return ParseParenExpr();
		case tok_if:
			return ParseIfExpr();
		case tok_for:
			return ParseForExpr();
		case '{':
			return ParseBlock();
	}
}

/// unary
///   ::= primary
///   ::= '!' unary
static std::unique_ptr<ExprAST> ParseUnary() {
	// If the current token is not an operator, it must be a primary expr.
	if(!isascii(CurTok) || CurTok == '(' || CurTok == '{' || CurTok == ',') {
		return ParsePrimary();
	}

	// If this is a unary operator, read it.
	int Opc = CurTok;
	getNextToken();
	if(auto Operand = ParseUnary()) {
		return std::make_unique<UnaryExprAST>(Opc, std::move(Operand));
	}
	return nullptr;
}

/// binoprhs
///   ::= ('+' unary)*
static std::unique_ptr<ExprAST> ParseBinOpRHS(int ExprPrec, std::unique_ptr<ExprAST> LHS) {
	// If this is a binop, find its precedence.
	while(true) {
		int TokPrec = GetTokPrecedence();

		// If this is a binop that binds at least as tightly as the current binop,
		// consume it, otherwise we are done.
		if(TokPrec < ExprPrec) {
			return LHS;
		}

		// Okay, we know this is a binop.
		int BinOp = CurTok;
		getNextToken(); // eat binop

		// Parse the unary expression after the binary operator.
		auto RHS = ParseUnary();
		if(!RHS) {
			return nullptr;
		}

		// If BinOp binds less tightly with RHS than the operator after RHS, let
		// the pending operator take RHS as its LHS.
		int NextPrec = GetTokPrecedence();
		if(TokPrec < NextPrec) {
			RHS = ParseBinOpRHS(TokPrec + 1, std::move(RHS));
			if(!RHS) {
				return nullptr;
			}
		}

		// Merge LHS/RHS.
		LHS = std::make_unique<BinaryExprAST>(BinOp, std::move(LHS), std::move(RHS));
	}
}

/// expression
///   ::= unary binoprhs
///
static std::unique_ptr<ExprAST> ParseExpression() {
	auto LHS = ParseUnary();
	if(!LHS) {
		return nullptr;
	}

	return ParseBinOpRHS(0, std::move(LHS));
}

/// prototype
///   ::= id '(' (id (':' type)? ',')* ')' ('->' type)?
///   ::= binary LETTER number? (id, id)
///   ::= unary LETTER (id)
static std::unique_ptr<PrototypeAST> ParsePrototype() {
	std::string FnName;

	unsigned Kind = 0; // 0 = identifier, 1 = unary, 2 = binary.
	unsigned BinaryPrecedence = 30;

	switch(CurTok) {
		default:
			return LogErrorP("Expected function name in prototype");
		case tok_identifier:
			FnName = IdentifierStr;
			Kind = 0;
			getNextToken();
			break;
		case tok_unary:
			getNextToken();
			if(!isascii(CurTok)) {
				return LogErrorP("Expected unary operator");
			}
			FnName = "unary";
			FnName += (char)CurTok;
			Kind = 1;
			getNextToken();
			break;
		case tok_binary:
			getNextToken();
			if(!isascii(CurTok)) {
				return LogErrorP("Expected binary operator");
			}
			FnName = "binary";
			FnName += (char)CurTok;
			Kind = 2;
			getNextToken();

			// Read the precedence if present.
			if(CurTok == tok_number) {
				if(NumVal < 1 || NumVal > 100) {
					return LogErrorP("Invalid precedence: must be 1..100");
				}
				BinaryPrecedence = (unsigned)NumVal;
				getNextToken();
			}
			break;
	}

	if(CurTok != '(') {
		return LogErrorP("Expected '(' in prototype");
	}

	std::vector<std::string> ArgNames;
	getNextToken(); // eat '('.
	while(CurTok == tok_identifier) {
		ArgNames.push_back(IdentifierStr);
		getNextToken(); // eat the name.
		if(CurTok == ':') {
			getNextToken();
			if(CurTok != tok_identifier || IdentifierStr != "f64") {
				return LogErrorP("only type 'f64' is supported");
			}
			getNextToken();
		}
		if(CurTok != ',') {
			break;
		}
		getNextToken(); // eat ','.
	}
	if(CurTok != ')') {
		return LogErrorP("Expected ')' in prototype");
	}

	// success.
	getNextToken(); // eat ')'.

	// Optional return type.
	if(CurTok == '-') {
		getNextToken();
		if(CurTok != '>') {
			return LogErrorP("Expected '->' before return type");
		}
		getNextToken();
		if(CurTok != tok_identifier || IdentifierStr != "f64") {
			return LogErrorP("only type 'f64' is supported");
		}
		getNextToken();
	}

	// Verify right number of names for operator.
	if(Kind && ArgNames.size() != Kind) {
		return LogErrorP("Invalid number of operands for operator");
	}

	return std::make_unique<PrototypeAST>(FnName, std::move(ArgNames), Kind != 0, BinaryPrecedence);
}

/// definition ::= 'fn' prototype block
std::unique_ptr<FunctionAST> ParseDefinition() {
	getNextToken(); // eat fn.
	auto Proto = ParsePrototype();
	if(!Proto) {
		return nullptr;
	}

	auto E = ParseBlock();
	if(!E) {
		return nullptr;
	}

	return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
}

/// toplevelexpr ::= expression
std::unique_ptr<FunctionAST> ParseTopLevelExpr() {
	if(auto E = ParseExpression()) {
		// Make an anonymous proto.
		auto Proto = std::make_unique<PrototypeAST>("__anon_expr", std::vector<std::string>());
		return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
	}
	return nullptr;
}

/// external ::= 'extern' 'fn' prototype ';'
std::unique_ptr<PrototypeAST> ParseExtern() {
	getNextToken(); // eat extern.
	if(CurTok != tok_fn) {
		return LogErrorP("Expected 'fn' after extern");
	}
	getNextToken(); // eat fn.
	return ParsePrototype();
}
