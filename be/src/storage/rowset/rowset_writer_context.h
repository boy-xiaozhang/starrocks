// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/src/olap/rowset/rowset_writer_context.h

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

#include "fs/fs.h"
#include "gen_cpp/olap_file.pb.h"
#include "runtime/global_dict/types_fwd_decl.h"
#include "storage/type_utils.h"

namespace starrocks {

class TabletSchema;

enum RowsetWriterType { kHorizontal = 0, kVertical = 1 };

class RowsetWriterContext {
public:
    RowsetWriterContext(DataFormatVersion mem_format_version, DataFormatVersion store_format_version)
            : memory_format_version(mem_format_version), storage_format_version(store_format_version) {
        load_id.set_hi(0);
        load_id.set_lo(0);
    }

    RowsetWriterContext(const RowsetWriterContext&) = default;
    RowsetWriterContext& operator=(const RowsetWriterContext&) = default;

    std::string rowset_path_prefix;

    const TabletSchema* tablet_schema = nullptr;
    std::shared_ptr<TabletSchema> partial_update_tablet_schema = nullptr;
    std::vector<int32_t> referenced_column_ids;

    RowsetId rowset_id{};
    int64_t tablet_id = 0;
    int64_t tablet_schema_hash = 0;
    int64_t partition_id = 0;
    int64_t txn_id = 0;
    Version version{};
    TabletUid tablet_uid = {0, 0};
    PUniqueId load_id{};
    // temporary segment files create or not, set false as default
    // only use for schema change vectorized by now
    bool write_tmp = false;

    RowsetStatePB rowset_state = PREPARED;
    RowsetTypePB rowset_type = BETA_ROWSET;
    SegmentsOverlapPB segments_overlap = OVERLAP_UNKNOWN;

    // segment file use uint32 to represent row number, therefore the maximum is UINT32_MAX.
    // the default is set to INT32_MAX to avoid overflow issue when casting from uint32_t to int.
    // test cases can change this value to control flush timing
    uint32_t max_rows_per_segment = INT32_MAX;

    // In-memory data format.
    DataFormatVersion memory_format_version;
    // On-disk data format.
    DataFormatVersion storage_format_version;

    vectorized::GlobalDictByNameMaps* global_dicts = nullptr;

    RowsetWriterType writer_type = kHorizontal;
};

} // namespace starrocks
