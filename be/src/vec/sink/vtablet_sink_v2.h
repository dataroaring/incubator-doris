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

#pragma once
#include <brpc/controller.h>
#include <bthread/types.h>
#include <butil/errno.h>
#include <fmt/format.h>
#include <gen_cpp/PaloInternalService_types.h>
#include <gen_cpp/Types_types.h>
#include <gen_cpp/internal_service.pb.h>
#include <gen_cpp/types.pb.h>
#include <glog/logging.h>
#include <google/protobuf/stubs/callback.h>
#include <stddef.h>
#include <stdint.h>

#include <atomic>
// IWYU pragma: no_include <bits/chrono.h>
#include <chrono> // IWYU pragma: keep
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <ostream>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/config.h"
#include "common/status.h"
#include "exec/data_sink.h"
#include "exec/tablet_info.h"
#include "gutil/ref_counted.h"
#include "runtime/decimalv2_value.h"
#include "runtime/exec_env.h"
#include "runtime/memory/mem_tracker.h"
#include "runtime/thread_context.h"
#include "runtime/types.h"
#include "util/bitmap.h"
#include "util/countdown_latch.h"
#include "util/runtime_profile.h"
#include "util/spinlock.h"
#include "util/stopwatch.hpp"
#include "vec/columns/column.h"
#include "vec/common/allocator.h"
#include "vec/core/block.h"
#include "vec/data_types/data_type.h"
#include "vec/exprs/vexpr_fwd.h"

namespace doris {
class DeltaWriter;
class ObjectPool;
class RowDescriptor;
class RuntimeState;
class TDataSink;
class TExpr;
class Thread;
class ThreadPoolToken;
class TupleDescriptor;
template <typename T>
class RefCountClosure;

namespace stream_load {

class VOlapTableSinkV2;

struct WriteMemtableTaskClosure {
    VOlapTableSinkV2* sink;
    std::shared_ptr<vectorized::Block> block;
    int64_t partition_id;
    int64_t index_id;
    int64_t tablet_id;
    std::vector<int32_t> row_idxes;
};

// <tablet_id, index_id>
using TabletID = std::pair<int64_t, int64_t>;
using DeltaWriterForTablet = std::unordered_map<TabletID, std::unique_ptr<DeltaWriter>>;
using StreamPool = std::vector<brpc::StreamId>;
using StreamPoolForNode = std::unordered_map<int64_t, StreamPool>;

class StreamSinkHandler : public brpc::StreamInputHandler {
public:
    StreamSinkHandler(VOlapTableSinkV2* sink) : _sink(sink) {}

    int on_received_messages(brpc::StreamId id, butil::IOBuf* const messages[],
                             size_t size) override;

    void on_idle_timeout(brpc::StreamId id) override {}

    void on_closed(brpc::StreamId id) override;

private:
    VOlapTableSinkV2* _sink;
};

struct TabletKey {
    int64_t partition_id;
    int64_t index_id;
    int64_t tablet_id;
    bool operator==(const TabletKey&) const = default;
};

struct TabletKeyHash {
    std::size_t operator()(const TabletKey& k) const {
        return (k.partition_id << 2) ^ (k.index_id << 1) ^ (k.tablet_id);
    }
};
// map<TabletKey, row_idxes>
using RowsForTablet = std::unordered_map<TabletKey, std::vector<int32_t>, TabletKeyHash>;

// Write block data to Olap Table.
// When OlapTableSink::open() called, there will be a consumer thread running in the background.
// When you call VOlapTableSinkV2::send(), you will be the producer who products pending batches.
// Join the consumer thread in close().
class VOlapTableSinkV2 final : public DataSink {
public:
    // Construct from thrift struct which is generated by FE.
    VOlapTableSinkV2(ObjectPool* pool, const RowDescriptor& row_desc,
                     const std::vector<TExpr>& texprs, Status* status);

    ~VOlapTableSinkV2() override;

    Status init(const TDataSink& sink) override;
    // TODO: unify the code of prepare/open/close with result sink
    Status prepare(RuntimeState* state) override;

    Status open(RuntimeState* state) override;

