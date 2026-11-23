#include "codegen.hpp"
#include "src/parser.hpp"
#include "src/utils.hpp"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include <cctype>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>

using namespace llvm;

std::unique_ptr<LLVMContext> TheContext;
std::unique_ptr<Module> TheModule;
std::unique_ptr<IRBuilder<>> Builder;
std::map<std::string, AllocaInst *> NamedValues;
std::unique_ptr<KaleidoscopeJIT> TheJIT;
bool JitMode = false;
std::unique_ptr<FunctionPassManager> TheFPM;
std::unique_ptr<LoopAnalysisManager> TheLAM;
std::unique_ptr<FunctionAnalysisManager> TheFAM;
std::unique_ptr<CGSCCAnalysisManager> TheCGAM;
std::unique_ptr<ModuleAnalysisManager> TheMAM;
std::unique_ptr<PassInstrumentationCallbacks> ThePIC;
std::unique_ptr<StandardInstrumentations> TheSI;
std::map<std::string, std::unique_ptr<PrototypeAST>> FunctionProtos;
ExitOnError ExitOnErr;

Function *getFunction(std::string Name) {
	// First, see if the function has already been added to the current module.
	if(auto *F = TheModule->getFunction(Name)) {
		return F;
	}

	// If not, check whether we can codegen the declaration from some existing
	// prototype.
	auto FI = FunctionProtos.find(Name);
	if(FI != FunctionProtos.end()) {
		return FI->second->codegen();
	}

	// If no existing prototype exists, return null.
	return nullptr;
}

Value *LogErrorV(const char *Str) {
	LogError(Str);
	return nullptr;
}

/// CreateEntryBlockAlloca - Create an alloca instruction in the entry block of
/// the function.  This is used for mutable variables etc.
static AllocaInst *CreateEntryBlockAlloca(Function *TheFunction, StringRef VarName) {
	IRBuilder<> TmpB(&TheFunction->getEntryBlock(), TheFunction->getEntryBlock().begin());
	return TmpB.CreateAlloca(Type::getDoubleTy(*TheContext), nullptr, VarName);
}

Value *NumberExprAST::codegen() {
	return ConstantFP::get(*TheContext, APFloat(Val));
}

Value *VariableExprAST::codegen() {
	// Look this variable up in the function.
	AllocaInst *A = NamedValues[Name];
	if(!A) {
		return LogErrorV("Unknown variable name");
	}

	// Load the value.
	return Builder->CreateLoad(A->getAllocatedType(), A, Name.c_str());
}

Value *UnaryExprAST::codegen() {
	Value *OperandV = Operand->codegen();
	if(!OperandV) {
		return nullptr;
	}

	Function *F = getFunction(std::string("unary") + Opcode);
	if(!F) {
		return LogErrorV("Unknown unary operator");
	}

	return Builder->CreateCall(F, OperandV, "unop");
}

Value *BinaryExprAST::codegen() {
	// Special case '=' because we don't want to emit the LHS as an expression.
	if(Op == '=') {
		// Assignment requires the LHS to be an identifier.
		const std::string *LHSName = LHS->variableName();
		if(!LHSName) {
			return LogErrorV("destination of '=' must be a variable");
		}

		Value *Val = RHS->codegen();
		if(!Val) {
			return nullptr;
		}

		AllocaInst *Alloca = NamedValues[*LHSName];
		if(!Alloca) {
			return LogErrorV("Unknown variable name");
		}

		Builder->CreateStore(Val, Alloca);
		return Val;
	}

	Value *L = LHS->codegen();
	Value *R = RHS->codegen();
	if(!L || !R) {
		return nullptr;
	}

	switch(Op) {
		case '+':
			return Builder->CreateFAdd(L, R, "addtmp");
		case '-':
			return Builder->CreateFSub(L, R, "subtmp");
		case '*':
			return Builder->CreateFMul(L, R, "multmp");
		case '<':
			L = Builder->CreateFCmpULT(L, R, "cmptmp");
			// Convert bool 0/1 to double 0.0 or 1.0
			return Builder->CreateUIToFP(L, Type::getDoubleTy(*TheContext), "booltmp");
		default:
			break;
	}

	// If it wasn't a builtin binary operator, it must be a user defined one. Emit
	// a call to it.
	Function *F = getFunction(std::string("binary") + Op);
	if(!F) {
		return LogErrorV("binary operator not found");
	}

	Value *Ops[] = {L, R};
	return Builder->CreateCall(F, Ops, "binop");
}

