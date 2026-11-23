#include "lexer.hpp"
#include "token.hpp"
#include <cstdlib>

std::string IdentifierStr;
double NumVal;

/// gettok - Return the next token from standard input.
int gettok() {
	static int LastChar = ' ';
	// Set after lexing "N.." so the first '.' of the range operator is returned next.
	static bool PendingDot = false;

	if(PendingDot) {
		PendingDot = false;
		return '.';
	}

	// Skip any whitespace.
	while(isspace(LastChar)) {
		LastChar = getchar();
	}

	if(isalpha(LastChar)) { // identifier: [a-zA-Z][a-zA-Z0-9]*
		IdentifierStr = LastChar;
		while(isalnum((LastChar = getchar()))) {
			IdentifierStr += LastChar;
		}

		if(IdentifierStr == "fn") {
			return tok_fn;
		}
		if(IdentifierStr == "extern") {
			return tok_extern;
		}
		if(IdentifierStr == "if") {
			return tok_if;
		}
		if(IdentifierStr == "mut") {
			return tok_mut;
		}
		if(IdentifierStr == "else") {
			return tok_else;
		}
		if(IdentifierStr == "for") {
			return tok_for;
		}
		if(IdentifierStr == "in") {
			return tok_in;
		}
		if(IdentifierStr == "binary") {
			return tok_binary;
		}
		if(IdentifierStr == "unary") {
			return tok_unary;
		}
		if(IdentifierStr == "let") {
			return tok_let;
		}
		return tok_identifier;
	}
	if(isdigit(LastChar)) { // Number: [0-9]+ ('.' [0-9]*)?  ("1..2" is a range, not "1." then ".2")
		std::string NumStr;
		while(isdigit(LastChar)) {
			NumStr += LastChar;
			LastChar = getchar();
		}
		if(LastChar == '.') {
			int Next = getchar();
			if(Next == '.') {
				// Range operator: leave the second '.' as the current char.
				PendingDot = true;
				LastChar = Next;
			} else {
				NumStr += '.';
				LastChar = Next;
				while(isdigit(LastChar)) {
					NumStr += LastChar;
					LastChar = getchar();
				}
			}
		}

		NumVal = strtod(NumStr.c_str(), 0);
		return tok_number;
	}
	if(LastChar == '/') {
		int Next = getchar();
		if(Next == '/') {
			// Comment until end of line.
			do {
				LastChar = getchar();
			} while(LastChar != EOF && LastChar != '\n' && LastChar != '\r');

			if(LastChar != EOF) {
				return gettok();
			}
		} else {
			LastChar = Next;
			return '/';
		}
	}
	// Check for end of file.  Don't eat the EOF.
	if(LastChar == EOF) {
		return tok_eof;
	}

	// Otherwise, just return the character as its ascii value.
	int ThisChar = LastChar;
	LastChar = getchar();
	return ThisChar;
}
