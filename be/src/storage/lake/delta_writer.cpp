// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

#include "storage/lake/delta_writer.h"

#include "column/chunk.h"
#include "column/column.h"
#include "gutil/strings/util.h"
#include "runtime/current_thread.h"
#include "runtime/exec_env.h"
#include "runtime/mem_tracker.h"
#include "storage/lake/tablet.h"
#include "storage/lake/tablet_manager.h"
#include "storage/lake/tablet_writer.h"
#include "storage/lake/txn_log.h"
#include "storage/memtable.h"
#include "storage/memtable_flush_executor.h"
#include "storage/memtable_sink.h"
#include "storage/storage_engine.h"

namespace starrocks::lake {

using Chunk = starrocks::vectorized::Chunk;
using Column = starrocks::vectorized::Column;
using MemTable = starrocks::vectorized::MemTable;
using MemTableSink = starrocks::vectorized::MemTableSink;

class TabletWriterSink : public MemTableSink {
public:
    explicit TabletWriterSink(TabletWriter* w) : _writer(w) {}

    ~TabletWriterSink() override = default;

    DISALLOW_COPY_AND_MOVE(TabletWriterSink);

    Status flush_chunk(const Chunk& chunk) override {
        RETURN_IF_ERROR(_writer->write(chunk));
        return _writer->flush();
    }

    Status flush_chunk_with_deletes(const Chunk& /*upserts*/, const Column& /*deletes*/) override {
        return Status::NotSupported("TabletWriterSink::flush_chunk_with_deletes");
    }

private:
    TabletWriter* _writer;
};

/// DeltaWriterImpl

class DeltaWriterImpl {
public:
    explicit DeltaWriterImpl(int64_t tablet_id, int64_t txn_id, int64_t partition_id,
                             const std::vector<SlotDescriptor*>* slots, MemTracker* mem_tracker)
            : _tablet_id(tablet_id),
              _txn_id(txn_id),
              _partition_id(partition_id),
              _slots(slots),
              _mem_tracker(mem_tracker) {}

    ~DeltaWriterImpl() = default;

    DISALLOW_COPY_AND_MOVE(DeltaWriterImpl);

    [[nodiscard]] Status open();

    [[nodiscard]] Status write(const Chunk& chunk, const uint32_t* indexes, uint32_t from, uint32_t size);

    [[nodiscard]] Status finish();

    void close();

    [[nodiscard]] int64_t partition_id() const { return _partition_id; }

    [[nodiscard]] int64_t tablet_id() const { return _tablet_id; }

    [[nodiscard]] int64_t txn_id() const { return _txn_id; }

    [[nodiscard]] MemTracker* mem_tracker() { return _mem_tracker; }

private:
    void reset_memtable();
    Status flush_memtable();
    Status flush_memtable_async();

    const int64_t _tablet_id;
    const int64_t _txn_id;
    const int64_t _partition_id;
    const std::vector<SlotDescriptor*>* const _slots;
    MemTracker* const _mem_tracker;

