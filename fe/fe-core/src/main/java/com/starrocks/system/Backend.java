// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/fe/fe-core/src/main/java/org/apache/doris/system/Backend.java

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

package com.starrocks.system;

import com.google.common.collect.ImmutableMap;
import com.google.common.collect.Lists;
import com.google.common.collect.Maps;
import com.google.common.collect.Sets;
import com.starrocks.alter.DecommissionBackendJob.DecommissionType;
import com.starrocks.catalog.DiskInfo;
import com.starrocks.catalog.DiskInfo.DiskState;
import com.starrocks.common.Config;
import com.starrocks.common.FeMetaVersion;
import com.starrocks.common.io.Text;
import com.starrocks.common.io.Writable;
import com.starrocks.server.GlobalStateMgr;
import com.starrocks.system.HeartbeatResponse.HbStatus;
import com.starrocks.thrift.TDisk;
import com.starrocks.thrift.TStorageMedium;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.io.DataInput;
import java.io.DataOutput;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * This class extends the primary identifier of a Backend with ephemeral state,
 * eg usage information, current administrative state etc.
 */
public class Backend implements Writable {

    public enum BackendState {
        using, /* backend is belong to a cluster*/
        offline,
        free /* backend is not belong to any clusters */
    }

    private static final Logger LOG = LogManager.getLogger(Backend.class);

    private long id;
    private String host;
    private String version;

    private int heartbeatPort; // heartbeat
    private volatile int bePort; // be
    private volatile int httpPort; // web service
    private volatile int beRpcPort; // be rpc port
    private volatile int brpcPort = -1;
    private volatile int cpuCores = 0; // Cpu cores of backend

    private volatile long lastUpdateMs;
    private volatile long lastStartTime;
    private AtomicBoolean isAlive;

    private AtomicBoolean isDecommissioned;
    private volatile int decommissionType;
    private volatile String ownerClusterName;
    // to index the state in some cluster
    private volatile int backendState;
    // private BackendState backendState;

    // rootPath -> DiskInfo
    private volatile ImmutableMap<String, DiskInfo> disksRef;

    private String heartbeatErrMsg = "";

    // This is used for the first time we init pathHashToDishInfo in SystemInfoService.
    // after init it, this variable is set to true.
    private boolean initPathInfo = false;

    private long lastMissingHeartbeatTime = -1;
    // the max tablet compaction score of this backend.
    // this field is set by tablet report, and just for metric monitor, no need to persist.
    private volatile long tabletMaxCompactionScore = 0;

    private int heartbeatRetryTimes = 0;

    // additional backendStatus information for BE, display in JSON format
    private BackendStatus backendStatus = new BackendStatus();

    // port of starlet on BE
    private volatile int starletPort;

    public Backend() {
        this.host = "";
        this.version = "";
        this.lastUpdateMs = 0;
        this.lastStartTime = 0;
        this.isAlive = new AtomicBoolean();
        this.isDecommissioned = new AtomicBoolean(false);

        this.bePort = 0;
        this.httpPort = 0;
        this.beRpcPort = 0;
        this.starletPort = 0;
        this.disksRef = ImmutableMap.of();

        this.ownerClusterName = "";
        this.backendState = BackendState.free.ordinal();

        this.decommissionType = DecommissionType.SystemDecommission.ordinal();
    }

    public Backend(long id, String host, int heartbeatPort) {
        this.id = id;
        this.host = host;
        this.version = "";
        this.heartbeatPort = heartbeatPort;
        this.bePort = -1;
        this.httpPort = -1;
        this.beRpcPort = -1;
        this.lastUpdateMs = -1L;
        this.lastStartTime = -1L;
        this.disksRef = ImmutableMap.of();

        this.isAlive = new AtomicBoolean(false);
        this.isDecommissioned = new AtomicBoolean(false);

        this.ownerClusterName = "";
        this.backendState = BackendState.free.ordinal();
        this.decommissionType = DecommissionType.SystemDecommission.ordinal();
    }

    public long getId() {
        return id;
    }

    public String getHost() {
        return host;
    }

    public String getVersion() {
        return version;
    }

    public int getBePort() {
        return bePort;
    }

    public int getHeartbeatPort() {
        return heartbeatPort;
    }

    public int getHttpPort() {
        return httpPort;
    }

    public int getBeRpcPort() {
        return beRpcPort;
    }

    public int getBrpcPort() {
        return brpcPort;
    }

