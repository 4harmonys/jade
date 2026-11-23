#pragma once

#include <cstdarg>
#include <cstdio>

/// Log - print a pipeline step to stderr as "[stage] message".
/// Stages: lex, parse, codegen, ai, emit.
inline void Log(const char *Stage, const char *Fmt, ...) __attribute__((format(printf, 2, 3)));
inline void Log(const char *Stage, const char *Fmt, ...) {
	fprintf(stderr, "[%s] ", Stage);
	va_list Args;
	va_start(Args, Fmt);
	vfprintf(stderr, Fmt, Args);
	va_end(Args);
	fputc('\n', stderr);
}