Value *CallExprAST::codegen() {
	// Look up the name in the global module table.
	Function *CalleeF = getFunction(Callee);
	if(!CalleeF) {
		return LogErrorV("Unknown function referenced");
	}

	// If argument mismatch error.
	if(CalleeF->arg_size() != Args.size()) {
		return LogErrorV("Incorrect # arguments passed");
	}

	std::vector<Value *> ArgsV;
	for(unsigned i = 0, e = Args.size(); i != e; ++i) {
		ArgsV.push_back(Args[i]->codegen());
		if(!ArgsV.back()) {
			return nullptr;
		}
	}

	return Builder->CreateCall(CalleeF, ArgsV, "calltmp");
}

Value *IfExprAST::codegen() {
	Value *CondV = Cond->codegen();
	if(!CondV) {
		return nullptr;
	}

	// Convert condition to a bool by comparing non-equal to 0.0.
	CondV = Builder->CreateFCmpONE(CondV, ConstantFP::get(*TheContext, APFloat(0.0)), "ifcond");

	Function *TheFunction = Builder->GetInsertBlock()->getParent();

	BasicBlock *ThenBB = BasicBlock::Create(*TheContext, "then", TheFunction);
	BasicBlock *ElseBB = BasicBlock::Create(*TheContext, "else");
	BasicBlock *MergeBB = BasicBlock::Create(*TheContext, "ifcont");

	Builder->CreateCondBr(CondV, ThenBB, ElseBB);

	// Emit then value.
	Builder->SetInsertPoint(ThenBB);
	Value *ThenV = Then->codegen();
	if(!ThenV) {
		return nullptr;
	}
	Builder->CreateBr(MergeBB);
	// Codegen of 'Then' can change the current block, update ThenBB for the PHI.
	ThenBB = Builder->GetInsertBlock();

	// Emit else block.
	TheFunction->insert(TheFunction->end(), ElseBB);
	Builder->SetInsertPoint(ElseBB);
	Value *ElseV = Else->codegen();
	if(!ElseV) {
		return nullptr;
	}
	Builder->CreateBr(MergeBB);
	ElseBB = Builder->GetInsertBlock();

	// Emit merge block.
	TheFunction->insert(TheFunction->end(), MergeBB);
	Builder->SetInsertPoint(MergeBB);
	PHINode *PN = Builder->CreatePHI(Type::getDoubleTy(*TheContext), 2, "iftmp");

	PN->addIncoming(ThenV, ThenBB);
	PN->addIncoming(ElseV, ElseBB);
	return PN;
}

