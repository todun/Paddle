// Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "paddle/fluid/framework/details/memory_optimize_pass.h"
#include <algorithm>
#include <atomic>
#include <deque>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <queue>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>
#include "gflags/gflags.h"
#include "paddle/fluid/framework/data_type.h"
#include "paddle/fluid/framework/ir/graph.h"
#include "paddle/fluid/framework/ir/graph_helper.h"

DEFINE_bool(enable_subgraph_optimize, false,
            "SubGraph also reuse global graph variables, it will reduce the "
            "memory occupation"
            "but a higher risk of memory reuse error. default disabled.");
DEFINE_string(memory_optimize_debug, "",
              "debug the operator output variable when do the variable reuse."
              "memory reuse pass."
              "only for debug, default disabled.");

namespace paddle {
namespace framework {
namespace details {

std::unique_ptr<ir::Graph> MemoryOptimizePass::ApplyImpl(
    std::unique_ptr<ir::Graph> graph) const {
  auto nodes = graph->Nodes();
  CollectSkipVarsSet(nodes);

  cfg_.reset(new details::ControlFlowGraph(*graph));
  cfg_->LiveVariableAnalysis();
  InitSSAGraphNodes();

  int reuse_id = 0;
  for (size_t idx = 0; idx < cfg_->Ops().size(); ++idx) {
    auto& op = cfg_->Ops()[idx];
    auto* op_desc = op->Op();
    // some op in graph has no op desc
    if (op_desc == nullptr) continue;
    if (OpHasSubBlock(op_desc)) {
      if (FLAGS_enable_subgraph_optimize) {
        SubGraphOptimize(op_desc);
      } else {
        VLOG(3) << op->Name()
                << " has subblock, but disable subgraph optimize. skipped.";
        continue;
      }
    }

    for (auto& var : op->outputs) {
      if (!NodeCanReused(var) || cfg_->Use(op).count(var->Name()) == 0 ||
          skip_set_.count(var->Name()))
        continue;
      ir::Node* cache = pool_.FindBestFitNode(var);

      if (var->Name() == FLAGS_memory_optimize_debug) {
        VLOG(3) << "start match var " << DebugString(var) << " of op "
                << op->Name();
        VLOG(3) << pool_.ToString();
        VLOG(3) << "matched in pool : "
                << ((cache == nullptr) ? "False" : "True");
      }

      if (cache == nullptr) continue;
      if (var->Name() == cache->Name()) {
        VLOG(3) << "The same cache variable is cascade reused." << var->Name()
                << " is re-filled to the pool after"
                << "the reused op is finished. Current op can not "
                << "replace it again. Skip this candidate.";
        continue;

        int node_idx_in_pool = pool_.GetNodeIndexInPool(cache);
        VLOG(3) << string::Sprintf(
            "!!! %s,  %s => %s, cache idx %d, pool size %d",
            std::to_string(reuse_id++), DebugString(var), DebugString(cache),
            node_idx_in_pool, static_cast<int>(pool_.size()));

        // update CFG Graph on the fly.
        // reused var maybe re-fill into the pool
        cfg_->RenameVarInCFGGraph(var->Name(), cache->Name(), idx);
        // NOTE(dzhwinter): we need to both update the ProgramDesc
        // and IR Graph. because op_desc/var_desc is used in CreateOp,
        // CreateVar when running happens. But IR Graph
        // define the dependence relationship between nodes.
        RenameVarInGraphDesc(var->Name(), cache->Name(), idx);
        RenameVarInGraphNode(var->Name(), cache->Name(), idx, graph.get());

        pool_.Erase(cache);
      }

      // fill the pool
      std::unordered_set<std::string> unlived_vars;
      for (auto var : cfg_->LiveIn(op)) {
        if (cfg_->LiveOut(op).count(var) == 0) {
          unlived_vars.emplace(var);
        }
      }
      for (auto var : unlived_vars) {
        ir::Node* var_node = cfg_->GetNodeByName(var, op);
        if (NodeCanReused(var_node) && !pool_.Has(var_node)) {
          pool_.Insert(var_node);
        }
      }
    }
  }
  graph->ResolveHazard(var_nodes_);

  return graph;
}

void MemoryOptimizePass::SubGraphOptimize(OpDesc* op_desc) const {
  // conditional block, while op and their grad op
  auto* sub_block_desc =
      AttrReader(op_desc->GetAttrMap()).Get<BlockDesc*>("sub_block");

  // create a mirror block to construct an IR Graph.
  ProgramDesc prog;
  auto* copy_block = prog.MutableBlock(0);
  for (auto* op : sub_block_desc->AllOps()) {
    auto* copy_op = copy_block->AppendOp();
    copy_op->CopyFrom(*op);
    copy_op->Flush();
  }

  for (auto* var : sub_block_desc->AllVars()) {
    auto* copy_var = copy_block->Var(var->Name());
    copy_var->SetDataType(var->GetDataType());
    // only lod tensor can be reused. So ignore the multiple dims case.
    copy_var->SetType(var->GetType());
    copy_var->SetShape(var->GetShape());
    copy_var->SetPersistable(var->Persistable());
  }

  ir::Graph sub_graph(prog);
  std::unordered_set<ir::Node*> sub_graph_all_ops;
  FilterVariables(sub_graph.Nodes(), [&](ir::Node* var) {
    // sub_graph_all_ops.emplace(var);
    if (var->IsVar() && !var->IsCtrlVar()) {
      sub_graph_all_ops.emplace(var);
    }
  });
  int sub_reuse_id = 0;
  // subgraph nodes is unordered, reuse need to follow the desc order.
  // find the right op node through the descs
  for (auto* sub_op_desc : sub_block_desc->AllOps()) {
    ir::Node* sub_op = nullptr;
    for (auto* node : sub_graph_all_ops) {
      if (node->Op() == sub_op_desc) {
        sub_op = node;
        break;
      }
    }
    PADDLE_ENFORCE(sub_op != nullptr);
    for (auto* var : sub_op->outputs) {
      if (NodeCanReused(var)) {
        ir::Node* cache = pool_.FindBestFitNode(var);
        if (cache != nullptr) {
          if (var->Var()->GetDataType() != cache->Var()->GetDataType()) {
            continue;
          }
          int node_idx_in_pool = pool_.GetNodeIndexInPool(cache);
          VLOG(3) << string::Sprintf(
              "!!! %s,  %s => %s, cache idx %d, pool size %d",
              std::to_string(sub_reuse_id++), DebugString(var),
              DebugString(cache), node_idx_in_pool,
              static_cast<int>(pool_.size()));
          // NOTE(dzh): subblock is not in IR graph. Modify the block_desc
          // immediately to make the subblock variable reuse strategy take
          // effect. Because it is a single op in graph. No need to
          // update the ir nodes.
          sub_op_desc->Rename(var->Name(), cache->Name());
          if (sub_op_desc->Block()->HasVar(var->Name())) {
            sub_op_desc->Block()->RemoveVar(var->Name());
          }
        }
      }
    }
  }
}

void MemoryOptimizePass::CollectSkipVarsSet(
    const std::unordered_set<ir::Node*>& nodes) const {
  auto update_skip_set = [&](OpDesc* op_desc) {
    auto inputs = op_desc->InputArgumentNames();
    auto outputs = op_desc->OutputArgumentNames();
    skip_set_.insert(inputs.begin(), inputs.end());
    skip_set_.insert(outputs.begin(), outputs.end());
  };
  for (auto& op : nodes) {
    if (!op->IsOp() || op->Op() == nullptr) continue;
    auto* op_desc = op->Op();
    // NOTE(dzhwinter):
    // current block can not reuse next level block vars.
    if (OpHasSubBlock(op_desc)) update_skip_set(op_desc);
    // NOTE(dzhwinter):
    // distributed ops input/output name need to
    // keep same bettwen trainer/pserver
    if (op_desc->Type() == "send") update_skip_set(op_desc);
    if (op_desc->Type() == "recv") update_skip_set(op_desc);
    if (op_desc->Type() == "prefetch") update_skip_set(op_desc);
  }
}

void MemoryOptimizePass::RenameVarInGraphDesc(const std::string& var,
                                              const std::string& cache_var,
                                              size_t idx) const {
  for (size_t i = idx; i < cfg_->Ops().size(); ++i) {
    auto* op = cfg_->Ops()[i];
    PADDLE_ENFORCE(op->IsOp() && op->Op());
    auto* op_desc = op->Op();
    op_desc->RenameInput(var, cache_var);
    op_desc->RenameOutput(var, cache_var);
    if (op_desc->Block()->HasVar(var)) op_desc->Block()->RemoveVar(var);
    op_desc->Flush();
  }
}

void MemoryOptimizePass::InitSSAGraphNodes() const {
  std::unordered_map<std::string, std::unordered_set<ir::Node*>> all_vars;
  if (var_nodes_.empty()) {
    for (auto* op : cfg_->Ops()) {
      for (auto* node : op->inputs) {
        if (all_vars[node->Name()].count(node) == 0) {
          all_vars[node->Name()].emplace(node);
          var_nodes_[node->Name()].emplace_back(node);
        }
      }
      for (auto* node : op->outputs) {
        if (all_vars[node->Name()].count(node) == 0) {
          all_vars[node->Name()].emplace(node);
          var_nodes_[node->Name()].emplace_back(node);
        }
      }
    }
  }
}

void MemoryOptimizePass::RenameVarInGraphNode(const std::string& var,
                                              const std::string& cache_var,
                                              size_t idx,
                                              ir::Graph* graph) const {
  // if replace happens, we need to create a newer version cache_var
  // but use the same dims/data_type with var.
  PADDLE_ENFORCE(var_nodes_[var].size() >= 1 &&
                 var_nodes_[var].at(0)->Var() != nullptr);
  std::unique_ptr<VarDesc> var_desc(new VarDesc(*var_nodes_[var].at(0)->Var()));
  var_desc->SetName(cache_var);

  for (size_t i = idx; i < cfg_->Ops().size(); ++i) {
    auto* op = cfg_->Ops()[i];

    // redirect the input to the latest version of cache_var
    for (auto* node : op->inputs) {
      if (node->Name() == var) {
        ir::Node* cache_node = graph->CreateVarNode(var_desc.get());
        var_nodes_[cache_var].emplace_back(cache_node);

        // swap node to cache_node
        cache_node->outputs.insert(cache_node->outputs.end(),
                                   node->outputs.begin(), node->outputs.end());
        PADDLE_ENFORCE(node->inputs.size() == 1 && node->inputs[0]->IsOp());
        auto* prev_op = node->inputs[0];
        std::replace(prev_op->outputs.begin(), prev_op->outputs.end(), node,
                     cache_node);
        cache_node->inputs.emplace_back(prev_op);
        for (auto* next_op : node->outputs) {
          std::replace(next_op->inputs.begin(), next_op->inputs.end(), node,
                       cache_node);
        }
      }
    }

    // if we need to rename the output,
    // always create a newer version of cache_var
    for (auto* node : op->outputs) {
      if (node->Name() == var) {
        ir::Node* cache_node = graph->CreateVarNode(var_desc.get());
        var_nodes_[cache_var].emplace_back(cache_node);

        // swap node to cache node
        cache_node->outputs.insert(cache_node->outputs.end(),
                                   node->outputs.begin(), node->outputs.end());
        cache_node->inputs.emplace_back(op);
        std::replace(op->outputs.begin(), op->outputs.end(), node, cache_node);
        for (auto* next_op : node->outputs) {
          std::replace(next_op->inputs.begin(), next_op->inputs.end(), node,
                       cache_node);
        }
      }
    }
  }

  // release node of unused var in graph
  for (auto* node : var_nodes_[var]) {
    graph->RemoveNode(node);
  }
  var_nodes_.at(var).clear();
}

}  // namespace details
}  // namespace framework
}  // namespace paddle

REGISTER_PASS(memory_optimize_pass,
              paddle::framework::details::MemoryOptimizePass)
    .RequireGraphAttr(paddle::framework::details::kAllOpDescs);
