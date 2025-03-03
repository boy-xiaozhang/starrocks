// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/src/olap/rowset/rowset.h

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

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "common/statusor.h"
#include "gen_cpp/olap_file.pb.h"
#include "gutil/macros.h"
#include "gutil/strings/substitute.h"
#include "storage/rowset/rowset_meta.h"

namespace starrocks {

class DataDir;
class OlapTuple;
class PrimaryIndex;
class Rowset;
using RowsetSharedPtr = std::shared_ptr<Rowset>;
class RowsetFactory;
class RowsetReader;
class TabletSchema;

namespace vectorized {
class RowsetReadOptions;
class Schema;

class ChunkIterator;
using ChunkIteratorPtr = std::shared_ptr<ChunkIterator>;
} // namespace vectorized

// the rowset state transfer graph:
//    ROWSET_UNLOADED    <--|
//          |               |
//          v               |
//    ROWSET_LOADED         |
//          |               |
//          v               |
//    ROWSET_UNLOADING   -->|
enum RowsetState {
    // state for new created rowset
    ROWSET_UNLOADED,
    // state after load() called
    ROWSET_LOADED,
    // state for closed() called but owned by some readers
    ROWSET_UNLOADING
};

class RowsetStateMachine {
public:
    RowsetStateMachine() {}

    Status on_load() {
        switch (_rowset_state) {
        case ROWSET_UNLOADED:
            _rowset_state = ROWSET_LOADED;
            break;
        default:
            return Status::InternalError(strings::Substitute("rowset state on_load error, $0", _rowset_state));
        }
        return Status::OK();
    }

    Status on_close(uint64_t refs_by_reader) {
        switch (_rowset_state) {
        case ROWSET_LOADED:
            if (refs_by_reader == 0) {
                _rowset_state = ROWSET_UNLOADED;
            } else {
                _rowset_state = ROWSET_UNLOADING;
            }
            break;

        default:
            return Status::InternalError(strings::Substitute("rowset state on_close error, $0", _rowset_state));
        }
        return Status::OK();
    }

    Status on_release() {
        switch (_rowset_state) {
        case ROWSET_UNLOADING:
            _rowset_state = ROWSET_UNLOADED;
            break;
        default:
            return Status::InternalError(strings::Substitute("rowset state on_release error, $0", _rowset_state));
        }
        return Status::OK();
    }

    RowsetState rowset_state() { return _rowset_state; }

private:
    RowsetState _rowset_state{ROWSET_UNLOADED};
};

class Rowset : public std::enable_shared_from_this<Rowset> {
public:
    virtual ~Rowset() = default;

    // Open all segment files in this rowset and load necessary metadata.
    //
    // May be called multiple times, subsequent calls will no-op.
    // Derived class implements the load logic by overriding the `do_load_once()` method.
    Status load();

    const TabletSchema& schema() const { return *_schema; }
    void set_schema(const TabletSchema* schema) { _schema = schema; }

    virtual StatusOr<vectorized::ChunkIteratorPtr> new_iterator(const vectorized::Schema& schema,
                                                                const vectorized::RowsetReadOptions& options) = 0;

    // For each segment in this rowset, create a `ChunkIterator` for it and *APPEND* it into
    // |segment_iterators|. If segments in this rowset has no overlapping, a single `UnionIterator`,
    // instead of multiple `ChunkIterator`s, will be created and appended into |segment_iterators|.
    virtual Status get_segment_iterators(const vectorized::Schema& schema, const vectorized::RowsetReadOptions& options,
                                         std::vector<vectorized::ChunkIteratorPtr>* seg_iterators) = 0;

    const RowsetMetaSharedPtr& rowset_meta() const { return _rowset_meta; }

    // publish rowset to make it visible to read
    void make_visible(Version version);

    // like make_visible but updatable tablet has different mechanism
    // NOTE: only used for updatable tablet's rowset
    void make_commit(int64_t version, uint32_t rowset_seg_id);

    // helper class to access RowsetMeta
    int64_t start_version() const { return rowset_meta()->version().first; }
    int64_t end_version() const { return rowset_meta()->version().second; }
    size_t index_disk_size() const { return rowset_meta()->index_disk_size(); }
    size_t data_disk_size() const { return rowset_meta()->total_disk_size(); }
    bool empty() const { return rowset_meta()->empty(); }
    bool zero_num_rows() const { return rowset_meta()->num_rows() == 0; }
    size_t num_rows() const { return rowset_meta()->num_rows(); }
    size_t total_row_size() const { return rowset_meta()->total_row_size(); }
    Version version() const { return rowset_meta()->version(); }
    RowsetId rowset_id() const { return rowset_meta()->rowset_id(); }
    int64_t creation_time() { return rowset_meta()->creation_time(); }
    PUniqueId load_id() const { return rowset_meta()->load_id(); }
    int64_t txn_id() const { return rowset_meta()->txn_id(); }
    int64_t partition_id() const { return rowset_meta()->partition_id(); }
    // flag for push delete rowset
    bool delete_flag() const { return rowset_meta()->delete_flag(); }
    int64_t num_segments() const { return rowset_meta()->num_segments(); }
    uint32_t num_delete_files() const { return rowset_meta()->get_num_delete_files(); }
    bool has_data_files() const { return num_segments() > 0 || num_delete_files() > 0; }

    // remove all files in this rowset
    // TODO should we rename the method to remove_files() to be more specific?
    virtual Status remove() = 0;

