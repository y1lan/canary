#include "MemoryLeak/MLDInstrumentation.h"
#include "DyckAA/DyckAliasAnalysis.h"
#include "MemoryLeak/MLDAllocationAnalysis.h"
#include "Support/API.h"
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalValue.h>
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
#include <string>
#include <tuple>
#include <utility>
#include <vector>

char MLDInstrumentation::ID = 0;
MLDInstrumentation::MLDInstrumentation()
    : llvm::ModulePass(ID) {
    std::string sourceFile = CSourceFunctions.getValue();
    assert(!sourceFile.empty());
    std::fstream sourceFileStream(sourceFile);
    int size = 0;
    sourceFileStream >> size;
    for (int i = 0; i < size; i++) {
        DeclareFunctionDesc funcDesc;
        sourceFileStream >> funcDesc;
        
        this->FuncDescMap.insert({funcDesc.FunctionName, new NaiveAliasGraph(funcDesc)});
        // API::HeapAllocFunctions.insert(funcDesc.FunctionName);
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
    FunctionType *mallocFT = FunctionType::get(PointerType::get(ctx, 0), paramTy, false);
    // Function *F = Function::Create(mallocFT, Function::ExternalWeakLinkage, "malloc", M);
    FunctionCallee mallocCallee = M.getOrInsertFunction("malloc", mallocFT);
    for (auto &F : M) {
        if (F.isIntrinsic()) {
            continue;
        }
        std::vector<CallInst *> callInstVec;
        for (auto &I : instructions(F)) {
            if (isa<CallInst>(I)) {
                CallInst *callInst = dyn_cast<CallInst>(&I);
                outs() << callInst->getCalledFunction() << "\n";
                if (callInst->getCalledFunction()) {
                    outs() << callInst->getCalledFunction()->getName().str() << "\n";
                    if (this->FuncDescMap.find(callInst->getCalledFunction()->getName().str()) != this->FuncDescMap.end()) {
                        callInstVec.push_back(callInst);
                    }
                }
            }
        }
        outs() << callInstVec.size() << "\n";
        for (CallInst *callInst : callInstVec) {
            IRBuilder<> irBuilder(callInst->getContext());
            BasicBlock *parent = callInst->getParent();
            irBuilder.SetInsertPoint(parent, ++callInst->getIterator());
            NaiveAliasGraph *aliasGraph = this->FuncDescMap.find(callInst->getCalledFunction()->getName().str())->second;
            aliasGraph->instrumentRe(irBuilder, callInst, M);
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

NaiveAliasGraph::NaiveAliasGraph(const DeclareFunctionDesc &FuncDesc)
    : FuncDesc(FuncDesc) {
    for (auto i : this->FuncDesc.IndexOfParameters) {
        bool aliasWithAllocation = this->FuncDesc.AllocationIndexes.find(i) != this->FuncDesc.AllocationIndexes.end();
        NaiveAliasGraphNode *paramNode = this->insertOrGetNode(i, aliasWithAllocation);
        this->ParamNodes.push_back(paramNode);
        // API::HeapAllocFunctionParameters[FuncDesc.FunctionName].push_back(aliasWithAllocation);
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
    if (returnIsAliasWithAllocation) {
        API::HeapAllocFunctions.insert(FuncDesc.FunctionName);
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
    std::set<int> visited;
    for (int i = 0; i < numOfArgs; i++) {
        aliasNode2ValueMap.insert({this->ParamNodes[i], callInst->getArgOperand(i)});
        workList.push(this->ParamNodes[i]);
        visited.insert(this->ParamNodes[i]->Index);
    }
    ConstantInt *LLVMInt4 = ConstantInt::get(Type::getInt64Ty(M.getContext()), 4);
    Type *LLVMInt64Type = Type::getInt64Ty(M.getContext());
    // PointerType *defaultPointerType = PointerType::get(M.getContext(), 8);
    while (!workList.empty()) {
        NaiveAliasGraphNode *current = workList.front();
        workList.pop();
        Value *currentValue = aliasNode2ValueMap[current];
        Type *currentType = currentValue->getType();
        if (isa<Instruction>(currentValue)) {
            Instruction *currentInst = dyn_cast<Instruction>(currentValue);
            switch (currentInst->getOpcode()) {
            case Instruction::GetElementPtr: {
                currentType = dyn_cast<GetElementPtrInst>(currentInst)->getResultElementType();
                break;
            }
            case Instruction::Alloca: {
                currentType = dyn_cast<AllocaInst>(currentInst)->getAllocatedType();
                break;
            }
            }
        }
        if (isa<GlobalValue>(currentValue)) {
            currentType = dyn_cast<GlobalValue>(currentValue)->getValueType();
        }
        // outs() << current->Index << *currentValue << "\t";
        // currentType->print(outs());
        // outs() << "\n";

        for (auto outEdge : current->OutEdges) {
            NaiveAliasGraphNode *outNode = outEdge.first;
            if (visited.find(outNode->Index) == visited.end()) {
                workList.push(outNode);
                visited.insert(outNode->Index);
            }
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
                        irBuilder.CreateStore(allocValue, currentValue);
                        aliasNode2ValueMap.insert({outNode, allocValue});
                    }
                    else {
                        LoadInst *loadValue = irBuilder.CreateLoad(currentType, currentValue);
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
                        irBuilder.CreateInsertValue(currentValue, allocValue, std::vector<unsigned>(offset + 1, offset));
                        aliasNode2ValueMap.insert({outNode, allocValue});
                    }
                    else {
                        Value *extractValue = irBuilder.CreateExtractValue(currentValue, std::vector<unsigned>(1, offset));
                        aliasNode2ValueMap.insert({outNode, extractValue});
                    }
                    break;
                case '@':
                    // For pointer offset edge, if the value is an allcoation value, this condition should not be exist for
                    // alloation value shouldn't be offset of others. So just use getelementptr instruction to model.
                    if (outNode->Allocation) {
                        errs() << "Offset of pointer should not be alias with allocation.\n";
                    }
                    Value *gepValue = irBuilder.CreateConstGEP1_32(currentType, currentValue, offset);
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
                    irBuilder.CreateLoad(currentType, currentValue, targetValue);
                    break;
                // For field index edge, if the value have been initialize, then insert the value into target.
                case '#':
                    irBuilder.CreateInsertValue(currentValue, targetValue, std::vector<unsigned>(offset + 1, offset));
                    break;
                // For pointer offset edge, if the value have been initialize, but gep instruction should be in front of others.
                // So replace the previous value by the new gep instruction and replace all the used and insert a load/insert
                // instruction.
                case '@':
                    Instruction *oldInst = dyn_cast<Instruction>(aliasNode2ValueMap[outNode]);
                    GetElementPtrInst *gepValue = GetElementPtrInst::Create(
                        currentType, currentValue,
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
                        outs() << "Unexpected Instruction Opcode for an existing instruction.";
                        outs() << outNode->Index << *aliasNode2ValueMap[outNode] << "\n";
                        while (true) {
                        }
                    }
                    break;
                }
            }
            outs() << outNode->Index << *aliasNode2ValueMap[outNode] << "\n";
        }
        outs() << "\n";
    }
}

void NaiveAliasGraph::instrumentRe(IRBuilder<> &irBuilder, CallInst *callInst, Module &M) {
    int numOfArgs = callInst->arg_size();
    if (numOfArgs != this->ParamNodes.size()) {
        outs() << "\033[31mError: The number of arguments doesn't match the size of parameters.\033[0m\n";
        return;
    }
    std::map<NaiveAliasGraphNode *, AllocaInst *> aliasNode2StackLocMap;
    std::queue<NaiveAliasGraphNode *> workList;
    std::set<int> visited;
    for (int i = 0; i < numOfArgs; i++) {
        AllocaInst *allocaInst = irBuilder.CreateAlloca(callInst->getArgOperand(i)->getType());
        irBuilder.CreateStore(callInst->getArgOperand(i), allocaInst);
        workList.push(this->ParamNodes[i]);
        visited.insert(this->ParamNodes[i]->Index);
        aliasNode2StackLocMap.insert({this->ParamNodes[i], allocaInst});
    }
    int allocationCount = 0;
    std::string allocationNamePrefix = this->FuncDesc.FunctionName.append("SideEffect");
    while (!workList.empty()) {
        NaiveAliasGraphNode *current = workList.front();
        workList.pop();
        // outs() << current->Index << *aliasNode2StackLocMap[current] << "\n";
        for (auto outEdge : current->OutEdges) {
            NaiveAliasGraphNode *outNode = outEdge.first;
            // outs() << current->Index << "\t" << outNode->Index << "\t" << outEdge.second << "\n";
            if (visited.find(outNode->Index) == visited.end()) {
                workList.push(outNode);
                visited.insert(outNode->Index);
            }
            std::string outLabel = outEdge.second;
            char kind = outLabel[0];
            int offset = -1;
            auto valueIt = aliasNode2StackLocMap.find(outNode);
            if (outLabel.size() > 1) {
                offset = atoi(outLabel.substr(1).c_str());
            }
            auto sizeOfStructyType = [&M](NaiveAliasGraphNode *node) {
                int outSize = 0;
                for (auto outEdge : node->OutEdges) {
                    if (outEdge.second.size() > 1) {
                        outSize = std::max<int>(atoi(outEdge.second.substr(1).c_str()), outSize);
                    }
                }
                Type *currentType = nullptr;
                currentType =
                    StructType::get(M.getContext(), std::vector<Type *>(outSize + 1, Type::getInt8Ty(M.getContext())));

                return std::make_pair(outSize, currentType);
            };
            if (valueIt == aliasNode2StackLocMap.end()) {
                switch (kind) {
                case 'D': {
                    if (outNode->Allocation) {
                        AllocaInst *allocaInst = irBuilder.CreateAlloca(PointerType::get(M.getContext(), 0));
                        CallInst *mallocValue = irBuilder.CreateCall(
                            M.getFunction("malloc"),
                            std::vector<Value *>(1, ConstantInt::get(Type::getInt64Ty(M.getContext()), 64)),
                            allocationNamePrefix + std::to_string(allocationCount++));
                        Value *currentValue =
                            irBuilder.CreateLoad(PointerType::get(M.getContext(), 0), aliasNode2StackLocMap[current]);
                        irBuilder.CreateStore(mallocValue, allocaInst);
                        irBuilder.CreateStore(mallocValue, currentValue);
                        aliasNode2StackLocMap.insert({outNode, allocaInst});
                    }
                    else {
                        auto sizeAndType = sizeOfStructyType(outNode);
                        AllocaInst *allocaInst = nullptr;
                        Type *outNodeType = sizeAndType.second;
                        if (sizeAndType.first >= 1) {
                            allocaInst = irBuilder.CreateAlloca(outNodeType);
                        }
                        else {
                            allocaInst = irBuilder.CreateAlloca(PointerType::get(M.getContext(), 0));
                        }
                        Value *currentValue =
                            irBuilder.CreateLoad(PointerType::get(M.getContext(), 0), aliasNode2StackLocMap[current]);
                        Value *outValue = irBuilder.CreateLoad(outNodeType, currentValue);
                        irBuilder.CreateStore(outValue, allocaInst);
                        aliasNode2StackLocMap.insert({outNode, allocaInst});
                    }
                    break;
                }
                case '#': {
                    if (outNode->Allocation) {
                        AllocaInst *allocaInst = irBuilder.CreateAlloca(PointerType::get(M.getContext(), 0));
                        CallInst *mallocValue = irBuilder.CreateCall(
                            M.getFunction("malloc"),
                            std::vector<Value *>(1, ConstantInt::get(Type::getInt64Ty(M.getContext()), 64)),
                            allocationNamePrefix + std::to_string(allocationCount++));
                        auto sizeAndType = sizeOfStructyType(current);
                        Type *currentType = sizeAndType.second;
                        LoadInst *currentValue = irBuilder.CreateLoad(currentType, aliasNode2StackLocMap[current]);
                        irBuilder.CreateStore(mallocValue, allocaInst);
                        irBuilder.CreateInsertValue(currentValue, mallocValue, std::vector<unsigned>(1, offset));
                        aliasNode2StackLocMap.insert({outNode, allocaInst});
                    }
                    else {
                        auto sizeAndTypeOfOutNode = sizeOfStructyType(outNode);
                        AllocaInst *allocaInst = nullptr;
                        Type *outNodeType = sizeAndTypeOfOutNode.second;
                        if (sizeAndTypeOfOutNode.first >= 1) {
                            allocaInst = irBuilder.CreateAlloca(outNodeType);
                        }
                        else {
                            allocaInst = irBuilder.CreateAlloca(PointerType::get(M.getContext(), 0));
                        }
                        auto sizeAndTypeOfCurrent = sizeOfStructyType(current);
                        Value *currentValue = irBuilder.CreateLoad(sizeAndTypeOfCurrent.second, aliasNode2StackLocMap[current]);
                        // outs() << *currentValue << "\n";
                        Value *outValue = irBuilder.CreateExtractValue(currentValue, std::vector<unsigned>(1, offset));
                        irBuilder.CreateStore(outValue, allocaInst);
                        aliasNode2StackLocMap.insert({outNode, allocaInst});
                    }
                    break;
                }
                case '@': {
                    if (outNode->Allocation && offset == 0) {
                        CallInst *mallocValue = irBuilder.CreateCall(
                            M.getFunction("malloc"),
                            std::vector<Value *>(1, ConstantInt::get(Type::getInt64Ty(M.getContext()), 64)),
                            allocationNamePrefix + std::to_string(allocationCount++));
                        irBuilder.CreateStore(mallocValue, aliasNode2StackLocMap[current]);
                        aliasNode2StackLocMap.insert({outNode, aliasNode2StackLocMap[current]});
                    }
                    else {
                        AllocaInst *allocaInst = irBuilder.CreateAlloca(PointerType::get(M.getContext(), 0));
                        LoadInst *currentValue =
                            irBuilder.CreateLoad(PointerType::get(M.getContext(), 0), aliasNode2StackLocMap[current]);
                        // auto sizeAndTypeOfCurrent = sizeOfStructyType(current);
                        Value *outValue = irBuilder.CreateGEP(
                            Type::getInt8Ty(M.getContext()), currentValue,
                            std::vector<Value *>(1, ConstantInt::get(Type::getInt32Ty(M.getContext()), offset)));
                        irBuilder.CreateStore(outValue, allocaInst);
                        aliasNode2StackLocMap.insert({outNode, allocaInst});
                    }
                }
                }
            }
            else {
                switch (kind) {
                case 'D': {

                    LoadInst *currentValue =
                        irBuilder.CreateLoad(PointerType::get(M.getContext(), 0), aliasNode2StackLocMap[current]);
                    LoadInst *outValue = irBuilder.CreateLoad(aliasNode2StackLocMap[outNode]->getAllocatedType(),
                                                              aliasNode2StackLocMap[outNode]);
                    irBuilder.CreateStore(outValue, currentValue);
                    break;
                }
                case '#': {
                    auto sizeAndTypeOfCurrent = sizeOfStructyType(current);
                    LoadInst *currentValue = irBuilder.CreateLoad(sizeAndTypeOfCurrent.second, aliasNode2StackLocMap[current]);
                    LoadInst *outValue = irBuilder.CreateLoad(sizeAndTypeOfCurrent.second->getStructElementType(offset),
                                                              aliasNode2StackLocMap[outNode]);
                    irBuilder.CreateInsertValue(currentValue, outValue, offset);
                    break;
                }
                case '@': {
                    LoadInst *currentValue =
                        irBuilder.CreateLoad(PointerType::get(M.getContext(), 0), aliasNode2StackLocMap[current]);
                    Value *outValue = irBuilder.CreateGEP(
                        Type::getInt8Ty(M.getContext()), currentValue,
                        std::vector<Value *>(1, ConstantInt::get(Type::getInt32Ty(M.getContext()), offset)));
                    irBuilder.CreateStore(outValue, aliasNode2StackLocMap[outNode]);
                }
                }
            }
            // outs() << outNode->Index << aliasNode2StackLocMap[outNode] << "\n";
        }
        // outs() << "\n";
    }
}