#include "MemoryLeak/MLDInstrumentation.h"
#include "DyckAA/DyckAliasAnalysis.h"
#include "MemoryLeak/MLDAllocationAnalysis.h"
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>
#include <llvm/Pass.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/raw_ostream.h>
#include <map>
#include <queue>
#include <tuple>
#include <utility>
#include <vector>

char MLDInstrumentation::ID = 0;
MLDInstrumentation::MLDInstrumentation()
    : llvm::ModulePass(ID) {
    std::string sourceFile = CSourceFunctions.getValue();
    assert(!sourceFile.empty());
    std::fstream sourceFileStream(sourceFile);
    DeclareFunctionDesc funcDesc;
    while (!sourceFileStream.eof()) {
        sourceFileStream >> funcDesc;
        std::cout << funcDesc;
        this->FuncDescMap.insert({funcDesc.FunctionName, new NaiveAliasGraph(funcDesc)});
    }
}

MLDInstrumentation::~MLDInstrumentation() {
    for (auto nameGraph : this->FuncDescMap) {
        delete nameGraph.second;
    }
}

// TODO: what about function pointer call? Ignore it now -- 2024-10-25

bool MLDInstrumentation::runOnModule(Module &M) {
    LLVMContext &ctx = M.getContext();
    std::vector<Type *> paramTy = std::vector<Type *>(1, Type::getInt64Ty(ctx));
    FunctionType *mallocFT = FunctionType::get(PointerType::get(ctx, 8), paramTy, false);
    // Function *F = Function::Create(mallocFT, Function::ExternalWeakLinkage, "malloc", M);
    FunctionCallee mallocCallee = M.getOrInsertFunction("malloc", mallocFT);
    for (auto &F : M) {
        if (F.isIntrinsic()) {
            return false;
        }
        std::vector<CallInst *> callInstVec;
        for (auto &I : instructions(F)) {
            if (isa<CallInst>(I)) {
                CallInst *callInst = dyn_cast<CallInst>(&I);
                if (this->FuncDescMap.find(callInst->getCalledFunction()->getName().str()) != this->FuncDescMap.end()) {
                    callInstVec.push_back(callInst);
                }
            }
        }
        // outs() << callInstVec.size() << "\n";
        for (CallInst *callInst : callInstVec) {
            IRBuilder<> irBuilder(callInst->getContext());
            BasicBlock *parent = callInst->getParent();
            irBuilder.SetInsertPoint(parent, callInst->getIterator()++);
            NaiveAliasGraph *aliasGraph = this->FuncDescMap.find(callInst->getCalledFunction()->getName().str())->second;
            aliasGraph->instrument(irBuilder, callInst, M);
        }
    }
    return false;
}

NaiveAliasGraphNode *NaiveAliasGraph::insertOrGetNode(int Index, bool Allocated) {
    auto newNodeIt = this->NodesMap.find(Index);
    if (newNodeIt != this->NodesMap.end()) {
        return newNodeIt->second;
    }
    NaiveAliasGraphNode *node = new NaiveAliasGraphNode(Index, Allocated);
    this->NodesMap.insert({Index, node});
    return node;
}