    Status close(RuntimeState* state, Status close_status) override;
    Status send(RuntimeState* state, vectorized::Block* block, bool eos = false) override;

    const RowDescriptor& row_desc() { return _input_row_desc; }

    // Returns the runtime profile for the sink.
    RuntimeProfile* profile() override { return _profile; }

private:
    Status _init_stream_pool(const NodeInfo& node_info, StreamPool& stream_pool);

    Status _init_stream_pools(StreamPoolForNode& stream_pool_for_node);

    void _generate_rows_for_tablet(RowsForTablet& rows_for_tablet,
                                   const VOlapTablePartition* partition, uint32_t tablet_index,
                                   int row_idx, size_t row_cnt);
    static void* _write_memtable_task(void* write_ctx);

    // make input data valid for OLAP table
    // return number of invalid/filtered rows.
    // invalid row number is set in Bitmap
    // set stop_processing if we want to stop the whole process now.
    Status _validate_data(RuntimeState* state, vectorized::Block* block, Bitmap* filter_bitmap,
                          int* filtered_rows, bool* stop_processing);

    template <bool is_min>
    DecimalV2Value _get_decimalv2_min_or_max(const TypeDescriptor& type);

    template <typename DecimalType, bool IsMin>
    DecimalType _get_decimalv3_min_or_max(const TypeDescriptor& type);

    Status _validate_column(RuntimeState* state, const TypeDescriptor& type, bool is_nullable,
                            vectorized::ColumnPtr column, size_t slot_index, Bitmap* filter_bitmap,
                            bool* stop_processing, fmt::memory_buffer& error_prefix,
                            vectorized::IColumn::Permutation* rows = nullptr);

    // some output column of output expr may have different nullable property with dest slot desc
    // so here need to do the convert operation
    void _convert_to_dest_desc_block(vectorized::Block* block);

    Status find_tablet(RuntimeState* state, vectorized::Block* block, int row_index,
                       const VOlapTablePartition** partition, uint32_t& tablet_index,
                       bool& stop_processing, bool& is_continue);

    std::shared_ptr<MemTracker> _mem_tracker;

    ObjectPool* _pool;
    const RowDescriptor& _input_row_desc;

    // unique load id
    PUniqueId _load_id;
    int64_t _txn_id = -1;
    int _num_replicas = -1;
    int _tuple_desc_id = -1;

    // this is tuple descriptor of destination OLAP table
    TupleDescriptor* _output_tuple_desc = nullptr;
    RowDescriptor* _output_row_desc = nullptr;

    // number of senders used to insert into OlapTable, if we only support single node insert,
    // all data from select should collectted and then send to OlapTable.
    // To support multiple senders, we maintain a channel for each sender.
    int _sender_id = -1;
    int _num_senders = -1;
    bool _is_high_priority = false;

    // TODO(zc): think about cache this data
    std::shared_ptr<OlapTableSchemaParam> _schema;
    OlapTableLocationParam* _location = nullptr;
    bool _write_single_replica = false;
    OlapTableLocationParam* _slave_location = nullptr;
    DorisNodesInfo* _nodes_info = nullptr;

    RuntimeProfile* _profile = nullptr;

    std::set<int64_t> _partition_ids;
    // only used for partition with random distribution
    std::map<int64_t, int64_t> _partition_to_tablet_map;

    Bitmap _filter_bitmap;

    std::map<std::pair<int, int>, DecimalV2Value> _max_decimalv2_val;
    std::map<std::pair<int, int>, DecimalV2Value> _min_decimalv2_val;

    std::map<int, int32_t> _max_decimal32_val;
    std::map<int, int32_t> _min_decimal32_val;
    std::map<int, int64_t> _max_decimal64_val;
    std::map<int, int64_t> _min_decimal64_val;
    std::map<int, int128_t> _max_decimal128_val;
    std::map<int, int128_t> _min_decimal128_val;

    // Stats for this
    int64_t _validate_data_ns = 0;
    int64_t _send_data_ns = 0;
    int64_t _number_input_rows = 0;
    int64_t _number_output_rows = 0;
    int64_t _number_filtered_rows = 0;
    int64_t _number_immutable_partition_filtered_rows = 0;
    int64_t _filter_ns = 0;