    public int getStarletPort() {
        return starletPort;
    }

    public String getHeartbeatErrMsg() {
        return heartbeatErrMsg;
    }

    // for test only
    public void updateOnce(int bePort, int httpPort, int beRpcPort) {
        if (this.bePort != bePort) {
            this.bePort = bePort;
        }

        if (this.httpPort != httpPort) {
            this.httpPort = httpPort;
        }

        if (this.beRpcPort != beRpcPort) {
            this.beRpcPort = beRpcPort;
        }

        long currentTime = System.currentTimeMillis();
        this.lastUpdateMs = currentTime;
        if (!isAlive.get()) {
            this.lastStartTime = currentTime;
            LOG.info("{} is alive,", this.toString());
            this.isAlive.set(true);
        }

        heartbeatErrMsg = "";
    }

    // for test only
    public void setStarletPort(int starletPort) {
        this.starletPort = starletPort;
    }

    public boolean setDecommissioned(boolean isDecommissioned) {
        if (this.isDecommissioned.compareAndSet(!isDecommissioned, isDecommissioned)) {
            LOG.warn("{} set decommission: {}", this.toString(), isDecommissioned);
            return true;
        }
        return false;
    }

    public void setId(long id) {
        this.id = id;
    }

    public void setHost(String host) {
        this.host = host;
    }

    public void setBackendState(BackendState state) {
        this.backendState = state.ordinal();
    }

    public void setAlive(boolean isAlive) {
        this.isAlive.set(isAlive);
    }

    public void setBePort(int agentPort) {
        this.bePort = agentPort;
    }

    public void setHttpPort(int httpPort) {
        this.httpPort = httpPort;
    }

    public void setBeRpcPort(int beRpcPort) {
        this.beRpcPort = beRpcPort;
    }

    public void setBrpcPort(int brpcPort) {
        this.brpcPort = brpcPort;
    }

    public long getLastUpdateMs() {
        return this.lastUpdateMs;
    }

    public void setLastUpdateMs(long currentTime) {
        this.lastUpdateMs = currentTime;
    }

    public long getLastStartTime() {
        return this.lastStartTime;
    }

    public void setLastStartTime(long currentTime) {
        this.lastStartTime = currentTime;
    }

    public long getLastMissingHeartbeatTime() {
        return lastMissingHeartbeatTime;
    }

    public boolean isAlive() {
        return this.isAlive.get();
    }

    public boolean isDecommissioned() {
        return this.isDecommissioned.get();
    }

    public boolean isAvailable() {
        return this.isAlive.get() && !this.isDecommissioned.get();
    }

    public void setDisks(ImmutableMap<String, DiskInfo> disks) {
        this.disksRef = disks;
    }

    public BackendStatus getBackendStatus() {
        return backendStatus;
    }

    public ImmutableMap<String, DiskInfo> getDisks() {
        return this.disksRef;
    }

    public boolean hasPathHash() {
        return disksRef.values().stream().allMatch(DiskInfo::hasPathHash);
    }

    public long getTotalCapacityB() {
        ImmutableMap<String, DiskInfo> disks = disksRef;
        long totalCapacityB = 0L;
        for (DiskInfo diskInfo : disks.values()) {
            if (diskInfo.getState() == DiskState.ONLINE) {
                totalCapacityB += diskInfo.getTotalCapacityB();
            }
        }
        return totalCapacityB;
    }

    public long getDataTotalCapacityB() {
        ImmutableMap<String, DiskInfo> disks = disksRef;
        long dataTotalCapacityB = 0L;
        for (DiskInfo diskInfo : disks.values()) {
            if (diskInfo.getState() == DiskState.ONLINE) {
                dataTotalCapacityB += diskInfo.getDataTotalCapacityB();
            }
        }
        return dataTotalCapacityB;
    }

    public long getAvailableCapacityB() {
        // when cluster init, disks is empty, return 1L.
        ImmutableMap<String, DiskInfo> disks = disksRef;
        long availableCapacityB = 1L;
        for (DiskInfo diskInfo : disks.values()) {
            if (diskInfo.getState() == DiskState.ONLINE) {
                availableCapacityB += diskInfo.getAvailableCapacityB();
            }
        }
        return availableCapacityB;
    }

