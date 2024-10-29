#ifndef MEMORYLEAK_MLDINSTRUMENTATION_H
#define MEMORYLEAK_MLDINSTRUMENTATION_H
#include "MemoryLeak/MLDAllocationAnalysis.h"
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Pass.h>
#include <map>
#include <vector>

class NaiveAliasGraphNode {
public:
    int Index;
    bool Allocation;
    std::vector<std::pair<NaiveAliasGraphNode *, std::string>> OutEdges;
    std::vector<std::pair<NaiveAliasGraphNode *, std::string>> InEdges;
    NaiveAliasGraphNode(int Index, bool Allocation)
        : Index(Index),
          Allocation(Allocation) {}
    void addEdge(NaiveAliasGraphNode *Target, std::string Label) {
        OutEdges.emplace_back(Target, Label);
        Target->InEdges.emplace_back(this, Label);
    }
};

class NaiveAliasGraph {
    NaiveAliasGraphNode *insertOrGetNode(int Index, bool Allocated);

public:
    DeclareFunctionDesc FuncDesc;
    std::vector<NaiveAliasGraphNode *> ParamNodes;
    NaiveAliasGraphNode *retNode;
    std::map<int, NaiveAliasGraphNode *> NodesMap;
    NaiveAliasGraph(DeclareFunctionDesc &desc);
    void instrument(IRBuilder<> &irBuilder, CallInst *callInst, Module& M);
    ~NaiveAliasGraph();
};

class MLDInstrumentation : public llvm::ModulePass {
private:
    std::map<std::string, NaiveAliasGraph *> FuncDescMap;

public:
    static char ID;
    MLDInstrumentation();
    // void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
    bool runOnModule(llvm::Module &F) override;
    llvm::StringRef getPassName() const override {
        return "MLDInstrumentation";
    }
    ~MLDInstrumentation() override;
};

#endif