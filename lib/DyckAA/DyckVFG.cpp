/*
 *  Canary features a fast unification-based alias analysis for C programs
 *  Copyright (C) 2021 Qingkai Shi <qingkaishi@gmail.com>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Affero General Public License as published
 *  by the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Affero General Public License for more details.
 *
 *  You should have received a copy of the GNU Affero General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include "DyckAA/DyckAliasAnalysis.h"
#include "DyckAA/DyckGraph.h"
#include "DyckAA/DyckGraphNode.h"
#include "DyckAA/DyckModRefAnalysis.h"
#include "DyckAA/DyckVFG.h"
#include "Support/CFG.h"

DyckVFG::DyckVFG(DyckAliasAnalysis *DAA, DyckModRefAnalysis* DMRA, Module *M) {
    // create a VFG for each function
    std::map<Function *, DyckVFG *> LocalVFGMap;
    for (auto &F: *M) {
        if (F.empty()) continue;
        LocalVFGMap[&F] = new DyckVFG(DAA, DMRA, &F);
    }

    // connect local VFGs
    auto *DyckCG = DAA->getDyckCallGraph();
    for (auto &It: LocalVFGMap) {
        auto *F = It.first;
        auto *G = It.second;
        auto *CGNode = DyckCG->getFunction(F);
        if (!CGNode) continue;
        for (auto &I: instructions(F)) {
            auto *CI = dyn_cast<CallInst>(&I);
            if (!CI) continue;
            auto *TheCall = CGNode->getCall(CI);
            if (auto *CC = dyn_cast<CommonCall>(TheCall)) {
                auto *Callee = dyn_cast<Function>(CC->getCalledFunction());
                assert(Callee);
                auto *&CalleeVFG = LocalVFGMap.at(Callee);
                G->connect(DAA, DMRA, TheCall, Callee, CalleeVFG);
                if (G == CalleeVFG) continue;
                G->mergeAndDelete(CalleeVFG);
                CalleeVFG = G; // update the graph G
            } else if (auto *PC = dyn_cast<PointerCall>(TheCall)) {
                for (Function *Callee: *PC) {
                    auto *&CalleeVFG = LocalVFGMap.at(Callee);
                    G->connect(DAA, DMRA, TheCall, Callee, CalleeVFG);
                    if (G == CalleeVFG) continue;
                    G->mergeAndDelete(CalleeVFG);
                    CalleeVFG = G; // update the graph G
                }
            } else {
                errs() << I << "\n";
                llvm_unreachable("unknown call type");
            }
        }
    }

    // finalize VFG (merge all VFGs to one), delete useless ones
    //  (a constant may have multi vfg nodes across diff functions)
    auto It = LocalVFGMap.begin();
    if (It == LocalVFGMap.end()) return;
    auto *G = LocalVFGMap.begin()->second;
    while (++It != LocalVFGMap.end()) {
        auto *&VFG = It->second;
        if (G != VFG) {
            G->mergeAndDelete(VFG);
            VFG = G;
        }
    }
    this->ValueNodeMap.swap(G->ValueNodeMap);
    assert(G->ValueNodeMap.empty());
    delete G;
}

DyckVFG::DyckVFG(DyckAliasAnalysis *DAA, DyckModRefAnalysis* DMRA, Function *F) {
    // direct value flow through cast, gep-0-0, select, phi
    for (auto &I: instructions(F)) {
        if (isa<CastInst>(I) || isa<PHINode>(I)) {
            auto *ToNode = getOrCreateVFGNode(&I);
            for (unsigned K = 0; K < I.getNumOperands(); ++K) {
                auto *From = I.getOperand(K);
                auto *FromNode = getOrCreateVFGNode(From);
                FromNode->addTarget(ToNode);
            }
        } else if (isa<SelectInst>(I)) {
            auto *ToNode = getOrCreateVFGNode(&I);
            for (unsigned K = 1; K < I.getNumOperands(); ++K) {
                auto *From = I.getOperand(K);
                auto *FromNode = getOrCreateVFGNode(From);
                FromNode->addTarget(ToNode);
            }
        } else if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
            bool VF = true;
            for (auto &Index: GEP->indices()) {
                if (auto *CI = dyn_cast<ConstantInt>(&Index)) {
                    if (CI->getSExtValue() != 0) {
                        VF = false;
                        break;
                    }
                }
            }
            if (VF) {
                auto *ToNode = getOrCreateVFGNode(&I);
                auto *FromNode = getOrCreateVFGNode(GEP->getPointerOperand());
                FromNode->addTarget(ToNode);
            }
        }
    }

    // indirect value flow through load/store
    auto *DG = DAA->getDyckGraph();
    std::map<DyckGraphNode *, std::vector<LoadInst *>> LoadMap; // ptr -> load
    std::map<DyckGraphNode *, std::vector<StoreInst *>> StoreMap; // ptr -> store
    for (auto &I: instructions(F)) {
        if (auto *Load = dyn_cast<LoadInst>(&I)) {
            auto *Ptr = Load->getPointerOperand();
            auto *DV = DG->findDyckVertex(Ptr);
            if (DV) LoadMap[DV].push_back(Load);
        } else if (auto *Store = dyn_cast<StoreInst>(&I)) {
            auto *Ptr = Store->getPointerOperand();
            auto *DV = DG->findDyckVertex(Ptr);
            if (DV) StoreMap[DV].push_back(Store);
        }
    }
    // match load and store:
    // if alias(load's ptr, store's ptr) and store -> load in CFG, add store's value -> load's value in VFG
    CFG CtrlFlowG(F);
    for (auto &LoadIt: LoadMap) {
        auto *DyckNode = LoadIt.first;
        auto &Loads = LoadIt.second;
        auto StoreIt = StoreMap.find(DyckNode);
        if (StoreIt == StoreMap.end()) continue;
        auto &Stores = StoreIt->second;
        for (auto *Load: Loads) {
            auto *LdNode = getOrCreateVFGNode(Load);
            for (auto *Store: Stores)
                if (CtrlFlowG.reachable(Store, Load)) getOrCreateVFGNode(Store->getValueOperand())->addTarget(LdNode);
        }
    }
}

DyckVFG::~DyckVFG() {
    for (auto &It: ValueNodeMap) delete It.second;
}

DyckVFGNode *DyckVFG::getVFGNode(Value *V) {
    auto It = ValueNodeMap.find(V);
    if (It == ValueNodeMap.end()) return nullptr;
    return It->second;
}

DyckVFGNode *DyckVFG::getOrCreateVFGNode(Value *V) {
    auto It = ValueNodeMap.find(V);
    if (It == ValueNodeMap.end()) {
        auto *Ret = new DyckVFGNode(V);
        ValueNodeMap[V] = Ret;
        return Ret;
    }
    return It->second;
}

void DyckVFG::mergeAndDelete(DyckVFG *G) {
    // move all vertices in the input VFG to this one (note: a constant may have multiple nodes)
    // G is released and invalid after this function
    for (auto &It: G->ValueNodeMap) {
        auto *Val = It.first;
        auto *Node = It.second;

        if (isa<Constant>(Val)) {
            auto ThisIt = ValueNodeMap.find(Val);
            if (ThisIt == ValueNodeMap.end()) {
                ValueNodeMap.emplace(Val, Node);
            } else {
                auto *ThisNode = ThisIt->second;
                for (auto *Tar: *Node) ThisNode->addTarget(Tar);
                delete Node;
                It.second = ThisNode;
            }
        } else {
            ValueNodeMap.emplace(Val, Node);
        }
    }
    G->ValueNodeMap.clear(); // to prevent the next delete from deleting nodes in G
    delete G;
}

void DyckVFG::connect(DyckAliasAnalysis *DAA, DyckModRefAnalysis *DMRA, Call *C, Function* Callee, DyckVFG *CalleeVFG) {
    // connect direct inputs
    for (unsigned K = 0; K < C->numArgs(); ++K) {
        if (K >= Callee->arg_size()) continue; // ignore var args
        auto *Actual = C->getArg(K);
        auto *ActualNode = getOrCreateVFGNode(Actual);
        auto *Formal = Callee->getArg(K);
        auto *FormalNode = CalleeVFG->getOrCreateVFGNode(Formal);
        ActualNode->addTarget(FormalNode);
    }
    // connect direct outputs
    if (!C->getInstruction()->getType()->isVoidTy()) {
        auto *ActualRet = getOrCreateVFGNode(C->getInstruction());
        for (auto &Inst: instructions(Callee)) {
            auto *RetInst = dyn_cast<ReturnInst>(&Inst);
            if (!RetInst) continue;
            if (RetInst->getNumOperands() != 1) continue;
            auto *FormalRet = CalleeVFG->getOrCreateVFGNode(Inst.getOperand(0));
            FormalRet->addTarget(ActualRet);
        }
    }

    if (!DMRA->count(Callee)) return; // this callee does not contain mod/refs except for formal parameters/rets

    // todo connect indirect inputs
    //  1. get refs, get ref values (in caller and callee)
    //  2. connect ref values (caller) -> ref values (callee)

    // todo connect indirect outputs
    //  1. get mods, get mod values (in caller and callee)
    //  2. connect ref values (callee) -> ref values (caller)
}
