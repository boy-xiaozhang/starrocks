// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/src/service/doris_main.cpp

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

#include <aws/core/Aws.h>
#include <gperftools/malloc_extension.h>
#include <sys/file.h>
#include <unistd.h>

#if defined(LEAK_SANITIZER)
#include <sanitizer/lsan_interface.h>
#endif

#include <curl/curl.h>
#include <gperftools/profiler.h>
#include <thrift/TOutput.h>

#include "agent/heartbeat_server.h"
#include "agent/status.h"
#include "common/config.h"
#include "common/daemon.h"
#include "common/logging.h"
#include "common/status.h"
#include "exec/pipeline/query_context.h"
#include "fs/fs_util.h"
#include "runtime/exec_env.h"
#include "runtime/heartbeat_flags.h"
#include "runtime/jdbc_driver_manager.h"
#include "service/backend_options.h"
#include "service/brpc_service.h"
#include "service/service.h"
#include "service/staros_worker.h"
#include "storage/options.h"
#include "storage/storage_engine.h"
#include "util/debug_util.h"
#include "util/logging.h"
#include "util/network_util.h"
#include "util/starrocks_metrics.h"
#include "util/thrift_rpc_helper.h"
#include "util/thrift_server.h"
#include "util/thrift_util.h"
#include "util/uid_util.h"

DECLARE_bool(s2debug);

static void help(const char*);

#include <dlfcn.h>

extern "C" {
void __lsan_do_leak_check();
}

namespace starrocks {
static void thrift_output(const char* x) {
    LOG(WARNING) << "thrift internal message: " << x;
}

} // namespace starrocks

