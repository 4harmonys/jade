#pragma once

#include <string>

void MainLoop();
/// Finishes the module (including the generated `main`) and stores its raw LLVM IR in IR.
/// Returns false on failure. Must be called before EmitObjectFile.
bool GenerateIR(std::string &IR);
/// Parses IR (the output of GenerateIR, optionally modified) and compiles it into an
/// object file. The object is emitted from this text, not from the in-memory module.
int EmitObjectFile(const char *Filename, const std::string &IR);