    MonotonicStopWatch _row_distribution_watch;

    RuntimeProfile::Counter* _input_rows_counter = nullptr;
    RuntimeProfile::Counter* _output_rows_counter = nullptr;
    RuntimeProfile::Counter* _filtered_rows_counter = nullptr;
    RuntimeProfile::Counter* _send_data_timer = nullptr;
    RuntimeProfile::Counter* _row_distribution_timer = nullptr;
    RuntimeProfile::Counter* _append_node_channel_timer = nullptr;
    RuntimeProfile::Counter* _filter_timer = nullptr;
    RuntimeProfile::Counter* _where_clause_timer = nullptr;
    RuntimeProfile::Counter* _wait_mem_limit_timer = nullptr;
    RuntimeProfile::Counter* _validate_data_timer = nullptr;
    RuntimeProfile::Counter* _open_timer = nullptr;
    RuntimeProfile::Counter* _close_timer = nullptr;
    RuntimeProfile::Counter* _non_blocking_send_timer = nullptr;
    RuntimeProfile::Counter* _non_blocking_send_work_timer = nullptr;
    RuntimeProfile::Counter* _serialize_batch_timer = nullptr;
    RuntimeProfile::Counter* _total_add_batch_exec_timer = nullptr;
    RuntimeProfile::Counter* _max_add_batch_exec_timer = nullptr;
    RuntimeProfile::Counter* _total_wait_exec_timer = nullptr;
    RuntimeProfile::Counter* _max_wait_exec_timer = nullptr;
    RuntimeProfile::Counter* _add_batch_number = nullptr;
    RuntimeProfile::Counter* _num_node_channels = nullptr;

    // load mem limit is for remote load channel
    int64_t _load_mem_limit = -1;

    // the timeout of load channels opened by this tablet sink. in second
    int64_t _load_channel_timeout_s = 0;

    int32_t _send_batch_parallelism = 1;
    // Save the status of close() method
    Status _close_status;

    // User can change this config at runtime, avoid it being modified during query or loading process.
    bool _transfer_large_data_by_brpc = false;

    // FIND_TABLET_EVERY_ROW is used for both hash and random distribution info, which indicates that we
    // should compute tablet index for every row
    // FIND_TABLET_EVERY_BATCH is only used for random distribution info, which indicates that we should
    // compute tablet index for every row batch
    // FIND_TABLET_EVERY_SINK is only used for random distribution info, which indicates that we should
    // only compute tablet index in the corresponding partition once for the whole time in olap table sink
    enum FindTabletMode { FIND_TABLET_EVERY_ROW, FIND_TABLET_EVERY_BATCH, FIND_TABLET_EVERY_SINK };
    FindTabletMode findTabletMode = FindTabletMode::FIND_TABLET_EVERY_ROW;

    VOlapTablePartitionParam* _vpartition = nullptr;
    vectorized::VExprContextSPtrs _output_vexpr_ctxs;

    RuntimeState* _state = nullptr;

    std::unordered_set<int64_t> _opened_partitions;

    std::shared_ptr<StreamPoolForNode> _stream_pool_for_node;
    size_t _stream_pool_index = 0;
    std::shared_ptr<DeltaWriterForTablet> _delta_writer_for_tablet;
    std::shared_ptr<bthread::Mutex> _delta_writer_for_tablet_mutex;
    std::vector<bthread_t> _write_memtable_threads;
    std::atomic<int32_t> _flying_task_count {0};
    std::atomic<int32_t> _flying_memtable_count {0};

    std::unordered_set<TabletID> _opened_tablets;

    std::unordered_map<TabletID, std::vector<int64_t>> _tablet_success_map;
    std::unordered_map<TabletID, std::vector<int64_t>> _tablet_failure_map;
    bthread::Mutex _tablet_success_map_mutex;
    bthread::Mutex _tablet_failure_map_mutex;

    friend class StreamSinkHandler;
};

} // namespace stream_load
} // namespace doris