// Output for-loop as:
//   var = alloca double
//   ...
//   start = startexpr
//   store start -> var
//   goto loop
// loop:
//   ...
//   bodyexpr
//   ...
// loopend:
//   step = stepexpr
//   endcond = endexpr
//
//   curvar = load var
//   nextvar = curvar + step
//   store nextvar -> var
//   br endcond, loop, endloop
// outloop:
Value *ForExprAST::codegen() {
	Function *TheFunction = Builder->GetInsertBlock()->getParent();

	// Create an alloca for the variable in the entry block.
	AllocaInst *Alloca = CreateEntryBlockAlloca(TheFunction, VarName);

	// Emit the start code first, without 'variable' in scope.
	Value *StartVal = Start->codegen();
	if(!StartVal) {
		return nullptr;
	}

	// Store the value into the alloca.
	Builder->CreateStore(StartVal, Alloca);

	BasicBlock *LoopBB = BasicBlock::Create(*TheContext, "loop", TheFunction);

	// Explicit fall through from the current block to the LoopBB.
	Builder->CreateBr(LoopBB);
	Builder->SetInsertPoint(LoopBB);

	// If the loop variable shadows an existing variable, save it to restore later.
	AllocaInst *OldVal = NamedValues[VarName];
	NamedValues[VarName] = Alloca;

	// Emit the body; its value is ignored, but it must not error.
	if(!Body->codegen()) {
		return nullptr;
	}

	Value *StepVal = nullptr;
	if(Step) {
		StepVal = Step->codegen();
		if(!StepVal) {
			return nullptr;
		}
	} else {
		StepVal = ConstantFP::get(*TheContext, APFloat(1.0));
	}

	// Compute the end condition.
	Value *EndCond = End->codegen();
	if(!EndCond) {
		return nullptr;
	}

	// Reload, increment, and restore the alloca.  This handles the case where
	// the body of the loop mutates the variable.
	Value *CurVar = Builder->CreateLoad(Alloca->getAllocatedType(), Alloca, VarName.c_str());
	Value *NextVar = Builder->CreateFAdd(CurVar, StepVal, "nextvar");
	Builder->CreateStore(NextVar, Alloca);

	EndCond = Builder->CreateFCmpONE(EndCond, ConstantFP::get(*TheContext, APFloat(0.0)), "loopcond");

	BasicBlock *AfterBB = BasicBlock::Create(*TheContext, "afterloop", TheFunction);

	Builder->CreateCondBr(EndCond, LoopBB, AfterBB);
	Builder->SetInsertPoint(AfterBB);

	// Restore the unshadowed variable.
	if(OldVal) {
		NamedValues[VarName] = OldVal;
	} else {
		NamedValues.erase(VarName);
	}

	// for expr always returns 0.0.
	return Constant::getNullValue(Type::getDoubleTy(*TheContext));
}

Value *VarExprAST::codegen() {
	std::vector<AllocaInst *> OldBindings;

	Function *TheFunction = Builder->GetInsertBlock()->getParent();

	// Register all variables and emit their initializer.
	for(unsigned i = 0, e = VarNames.size(); i != e; ++i) {
		const std::string &VarName = VarNames[i].first;
		ExprAST *Init = VarNames[i].second.get();

		// Emit the initializer before adding the variable to scope, so it can't
		// reference the variable itself (`var a = a in ...` refers to outer 'a').
		Value *InitVal;
		if(Init) {
			InitVal = Init->codegen();
			if(!InitVal) {
				return nullptr;
			}
		} else { // If not specified, use 0.0.
			InitVal = ConstantFP::get(*TheContext, APFloat(0.0));
		}

		AllocaInst *Alloca = CreateEntryBlockAlloca(TheFunction, VarName);
		Builder->CreateStore(InitVal, Alloca);

		// Remember the old binding so we can restore it when leaving scope.
		OldBindings.push_back(NamedValues[VarName]);
		NamedValues[VarName] = Alloca;
	}

	// Codegen the body, now that all vars are in scope.
	Value *BodyVal = Body->codegen();
	if(!BodyVal) {
		return nullptr;
	}

	// Pop all our variables from scope.
	for(unsigned i = 0, e = VarNames.size(); i != e; ++i) {
		if(OldBindings[i]) {
			NamedValues[VarNames[i].first] = OldBindings[i];
		} else {
			NamedValues.erase(VarNames[i].first);
		}
	}

	return BodyVal;
}