    public long getDataUsedCapacityB() {
        ImmutableMap<String, DiskInfo> disks = disksRef;
        long dataUsedCapacityB = 0L;
        for (DiskInfo diskInfo : disks.values()) {
            if (diskInfo.getState() == DiskState.ONLINE) {
                dataUsedCapacityB += diskInfo.getDataUsedCapacityB();
            }
        }
        return dataUsedCapacityB;
    }

    public double getMaxDiskUsedPct() {
        ImmutableMap<String, DiskInfo> disks = disksRef;
        double maxPct = 0.0;
        for (DiskInfo diskInfo : disks.values()) {
            if (diskInfo.getState() == DiskState.ONLINE) {
                double percent = diskInfo.getUsedPct();
                if (percent > maxPct) {
                    maxPct = percent;
                }
            }
        }
        return maxPct;
    }

    public boolean diskExceedLimitByStorageMedium(TStorageMedium storageMedium) {
        if (getDiskNumByStorageMedium(storageMedium) <= 0) {
            return true;
        }
        ImmutableMap<String, DiskInfo> diskInfos = disksRef;
        boolean exceedLimit = true;
        for (DiskInfo diskInfo : diskInfos.values()) {
            if (diskInfo.getState() == DiskState.ONLINE && diskInfo.getStorageMedium() == storageMedium &&
                    !diskInfo.exceedLimit(true)) {
                exceedLimit = false;
                break;
            }
        }
        return exceedLimit;
    }

    public boolean diskExceedLimit() {
        if (getDiskNum() <= 0) {
            return true;
        }
        ImmutableMap<String, DiskInfo> diskInfos = disksRef;
        boolean exceedLimit = true;
        for (DiskInfo diskInfo : diskInfos.values()) {
            if (diskInfo.getState() == DiskState.ONLINE && !diskInfo.exceedLimit(true)) {
                exceedLimit = false;
                break;
            }
        }
        return exceedLimit;
    }

    public String getPathByPathHash(long pathHash) {
        for (DiskInfo diskInfo : disksRef.values()) {
            if (diskInfo.getPathHash() == pathHash) {
                return diskInfo.getRootPath();
            }
        }
        return null;
    }

    public void updateDisks(Map<String, TDisk> backendDisks) {
        ImmutableMap<String, DiskInfo> disks = disksRef;
        // The very first time to init the path info
        if (!initPathInfo) {
            boolean allPathHashUpdated = true;
            for (DiskInfo diskInfo : disks.values()) {
                if (diskInfo.getPathHash() == 0) {
                    allPathHashUpdated = false;
                    break;
                }
            }
            if (allPathHashUpdated) {
                initPathInfo = true;
                GlobalStateMgr.getCurrentSystemInfo()
                        .updatePathInfo(new ArrayList<>(disks.values()), Lists.newArrayList());
            }
        }

        // update status or add new diskInfo
        Map<String, DiskInfo> newDiskInfos = Maps.newHashMap();
        List<DiskInfo> addedDisks = Lists.newArrayList();
        List<DiskInfo> removedDisks = Lists.newArrayList();
        /*
         * set isChanged to true only if new disk is added or old disk is dropped.
         * we ignore the change of capacity, because capacity info is only used in master FE.
         */
        boolean isChanged = false;
        for (TDisk tDisk : backendDisks.values()) {
            String rootPath = tDisk.getRoot_path();
            long totalCapacityB = tDisk.getDisk_total_capacity();
            long dataUsedCapacityB = tDisk.getData_used_capacity();
            long diskAvailableCapacityB = tDisk.getDisk_available_capacity();
            boolean isUsed = tDisk.isUsed();

            DiskInfo diskInfo = disks.get(rootPath);
            if (diskInfo == null) {
                diskInfo = new DiskInfo(rootPath);
                addedDisks.add(diskInfo);
                isChanged = true;
                LOG.info("add new disk info. backendId: {}, rootPath: {}", id, rootPath);
            }
            newDiskInfos.put(rootPath, diskInfo);

            diskInfo.setTotalCapacityB(totalCapacityB);
            diskInfo.setDataUsedCapacityB(dataUsedCapacityB);
            diskInfo.setAvailableCapacityB(diskAvailableCapacityB);
            if (tDisk.isSetPath_hash()) {
                diskInfo.setPathHash(tDisk.getPath_hash());
            }

            if (tDisk.isSetStorage_medium()) {
                diskInfo.setStorageMedium(tDisk.getStorage_medium());
            }

            if (isUsed) {
                if (diskInfo.setState(DiskState.ONLINE)) {
                    isChanged = true;
                }
            } else {
                if (diskInfo.setState(DiskState.OFFLINE)) {
                    isChanged = true;
                }
            }
            LOG.debug("update disk info. backendId: {}, diskInfo: {}", id, diskInfo.toString());
        }

        // remove not exist rootPath in backend
        for (DiskInfo diskInfo : disks.values()) {
            String rootPath = diskInfo.getRootPath();
            if (!backendDisks.containsKey(rootPath)) {
                removedDisks.add(diskInfo);
                isChanged = true;
                LOG.warn("remove not exist rootPath. backendId: {}, rootPath: {}", id, rootPath);
            }
        }

        if (isChanged) {
            // update disksRef
            disksRef = ImmutableMap.copyOf(newDiskInfos);
            GlobalStateMgr.getCurrentSystemInfo().updatePathInfo(addedDisks, removedDisks);
            // log disk changing
            GlobalStateMgr.getCurrentState().getEditLog().logBackendStateChange(this);
        }
    }

