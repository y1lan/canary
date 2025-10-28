#include "MemoryLeak/MLDAllocationAnalysis.h"
#include "DyckAA/DyckAliasAnalysis.h"
#include "DyckAA/DyckCallGraph.h"
#include "DyckAA/DyckCallGraphNode.h"
#include "DyckAA/DyckGraph.h"
#include "DyckAA/DyckGraphEdgeLabel.h"
#include "DyckAA/DyckGraphNode.h"
#include "DyckAA/DyckModRefAnalysis.h"
#include "DyckAA/DyckValueFlowAnalysis.h"
#include "MemoryLeak/MLDReport.h"
#include "MemoryLeak/MLDVFG.h"
#include "MemoryLeak/MLDValueFlowAnalysis.h"
#include "Support/RecursiveTimer.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <llvm/IR/Argument.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/raw_os_ostream.h>
#include <llvm/Support/raw_ostream.h>
#include <system_error>
#include <utility>
#include <vector>

char MLDAllocationAnalysis::ID = 0;

MLDAllocationAnalysis::MLDAllocationAnalysis()
    : llvm::ModulePass(ID) {}

MLDAllocationAnalysis::~MLDAllocationAnalysis() {}

void MLDAllocationAnalysis::getAnalysisUsage(AnalysisUsage &AU) const {
    AU.setPreservesAll();
    AU.addRequired<DyckAliasAnalysis>();
    AU.addRequired<DyckModRefAnalysis>();
    AU.addRequired<DyckValueFlowAnalysis>();
}

bool MLDAllocationAnalysis::runOnModule(llvm::Module &M) {
    RecursiveTimer mldallocationanalysis("MLDAllocationAnalysis");
    auto *DyckAA = &getAnalysis<DyckAliasAnalysis>();
    auto *DyckVFG = &getAnalysis<DyckValueFlowAnalysis>();
    DyckAA->getDyckCallGraph()->constructCallSiteMap();
    // Now we add notation to alias space of these vfg nodes where source nodes
    // can reach
    MLDVFG *VFG = new MLDVFG(DyckVFG->getDyckVFGraph(), DyckAA->getDyckCallGraph(), DyckAA);
    std::vector<DeclareFunctionDesc> funcDescVec = collectFunctionDescription(M, DyckAA);
    assert(!PrintCSourceFunctions.getValue().empty());
    std::ofstream out_stream(PrintCSourceFunctions.getValue());
    out_stream << funcDescVec.size() << "\n";
    for (DeclareFunctionDesc desc : funcDescVec) {
        out_stream << desc;
    }
    out_stream.close();
    return false;
}

// Naive implementation of searching for allocation sites reached by parameter, which cannout ensure precise and correct.
std::vector<DeclareFunctionDesc> MLDAllocationAnalysis::collectFunctionDescription(llvm::Module &M, DyckAliasAnalysis *DyckAA) {
    std::vector<DeclareFunctionDesc> funcDescVec;
    for (Function &func : M) {
        std::set<int> allocationIndexes;
        std::set<AliasNodeEdgeDesc> paramEdgeSet;
        std::vector<int> indexOfParameters;
        std::vector<int> indexOfReturns;
        for (auto argIt = func.arg_begin(); argIt != func.arg_end(); argIt++) {
            DyckGraphNode *start = DyckAA->getDyckGraph()->retrieveDyckVertex(dyn_cast<Value>(argIt)).first;
            indexOfParameters.push_back(start->getIndex());
            std::set<int> visited;
            dfsearchAllocationSiteReached(start, DyckAA->getDyckGraph(), 5, visited, paramEdgeSet, allocationIndexes);
        }
        std::set<AliasNodeEdgeDesc> retEdgeSet;
        DyckCallGraphNode *CGNode = DyckAA->getDyckCallGraph()->getFunction(&func);
        for (auto retIt = CGNode->getReturns().begin(); retIt != CGNode->getReturns().end(); retIt++) {
            DyckGraphNode *start = DyckAA->getDyckGraph()->retrieveDyckVertex(*retIt).first;
            indexOfReturns.push_back(start->getIndex());
            std::set<int> visited;
            dfsearchAllocationSiteReached(start, DyckAA->getDyckGraph(), 5, visited, retEdgeSet, allocationIndexes);
        }
        if (allocationIndexes.empty()) {
            continue;
        }
        if (allocationIndexes.size() > 0) {
            funcDescVec.push_back({func.getName().str(), static_cast<int>(func.arg_size()), indexOfParameters, indexOfReturns,
                                   paramEdgeSet, retEdgeSet, allocationIndexes});
        }
    }
    return funcDescVec;
}

