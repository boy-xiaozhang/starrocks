// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/fe/fe-core/src/main/java/org/apache/doris/master/Checkpoint.java

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

package com.starrocks.master;

import com.starrocks.common.Config;
import com.starrocks.common.FeConstants;
import com.starrocks.common.util.MasterDaemon;
import com.starrocks.metric.MetricRepo;
import com.starrocks.persist.EditLog;
import com.starrocks.persist.MetaCleaner;
import com.starrocks.persist.Storage;
import com.starrocks.server.GlobalStateMgr;
import com.starrocks.system.Frontend;
import org.apache.commons.io.output.NullOutputStream;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.io.IOException;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.List;

/**
 * Checkpoint daemon is running on master node. handle the checkpoint work for starrocks.
 */
public class Checkpoint extends MasterDaemon {
    public static final Logger LOG = LogManager.getLogger(Checkpoint.class);
    private static final int PUT_TIMEOUT_SECOND = 3600;
    private static final int CONNECT_TIMEOUT_SECOND = 1;
    private static final int READ_TIMEOUT_SECOND = 1;

    private GlobalStateMgr globalStateMgr;
    private String imageDir;
    private EditLog editLog;

    public Checkpoint(EditLog editLog) {
        super("leaderCheckpointer", FeConstants.checkpoint_interval_second * 1000L);
        this.imageDir = GlobalStateMgr.getServingState().getImageDir();
        this.editLog = editLog;
    }

    @Override
    protected void runAfterCatalogReady() {
        long imageVersion = 0;
        long checkPointVersion = 0;
        Storage storage = null;
        try {
            storage = new Storage(imageDir);
            // get max image version
            imageVersion = storage.getImageJournalId();
            // get max finalized journal id
            checkPointVersion = editLog.getFinalizedJournalId();
            LOG.info("checkpoint imageVersion {}, checkPointVersion {}", imageVersion, checkPointVersion);
            if (imageVersion >= checkPointVersion) {
                return;
            }
        } catch (IOException e) {
            LOG.error("Does not get storage info", e);
            return;
        }

        long replayedJournalId = -1;
        // generate new image file
        LOG.info("begin to generate new image: image.{}", checkPointVersion);
        globalStateMgr = GlobalStateMgr.getCurrentState();
        globalStateMgr.setEditLog(editLog);
        try {
            globalStateMgr.loadImage(imageDir);
            globalStateMgr.replayJournal(checkPointVersion);
            if (globalStateMgr.getReplayedJournalId() != checkPointVersion) {
                LOG.error("checkpoint version should be {}, actual replayed journal id is {}",
                        checkPointVersion, globalStateMgr.getReplayedJournalId());
                return;
            }

            globalStateMgr.clearExpiredJobs();

            globalStateMgr.saveImage();
            replayedJournalId = globalStateMgr.getReplayedJournalId();
            if (MetricRepo.isInit) {
                MetricRepo.COUNTER_IMAGE_WRITE.increase(1L);
            }
            GlobalStateMgr.getServingState().setImageJournalId(checkPointVersion);
            LOG.info("checkpoint finished save image.{}", replayedJournalId);
        } catch (Exception e) {
            e.printStackTrace();
            LOG.error("Exception when generate new image file", e);
            return;
        } finally {
            // destroy checkpoint globalStateMgr, reclaim memory
            globalStateMgr = null;
            GlobalStateMgr.destroyCheckpoint();
        }

        // push image file to all the other non master nodes
        // DO NOT get other nodes from HaProtocol, because node may not in bdbje replication group yet.
        List<Frontend> allFrontends = GlobalStateMgr.getServingState().getFrontends(null);
        int successPushed = 0;
        int otherNodesCount = 0;
        if (!allFrontends.isEmpty()) {
            otherNodesCount = allFrontends.size() - 1; // skip master itself
            for (Frontend fe : allFrontends) {
                String host = fe.getHost();
                if (host.equals(GlobalStateMgr.getServingState().getMasterIp())) {
                    // skip master itself
                    continue;
                }
                int port = Config.http_port;

                String url = "http://" + host + ":" + port + "/put?version=" + replayedJournalId
                        + "&port=" + port;
                LOG.info("Put image:{}", url);

                try {
                    MetaHelper.getRemoteFile(url, PUT_TIMEOUT_SECOND * 1000, new NullOutputStream());
                    successPushed++;
                } catch (IOException e) {
                    LOG.error("Exception when pushing image file. url = {}", url, e);
                }
            }

            LOG.info("push image.{} to other nodes. totally {} nodes, push successed {} nodes",
                    replayedJournalId, otherNodesCount, successPushed);
        }

        // Delete old journals
        if (successPushed == otherNodesCount) {
            long minOtherNodesJournalId = Long.MAX_VALUE;
            long deleteVersion = checkPointVersion;
            if (successPushed > 0) {
                for (Frontend fe : allFrontends) {
                    String host = fe.getHost();
                    if (host.equals(GlobalStateMgr.getServingState().getMasterIp())) {
                        // skip master itself
                        continue;
                    }
                    int port = Config.http_port;
                    URL idURL;
                    HttpURLConnection conn = null;
                    try {
                        /*
                         * get current replayed journal id of each non-master nodes.
                         * when we delete bdb database, we cannot delete db newer than
                         * any non-master node's current replayed journal id. otherwise,
                         * this lagging node can never get the deleted journal.
                         */
                        idURL = new URL("http://" + host + ":" + port + "/journal_id");
                        conn = (HttpURLConnection) idURL.openConnection();
                        conn.setConnectTimeout(CONNECT_TIMEOUT_SECOND * 1000);
                        conn.setReadTimeout(READ_TIMEOUT_SECOND * 1000);
                        String idString = conn.getHeaderField("id");
                        long id = Long.parseLong(idString);
                        if (minOtherNodesJournalId > id) {
                            minOtherNodesJournalId = id;
                        }
                    } catch (IOException e) {
                        LOG.error("Exception when getting current replayed journal id. host={}, port={}",
                                host, port, e);
                        minOtherNodesJournalId = 0;
                        break;
                    } finally {
                        if (conn != null) {
                            conn.disconnect();
                        }
                    }
                }
                deleteVersion = Math.min(minOtherNodesJournalId, checkPointVersion);
            }
            editLog.deleteJournals(deleteVersion + 1);
            if (MetricRepo.isInit) {
                MetricRepo.COUNTER_IMAGE_PUSH.increase(1L);
            }
            LOG.info("journals <= {} are deleted. image version {}, other nodes min version {}",
                    deleteVersion, checkPointVersion, minOtherNodesJournalId);
        }

        // Delete old image files
        MetaCleaner cleaner = new MetaCleaner(Config.meta_dir + "/image");
        try {
            cleaner.clean();
        } catch (IOException e) {
            LOG.error("Master delete old image file fail.", e);
        }

    }
}
