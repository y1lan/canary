#include "MemoryLeak/MLDDeallocationAnalysis.h"
#include "DyckAA/DyckAliasAnalysis.h"
#include "DyckAA/DyckCallGraph.h"
#include "DyckAA/DyckCallGraphNode.h"
#include "DyckAA/DyckGraph.h"
#include "DyckAA/DyckGraphEdgeLabel.h"
#include "DyckAA/DyckGraphNode.h"
#include "DyckAA/DyckModRefAnalysis.h"
#include "DyckAA/DyckValueFlowAnalysis.h"
#include "MemoryLeak/MLDAllocationAnalysis.h"
#include "MemoryLeak/MLDVFG.h"
#include "Support/RecursiveTimer.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Pass.h"
#include <cassert>
#include <fstream>
#include <ostream>
#include <vector>
#include <iostream>

char MLDDeallocationAnalysis::ID = 0;

MLDDeallocationAnalysis::MLDDeallocationAnalysis()
    : llvm::ModulePass(ID) {}
    
MLDDeallocationAnalysis::~MLDDeallocationAnalysis(){
    
}

void MLDDeallocationAnalysis::getAnalysisUsage(llvm::AnalysisUsage &AU) const {
    AU.setPreservesAll();
    AU.addRequired<DyckAliasAnalysis>();
    AU.addRequired<DyckModRefAnalysis>();
    AU.addRequired<DyckValueFlowAnalysis>();
    AU.addRequired<MLDAllocationAnalysis>();
}

bool MLDDeallocationAnalysis::runOnModule(llvm::Module &M) {
    RecursiveTimer mlddeallocationanalysis("MLDDeallocationAnalysis");
    auto *DyckAA = &getAnalysis<DyckAliasAnalysis>();
    auto *DyckVFG = &getAnalysis<DyckValueFlowAnalysis>();
    DyckAA->getDyckCallGraph()->constructCallSiteMap();
    // Now we add notation to alias space of these vfg nodes where source nodes
    // can reach
    MLDVFG *VFG = new MLDVFG(DyckVFG->getDyckVFGraph(), DyckAA->getDyckCallGraph(), DyckAA);
    std::vector<DeclareFunctionDesc> funcDescVec = collectFunctionDescription(M, DyckAA, VFG);
    assert(!PrintCSinkFunctions.getValue().empty());
    std::ofstream out_stream(PrintCSinkFunctions.getValue());

    out_stream << funcDescVec.size() << "\n";
    for (DeclareFunctionDesc desc : funcDescVec) {
        out_stream << desc;
    }
    out_stream.close();
    return false;
}

std::vector<DeclareFunctionDesc> MLDDeallocationAnalysis::collectFunctionDescription(llvm::Module &M, DyckAliasAnalysis *DyckAA,
                                                                                     MLDVFG *VFG) {
    std::vector<DeclareFunctionDesc> funcDescVec;
    std::set<llvm::Function *> freeWrapperFunction;
    std::map<llvm::Function *, std::set<DyckGraphNode *>> function2DeallocationSites;
    for (auto &F : M) {
        for (auto &Inst : instructions(F)) {
            if (CallInst *callInst = dyn_cast_or_null<CallInst>(&Inst)) {
                if (Function *calledFunction = callInst->getCalledFunction()) {
                    if (calledFunction->getName().str() == "free") {
                        freeWrapperFunction.insert(&F);
                        Value *freeArgument = callInst->getArgOperand(0);
                        function2DeallocationSites[&F].insert(DyckAA->getDyckGraph()->retrieveDyckVertex(freeArgument).first);
                    }
                }
            }
        }
    }
    for (auto *func : freeWrapperFunction) {
        std::set<int> allocationIndexes;
        std::set<AliasNodeEdgeDesc> paramEdgeSet;
        std::vector<int> indexOfParameters;
        std::vector<int> indexOfReturns;
        for (auto argIt = func->arg_begin(); argIt != func->arg_end(); argIt++) {
            DyckGraphNode *start = DyckAA->getDyckGraph()->retrieveDyckVertex(dyn_cast<Value>(argIt)).first;
            indexOfParameters.push_back(start->getIndex());
            std::set<int> visited;
            dfsearchAllocationSiteReached(start, DyckAA->getDyckGraph(), 5, visited, paramEdgeSet, allocationIndexes,
                                          function2DeallocationSites[func]);
        }
        std::set<AliasNodeEdgeDesc> retEdgeSet;
        // DyckCallGraphNode *CGNode = DyckAA->getDyckCallGraph()->getFunction(func);
        // for (auto retIt = CGNode->getReturns().begin(); retIt != CGNode->getReturns().end(); retIt++) {
        //     DyckGraphNode *start = DyckAA->getDyckGraph()->retrieveDyckVertex(*retIt).first;
        //     indexOfReturns.push_back(start->getIndex());
        //     std::set<int> visited;
        //     dfsearchAllocationSiteReached(start, DyckAA->getDyckGraph(), 5, visited, retEdgeSet, allocationIndexes,
        //                                   function2DeallocationSites[func]);
        // }
        if (allocationIndexes.empty()) {
            continue;
        }
        if (allocationIndexes.size() > 0) {
            funcDescVec.push_back({func->getName().str(), static_cast<int>(func->arg_size()), indexOfParameters, indexOfReturns,
                                   paramEdgeSet, retEdgeSet, allocationIndexes});
        }
    }
    return funcDescVec;
}

bool MLDDeallocationAnalysis::dfsearchAllocationSiteReached(DyckGraphNode *Start, DyckGraph *DyckGraph, int DepthLimit,
                                                            std::set<int> &Visited, std::set<AliasNodeEdgeDesc> &EdgeSet,
                                                            std::set<int> &AllocationIndexes,
                                                            std::set<DyckGraphNode *> &FreeSites) {
    auto inFreeSite = [&FreeSites](DyckGraphNode *Current) { return FreeSites.find(Current) != FreeSites.end(); };
    if (DepthLimit == 0 || Visited.find((Start)->getIndex()) != Visited.end()) {
        if (inFreeSite(Start)) {
            return true;
        }
        else {
            return false;
        }
    }
    Visited.insert(Start->getIndex());
    bool ret = false;
    if (inFreeSite(Start)) {
        ret = true;
        AllocationIndexes.insert(Start->getIndex());
    }
    std::map<DyckGraphEdgeLabel *, std::set<DyckGraphNode *>> &outNodes = Start->getOutVertices();
    for (auto edgeNodeSetPairIt = outNodes.begin(); edgeNodeSetPairIt != outNodes.end(); edgeNodeSetPairIt++) {
        for (auto outNodeIt = edgeNodeSetPairIt->second.begin(); outNodeIt != edgeNodeSetPairIt->second.end(); outNodeIt++) {
            if (dfsearchAllocationSiteReached(*outNodeIt, DyckGraph, DepthLimit - 1, Visited, EdgeSet, AllocationIndexes,
                                              FreeSites)) {
                EdgeSet.insert(
                    {Start->getIndex(), (*outNodeIt)->getIndex(), edgeNodeSetPairIt->first->getEdgeLabelDescription()});
                ret = true;
            }
        }
    }
    Visited.erase(Start->getIndex());
    return ret;
}