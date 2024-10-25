#ifndef MEMORYLEAK_MLDREPORT_H
#define MEMORYLEAK_MLDREPORT_H

#include "DyckAA/DyckAliasAnalysis.h"
#include "DyckAA/DyckCallGraph.h"
#include <llvm/Pass.h>
#include <vector>


class MLDAllocationAnalysis : public llvm::ModulePass {
public:
    static char ID;
    MLDAllocationAnalysis();
    void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
    bool runOnModule(llvm::Module &M) override;
    std::vector<DeclareFunctionDesc> collectFunctionDescription(llvm::Module &M, DyckAliasAnalysis *DyckAA);
    bool dfsearchAllocationSiteReached(DyckGraphNode *Start, DyckGraph *DyckGraph, int DepthLimit, std::set<int> &Visited,
                                       std::set<AliasNodeEdgeDesc> &EdgeSet,std::set<int> &AllocationIndexes);
    ~MLDAllocationAnalysis();
};

#endif
