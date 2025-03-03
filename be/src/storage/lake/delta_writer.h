// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

#pragma once

#include <memory>
#include <vector>

#include "common/statusor.h"
#include "gutil/macros.h"

namespace starrocks {
class MemTracker;
class SlotDescriptor;
} // namespace starrocks

namespace starrocks::vectorized {
class Chunk;
}

namespace starrocks::lake {

class DeltaWriterImpl;

class DeltaWriter {
    using Chunk = starrocks::vectorized::Chunk;

public:
    static std::unique_ptr<DeltaWriter> create(int64_t tablet_id, int64_t txn_id, int64_t partition_id,
                                               const std::vector<SlotDescriptor*>* slots, MemTracker* mem_tracker);

    explicit DeltaWriter(DeltaWriterImpl* impl) : _impl(impl) {}

    ~DeltaWriter();

    DISALLOW_COPY_AND_MOVE(DeltaWriter);

    [[nodiscard]] Status open();

    [[nodiscard]] Status write(const Chunk& chunk, const uint32_t* indexes, uint32_t from, uint32_t size);

    [[nodiscard]] Status finish();

    void close();

    [[nodiscard]] int64_t partition_id() const;

    [[nodiscard]] int64_t tablet_id() const;

    [[nodiscard]] int64_t txn_id() const;

    [[nodiscard]] MemTracker* mem_tracker();

private:
    DeltaWriterImpl* _impl;
};

} // namespace starrocks::lake