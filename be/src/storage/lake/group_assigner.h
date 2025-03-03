// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

#pragma once

#include <string>
#include <vector>

#include "common/statusor.h"

namespace starrocks::lake {

// GroupAssigner is used to assign a storage directory for a tablet.
class GroupAssigner {
public:
    virtual ~GroupAssigner() = default;

    // Given a tablet id return the URI of its associated storage group.
    //
    // A storage group is just a directory on filesystem or a common prefix
    // of object key name on object storage.
    //
    // The result must be fixed for a given tablet id.
    //
    // All implementations must be thread-safe.
    virtual StatusOr<std::string> get_group(int64_t tablet_id) = 0;

    // Return a list of storage groups.
    // This may not be a full list of storage groups and may differed
    // in each return.
    //
    // All implementations must be thread-safe.
    virtual Status list_group(std::vector<std::string>* groups) = 0;
};

} // namespace starrocks::lake
