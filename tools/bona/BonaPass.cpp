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

#include <llvm/IR/Dominators.h>
#include "BonaPass.h"
#include "DyckAA/DyckAliasAnalysis.h"
#include "NCA/LocalNullCheckAnalysis.h"
#include "Support/ThreadPool.h"
#include "Support/TimeRecorder.h"

char BonaPass::ID = 0;
static RegisterPass<BonaPass> X("bona", "soundly checking if a pointer may be nullptr.");

BonaPass::BonaPass() : ModulePass(ID) {}

BonaPass::~BonaPass() = default;

void BonaPass::getAnalysisUsage(AnalysisUsage &AU) const {
    AU.addRequired<DyckAliasAnalysis>();
    AU.addRequired<DominatorTreeWrapperPass>();
    AU.setPreservesAll();
}

bool BonaPass::runOnModule(Module &M) {
    TimeRecorder TR("NCA");
    for (auto &F: M)
        if (!F.empty()) {
            ThreadPool::get()->enqueue([this, &F]() {
                LocalNullCheckAnalysis(this, &F).run();
            });
        }
    ThreadPool::get()->waitAll();
    return false;
}