bool MLDAllocationAnalysis::dfsearchAllocationSiteReached(DyckGraphNode *Start, DyckGraph *DyckGraph, int DepthLimit,
                                                          std::set<int> &Visited, std::set<AliasNodeEdgeDesc> &EdgeSet,
                                                          std::set<int> &AllocationIndexes) {
    if (DepthLimit == 0 || Visited.find((Start)->getIndex()) != Visited.end()) {
        if (Start->isAliasOfHeapAlloc()) {
            return true;
        }
        else {
            return false;
        }
    }
    Visited.insert(Start->getIndex());
    bool ret = false;
    if (Start->isAliasOfHeapAlloc()) {
        ret = true;
        AllocationIndexes.insert(Start->getIndex());
    }
    std::map<DyckGraphEdgeLabel *, std::set<DyckGraphNode *>> &outNodes = Start->getOutVertices();
    for (auto edgeNodeSetPairIt = outNodes.begin(); edgeNodeSetPairIt != outNodes.end(); edgeNodeSetPairIt++) {
        for (auto outNodeIt = edgeNodeSetPairIt->second.begin(); outNodeIt != edgeNodeSetPairIt->second.end(); outNodeIt++) {
            if (dfsearchAllocationSiteReached(*outNodeIt, DyckGraph, DepthLimit - 1, Visited, EdgeSet, AllocationIndexes)) {
                EdgeSet.insert(
                    {Start->getIndex(), (*outNodeIt)->getIndex(), edgeNodeSetPairIt->first->getEdgeLabelDescription()});
                ret = true;
            }
        }
    }
    Visited.erase(Start->getIndex());
    return ret;
}
std::ostream &operator<<(std::ostream &Out, DeclareFunctionDesc &DecFuncDesc) {
    Out << DecFuncDesc.FunctionName << std::endl << DecFuncDesc.IndexOfParameters.size() << std::endl;
    for (auto i : DecFuncDesc.IndexOfParameters) {
        Out << i << " ";
    }
    Out << std::endl;
    Out << DecFuncDesc.IndexOfReturns.size() << std::endl;

    for (auto i : DecFuncDesc.IndexOfReturns) {
        Out << i << " ";
    }
    Out << std::endl;
    Out << DecFuncDesc.AliasGraphOfParameters.size() << std::endl;
    for (auto edgeDesc : DecFuncDesc.AliasGraphOfParameters) {
        Out << edgeDesc.StartIndex << " " << edgeDesc.EndIndex << " " << edgeDesc.LabelDesc << " ";
    }
    Out << std::endl;
    Out << DecFuncDesc.AliasGraphOfReturns.size() << std::endl;
    for (auto edgeDesc : DecFuncDesc.AliasGraphOfReturns) {
        Out << edgeDesc.StartIndex << " " << edgeDesc.EndIndex << " " << edgeDesc.LabelDesc << " ";
    }
    Out << std::endl;
    Out << DecFuncDesc.AllocationIndexes.size() << std::endl;
    for (auto i : DecFuncDesc.AllocationIndexes) {
        Out << i << " ";
    }
    Out << std::endl;
    return Out;
}
std::istream &operator>>(std::istream &In, DeclareFunctionDesc &DecFuncDesc) {
    int numOfParameters;
    In >> DecFuncDesc.FunctionName >> numOfParameters;
    for (int i = 0; i < numOfParameters; i++) {
        int j = 0;
        In >> j;
        DecFuncDesc.IndexOfParameters.emplace_back(j);
    }
    int numOfReturns;
    In >> numOfReturns;
    for (int i = 0; i < numOfReturns; i++) {
        int j = 0;
        In >> j;
        DecFuncDesc.IndexOfReturns.emplace_back(j);
    }
    size_t sizeOfAliasGraphOfParameters = 0;
    In >> sizeOfAliasGraphOfParameters;
    for (int i = 0; i < sizeOfAliasGraphOfParameters; i++) {
        int startIndex = 0, endIndex = 0;
        std::string labelDesc;
        In >> startIndex >> endIndex >> labelDesc;
        DecFuncDesc.AliasGraphOfParameters.emplace(startIndex, endIndex, labelDesc);
    }
    size_t sizeOfAliasGraphOfReturns = 0;
    In >> sizeOfAliasGraphOfReturns;
    for (int i = 0; i < sizeOfAliasGraphOfReturns; i++) {
        int startIndex = 0, endIndex = 0;
        std::string labelDesc;
        In >> startIndex >> endIndex >> labelDesc;
        DecFuncDesc.AliasGraphOfReturns.emplace(startIndex, endIndex, labelDesc);
    }
    size_t sizeOfAllocationIndexes = 0;
    In >> sizeOfAllocationIndexes;
    for (int i = 0; i < sizeOfAllocationIndexes; i++) {
        int index = 0;
        In >> index;
        DecFuncDesc.AllocationIndexes.emplace(index);
    }
    return In;
}