    // close to clear the resource owned by rowset
    // including: open files, indexes and so on
    // NOTICE: can not call this function in multithreads
    void close() {
        RowsetState old_state = _rowset_state_machine.rowset_state();
        if (old_state != ROWSET_LOADED) {
            return;
        }
        Status st;
        {
            std::lock_guard<std::mutex> close_lock(_lock);
            uint64_t current_refs = _refs_by_reader;
            old_state = _rowset_state_machine.rowset_state();
            if (old_state != ROWSET_LOADED) {
                return;
            }
            if (current_refs == 0) {
                do_close();
            }
            st = _rowset_state_machine.on_close(current_refs);
        }
        if (!st.ok()) {
            LOG(WARNING) << "state transition failed from:" << st.to_string();
            return;
        }
        VLOG(3) << "rowset is close. rowset state from:" << old_state << " to " << _rowset_state_machine.rowset_state()
                << ", version:" << start_version() << "-" << end_version()
                << ", tabletid:" << _rowset_meta->tablet_id();
    }

    // hard link all files in this rowset to `dir` to form a new rowset with id `new_rowset_id`.
    virtual Status link_files_to(const std::string& dir, RowsetId new_rowset_id) = 0;

    // copy all files to `dir`
    virtual Status copy_files_to(const std::string& dir) = 0;

    // return whether `path` is one of the files in this rowset
    virtual bool check_path(const std::string& path) = 0;

    // return an unique identifier string for this rowset
    std::string unique_id() const { return _rowset_path + "/" + rowset_id().to_string(); }

    std::string rowset_path() const { return _rowset_path; }

    bool need_delete_file() const { return _need_delete_file; }

    void set_need_delete_file() { _need_delete_file = true; }

    bool contains_version(Version version) { return rowset_meta()->version().contains(version); }

    static bool comparator(const RowsetSharedPtr& left, const RowsetSharedPtr& right) {
        return left->end_version() < right->end_version();
    }

    // this function is called by reader to increase reference of rowset
    void acquire() { ++_refs_by_reader; }

    void release() {
        // if the refs by reader is 0 and the rowset is closed, should release the resouce
        uint64_t current_refs = --_refs_by_reader;
        if (current_refs == 0) {
            {
                std::lock_guard<std::mutex> release_lock(_lock);
                // rejudge _refs_by_reader because we do not add lock in create reader
                if (_refs_by_reader == 0 && _rowset_state_machine.rowset_state() == ROWSET_UNLOADING) {
                    // first do close, then change state
                    do_close();
                    _rowset_state_machine.on_release();
                }
            }
            if (_rowset_state_machine.rowset_state() == ROWSET_UNLOADED) {
                VLOG(3) << "close the rowset. rowset state from ROWSET_UNLOADING to ROWSET_UNLOADED"
                        << ", version:" << start_version() << "-" << end_version()
                        << ", tabletid:" << _rowset_meta->tablet_id();
            }
        }
    }

    static size_t get_segment_num(const std::vector<RowsetSharedPtr>& rowsets) {
        size_t num_segments = 0;
        std::for_each(rowsets.begin(), rowsets.end(),
                      [&num_segments](const RowsetSharedPtr& rowset) { num_segments += rowset->num_segments(); });
        return num_segments;
    }

    static void acquire_readers(const std::vector<RowsetSharedPtr>& rowsets) {
        std::for_each(rowsets.begin(), rowsets.end(), [](const RowsetSharedPtr& rowset) { rowset->acquire(); });
    }

    static void release_readers(const std::vector<RowsetSharedPtr>& rowsets) {
        std::for_each(rowsets.begin(), rowsets.end(), [](const RowsetSharedPtr& rowset) { rowset->release(); });
    }

    static void close_rowsets(const std::vector<RowsetSharedPtr>& rowsets) {
        std::for_each(rowsets.begin(), rowsets.end(), [](const RowsetSharedPtr& rowset) { rowset->close(); });
    }

protected:
    friend class RowsetFactory;

    Rowset(const Rowset&) = delete;
    const Rowset& operator=(const Rowset&) = delete;
    // this is non-public because all clients should use RowsetFactory to obtain pointer to initialized Rowset
    Rowset(const TabletSchema* schema, std::string rowset_path, RowsetMetaSharedPtr rowset_meta);

    // this is non-public because all clients should use RowsetFactory to obtain pointer to initialized Rowset
    virtual Status init() = 0;

    // The actual implementation of load(). Guaranteed by to called exactly once.
    virtual Status do_load() = 0;

    // release resources in this api
    virtual void do_close() = 0;

    // allow subclass to add custom logic when rowset is being published
    virtual void make_visible_extra(Version version) {}

    const TabletSchema* _schema;
    std::string _rowset_path;
    RowsetMetaSharedPtr _rowset_meta;

    // mutex lock for load/close api because it is costly
    std::mutex _lock;
    bool _need_delete_file = false;
    // variable to indicate how many rowset readers owned this rowset
    std::atomic<uint64_t> _refs_by_reader;
    // rowset state machine
    RowsetStateMachine _rowset_state_machine;
};

class RowsetReleaseGuard {
public:
    explicit RowsetReleaseGuard(std::shared_ptr<Rowset> rowset) : _rowset(std::move(rowset)) { _rowset->acquire(); }
    ~RowsetReleaseGuard() { _rowset->release(); }

private:
    std::shared_ptr<Rowset> _rowset;
};

} // namespace starrocks