    std::unique_ptr<TabletWriter> _tablet_writer;
    std::unique_ptr<MemTable> _mem_table;
    std::unique_ptr<MemTableSink> _mem_table_sink;
    std::unique_ptr<FlushToken> _flush_token;
    std::shared_ptr<const TabletSchema> _tablet_schema;
};

inline void DeltaWriterImpl::reset_memtable() {
    _mem_table =
            std::make_unique<MemTable>(_tablet_id, _tablet_schema.get(), _slots, _mem_table_sink.get(), _mem_tracker);
}

inline Status DeltaWriterImpl::flush_memtable_async() {
    RETURN_IF_ERROR(_mem_table->finalize());
    return _flush_token->submit(std::move(_mem_table));
}

inline Status DeltaWriterImpl::flush_memtable() {
    RETURN_IF_ERROR(flush_memtable_async());
    return _flush_token->wait();
}

Status DeltaWriterImpl::open() {
    SCOPED_THREAD_LOCAL_MEM_SETTER(_mem_tracker, false);

    DCHECK(_tablet_writer == nullptr);
    ASSIGN_OR_RETURN(auto tablet, ExecEnv::GetInstance()->lake_tablet_manager()->get_tablet(_tablet_id));
    ASSIGN_OR_RETURN(_tablet_schema, tablet.get_schema());
    ASSIGN_OR_RETURN(_tablet_writer, tablet.new_writer());
    _mem_table_sink = std::make_unique<TabletWriterSink>(_tablet_writer.get());
    _flush_token = ExecEnv::GetInstance()->storage_engine()->memtable_flush_executor()->create_flush_token();
    return Status::OK();
}

Status DeltaWriterImpl::write(const Chunk& chunk, const uint32_t* indexes, uint32_t from, uint32_t size) {
    SCOPED_THREAD_LOCAL_MEM_SETTER(_mem_tracker, false);

    if (_mem_table == nullptr) {
        reset_memtable();
    }
    Status st;
    bool full = _mem_table->insert(chunk, indexes, from, size);
    if (_mem_tracker->limit_exceeded()) {
        VLOG(2) << "Flushing memory table due to memory limit exceeded";
        st = flush_memtable();
        _mem_table = nullptr;
    } else if (_mem_tracker->parent() && _mem_tracker->parent()->limit_exceeded()) {
        VLOG(2) << "Flushing memory table due to parent memory limit exceeded";
        st = flush_memtable();
        _mem_table = nullptr;
    } else if (full) {
        st = flush_memtable_async();
        _mem_table = nullptr;
    }
    return st;
}

Status DeltaWriterImpl::finish() {
    SCOPED_THREAD_LOCAL_MEM_SETTER(_mem_tracker, false);

    // TODO: move file type checking to a common place
    auto is_seg_file = [](const std::string& name) -> bool { return HasSuffixString(name, ".dat"); };
    auto is_del_file = [](const std::string& name) -> bool { return HasSuffixString(name, ".del"); };

    RETURN_IF_ERROR(_flush_token->wait());
    RETURN_IF_ERROR(_tablet_writer->finish());
    ASSIGN_OR_RETURN(auto tablet, ExecEnv::GetInstance()->lake_tablet_manager()->get_tablet(_tablet_id));
    auto txn_log = std::make_shared<TxnLog>();
    txn_log->set_tablet_id(_tablet_id);
    txn_log->set_txn_id(_txn_id);
    auto op_write = txn_log->mutable_op_write();
    for (auto& f : _tablet_writer->files()) {
        if (is_seg_file(f)) {
            op_write->mutable_rowset()->add_segments(std::move(f));
        } else if (is_del_file(f)) {
            op_write->add_deletes(std::move(f));
        } else {
            return Status::InternalError(fmt::format("unknown file {}", f));
        }
    }
    op_write->mutable_rowset()->set_num_rows(_tablet_writer->num_rows());
    op_write->mutable_rowset()->set_data_size(_tablet_writer->data_size());
    op_write->mutable_rowset()->set_overlapped(op_write->rowset().segments_size() > 1);
    RETURN_IF_ERROR(tablet.put_txn_log(std::move(txn_log)));
    return Status::OK();
}

void DeltaWriterImpl::close() {
    SCOPED_THREAD_LOCAL_MEM_SETTER(_mem_tracker, false);

    (void)_flush_token->wait();

    // Destruct variables manually for counting memory usage into |_mem_tracker|
    if (_tablet_writer != nullptr) {
        _tablet_writer->close();
    }
    _tablet_writer.reset();
    _mem_table.reset();
    _mem_table_sink.reset();
    _flush_token.reset();
    _tablet_schema.reset();
}

//// DeltaWriter

DeltaWriter::~DeltaWriter() {
    delete _impl;
}

Status DeltaWriter::open() {
    return _impl->open();
}

Status DeltaWriter::write(const Chunk& chunk, const uint32_t* indexes, uint32_t from, uint32_t size) {
    return _impl->write(chunk, indexes, from, size);
}

Status DeltaWriter::finish() {
    return _impl->finish();
}

void DeltaWriter::close() {
    _impl->close();
}

int64_t DeltaWriter::partition_id() const {
    return _impl->partition_id();
}

int64_t DeltaWriter::tablet_id() const {
    return _impl->tablet_id();
}

int64_t DeltaWriter::txn_id() const {
    return _impl->txn_id();
}

MemTracker* DeltaWriter::mem_tracker() {
    return _impl->mem_tracker();
}

std::unique_ptr<DeltaWriter> DeltaWriter::create(int64_t tablet_id, int64_t txn_id, int64_t partition_id,
                                                 const std::vector<SlotDescriptor*>* slots, MemTracker* mem_tracker) {
    auto impl = new DeltaWriterImpl(tablet_id, txn_id, partition_id, slots, mem_tracker);
    return std::make_unique<DeltaWriter>(impl);
}

} // namespace starrocks::lake