    public static Backend read(DataInput in) throws IOException {
        Backend backend = new Backend();
        backend.readFields(in);
        return backend;
    }

    @Override
    public void write(DataOutput out) throws IOException {
        out.writeLong(id);
        Text.writeString(out, host);
        out.writeInt(heartbeatPort);
        out.writeInt(bePort);
        out.writeInt(httpPort);
        out.writeInt(beRpcPort);
        out.writeBoolean(isAlive.get());
        out.writeBoolean(isDecommissioned.get());
        out.writeLong(lastUpdateMs);

        out.writeLong(lastStartTime);

        ImmutableMap<String, DiskInfo> disks = disksRef;
        out.writeInt(disks.size());
        for (Map.Entry<String, DiskInfo> entry : disks.entrySet()) {
            Text.writeString(out, entry.getKey());
            entry.getValue().write(out);
        }

        Text.writeString(out, ownerClusterName);
        out.writeInt(backendState);
        out.writeInt(decommissionType);

        out.writeInt(brpcPort);
    }

    public void readFields(DataInput in) throws IOException {
        id = in.readLong();
        host = Text.readString(in);
        heartbeatPort = in.readInt();
        bePort = in.readInt();
        httpPort = in.readInt();
        if (GlobalStateMgr.getCurrentStateJournalVersion() >= FeMetaVersion.VERSION_31) {
            beRpcPort = in.readInt();
        }
        isAlive.set(in.readBoolean());

        if (GlobalStateMgr.getCurrentStateJournalVersion() >= 5) {
            isDecommissioned.set(in.readBoolean());
        }

        lastUpdateMs = in.readLong();

        if (GlobalStateMgr.getCurrentStateJournalVersion() >= 2) {
            lastStartTime = in.readLong();

            Map<String, DiskInfo> disks = Maps.newHashMap();
            int size = in.readInt();
            for (int i = 0; i < size; i++) {
                String rootPath = Text.readString(in);
                DiskInfo diskInfo = DiskInfo.read(in);
                disks.put(rootPath, diskInfo);
            }

            disksRef = ImmutableMap.copyOf(disks);
        }
        if (GlobalStateMgr.getCurrentStateJournalVersion() >= FeMetaVersion.VERSION_30) {
            ownerClusterName = Text.readString(in);
            backendState = in.readInt();
            decommissionType = in.readInt();
        } else {
            ownerClusterName = SystemInfoService.DEFAULT_CLUSTER;
            backendState = BackendState.using.ordinal();
            decommissionType = DecommissionType.SystemDecommission.ordinal();
        }

        if (GlobalStateMgr.getCurrentStateJournalVersion() >= FeMetaVersion.VERSION_40) {
            brpcPort = in.readInt();
        }
    }

    @Override
    public boolean equals(Object obj) {
        if (this == obj) {
            return true;
        }
        if (!(obj instanceof Backend)) {
            return false;
        }

        Backend backend = (Backend) obj;

        return (id == backend.id) && (host.equals(backend.host)) && (heartbeatPort == backend.heartbeatPort)
                && (bePort == backend.bePort) && (isAlive.get() == backend.isAlive.get());
    }

    @Override
    public String toString() {
        return "Backend [id=" + id + ", host=" + host + ", heartbeatPort=" + heartbeatPort + ", alive=" +
                isAlive.get() + "]";
    }

    public String getOwnerClusterName() {
        return ownerClusterName;
    }