NaiveAliasGraph::NaiveAliasGraph(DeclareFunctionDesc &FuncDesc)
    : FuncDesc(FuncDesc) {
    for (auto i : this->FuncDesc.IndexOfParameters) {
        NaiveAliasGraphNode *paramNode =
            this->insertOrGetNode(i, this->FuncDesc.AllocationIndexes.find(i) != this->FuncDesc.AllocationIndexes.end());
        this->ParamNodes.push_back(paramNode);
    }
    // Manually merge all return nodes into one abstract nodes;
    std::set<int> indexOfReturns(this->FuncDesc.IndexOfReturns.begin(), this->FuncDesc.IndexOfReturns.end());
    int returnNodeIndex = -1;
    if (!this->FuncDesc.IndexOfReturns.empty()) {
        returnNodeIndex = this->FuncDesc.IndexOfReturns.front();
    }
    bool returnIsAliasWithAllocation = false;
    for (int i : this->FuncDesc.IndexOfReturns) {
        returnIsAliasWithAllocation |= this->FuncDesc.AllocationIndexes.find(i) != this->FuncDesc.AllocationIndexes.end();
    }
    if (returnNodeIndex != -1) {
        this->retNode = this->insertOrGetNode(returnNodeIndex, returnIsAliasWithAllocation);
    }
    else {
        this->retNode = nullptr;
    }
    // if access to any return node, then inserOrGetNode function would return the one created previously, and the second
    // argument would not be used.
    for (auto edge : this->FuncDesc.AliasGraphOfParameters) {
        int startIndex = indexOfReturns.find(edge.StartIndex) == indexOfReturns.end() ? edge.StartIndex : returnNodeIndex;
        NaiveAliasGraphNode *startNode = this->insertOrGetNode(startIndex, this->FuncDesc.AllocationIndexes.find(startIndex) !=
                                                                               this->FuncDesc.AllocationIndexes.end());
        int endIndex = indexOfReturns.find(edge.EndIndex) == indexOfReturns.end() ? edge.EndIndex : returnNodeIndex;
        NaiveAliasGraphNode *endNode = this->insertOrGetNode(endIndex, this->FuncDesc.AllocationIndexes.find(endIndex) !=
                                                                           this->FuncDesc.AllocationIndexes.end());
        startNode->addEdge(endNode, edge.LabelDesc);
    }
    for (auto edge : this->FuncDesc.AliasGraphOfReturns) {
        int startIndex = indexOfReturns.find(edge.StartIndex) == indexOfReturns.end() ? edge.StartIndex : returnNodeIndex;
        NaiveAliasGraphNode *startNode = this->insertOrGetNode(startIndex, this->FuncDesc.AllocationIndexes.find(startIndex) !=
                                                                               this->FuncDesc.AllocationIndexes.end());
        int endIndex = indexOfReturns.find(edge.EndIndex) == indexOfReturns.end() ? edge.EndIndex : returnNodeIndex;
        NaiveAliasGraphNode *endNode = this->insertOrGetNode(endIndex, this->FuncDesc.AllocationIndexes.find(endIndex) !=
                                                                           this->FuncDesc.AllocationIndexes.end());
        startNode->addEdge(endNode, edge.LabelDesc);
    }
}

NaiveAliasGraph::~NaiveAliasGraph() {
    for (auto indexNodePair : NodesMap) {
        delete indexNodePair.second;
    }
}

