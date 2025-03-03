// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

#pragma once

#include <ostream>

#include "column/chunk.h"
#include "exec/vectorized/sorting/sort_permute.h"
#include "gen_cpp/olap_file.pb.h"
#include "storage/chunk_aggregator.h"
#include "storage/olap_define.h"

namespace starrocks {

class SlotDescriptor;
class TabletSchema;

namespace vectorized {

class MemTableSink;

class MemTable {
public:
    MemTable(int64_t tablet_id, const TabletSchema* tablet_schema, const std::vector<SlotDescriptor*>* slot_descs,
             MemTableSink* sink, MemTracker* mem_tracker);

    MemTable(int64_t tablet_id, const Schema& schema, MemTableSink* sink, int64_t max_buffer_size,
             MemTracker* mem_tracker);

    ~MemTable();

    int64_t tablet_id() const { return _tablet_id; }

    // the total memory used (contain tmp chunk and aggregator chunk)
    size_t memory_usage() const;
    MemTracker* mem_tracker() { return _mem_tracker; }

    // buffer memory usage for write segment
    size_t write_buffer_size() const;

    // return true suggests caller should flush this memory table
    bool insert(const Chunk& chunk, const uint32_t* indexes, uint32_t from, uint32_t size);

    Status flush();

    Status finalize();

    bool is_full() const;

private:
    void _merge();

    void _sort(bool is_final);
    void _sort_column_inc();
    void _append_to_sorted_chunk(Chunk* src, Chunk* dest, bool is_final);

    bool _is_aggregate_needed();
    void _init_aggregator_if_needed();
    void _aggregate(bool is_final);

    Status _split_upserts_deletes(ChunkPtr& src, ChunkPtr* upserts, std::unique_ptr<Column>* deletes);

    ChunkPtr _chunk;
    ChunkPtr _result_chunk;
    vector<uint8_t> _result_deletes;

    // for sort by columns
    SmallPermutation _permutations;
    std::vector<uint32_t> _selective_values;

    int64_t _tablet_id;
    Schema _vectorized_schema;
    const TabletSchema* _tablet_schema;
    // the slot in _slot_descs are in order of tablet's schema
    const std::vector<SlotDescriptor*>* _slot_descs;
    KeysType _keys_type;

    MemTableSink* _sink;

    // aggregate
    std::unique_ptr<ChunkAggregator> _aggregator;

    uint64_t _merge_count = 0;

    bool _has_op_slot = false;
    std::unique_ptr<Column> _deletes;

    bool _use_slot_desc = true;
    int64_t _max_buffer_size = config::write_buffer_size;

    // memory statistic
    MemTracker* _mem_tracker = nullptr;
    // memory usage and bytes usage calculation cost of object column is high,
    // so cache calculated memory usage and bytes usage to avoid repeated calculation.
    size_t _chunk_memory_usage = 0;
    size_t _chunk_bytes_usage = 0;
    size_t _aggregator_memory_usage = 0;
    size_t _aggregator_bytes_usage = 0;
};

} // namespace vectorized

inline std::ostream& operator<<(std::ostream& os, const vectorized::MemTable& table) {
    os << "MemTable(addr=" << &table << ", tablet=" << table.tablet_id() << ", mem=" << table.memory_usage();
    return os;
}

} // namespace starrocks
