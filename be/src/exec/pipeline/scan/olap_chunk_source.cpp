// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

#include "exec/pipeline/scan/olap_chunk_source.h"

#include "column/column_helper.h"
#include "common/constexpr.h"
#include "exec/pipeline/scan/olap_scan_context.h"
#include "exec/pipeline/scan/scan_operator.h"
#include "exec/vectorized/olap_scan_node.h"
#include "exec/vectorized/olap_scan_prepare.h"
#include "exec/workgroup/work_group.h"
#include "exprs/vectorized/runtime_filter.h"
#include "gutil/map_util.h"
#include "runtime/current_thread.h"
#include "runtime/descriptors.h"
#include "runtime/exec_env.h"
#include "runtime/primitive_type.h"
#include "storage/chunk_helper.h"
#include "storage/column_predicate_rewriter.h"
#include "storage/predicate_parser.h"
#include "storage/projection_iterator.h"
#include "storage/storage_engine.h"

namespace starrocks::pipeline {
using namespace vectorized;

OlapChunkSource::OlapChunkSource(RuntimeProfile* runtime_profile, MorselPtr&& morsel,
                                 vectorized::OlapScanNode* scan_node, OlapScanContext* scan_ctx)
        : ChunkSource(runtime_profile, std::move(morsel)),
          _scan_node(scan_node),
          _scan_ctx(scan_ctx),
          _limit(scan_node->limit()),
          _scan_range(down_cast<ScanMorsel*>(_morsel.get())->get_olap_scan_range()) {}

OlapChunkSource::~OlapChunkSource() {
    _reader.reset();
    _predicate_free_pool.clear();
}

void OlapChunkSource::close(RuntimeState* state) {
    _update_counter();
    _prj_iter->close();
    _reader.reset();
    _predicate_free_pool.clear();
}

Status OlapChunkSource::prepare(RuntimeState* state) {
    _runtime_state = state;
    const TOlapScanNode& thrift_olap_scan_node = _scan_node->thrift_olap_scan_node();
    const TupleDescriptor* tuple_desc = state->desc_tbl().get_tuple_descriptor(thrift_olap_scan_node.tuple_id);
    _slots = &tuple_desc->slots();

    _runtime_profile->add_info_string("Table", tuple_desc->table_desc()->name());
    if (thrift_olap_scan_node.__isset.rollup_name) {
        _runtime_profile->add_info_string("Rollup", thrift_olap_scan_node.rollup_name);
    }
    if (thrift_olap_scan_node.__isset.sql_predicates) {
        _runtime_profile->add_info_string("Predicates", thrift_olap_scan_node.sql_predicates);
    }

    _init_counter(state);

    RETURN_IF_ERROR(_init_olap_reader(_runtime_state));

    return Status::OK();
}

void OlapChunkSource::_init_counter(RuntimeState* state) {
    _scan_timer = ADD_TIMER(_runtime_profile, "ScanTime");
    _bytes_read_counter = ADD_COUNTER(_runtime_profile, "BytesRead", TUnit::BYTES);
    _rows_read_counter = ADD_COUNTER(_runtime_profile, "RowsRead", TUnit::UNIT);

    _create_seg_iter_timer = ADD_TIMER(_runtime_profile, "CreateSegmentIter");

    _read_compressed_counter = ADD_COUNTER(_runtime_profile, "CompressedBytesRead", TUnit::BYTES);
    _read_uncompressed_counter = ADD_COUNTER(_runtime_profile, "UncompressedBytesRead", TUnit::BYTES);

    _raw_rows_counter = ADD_COUNTER(_runtime_profile, "RawRowsRead", TUnit::UNIT);
    _read_pages_num_counter = ADD_COUNTER(_runtime_profile, "ReadPagesNum", TUnit::UNIT);
    _cached_pages_num_counter = ADD_COUNTER(_runtime_profile, "CachedPagesNum", TUnit::UNIT);
    _pushdown_predicates_counter = ADD_COUNTER(_runtime_profile, "PushdownPredicates", TUnit::UNIT);

    // SegmentInit
    _seg_init_timer = ADD_TIMER(_runtime_profile, "SegmentInit");
    _bi_filter_timer = ADD_CHILD_TIMER(_runtime_profile, "BitmapIndexFilter", "SegmentInit");
    _bi_filtered_counter = ADD_CHILD_COUNTER(_runtime_profile, "BitmapIndexFilterRows", TUnit::UNIT, "SegmentInit");
    _bf_filtered_counter = ADD_CHILD_COUNTER(_runtime_profile, "BloomFilterFilterRows", TUnit::UNIT, "SegmentInit");
    _seg_zm_filtered_counter =
            ADD_CHILD_COUNTER(_runtime_profile, "SegmentZoneMapFilterRows", TUnit::UNIT, "SegmentInit");
    _zm_filtered_counter = ADD_CHILD_COUNTER(_runtime_profile, "ZoneMapIndexFilterRows", TUnit::UNIT, "SegmentInit");
    _sk_filtered_counter = ADD_CHILD_COUNTER(_runtime_profile, "ShortKeyFilterRows", TUnit::UNIT, "SegmentInit");

    // SegmentRead
    _block_load_timer = ADD_TIMER(_runtime_profile, "SegmentRead");
    _block_fetch_timer = ADD_CHILD_TIMER(_runtime_profile, "BlockFetch", "SegmentRead");
    _block_load_counter = ADD_CHILD_COUNTER(_runtime_profile, "BlockFetchCount", TUnit::UNIT, "SegmentRead");
    _block_seek_timer = ADD_CHILD_TIMER(_runtime_profile, "BlockSeek", "SegmentRead");
    _block_seek_counter = ADD_CHILD_COUNTER(_runtime_profile, "BlockSeekCount", TUnit::UNIT, "SegmentRead");
    _pred_filter_timer = ADD_CHILD_TIMER(_runtime_profile, "PredFilter", "SegmentRead");
    _pred_filter_counter = ADD_CHILD_COUNTER(_runtime_profile, "PredFilterRows", TUnit::UNIT, "SegmentRead");
    _del_vec_filter_counter = ADD_CHILD_COUNTER(_runtime_profile, "DelVecFilterRows", TUnit::UNIT, "SegmentRead");
    _chunk_copy_timer = ADD_CHILD_TIMER(_runtime_profile, "ChunkCopy", "SegmentRead");
    _decompress_timer = ADD_CHILD_TIMER(_runtime_profile, "DecompressT", "SegmentRead");
    _index_load_timer = ADD_CHILD_TIMER(_runtime_profile, "IndexLoad", "SegmentRead");
    _rowsets_read_count = ADD_CHILD_COUNTER(_runtime_profile, "RowsetsReadCount", TUnit::UNIT, "SegmentRead");
    _segments_read_count = ADD_CHILD_COUNTER(_runtime_profile, "SegmentsReadCount", TUnit::UNIT, "SegmentRead");
    _total_columns_data_page_count =
            ADD_CHILD_COUNTER(_runtime_profile, "TotalColumnsDataPageCount", TUnit::UNIT, "SegmentRead");

    // IOTime
    _io_timer = ADD_TIMER(_runtime_profile, "IOTime");
}

Status OlapChunkSource::_get_tablet(const TInternalScanRange* scan_range) {
    _version = strtoul(scan_range->version.c_str(), nullptr, 10);

    ASSIGN_OR_RETURN(_tablet, vectorized::OlapScanNode::get_tablet(scan_range));

    return Status::OK();
}

void OlapChunkSource::_decide_chunk_size() {
    bool has_huge_length_type = std::any_of(_query_slots.begin(), _query_slots.end(),
                                            [](auto& slot) { return slot->type().is_huge_type(); });
    if (_limit != -1 && _limit < _runtime_state->chunk_size()) {
        // Improve for select * from table limit x, x is small
        _params.chunk_size = _limit;
    } else {
        _params.chunk_size = _runtime_state->chunk_size();
    }
    if (has_huge_length_type) {
        _params.chunk_size = std::min(_params.chunk_size, CHUNK_SIZE_FOR_HUGE_TYPE);
    }
}

Status OlapChunkSource::_init_reader_params(const std::vector<std::unique_ptr<OlapScanRange>>& key_ranges,
                                            const std::vector<uint32_t>& scanner_columns,
                                            std::vector<uint32_t>& reader_columns) {
    const TOlapScanNode& thrift_olap_scan_node = _scan_node->thrift_olap_scan_node();
    bool skip_aggregation = thrift_olap_scan_node.is_preaggregation;
    _params.is_pipeline = true;
    _params.reader_type = READER_QUERY;
    _params.skip_aggregation = skip_aggregation;
    _params.profile = _runtime_profile;
    _params.runtime_state = _runtime_state;
    _params.use_page_cache = !config::disable_storage_page_cache;
    _morsel->init_tablet_reader_params(&_params);
    _decide_chunk_size();

    PredicateParser parser(_tablet->tablet_schema());
    std::vector<PredicatePtr> preds;
    RETURN_IF_ERROR(_scan_ctx->conjuncts_manager().get_column_predicates(&parser, &preds));
    for (auto& p : preds) {
        if (parser.can_pushdown(p.get())) {
            _params.predicates.push_back(p.get());
        } else {
            _not_push_down_predicates.add(p.get());
        }
        _predicate_free_pool.emplace_back(std::move(p));
    }

    {
        vectorized::ConjunctivePredicatesRewriter not_pushdown_predicate_rewriter(_not_push_down_predicates,
                                                                                  *_params.global_dictmaps);
        not_pushdown_predicate_rewriter.rewrite_predicate(&_obj_pool);
    }

    // Range
    for (const auto& key_range : key_ranges) {
        if (key_range->begin_scan_range.size() == 1 && key_range->begin_scan_range.get_value(0) == NEGATIVE_INFINITY) {
            continue;
        }

        _params.range = key_range->begin_include ? TabletReaderParams::RangeStartOperation::GE
                                                 : TabletReaderParams::RangeStartOperation::GT;
        _params.end_range = key_range->end_include ? TabletReaderParams::RangeEndOperation::LE
                                                   : TabletReaderParams::RangeEndOperation::LT;

        _params.start_key.push_back(key_range->begin_scan_range);
        _params.end_key.push_back(key_range->end_scan_range);
    }

    // Return columns
    if (skip_aggregation) {
        reader_columns = scanner_columns;
    } else {
        for (size_t i = 0; i < _tablet->num_key_columns(); i++) {
            reader_columns.push_back(i);
        }
        for (auto index : scanner_columns) {
            if (!_tablet->tablet_schema().column(index).is_key()) {
                reader_columns.push_back(index);
            }
        }
    }
    // Actually only the key columns need to be sorted by id, here we check all
    // for simplicity.
    DCHECK(std::is_sorted(reader_columns.begin(), reader_columns.end()));
    return Status::OK();
}

Status OlapChunkSource::_init_scanner_columns(std::vector<uint32_t>& scanner_columns) {
    for (auto slot : *_slots) {
        DCHECK(slot->is_materialized());
        int32_t index = _tablet->field_index(slot->col_name());
        if (index < 0) {
            std::stringstream ss;
            ss << "invalid field name: " << slot->col_name();
            LOG(WARNING) << ss.str();
            return Status::InternalError(ss.str());
        }
        scanner_columns.push_back(index);
        if (!_unused_output_column_ids.count(index)) {
            _query_slots.push_back(slot);
        }
    }
    // Put key columns before non-key columns, as the `MergeIterator` and `AggregateIterator`
    // required.
    std::sort(scanner_columns.begin(), scanner_columns.end());
    if (scanner_columns.empty()) {
        return Status::InternalError("failed to build storage scanner, no materialized slot!");
    }
    return Status::OK();
}

Status OlapChunkSource::_init_unused_output_columns(const std::vector<std::string>& unused_output_columns) {
    for (const auto& col_name : unused_output_columns) {
        int32_t index = _tablet->field_index(col_name);
        if (index < 0) {
            std::stringstream ss;
            ss << "invalid field name: " << col_name;
            LOG(WARNING) << ss.str();
            return Status::InternalError(ss.str());
        }
        _unused_output_column_ids.insert(index);
    }
    _params.unused_output_column_ids = &_unused_output_column_ids;
    return Status::OK();
}

Status OlapChunkSource::_init_olap_reader(RuntimeState* runtime_state) {
    const TOlapScanNode& thrift_olap_scan_node = _scan_node->thrift_olap_scan_node();
    // output columns of `this` OlapScanner, i.e, the final output columns of `get_chunk`.
    std::vector<uint32_t> scanner_columns;
    // columns fetched from |_reader|.
    std::vector<uint32_t> reader_columns;

    RETURN_IF_ERROR(_get_tablet(_scan_range));
    RETURN_IF_ERROR(_init_global_dicts(&_params));
    RETURN_IF_ERROR(_init_unused_output_columns(thrift_olap_scan_node.unused_output_column_name));
    RETURN_IF_ERROR(_init_scanner_columns(scanner_columns));
    RETURN_IF_ERROR(_init_reader_params(_scan_ctx->key_ranges(), scanner_columns, reader_columns));
    const TabletSchema& tablet_schema = _tablet->tablet_schema();
    starrocks::vectorized::Schema child_schema =
            ChunkHelper::convert_schema_to_format_v2(tablet_schema, reader_columns);

    _reader = std::make_shared<TabletReader>(_tablet, Version(0, _version), std::move(child_schema));
    if (reader_columns.size() == scanner_columns.size()) {
        _prj_iter = _reader;
    } else {
        starrocks::vectorized::Schema output_schema =
                ChunkHelper::convert_schema_to_format_v2(tablet_schema, scanner_columns);
        _prj_iter = new_projection_iterator(output_schema, _reader);
    }

    if (!_scan_ctx->not_push_down_conjuncts().empty() || !_not_push_down_predicates.empty()) {
        _expr_filter_timer = ADD_TIMER(_runtime_profile, "ExprFilterTime");
    }

    DCHECK(_params.global_dictmaps != nullptr);
    RETURN_IF_ERROR(_prj_iter->init_encoded_schema(*_params.global_dictmaps));
    RETURN_IF_ERROR(_prj_iter->init_output_schema(*_params.unused_output_column_ids));

    RETURN_IF_ERROR(_reader->prepare());
    RETURN_IF_ERROR(_reader->open(_params));

    return Status::OK();
}

bool OlapChunkSource::has_next_chunk() const {
    // If we need and could get next chunk from storage engine,
    // the _status must be ok.
    return _status.ok();
}

bool OlapChunkSource::has_output() const {
    return !_chunk_buffer.empty();
}

size_t OlapChunkSource::get_buffer_size() const {
    return _chunk_buffer.get_size();
}

StatusOr<vectorized::ChunkPtr> OlapChunkSource::get_next_chunk_from_buffer() {
    vectorized::ChunkPtr chunk = nullptr;
    _chunk_buffer.try_get(&chunk);
    return chunk;
}

Status OlapChunkSource::buffer_next_batch_chunks_blocking(size_t batch_size, RuntimeState* state) {
    if (!_status.ok()) {
        return _status;
    }
    using namespace vectorized;

    for (size_t i = 0; i < batch_size && !state->is_cancelled(); ++i) {
        ChunkUniquePtr chunk(
                ChunkHelper::new_chunk_pooled(_prj_iter->output_schema(), _runtime_state->chunk_size(), true));
        _status = _read_chunk_from_storage(_runtime_state, chunk.get());
        if (!_status.ok()) {
            // end of file is normal case, need process chunk
            if (_status.is_end_of_file()) {
                _chunk_buffer.put(std::move(chunk));
            }
            break;
        }
        _chunk_buffer.put(std::move(chunk));
    }
    return _status;
}

Status OlapChunkSource::buffer_next_batch_chunks_blocking_for_workgroup(size_t batch_size, RuntimeState* state,
                                                                        size_t* num_read_chunks, int worker_id,
                                                                        workgroup::WorkGroupPtr running_wg) {
    if (!_status.ok()) {
        return _status;
    }

    using namespace vectorized;
    int64_t time_spent = 0;
    for (size_t i = 0; i < batch_size && !state->is_cancelled(); ++i) {
        {
            SCOPED_RAW_TIMER(&time_spent);

            ChunkUniquePtr chunk(
                    ChunkHelper::new_chunk_pooled(_prj_iter->output_schema(), _runtime_state->chunk_size(), true));
            _status = _read_chunk_from_storage(_runtime_state, chunk.get());
            if (!_status.ok()) {
                // end of file is normal case, need process chunk
                if (_status.is_end_of_file()) {
                    ++(*num_read_chunks);
                    _chunk_buffer.put(std::move(chunk));
                }
                break;
            }

            ++(*num_read_chunks);
            _chunk_buffer.put(std::move(chunk));
        }

        if (time_spent >= YIELD_MAX_TIME_SPENT) {
            break;
        }

        if (time_spent >= YIELD_PREEMPT_MAX_TIME_SPENT &&
            workgroup::WorkGroupManager::instance()->get_owners_of_scan_worker(workgroup::TypeOlapScanExecutor,
                                                                               worker_id, running_wg)) {
            break;
        }
    }

    return _status;
}

// mapping a slot-column-id to schema-columnid
Status OlapChunkSource::_init_global_dicts(vectorized::TabletReaderParams* params) {
    const TOlapScanNode& thrift_olap_scan_node = _scan_node->thrift_olap_scan_node();
    const auto& global_dict_map = _runtime_state->get_query_global_dict_map();
    auto global_dict = _obj_pool.add(new ColumnIdToGlobalDictMap());
    // mapping column id to storage column ids
    const TupleDescriptor* tuple_desc = _runtime_state->desc_tbl().get_tuple_descriptor(thrift_olap_scan_node.tuple_id);
    for (auto slot : tuple_desc->slots()) {
        if (!slot->is_materialized()) {
            continue;
        }
        auto iter = global_dict_map.find(slot->id());
        if (iter != global_dict_map.end()) {
            auto& dict_map = iter->second.first;
            int32_t index = _tablet->field_index(slot->col_name());
            DCHECK(index >= 0);
            global_dict->emplace(index, const_cast<GlobalDictMap*>(&dict_map));
        }
    }
    params->global_dictmaps = global_dict;

    return Status::OK();
}

Status OlapChunkSource::_read_chunk_from_storage(RuntimeState* state, vectorized::Chunk* chunk) {
    if (state->is_cancelled()) {
        return Status::Cancelled("canceled state");
    }

    SCOPED_TIMER(_scan_timer);
    do {
        RETURN_IF_ERROR(state->check_mem_limit("read chunk from storage"));
        RETURN_IF_ERROR(_prj_iter->get_next(chunk));

        TRY_CATCH_ALLOC_SCOPE_START()

        for (auto slot : _query_slots) {
            size_t column_index = chunk->schema()->get_field_index_by_name(slot->col_name());
            chunk->set_slot_id_to_index(slot->id(), column_index);
        }

        if (!_not_push_down_predicates.empty()) {
            SCOPED_TIMER(_expr_filter_timer);
            size_t nrows = chunk->num_rows();
            _selection.resize(nrows);
            _not_push_down_predicates.evaluate(chunk, _selection.data(), 0, nrows);
            chunk->filter(_selection);
            DCHECK_CHUNK(chunk);
        }
        if (!_scan_ctx->not_push_down_conjuncts().empty()) {
            SCOPED_TIMER(_expr_filter_timer);
            RETURN_IF_ERROR(ExecNode::eval_conjuncts(_scan_ctx->not_push_down_conjuncts(), chunk));
            DCHECK_CHUNK(chunk);
        }
        TRY_CATCH_ALLOC_SCOPE_END()

    } while (chunk->num_rows() == 0);
    _update_realtime_counter(chunk);
    // Improve for select * from table limit x, x is small
    if (_limit != -1 && _num_rows_read >= _limit) {
        return Status::EndOfFile("limit reach");
    }
    return Status::OK();
}

int64_t OlapChunkSource::last_spent_cpu_time_ns() {
    int64_t time_ns = _last_spent_cpu_time_ns;
    _last_spent_cpu_time_ns += _reader->stats().decompress_ns;
    _last_spent_cpu_time_ns += _reader->stats().vec_cond_ns;
    _last_spent_cpu_time_ns += _reader->stats().del_filter_ns;
    return _last_spent_cpu_time_ns - time_ns;
}

void OlapChunkSource::_update_realtime_counter(vectorized::Chunk* chunk) {
    COUNTER_UPDATE(_read_compressed_counter, _reader->stats().compressed_bytes_read);
    _compressed_bytes_read += _reader->stats().compressed_bytes_read;
    _reader->mutable_stats()->compressed_bytes_read = 0;

    COUNTER_UPDATE(_raw_rows_counter, _reader->stats().raw_rows_read);
    _raw_rows_read += _reader->stats().raw_rows_read;
    _last_scan_rows_num += _reader->stats().raw_rows_read;
    _last_scan_bytes += _reader->stats().bytes_read;

    _reader->mutable_stats()->raw_rows_read = 0;
    _num_rows_read += chunk->num_rows();
}

void OlapChunkSource::_update_counter() {
    COUNTER_UPDATE(_create_seg_iter_timer, _reader->stats().create_segment_iter_ns);
    COUNTER_UPDATE(_rows_read_counter, _num_rows_read);

    COUNTER_UPDATE(_io_timer, _reader->stats().io_ns);
    COUNTER_UPDATE(_read_compressed_counter, _reader->stats().compressed_bytes_read);
    _compressed_bytes_read += _reader->stats().compressed_bytes_read;
    COUNTER_UPDATE(_decompress_timer, _reader->stats().decompress_ns);
    COUNTER_UPDATE(_read_uncompressed_counter, _reader->stats().uncompressed_bytes_read);
    COUNTER_UPDATE(_bytes_read_counter, _reader->stats().bytes_read);

    COUNTER_UPDATE(_block_load_timer, _reader->stats().block_load_ns);
    COUNTER_UPDATE(_block_load_counter, _reader->stats().blocks_load);
    COUNTER_UPDATE(_block_fetch_timer, _reader->stats().block_fetch_ns);
    COUNTER_UPDATE(_block_seek_timer, _reader->stats().block_seek_ns);

    COUNTER_UPDATE(_raw_rows_counter, _reader->stats().raw_rows_read);
    _raw_rows_read += _reader->mutable_stats()->raw_rows_read;
    _last_scan_rows_num += _reader->mutable_stats()->raw_rows_read;
    _last_scan_bytes += _reader->mutable_stats()->bytes_read;

    COUNTER_UPDATE(_chunk_copy_timer, _reader->stats().vec_cond_chunk_copy_ns);

    COUNTER_UPDATE(_seg_init_timer, _reader->stats().segment_init_ns);

    int64_t cond_evaluate_ns = 0;
    cond_evaluate_ns += _reader->stats().vec_cond_evaluate_ns;
    cond_evaluate_ns += _reader->stats().branchless_cond_evaluate_ns;
    cond_evaluate_ns += _reader->stats().expr_cond_evaluate_ns;
    // In order to avoid exposing too detailed metrics, we still record these infos on `_pred_filter_timer`
    // When we support metric classification, we can disassemble it again.
    COUNTER_UPDATE(_pred_filter_timer, cond_evaluate_ns);
    COUNTER_UPDATE(_pred_filter_counter, _reader->stats().rows_vec_cond_filtered);
    COUNTER_UPDATE(_del_vec_filter_counter, _reader->stats().rows_del_vec_filtered);

    COUNTER_UPDATE(_seg_zm_filtered_counter, _reader->stats().segment_stats_filtered);
    COUNTER_UPDATE(_zm_filtered_counter, _reader->stats().rows_stats_filtered);
    COUNTER_UPDATE(_bf_filtered_counter, _reader->stats().rows_bf_filtered);
    COUNTER_UPDATE(_sk_filtered_counter, _reader->stats().rows_key_range_filtered);
    COUNTER_UPDATE(_index_load_timer, _reader->stats().index_load_ns);

    COUNTER_UPDATE(_read_pages_num_counter, _reader->stats().total_pages_num);
    COUNTER_UPDATE(_cached_pages_num_counter, _reader->stats().cached_pages_num);

    COUNTER_UPDATE(_bi_filtered_counter, _reader->stats().rows_bitmap_index_filtered);
    COUNTER_UPDATE(_bi_filter_timer, _reader->stats().bitmap_index_filter_timer);
    COUNTER_UPDATE(_block_seek_counter, _reader->stats().block_seek_num);

    COUNTER_UPDATE(_rowsets_read_count, _reader->stats().rowsets_read_count);
    COUNTER_UPDATE(_segments_read_count, _reader->stats().segments_read_count);
    COUNTER_UPDATE(_total_columns_data_page_count, _reader->stats().total_columns_data_page_count);

    COUNTER_SET(_pushdown_predicates_counter, (int64_t)_params.predicates.size());

    StarRocksMetrics::instance()->query_scan_bytes.increment(_compressed_bytes_read);
    StarRocksMetrics::instance()->query_scan_rows.increment(_raw_rows_read);

    if (_reader->stats().decode_dict_ns > 0) {
        RuntimeProfile::Counter* c = ADD_TIMER(_runtime_profile, "DictDecode");
        COUNTER_UPDATE(c, _reader->stats().decode_dict_ns);
    }
    if (_reader->stats().late_materialize_ns > 0) {
        RuntimeProfile::Counter* c = ADD_TIMER(_runtime_profile, "LateMaterialize");
        COUNTER_UPDATE(c, _reader->stats().late_materialize_ns);
    }
    if (_reader->stats().del_filter_ns > 0) {
        RuntimeProfile::Counter* c1 = ADD_TIMER(_runtime_profile, "DeleteFilter");
        RuntimeProfile::Counter* c2 = ADD_COUNTER(_runtime_profile, "DeleteFilterRows", TUnit::UNIT);
        COUNTER_UPDATE(c1, _reader->stats().del_filter_ns);
        COUNTER_UPDATE(c2, _reader->stats().rows_del_filtered);
    }
}

} // namespace starrocks::pipeline
