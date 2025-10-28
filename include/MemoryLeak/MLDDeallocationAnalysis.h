#ifndef MEMORYLEAK_MLDDEALLOCATIONANALYSIS_H
#define MEMORYLEAK_MLDDEALLOCATIONANALYSIS_H



#include <llvm/Pass.h>
#include "DyckAA/DyckAliasAnalysis.h"
#include "DyckAA/DyckGraphNode.h"
#include "MemoryLeak/MLDAllocationAnalysis.h"
#include "MemoryLeak/MLDVFG.h"
class MLDDeallocationAnalysis : public llvm::ModulePass{
public:
    static char ID;
    MLDDeallocationAnalysis();
    void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
    bool runOnModule(llvm::Module &M) override;
    llvm::StringRef getPassName() const override{
        return "MLDDeallocationAnalysisPass";
    }
    ~MLDDeallocationAnalysis() override;
    std::vector<DeclareFunctionDesc> collectFunctionDescription(llvm::Module &M, DyckAliasAnalysis *DyckAA, MLDVFG * VFG);
    bool dfsearchAllocationSiteReached(DyckGraphNode *Start, DyckGraph *DyckGraph, int DepthLimit, std::set<int> &Visited,
                                       std::set<AliasNodeEdgeDesc> &EdgeSet, std::set<int> &AllocationIndexes, std::set<DyckGraphNode *> &FreeSites);
};

#endif