    public void setOwnerClusterName(String name) {
        ownerClusterName = name;
    }

    public void clearClusterName() {
        ownerClusterName = "";
    }

    public BackendState getBackendState() {
        switch (backendState) {
            case 0:
                return BackendState.using;
            case 1:
                return BackendState.offline;
            default:
                return BackendState.free;
        }
    }

    public void setDecommissionType(DecommissionType type) {
        decommissionType = type.ordinal();
    }

    public DecommissionType getDecommissionType() {
        if (decommissionType == DecommissionType.ClusterDecommission.ordinal()) {
            return DecommissionType.ClusterDecommission;
        }
        return DecommissionType.SystemDecommission;
    }

    /**
     * handle Backend's heartbeat response.
     * return true if any port changed, or alive state is changed.
     */
    public boolean handleHbResponse(BackendHbResponse hbResponse) {
        boolean isChanged = false;
        if (hbResponse.getStatus() == HbStatus.OK) {
            if (!this.version.equals(hbResponse.getVersion())) {
                isChanged = true;
                this.version = hbResponse.getVersion();
            }

            if (this.bePort != hbResponse.getBePort()) {
                isChanged = true;
                this.bePort = hbResponse.getBePort();
            }

            if (this.httpPort != hbResponse.getHttpPort()) {
                isChanged = true;
                this.httpPort = hbResponse.getHttpPort();
            }

            if (this.brpcPort != hbResponse.getBrpcPort()) {
                isChanged = true;
                this.brpcPort = hbResponse.getBrpcPort();
            }

            if (Config.integrate_starmgr && this.starletPort != hbResponse.getStarletPort()) {
                isChanged = true;
                this.starletPort = hbResponse.getStarletPort();
            }

            this.lastUpdateMs = hbResponse.getHbTime();
            if (!isAlive.get()) {
                isChanged = true;
                this.lastStartTime = hbResponse.getHbTime();
                LOG.info("{} is alive, last start time: {}", this.toString(), hbResponse.getHbTime());
                this.isAlive.set(true);
            } else if (this.lastStartTime <= 0) {
                this.lastStartTime = hbResponse.getHbTime();
            }

            if (this.cpuCores != hbResponse.getCpuCores()) {
                isChanged = true;
                this.cpuCores = hbResponse.getCpuCores();
                BackendCoreStat.setNumOfHardwareCoresOfBe(hbResponse.getBeId(), hbResponse.getCpuCores());
            }

            heartbeatErrMsg = "";
            this.heartbeatRetryTimes = 0;
        } else {
            if (this.heartbeatRetryTimes < Config.heartbeat_retry_times) {
                this.heartbeatRetryTimes++;
            } else {
                if (isAlive.compareAndSet(true, false)) {
                    isChanged = true;
                    LOG.info("{} is dead,", this.toString());
                }

                heartbeatErrMsg = hbResponse.getMsg() == null ? "Unknown error" : hbResponse.getMsg();
                lastMissingHeartbeatTime = System.currentTimeMillis();
            }
        }

        return isChanged;
    }

    public void setTabletMaxCompactionScore(long compactionScore) {
        tabletMaxCompactionScore = compactionScore;
    }

    public long getTabletMaxCompactionScore() {
        return tabletMaxCompactionScore;
    }

    private long getDiskNumByStorageMedium(TStorageMedium storageMedium) {
        return disksRef.values().stream().filter(v -> v.getStorageMedium() == storageMedium).count();
    }

    public int getAvailableBackendStorageTypeCnt() {
        if (!this.isAlive.get()) {
            return 0;
        }
        ImmutableMap<String, DiskInfo> disks = this.getDisks();
        Set<TStorageMedium> set = Sets.newHashSet();
        for (DiskInfo diskInfo : disks.values()) {
            if (diskInfo.getState() == DiskState.ONLINE) {
                set.add(diskInfo.getStorageMedium());
            }
        }
        return set.size();
    }

    private int getDiskNum() {
        return disksRef.size();
    }

    /**
     * Note: This class must be a POJO in order to display in JSON format
     * Add additional information in the class to show in `show backends`
     * if just change new added backendStatus, you can do like following
     * BackendStatus status = Backend.getBackendStatus();
     * status.newItem = xxx;
     */
    public class BackendStatus {
        // this will be output as json, so not using FeConstants.null_string;
        public String lastSuccessReportTabletsTime = "N/A";
    }
}

