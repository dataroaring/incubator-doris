// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "sort_source_operator.h"

#include <string>

#include "pipeline/exec/operator.h"

namespace doris::pipeline {
#include "common/compile_check_begin.h"

SortLocalState::SortLocalState(RuntimeState* state, OperatorXBase* parent)
        : PipelineXLocalState<SortSharedState>(state, parent) {}

SortSourceOperatorX::SortSourceOperatorX(ObjectPool* pool, const TPlanNode& tnode, int operator_id,
                                         const DescriptorTbl& descs)
        : OperatorX<SortLocalState>(pool, tnode, operator_id, descs),
          _merge_by_exchange(tnode.sort_node.merge_by_exchange),
          _offset(tnode.sort_node.__isset.offset ? tnode.sort_node.offset : 0) {}

Status SortSourceOperatorX::init(const TPlanNode& tnode, RuntimeState* state) {
    RETURN_IF_ERROR(Base::init(tnode, state));
    RETURN_IF_ERROR(_vsort_exec_exprs.init(tnode.sort_node.sort_info, _pool));
    _is_asc_order = tnode.sort_node.sort_info.is_asc_order;
    _nulls_first = tnode.sort_node.sort_info.nulls_first;
    return Status::OK();
}

Status SortSourceOperatorX::prepare(RuntimeState* state) {
    RETURN_IF_ERROR(Base::prepare(state));
    // spill sort _child may be nullptr.
    if (_child) {
        RETURN_IF_ERROR(_vsort_exec_exprs.prepare(state, _child->row_desc(), _row_descriptor));
        RETURN_IF_ERROR(_vsort_exec_exprs.open(state));
    }
    return Status::OK();
}

Status SortSourceOperatorX::get_block(RuntimeState* state, vectorized::Block* block, bool* eos) {
    auto& local_state = get_local_state(state);
    SCOPED_TIMER(local_state.exec_time_counter());
    SCOPED_PEAK_MEM(&local_state._estimate_memory_usage);

    RETURN_IF_ERROR(local_state._shared_state->sorter->get_next(state, block, eos));
    local_state.reached_limit(block, eos);
    return Status::OK();
}

const vectorized::SortDescription& SortSourceOperatorX::get_sort_description(
        RuntimeState* state) const {
    auto& local_state = get_local_state(state);
    return local_state._shared_state->sorter->get_sort_description();
}

#include "common/compile_check_end.h"
} // namespace doris::pipeline
