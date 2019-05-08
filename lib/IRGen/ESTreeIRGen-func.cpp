/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the LICENSE
 * file in the root directory of this source tree.
 */
#include "ESTreeIRGen.h"

#include "llvm/ADT/SmallString.h"

namespace hermes {
namespace irgen {

//===----------------------------------------------------------------------===//
// FunctionContext

FunctionContext::FunctionContext(
    ESTreeIRGen *irGen,
    Function *function,
    sem::FunctionInfo *semInfo)
    : irGen_(irGen),
      semInfo_(semInfo),
      oldContext_(irGen->functionContext_),
      builderSaveState_(irGen->Builder),
      function(function),
      scope(irGen->nameTable_) {
  irGen->functionContext_ = this;

  // Initialize it to LiteraUndefined by default to avoid corner cases.
  this->capturedNewTarget = irGen->Builder.getLiteralUndefined();

  if (semInfo_) {
    // Allocate the label table. Each label definition will be encountered in
    // the AST before it is referenced (because of the nature of JavaScript), at
    // which point we will initialize the GotoLabel structure with basic blocks
    // targets.
    labels.resize(semInfo_->labels.size());
  }
}

FunctionContext::~FunctionContext() {
  irGen_->functionContext_ = oldContext_;
}

Identifier FunctionContext::genAnonymousLabelName(StringRef hint) {
  llvm::SmallString<16> buf;
  llvm::raw_svector_ostream nameBuilder{buf};
  nameBuilder << "?anon_" << anonymousLabelCounter++ << "_" << hint;
  return function->getContext().getIdentifier(nameBuilder.str());
}

//===----------------------------------------------------------------------===//
// ESTreeIRGen

void ESTreeIRGen::genFunctionDeclaration(
    ESTree::FunctionDeclarationNode *func) {
  // Find the name of the function.
  Identifier functionName = getNameFieldFromID(func->_id);
  DEBUG(dbgs() << "IRGen function \"" << functionName << "\".\n");

  auto *funcStorage = nameTable_.lookup(functionName);
  assert(
      funcStorage && "function declaration variable should have been hoisted");

  Function *newFunc = func->_generator
      ? genGeneratorFunction(functionName, nullptr, func)
      : genES5Function(functionName, nullptr, func);

  // Store the newly created closure into a frame variable with the same name.
  auto *newClosure = Builder.createCreateFunctionInst(newFunc);

  emitStore(Builder, newClosure, funcStorage);
}

Value *ESTreeIRGen::genFunctionExpression(
    ESTree::FunctionExpressionNode *FE,
    Identifier nameHint) {
  DEBUG(
      dbgs() << "Creating anonymous closure. "
             << Builder.getInsertionBlock()->getParent()->getInternalName()
             << ".\n");

  NameTableScopeTy newScope(nameTable_);
  Variable *tempClosureVar = nullptr;

  Identifier originalNameIden = nameHint;
  if (FE->_id) {
    auto closureName = genAnonymousLabelName("closure");
    tempClosureVar = Builder.createVariable(
        curFunction()->function->getFunctionScope(), closureName);

    // Insert the synthesized variable into the name table, so it can be
    // looked up internally as well.
    nameTable_.insertIntoScope(
        &curFunction()->scope, tempClosureVar->getName(), tempClosureVar);

    // Alias the lexical name to the synthesized variable.
    originalNameIden = getNameFieldFromID(FE->_id);
    nameTable_.insert(originalNameIden, tempClosureVar);
  }

  Function *newFunc = FE->_generator
      ? genGeneratorFunction(originalNameIden, tempClosureVar, FE)
      : genES5Function(originalNameIden, tempClosureVar, FE);

  Value *closure = Builder.createCreateFunctionInst(newFunc);

  if (tempClosureVar)
    emitStore(Builder, closure, tempClosureVar);

  return closure;
}

Value *ESTreeIRGen::genArrowFunctionExpression(
    ESTree::ArrowFunctionExpressionNode *AF,
    Identifier nameHint) {
  DEBUG(
      dbgs() << "Creating arrow function. "
             << Builder.getInsertionBlock()->getParent()->getInternalName()
             << ".\n");

  auto *newFunc = Builder.createFunction(
      nameHint,
      Function::DefinitionKind::ES6Arrow,
      ESTree::isStrict(AF->strictness),
      AF->getSourceRange());

  {
    FunctionContext newFunctionContext{this, newFunc, AF->getSemInfo()};

    emitFunctionPrologue(AF, Builder.createBasicBlock(newFunc));

    // Propagate captured "this", "new.target" and "arguments" from parents.
    auto *prev = curFunction()->getPreviousContext();
    curFunction()->capturedThis = prev->capturedThis;
    curFunction()->capturedNewTarget = prev->capturedNewTarget;
    curFunction()->capturedArguments = prev->capturedArguments;

    genStatement(AF->_body);
    emitFunctionEpilogue(Builder.getLiteralUndefined());
  }

  // Emit CreateFunctionInst after we have restored the builder state.
  return Builder.createCreateFunctionInst(newFunc);
}

#ifndef HERMESVM_LEAN
Function *ESTreeIRGen::genES5Function(
    Identifier originalName,
    Variable *lazyClosureAlias,
    ESTree::FunctionLikeNode *functionNode,
    bool isGeneratorInnerFunction) {
  assert(functionNode && "Function AST cannot be null");

  auto *body = ESTree::getBlockStatement(functionNode);
  assert(body && "body of ES5 function cannot be null");

  Function *newFunction = isGeneratorInnerFunction
      ? Builder.createGeneratorInnerFunction(
            originalName,
            Function::DefinitionKind::ES5Function,
            ESTree::isStrict(functionNode->strictness),
            body->getSourceRange(),
            /* insertBefore */ nullptr)
      : Builder.createFunction(
            originalName,
            Function::DefinitionKind::ES5Function,
            ESTree::isStrict(functionNode->strictness),
            body->getSourceRange(),
            /* isGlobal */ false,
            /* insertBefore */ nullptr);

  newFunction->setLazyClosureAlias(lazyClosureAlias);

  if (auto *bodyBlock = dyn_cast<ESTree::BlockStatementNode>(body)) {
    if (bodyBlock->isLazyFunctionBody) {
      // Set the AST position and variable context so we can continue later.
      newFunction->setLazyScope(saveCurrentScope());
      auto &lazySource = newFunction->getLazySource();
      lazySource.bufferId = bodyBlock->bufferId;
      lazySource.nodeKind = functionNode->getKind();
      lazySource.functionRange = functionNode->getSourceRange();

      // Give the stub parameters so that we'll know the function's .length .
      Builder.createParameter(newFunction, "this");
      for (auto &param : ESTree::getParams(functionNode)) {
        auto idenNode = cast<ESTree::IdentifierNode>(&param);
        Identifier paramName = getNameFieldFromID(idenNode);
        Builder.createParameter(newFunction, paramName);
      }

      return newFunction;
    }
  }

  FunctionContext newFunctionContext{
      this, newFunction, functionNode->getSemInfo()};

  if (isGeneratorInnerFunction) {
    // StartGeneratorInst
    // ResumeGeneratorInst
    // at the beginning of the function, to allow for the first .next() call.
    auto *initGenBB = Builder.createBasicBlock(newFunction);
    Builder.setInsertionBlock(initGenBB);
    Builder.createStartGeneratorInst();
    auto *resumeIsReturn =
        Builder.createAllocStackInst(genAnonymousLabelName("isReturn"));
    auto *entryPoint = Builder.createBasicBlock(newFunction);
    genResumeGenerator(nullptr, resumeIsReturn, entryPoint);
    emitFunctionPrologue(functionNode, entryPoint);
  } else {
    emitFunctionPrologue(functionNode, Builder.createBasicBlock(newFunction));
  }

  initCaptureStateInES5Function();

  genStatement(body);
  emitFunctionEpilogue(Builder.getLiteralUndefined());

  return curFunction()->function;
}
#endif

Function *ESTreeIRGen::genGeneratorFunction(
    Identifier originalName,
    Variable *lazyClosureAlias,
    ESTree::FunctionLikeNode *functionNode) {
  assert(functionNode && "Function AST cannot be null");

  // Build the outer function which creates the generator.
  // Does not have an associated source range.
  auto *outerFn = Builder.createGeneratorFunction(
      originalName,
      Function::DefinitionKind::ES5Function,
      ESTree::isStrict(functionNode->strictness),
      /* insertBefore */ nullptr);

  auto *innerFn = genES5Function(
      genAnonymousLabelName(originalName.isValid() ? originalName.str() : ""),
      lazyClosureAlias,
      functionNode,
      true);

  {
    FunctionContext outerFnContext{this, outerFn, functionNode->getSemInfo()};
    emitFunctionPrologue(functionNode, Builder.createBasicBlock(outerFn));
    initCaptureStateInES5Function();

    // Create a generator function, which will store the arguments.
    auto *gen = Builder.createCreateGeneratorInst(innerFn);

    emitFunctionEpilogue(gen);
  }

  return outerFn;
}

void ESTreeIRGen::initCaptureStateInES5Function() {
  // Capture "this", "new.target" and "arguments" if there are inner arrows.
  if (!curFunction()->getSemInfo()->containsArrowFunctions)
    return;

  auto *scope = curFunction()->function->getFunctionScope();

  // "this".
  curFunction()->capturedThis =
      Builder.createVariable(scope, genAnonymousLabelName("this"));
  emitStore(
      Builder,
      Builder.getFunction()->getThisParameter(),
      curFunction()->capturedThis);

  // "new.target".
  curFunction()->capturedNewTarget =
      Builder.createVariable(scope, genAnonymousLabelName("new.target"));
  emitStore(
      Builder,
      Builder.createGetNewTargetInst(),
      curFunction()->capturedNewTarget);

  // "arguments".
  if (curFunction()->getSemInfo()->containsArrowFunctionsUsingArguments) {
    curFunction()->capturedArguments =
        Builder.createVariable(scope, genAnonymousLabelName("arguments"));
    emitStore(
        Builder,
        Builder.createCreateArgumentsInst(),
        curFunction()->capturedArguments);
  }
}

void ESTreeIRGen::emitFunctionPrologue(
    ESTree::FunctionLikeNode *funcNode,
    BasicBlock *entry) {
  auto *newFunc = curFunction()->function;
  auto *semInfo = curFunction()->getSemInfo();
  DEBUG(
      dbgs() << "Hoisting "
             << (semInfo->varDecls.size() + semInfo->closures.size())
             << " variable decls.\n");

  Builder.setLocation(newFunc->getSourceRange().Start);

  // Start pumping instructions into the entry basic block.
  Builder.setInsertionBlock(entry);

  // Create variable declarations for each of the hoisted variables and
  // functions. Initialize only the variables to undefined.
  for (auto *id : semInfo->varDecls) {
    auto res = declareVariableOrGlobalProperty(newFunc, getNameFieldFromID(id));
    // If this is not a frame variable or it was already declared, skip.
    auto *var = dyn_cast<Variable>(res.first);
    if (!var || !res.second)
      continue;

    // Otherwise, initialize it to undefined.
    Builder.createStoreFrameInst(Builder.getLiteralUndefined(), var);
  }
  for (auto *fd : semInfo->closures)
    declareVariableOrGlobalProperty(newFunc, getNameFieldFromID(fd->_id));

  // Construct the parameter list. Create function parameters and register
  // them in the scope.
  emitParameters(funcNode);

  // Generate and initialize the code for the hoisted function declarations
  // before generating the rest of the body.
  for (auto funcDecl : semInfo->closures) {
    genFunctionDeclaration(funcDecl);
  }

  // Separate the next block, so we can append instructions to the entry block
  // in the future.
  auto *nextBlock = Builder.createBasicBlock(newFunc);
  curFunction()->entryTerminator = Builder.createBranchInst(nextBlock);
  Builder.setInsertionBlock(nextBlock);
}

void ESTreeIRGen::emitParameters(ESTree::FunctionLikeNode *funcNode) {
  auto *newFunc = curFunction()->function;

  DEBUG(dbgs() << "IRGen function parameters.\n");

  // Always create the "this" parameter.
  Builder.createParameter(newFunc, "this");

  // Create a variable for every parameter.
  for (auto *idNode : funcNode->getSemInfo()->paramNames) {
    Identifier paramName = getNameFieldFromID(idNode);
    DEBUG(dbgs() << "Adding parameter: " << paramName << "\n");
    auto *paramStorage =
        Builder.createVariable(newFunc->getFunctionScope(), paramName);
    // Register the storage for the parameter.
    nameTable_.insert(paramName, paramStorage);
  }

  // FIXME: T42569352 TDZ for parameters used in initializer expressions.
  uint32_t paramIndex = uint32_t{0} - 1;
  for (auto &elem : ESTree::getParams(funcNode)) {
    ESTree::Node *param = &elem;
    ESTree::Node *init = nullptr;
    ++paramIndex;

    if (auto *rest = dyn_cast<ESTree::RestElementNode>(param)) {
      createLRef(rest->_argument)
          .emitStore(genHermesInternalCall(
              "copyRestArgs",
              Builder.getLiteralUndefined(),
              Builder.getLiteralNumber(paramIndex)));
      break;
    }

    // Unpack the optional initialization.
    if (auto *assign = dyn_cast<ESTree::AssignmentPatternNode>(param)) {
      param = assign->_left;
      init = assign->_right;
    }

    Identifier formalParamName = isa<ESTree::IdentifierNode>(param)
        ? getNameFieldFromID(param)
        : genAnonymousLabelName("param");

    auto *formalParam = Builder.createParameter(newFunc, formalParamName);
    createLRef(param).emitStore(emitOptionalInitialization(formalParam, init));
  }
}

void ESTreeIRGen::emitFunctionEpilogue(Value *returnValue) {
  if (returnValue) {
    Builder.setLocation(SourceErrorManager::convertEndToLocation(
        Builder.getFunction()->getSourceRange()));
    Builder.createReturnInst(returnValue);
  }

  // If Entry is the only user of nextBlock, merge Entry and nextBlock, to
  // create less "noise" when optimization is disabled.
  BasicBlock *nextBlock = nullptr;

  if (curFunction()->entryTerminator->getNumSuccessors() == 1)
    nextBlock = curFunction()->entryTerminator->getSuccessor(0);

  if (nextBlock->getNumUsers() == 1 &&
      nextBlock->hasUser(curFunction()->entryTerminator)) {
    DEBUG(dbgs() << "Merging entry and nextBlock.\n");

    // Move all instructions from nextBlock into Entry.
    while (nextBlock->begin() != nextBlock->end())
      nextBlock->begin()->moveBefore(curFunction()->entryTerminator);

    // Now we can delete the original terminator;
    curFunction()->entryTerminator->eraseFromParent();
    curFunction()->entryTerminator = nullptr;

    // Delete the now empty next block
    nextBlock->eraseFromParent();
    nextBlock = nullptr;
  } else {
    DEBUG(dbgs() << "Could not merge entry and nextBlock.\n");
  }

  curFunction()->function->clearStatementCount();
}

void ESTreeIRGen::genDummyFunction(Function *dummy) {
  IRBuilder builder{dummy};

  builder.createParameter(dummy, "this");
  BasicBlock *firstBlock = builder.createBasicBlock(dummy);
  builder.setInsertionBlock(firstBlock);
  builder.createUnreachableInst();
  builder.createReturnInst(builder.getLiteralUndefined());
}

/// Generate a function which immediately throws the specified SyntaxError
/// message.
Function *ESTreeIRGen::genSyntaxErrorFunction(
    Module *M,
    Identifier originalName,
    SMRange sourceRange,
    StringRef error) {
  IRBuilder builder{M};

  Function *function = builder.createFunction(
      originalName,
      Function::DefinitionKind::ES5Function,
      true,
      sourceRange,
      false);

  builder.createParameter(function, "this");
  BasicBlock *firstBlock = builder.createBasicBlock(function);
  builder.setInsertionBlock(firstBlock);

  builder.createThrowInst(builder.createCallInst(
      emitLoad(
          builder, builder.createGlobalObjectProperty("SyntaxError", false)),
      builder.getLiteralUndefined(),
      builder.getLiteralString(error)));

  return function;
}

} // namespace irgen
} // namespace hermes