extern int meta_tool_main(int argc, char** argv);

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "meta_tool") == 0) {
        return meta_tool_main(argc - 1, argv + 1);
    }
    bool as_cn = false;
    // Check if print version or help or cn.
    if (argc > 1) {
        if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0) {
            puts(starrocks::get_build_version(false).c_str());
            exit(0);
        } else if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0) {
            help(basename(argv[0]));
            exit(0);
        } else if (strcmp(argv[1], "--cn") == 0) {
            as_cn = true;
        }
    }
    bool without_storage = as_cn;

    if (getenv("STARROCKS_HOME") == nullptr) {
        fprintf(stderr, "you need set STARROCKS_HOME environment variable.\n");
        exit(-1);
    }

    if (getenv("TCMALLOC_HEAP_LIMIT_MB") == nullptr) {
        fprintf(stderr, "you need replace bin dir of be with new version.\n");
        exit(-1);
    }

    // S2 will crashes when deserialization fails and FLAGS_s2debug was true.
    FLAGS_s2debug = false;

    using starrocks::Status;
    using std::string;

    // Open pid file, obtain file lock and save pid.
    string pid_file = string(getenv("PID_DIR"));
    if (as_cn) {
        pid_file += "/cn.pid";
    } else {
        pid_file += "/be.pid";
    }
    int fd = open(pid_file.c_str(), O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    if (fd < 0) {
        fprintf(stderr, "fail to create pid file.");
        exit(-1);
    }

    string pid = std::to_string((long)getpid());
    pid += "\n";
    size_t length = write(fd, pid.c_str(), pid.size());
    if (length != pid.size()) {
        fprintf(stderr, "fail to save pid into pid file.");
        exit(-1);
    }

    // Descriptor will be leaked if failing to close fd.
    if (::close(fd) < 0) {
        fprintf(stderr, "failed to close fd of pidfile.");
        exit(-1);
    }

    string conffile = string(getenv("STARROCKS_HOME"));
    if (as_cn) {
        conffile += "/conf/cn.conf";
    } else {
        conffile += "/conf/be.conf";
    }
    if (!starrocks::config::init(conffile.c_str(), true)) {
        fprintf(stderr, "error read config file. \n");
        return -1;
    }

#if !defined(ADDRESS_SANITIZER) && !defined(LEAK_SANITIZER) && !defined(THREAD_SANITIZER)
    // Aggressive decommit is required so that unused pages in the TCMalloc page heap are
    // not backed by physical pages and do not contribute towards memory consumption.
    //
    //  2020-08-31: Disable aggressive decommit,  which will decrease the performance of
    //  memory allocation and deallocation.
    // MallocExtension::instance()->SetNumericProperty("tcmalloc.aggressive_memory_decommit", 1);

    // Change the total TCMalloc thread cache size if necessary.
    if (!MallocExtension::instance()->SetNumericProperty("tcmalloc.max_total_thread_cache_bytes",
                                                         starrocks::config::tc_max_total_thread_cache_bytes)) {
        fprintf(stderr, "Failed to change TCMalloc total thread cache size.\n");
        return -1;
    }
#endif

    Aws::SDKOptions aws_sdk_options;
    Aws::InitAPI(aws_sdk_options);

    std::vector<starrocks::StorePath> paths;
    if (!without_storage) {
        auto olap_res = starrocks::parse_conf_store_paths(starrocks::config::storage_root_path, &paths);
        if (!olap_res.ok()) {
            LOG(FATAL) << "parse config storage path failed, path=" << starrocks::config::storage_root_path;
            exit(-1);
        }
        auto it = paths.begin();
        for (; it != paths.end();) {
            if (!starrocks::check_datapath_rw(it->path)) {
                if (starrocks::config::ignore_broken_disk) {
                    LOG(WARNING) << "read write test file failed, path=" << it->path;
                    it = paths.erase(it);
                } else {
                    LOG(FATAL) << "read write test file failed, path=" << it->path;
                    exit(-1);
                }
            } else {
                ++it;
            }
        }

        if (paths.empty()) {
            LOG(FATAL) << "All disks are broken, exit.";
            exit(-1);
        }
    }

    // Initilize libcurl here to avoid concurrent initialization.
    auto curl_ret = curl_global_init(CURL_GLOBAL_ALL);
    if (curl_ret != 0) {
        LOG(FATAL) << "fail to initialize libcurl, curl_ret=" << curl_ret;
        exit(-1);
    }
    // Add logger for thrift internal.
    apache::thrift::GlobalOutput.setOutputFunction(starrocks::thrift_output);

    std::unique_ptr<starrocks::Daemon> daemon(new starrocks::Daemon());
    daemon->init(argc, argv, paths);

    // init jdbc driver manager
    EXIT_IF_ERROR(starrocks::JDBCDriverManager::getInstance()->init(std::string(getenv("STARROCKS_HOME")) +
                                                                    "/lib/jdbc_drivers"));

    if (!starrocks::BackendOptions::init()) {
        exit(-1);
    }

    auto* exec_env = starrocks::ExecEnv::GetInstance();
    EXIT_IF_ERROR(exec_env->init_mem_tracker());

    // Init and open storage engine.
    starrocks::EngineOptions options;
    options.store_paths = paths;
    options.backend_uid = starrocks::UniqueId::gen_uid();
    options.tablet_meta_mem_tracker = exec_env->tablet_meta_mem_tracker();
    options.schema_change_mem_tracker = exec_env->schema_change_mem_tracker();
    options.compaction_mem_tracker = exec_env->compaction_mem_tracker();
    options.update_mem_tracker = exec_env->update_mem_tracker();
    options.conf_path = string(getenv("STARROCKS_HOME")) + "/conf/";
    starrocks::StorageEngine* engine = nullptr;

    if (without_storage) {
        auto st = starrocks::DummyStorageEngine::open(options, &engine);
        if (!st.ok()) {
            LOG(FATAL) << "fail to open StorageEngine, res=" << st.get_error_msg();
            exit(-1);
        }
    } else {
        auto st = starrocks::StorageEngine::open(options, &engine);
        if (!st.ok()) {
            LOG(FATAL) << "fail to open StorageEngine, res=" << st.get_error_msg();
            exit(-1);
        }
    }

    // Init exec env.
    EXIT_IF_ERROR(starrocks::ExecEnv::init(exec_env, paths));
    exec_env->set_storage_engine(engine);
    engine->set_heartbeat_flags(exec_env->heartbeat_flags());

    // Start all background threads of storage engine.
    // SHOULD be called after exec env is initialized.
    EXIT_IF_ERROR(engine->start_bg_threads());

    // Begin to start Heartbeat services
    starrocks::ThriftRpcHelper::setup(exec_env);
    starrocks::TMasterInfo* master_info = exec_env->master_info();
    starrocks::ThriftServer* heartbeat_thrift_server;
    starrocks::AgentStatus heartbeat_status = starrocks::create_heartbeat_server(
            exec_env, starrocks::config::heartbeat_service_port, &heartbeat_thrift_server,
            starrocks::config::heartbeat_service_thread_count, master_info);

    if (starrocks::AgentStatus::STARROCKS_SUCCESS != heartbeat_status) {
        LOG(ERROR) << "Heartbeat services did not start correctly, exiting";
        starrocks::shutdown_logging();
        exit(1);
    }

    starrocks::Status status = heartbeat_thrift_server->start();
    if (!status.ok()) {
        LOG(ERROR) << "StarRocks BE HeartBeat Service did not start correctly. Error=" << status.to_string();
        starrocks::shutdown_logging();
        exit(1);
    } else {
        LOG(INFO) << "StarRocks BE HeartBeat Service started correctly.";
    }

#ifdef USE_STAROS
    starrocks::init_staros_worker();
#endif

    if (as_cn) {
        start_cn();
    } else {
        start_be();
    }

    daemon->stop();
    daemon.reset();

#ifdef USE_STAROS
    starrocks::shutdown_staros_worker();
#endif

    Aws::ShutdownAPI(aws_sdk_options);

    heartbeat_thrift_server->stop();
    heartbeat_thrift_server->join();
    delete heartbeat_thrift_server;

    engine->stop();
    delete engine;
    exec_env->set_storage_engine(nullptr);
    starrocks::ExecEnv::destroy(exec_env);

    return 0;
}

static void help(const char* progname) {
    printf("%s is the StarRocks backend server.\n\n", progname);
    printf("Usage:\n  %s [OPTION]...\n\n", progname);
    printf("Options:\n");
    printf("      --cn           start as compute node\n");
    printf("  -v, --version      output version information, then exit\n");
    printf("  -?, --help         show this help, then exit\n");
}
