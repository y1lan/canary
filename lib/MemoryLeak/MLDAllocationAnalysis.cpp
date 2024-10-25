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
#include <cassert>
#include <fstream>
#include <llvm/IR/Argument.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/raw_os_ostream.h>
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
    auto *DyckAA = &getAnalysis<DyckAliasAnalysis>();
    auto *DyckVFG = &getAnalysis<DyckValueFlowAnalysis>();
    DyckAA->getDyckCallGraph()->constructCallSiteMap();
    // Now we add notation to alias space of these vfg nodes where source nodes
    // can reach
    MLDVFG *VFG = new MLDVFG(DyckVFG->getDyckVFGraph(), DyckAA->getDyckCallGraph(), DyckAA);
    std::vector<DeclareFunctionDesc> funcDescVec = collectFunctionDescription(M, DyckAA);
    assert(PrintCSourceFunctions);
    std::fstream out_stream(PrintCSourceFunctionsFileName);
    for (DeclareFunctionDesc desc :funcDescVec){
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
        for (auto argIt = func.arg_begin(); argIt != func.arg_end(); argIt++) {
            DyckGraphNode *start = DyckAA->getDyckGraph()->retrieveDyckVertex(dyn_cast<Value>(argIt)).first;
            indexOfParameters.push_back(start->getIndex());
            std::set<int> visited;
            dfsearchAllocationSiteReached(start, DyckAA->getDyckGraph(), 20, visited, paramEdgeSet, allocationIndexes);
        }
        std::set<AliasNodeEdgeDesc> retEdgeSet;
        DyckCallGraphNode *CGNode = DyckAA->getDyckCallGraph()->getFunction(&func);
        for (auto retIt = CGNode->getReturns().begin(); retIt != CGNode->getReturns().end(); retIt++) {
            DyckGraphNode *start = DyckAA->getDyckGraph()->retrieveDyckVertex(*retIt).first;
            std::set<int> visited;
            dfsearchAllocationSiteReached(start, DyckAA->getDyckGraph(), 20, visited, retEdgeSet, allocationIndexes);
        }
        funcDescVec.push_back({func.getName().str(), static_cast<int>(func.arg_size()), indexOfParameters, paramEdgeSet,
                               retEdgeSet, allocationIndexes});
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
    return true;
}