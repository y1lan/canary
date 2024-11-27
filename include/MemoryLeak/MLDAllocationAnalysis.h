#ifndef MEMORYLEAK_MLDREPORT_H
#define MEMORYLEAK_MLDREPORT_H

#include "DyckAA/DyckAliasAnalysis.h"
#include "DyckAA/DyckCallGraph.h"
#include <llvm/ADT/StringRef.h>
#include <llvm/Pass.h>
#include <tuple>
#include <vector>
/// Function description for initializing arguments and return value of declared functions.
struct AliasNodeEdgeDesc {
    int StartIndex;
    int EndIndex;
    std::string LabelDesc;
    AliasNodeEdgeDesc(int StartIndex, int EndIndex, std::string LabelDesc)
        : StartIndex(StartIndex),
          EndIndex(EndIndex),
          LabelDesc(LabelDesc) {}
    bool operator<(const AliasNodeEdgeDesc other) const {
        return std::tie(StartIndex, EndIndex, LabelDesc) < std::tie(other.StartIndex, other.EndIndex, other.LabelDesc);
    }
};
struct DeclareFunctionDesc {

    std::string FunctionName;
    std::vector<int> IndexOfParameters;
    std::vector<int> IndexOfReturns;
    std::set<AliasNodeEdgeDesc> AliasGraphOfParameters;
    std::set<AliasNodeEdgeDesc> AliasGraphOfReturns;
    std::set<int> AllocationIndexes;
    DeclareFunctionDesc() {}
    DeclareFunctionDesc(std::string FunctionName, int NumOfParameters, std::vector<int> IndexOfParameters,
                        std::vector<int> IndexOfReturns, std::set<AliasNodeEdgeDesc> AliasGraphOfParameters,
                        std::set<AliasNodeEdgeDesc> AliasGraphOfReturns, std::set<int> AllocationIndexes)
        : FunctionName(FunctionName),
          IndexOfParameters(IndexOfParameters),
          IndexOfReturns(IndexOfReturns),
          AliasGraphOfParameters(AliasGraphOfParameters),
          AliasGraphOfReturns(AliasGraphOfReturns),
          AllocationIndexes(AllocationIndexes) {}
    DeclareFunctionDesc(const DeclareFunctionDesc &other)
        : FunctionName(other.FunctionName),
          IndexOfParameters(other.IndexOfParameters),
          IndexOfReturns(other.IndexOfReturns),
          AliasGraphOfParameters(other.AliasGraphOfParameters),
          AliasGraphOfReturns(other.AliasGraphOfReturns),
          AllocationIndexes(other.AllocationIndexes) {}

    friend std::ostream &operator<<(std::ostream &, DeclareFunctionDesc &);
    friend std::istream &operator>>(std::istream &, DeclareFunctionDesc &);
};

class MLDAllocationAnalysis : public llvm::ModulePass {
public:
    static char ID;
    MLDAllocationAnalysis();
    void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
    bool runOnModule(llvm::Module &M) override;
    std::vector<DeclareFunctionDesc> collectFunctionDescription(llvm::Module &M, DyckAliasAnalysis *DyckAA);
    bool dfsearchAllocationSiteReached(DyckGraphNode *Start, DyckGraph *DyckGraph, int DepthLimit, std::set<int> &Visited,
                                       std::set<AliasNodeEdgeDesc> &EdgeSet, std::set<int> &AllocationIndexes);
    StringRef getPassName() const override {
        return "MLDAllocationAnalysisPass";
    }
    ~MLDAllocationAnalysis() override;
};

#endif