// In this implementation, allocation notaion for directed arguments and return values is leave until Alias Analysis for SSA
// form is allowed to assign only once.
// We assume that there would not be the case that a node receives a pointer offset edge and field offset edge at the same time.
// FIXME: YangLin is not familiar enough with the bahavior of llvm APIs, so there may be a lot of bugs.
void NaiveAliasGraph::instrument(IRBuilder<> &irBuilder, CallInst *callInst, Module &M) {
    int numOfArgs = callInst->arg_size();
    if (numOfArgs != this->ParamNodes.size()) {
        errs() << "\033[31mError: The number of arguments doesn't match the size of parameters.\033[0m\n";
        return;
    }
    std::map<NaiveAliasGraphNode *, Value *> aliasNode2ValueMap;
    std::queue<NaiveAliasGraphNode *> workList;
    for (int i = 0; i < numOfArgs; i++) {
        aliasNode2ValueMap.insert({this->ParamNodes[i], callInst->getArgOperand(i)});
        workList.push(this->ParamNodes[i]);
    }
    ConstantInt *LLVMInt4 = ConstantInt::get(Type::getInt64Ty(M.getContext()), 4);
    Type *LLVMInt64Type = Type::getInt64Ty(M.getContext());
    PointerType *defaultPointerType = PointerType::get(M.getContext(), 8);
    while (!workList.empty()) {
        NaiveAliasGraphNode *current = workList.front();
        workList.pop();
        for (auto outEdge : current->OutEdges) {
            NaiveAliasGraphNode *outNode = outEdge.first;
            workList.push(outNode);
            std::string outLabel = outEdge.second;
            auto valueIt = aliasNode2ValueMap.find(outNode);
            char kind = outLabel[0];
            int offset = -1;
            if (outLabel.size() > 1) {
                offset = atoi(outLabel.substr(1).c_str());
            }
            if (valueIt == aliasNode2ValueMap.end()) {
                // In the case that the value haven't been initialized.
                switch (kind) {
                case 'D':
                    // For dereference edge, if the value is an allocation value, first crate a value using malloc function,
                    // then store the value into target. if not, load the value from the target and get the value.
                    if (outNode->Allocation) {
                        CallInst *allocValue = irBuilder.CreateCall(
                            M.getFunction("malloc"), std::vector<Value *>(1, ConstantInt::get(LLVMInt64Type, 4)));
                        irBuilder.CreateStore(allocValue, aliasNode2ValueMap[current]);
                        aliasNode2ValueMap.insert({outNode, allocValue});
                    }
                    else {
                        LoadInst *loadValue = irBuilder.CreateLoad(defaultPointerType, aliasNode2ValueMap[current]);
                        aliasNode2ValueMap.insert({outNode, loadValue});
                    }

                    break;
                case '#':
                    // For field index edge, if the value is an allocation value, first crate a value using malloc function,
                    // then insert the value into target. if not, extract the value from the target and get the value.
                    if (outNode->Allocation) {
                        CallInst *allocValue = irBuilder.CreateCall(
                            M.getFunction("malloc"),
                            std::vector<Value *>(1, ConstantInt::get(Type::getInt64Ty(M.getContext()), 4)));
                        irBuilder.CreateInsertValue(aliasNode2ValueMap[current], allocValue,
                                                    std::vector<unsigned>(offset + 1, offset));
                        aliasNode2ValueMap.insert({outNode, allocValue});
                    }
                    else {
                        Value *extractValue = irBuilder.CreateExtractValue(aliasNode2ValueMap[current],
                                                                           std::vector<unsigned>(1, offset));
                        aliasNode2ValueMap.insert({outNode, extractValue});
                    }
                    break;
                case '@':
                    // For pointer offset edge, if the value is an allcoation value, this condition should not be exist for
                    // alloation value shouldn't be offset of others. So just use getelementptr instruction to model.
                    if (outNode->Allocation) {
                        errs() << "Offset of pointer should not be alias with allocation.\n";
                    }
                    StructType *meaninglessStructType =
                        StructType::get(M.getContext(), std::vector<Type *>(offset + 1, LLVMInt64Type));
                    Value *gepValue = irBuilder.CreateConstGEP1_32(meaninglessStructType, aliasNode2ValueMap[current], offset);
                    aliasNode2ValueMap.insert({outNode, gepValue});
                    break;
                }
            }
            else {
                Value *targetValue = valueIt->second;
                // In the case that the value have been initialized.
                switch (kind) {
                // For dereference edge, the value have been initialized, then we just store the value into target.
                case 'D':
                    irBuilder.CreateLoad(defaultPointerType, aliasNode2ValueMap[current], targetValue);
                    break;
                // For field index edge, if the value have been initialize, then insert the value into target.
                case '#':
                    irBuilder.CreateInsertValue(aliasNode2ValueMap[current], targetValue,
                                                std::vector<unsigned>(offset + 1, offset));
                    break;
                // For pointer offset edge, if the value have been initialize, but gep instruction should be in front of others.
                // So replace the previous value by the new gep instruction and replace all the used and insert a load/insert
                // instruction.
                case '@':
                    Instruction *oldInst = dyn_cast<Instruction>(aliasNode2ValueMap[outNode]);
                    StructType *meaninglessStructType =
                        StructType::get(M.getContext(), std::vector<Type *>(offset + 1, LLVMInt64Type));
                    GetElementPtrInst *gepValue = GetElementPtrInst::Create(
                        meaninglessStructType, aliasNode2ValueMap[current],
                        std::vector<Value *>(1, ConstantInt::get(Type::getInt64Ty(M.getContext()), 4)));
                    // Value *gepValue  = CreateConstGEP1_32(meaninglessStructType, aliasNode2ValueMap[current], offset);
                    oldInst->replaceAllUsesWith(gepValue);
                    gepValue->insertBefore(oldInst);
                    switch (oldInst->getOpcode()) {
                    case Instruction::Load: {
                        LoadInst *loadInst = dyn_cast<LoadInst>(oldInst);
                        StoreInst *storeInst =
                            new StoreInst(gepValue, loadInst->getPointerOperand(), irBuilder.GetInsertBlock());
                        storeInst->insertBefore(oldInst);
                        oldInst->eraseFromParent();
                        break;
                    }
                    case Instruction::ExtractValue: {
                        ExtractValueInst *extractInst = dyn_cast<ExtractValueInst>(oldInst);
                        InsertValueInst *insertInst = InsertValueInst::Create(extractInst->getAggregateOperand(), gepValue,
                                                                              std::vector<unsigned>(1, offset));
                        insertInst->insertBefore(oldInst);
                        oldInst->eraseFromParent();
                        break;
                    }
                    default:
                        errs() << "Unexpected Instruction Opcode for an existing instruction.\n";
                    }
                    break;
                }
            }
        }
    }
}