Function *PrototypeAST::codegen() {
	// Make the function type:  double(double,double) etc.
	std::vector<Type *> Doubles(Args.size(), Type::getDoubleTy(*TheContext));
	FunctionType *FT = FunctionType::get(Type::getDoubleTy(*TheContext), Doubles, false);

	Function *F = Function::Create(FT, Function::ExternalLinkage, Name, TheModule.get());

	// Set names for all arguments.
	unsigned Idx = 0;
	for(auto &Arg : F->args()) {
		Arg.setName(Args[Idx++]);
	}

	return F;
}

Function *FunctionAST::codegen() {
	// Transfer ownership of the prototype to the FunctionProtos map, but keep a
	// reference to it for use below.
	auto &P = *Proto;
	FunctionProtos[Proto->getName()] = std::move(Proto);
	Function *TheFunction = getFunction(P.getName());
	if(!TheFunction) {
		return nullptr;
	}

	// If this is an operator, install it.
	if(P.isBinaryOp()) {
		BinopPrecedence[P.getOperatorName()] = P.getBinaryPrecedence();
	}

	// Create a new basic block to start insertion into.
	BasicBlock *BB = BasicBlock::Create(*TheContext, "entry", TheFunction);
	Builder->SetInsertPoint(BB);

	// Record the function arguments in the NamedValues map.
	NamedValues.clear();
	for(auto &Arg : TheFunction->args()) {
		// Create an alloca for this variable and store the initial value into it.
		AllocaInst *Alloca = CreateEntryBlockAlloca(TheFunction, Arg.getName());
		Builder->CreateStore(&Arg, Alloca);
		NamedValues[std::string(Arg.getName())] = Alloca;
	}

	if(Value *RetVal = Body->codegen()) {
		// Finish off the function.
		Builder->CreateRet(RetVal);

		// Validate the generated code, checking for consistency.
		verifyFunction(*TheFunction);

		// Run the optimizer on the function.
		TheFPM->run(*TheFunction, *TheFAM);

		return TheFunction;
	}

	// Error reading body, remove function.
	TheFunction->eraseFromParent();

	if(P.isBinaryOp()) {
		BinopPrecedence.erase(P.getOperatorName());
	}
	return nullptr;
}

void InitializeModuleAndManagers(void) {
	// Open a new context and module.
	TheContext = std::make_unique<LLVMContext>();
	TheModule = std::make_unique<Module>("jade", *TheContext);
	if(JitMode) {
		TheModule->setTargetTriple(TheJIT->getTargetTriple());
		TheModule->setDataLayout(TheJIT->getDataLayout());
	}

	// Create a new builder for the module.
	Builder = std::make_unique<IRBuilder<>>(*TheContext);

	// Create new pass and analysis managers.
	TheFPM = std::make_unique<FunctionPassManager>();
	TheLAM = std::make_unique<LoopAnalysisManager>();
	TheFAM = std::make_unique<FunctionAnalysisManager>();
	TheCGAM = std::make_unique<CGSCCAnalysisManager>();
	TheMAM = std::make_unique<ModuleAnalysisManager>();
	ThePIC = std::make_unique<PassInstrumentationCallbacks>();
	TheSI = std::make_unique<StandardInstrumentations>(*TheContext,
	                                                   /*DebugLogging*/ true);
	TheSI->registerCallbacks(*ThePIC, TheMAM.get());
	// Add transform passes.
	// Promote allocas to registers.
	TheFPM->addPass(PromotePass());
	// Do simple "peephole" optimizations and bit-twiddling optzns.
	TheFPM->addPass(InstCombinePass());
	// Reassociate expressions.
	TheFPM->addPass(ReassociatePass());
	// Eliminate Common SubExpressions.
	TheFPM->addPass(GVNPass());
	// Simplify the control flow graph (deleting unreachable blocks, etc).
	TheFPM->addPass(SimplifyCFGPass());
	// Register analysis passes used in these transform passes.
	PassBuilder PB;
	PB.registerModuleAnalyses(*TheMAM);
	PB.registerFunctionAnalyses(*TheFAM);
	PB.crossRegisterProxies(*TheLAM, *TheFAM, *TheCGAM, *TheMAM);
}
