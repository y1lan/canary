#ifndef MEMORYLEAK_MLDINSTRUMENTATION_H
#define MEMORYLEAK_MLDINSTRUMENTATION_H
#include "MemoryLeak/MLDAllocationAnalysis.h"
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Pass.h>
#include <map>
#include <string>
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
    NaiveAliasGraph(const DeclareFunctionDesc &desc);
    void instrument(IRBuilder<> &irBuilder, CallInst *callInst, Module &M);
    void instrumentRe(IRBuilder<> &irBuilder, CallInst *callInst, Module &M);
    void instrumentReFree(IRBuilder<> &irBuilder, CallInst *callInst, Module &M);
    ~NaiveAliasGraph();
    friend std::ostream &operator<<(std::ostream &os, const NaiveAliasGraph &graph) {
        os << graph.FuncDesc.FunctionName << std::endl;
        os << graph.ParamNodes.size() << std::endl;
        for (auto node : graph.ParamNodes) {
            os << node->Index << " ";
        }
        os << std::endl;
        if (graph.retNode == nullptr) {
            os << -1 << std::endl;
        }
        else {
            os << graph.retNode->Index << std::endl;
        }
        os << graph.FuncDesc.IndexOfParameters.size() << std::endl;
        for (auto edge : graph.FuncDesc.AliasGraphOfParameters) {
            os << edge.StartIndex << " " << edge.EndIndex << " " << edge.LabelDesc << " ";
        }
        os << std::endl;
        os << graph.FuncDesc.IndexOfReturns.size() << std::endl;
        for (auto edge : graph.FuncDesc.AliasGraphOfReturns) {
            os << edge.StartIndex << " " << edge.EndIndex << " " << edge.LabelDesc << " ";
        }
        os << std::endl;
        os << graph.FuncDesc.AllocationIndexes.size() << std::endl;
        for (auto index : graph.FuncDesc.AllocationIndexes) {
            os << index << " ";
        }
        os << std::endl;
        return os;
    }
};
class MLDInstrumentation : public llvm::ModulePass {
private:
    std::map<std::string, NaiveAliasGraph *> FuncDescMap;
    std::map<std::string, NaiveAliasGraph *> freeWrapperFuncDescMap;

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