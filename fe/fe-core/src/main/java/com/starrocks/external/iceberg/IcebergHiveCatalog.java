// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

package com.starrocks.external.iceberg;

import com.google.common.annotations.VisibleForTesting;
import com.google.common.base.Preconditions;
import com.starrocks.catalog.IcebergTable;
import com.starrocks.external.iceberg.hive.CachedClientPool;
import com.starrocks.external.iceberg.hive.HiveTableOperations;
import com.starrocks.external.iceberg.io.IcebergCachingFileIO;
import org.apache.hadoop.conf.Configuration;
import org.apache.hadoop.hive.conf.HiveConf;
import org.apache.hadoop.hive.metastore.IMetaStoreClient;
import org.apache.iceberg.BaseMetastoreCatalog;
import org.apache.iceberg.CatalogProperties;
import org.apache.iceberg.CatalogUtil;
import org.apache.iceberg.ClientPool;
import org.apache.iceberg.Table;
import org.apache.iceberg.TableOperations;
import org.apache.iceberg.catalog.Namespace;
import org.apache.iceberg.catalog.TableIdentifier;
import org.apache.iceberg.hadoop.HadoopFileIO;
import org.apache.iceberg.io.FileIO;
import org.apache.iceberg.relocated.com.google.common.base.MoreObjects;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;
import org.apache.thrift.TException;

import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

public class IcebergHiveCatalog extends BaseMetastoreCatalog implements IcebergCatalog {
    private static final Logger LOG = LogManager.getLogger(IcebergHiveCatalog.class);

    private static final ConcurrentHashMap<String, IcebergHiveCatalog> metastoreUriToCatalog =
            new ConcurrentHashMap<>();

    public static synchronized IcebergHiveCatalog getInstance(String uri, Map<String, String> properties) {
        if (!metastoreUriToCatalog.containsKey(uri)) {
            properties.put(CatalogProperties.URI, uri);
            metastoreUriToCatalog.put(uri, (IcebergHiveCatalog) CatalogLoader.hive(String.format("hive-%s", uri),
                    new Configuration(), properties).loadCatalog());
        }
        return metastoreUriToCatalog.get(uri);
    }

    private String name;
    private Configuration conf;
    private FileIO fileIO;
    private ClientPool<IMetaStoreClient, TException> clients;

    @VisibleForTesting
    public IcebergHiveCatalog() {
    }

    @Override
    public IcebergCatalogType getIcebergCatalogType() {
        return IcebergCatalogType.HIVE_CATALOG;
    }

    @Override
    public Table loadTable(IcebergTable table) throws StarRocksIcebergException {
        TableIdentifier tableId = IcebergUtil.getIcebergTableIdentifier(table);
        return loadTable(tableId, null, null);
    }

    @Override
    public Table loadTable(TableIdentifier tableIdentifier) throws StarRocksIcebergException {
        return loadTable(tableIdentifier, null, null);
    }

    @Override
    public Table loadTable(TableIdentifier tableId, String tableLocation,
                           Map<String, String> properties) throws StarRocksIcebergException {
        Preconditions.checkState(tableId != null);
        try {
            return super.loadTable(tableId);
        } catch (Exception e) {
            throw new StarRocksIcebergException(String.format(
                    "Failed to load Iceberg table with id: %s", tableId), e);
        }
    }

    @Override
    public void initialize(String inputName, Map<String, String> properties) {
        this.name = inputName;
        if (conf == null) {
            LOG.warn("No Hadoop Configuration was set, using the default environment Configuration");
            this.conf = new Configuration();
        }

        if (properties.containsKey(CatalogProperties.URI)) {
            this.conf.set(HiveConf.ConfVars.METASTOREURIS.varname, properties.get(CatalogProperties.URI));
        }

        if (properties.containsKey(CatalogProperties.WAREHOUSE_LOCATION)) {
            this.conf.set(HiveConf.ConfVars.METASTOREWAREHOUSE.varname,
                    properties.get(CatalogProperties.WAREHOUSE_LOCATION));
        }

        String fileIOImpl = properties.get(CatalogProperties.FILE_IO_IMPL);
        this.fileIO =
                fileIOImpl == null ? new HadoopFileIO(conf) : CatalogUtil.loadFileIO(fileIOImpl, properties, conf);

        // warp cache fileIO
        IcebergCachingFileIO cachingFileIO = new IcebergCachingFileIO(fileIO);
        cachingFileIO.initialize(properties);
        this.fileIO = cachingFileIO;

        this.clients = new CachedClientPool(conf, properties);
    }

    @Override
    public String name() {
        return name;
    }

    @Override
    protected boolean isValidIdentifier(TableIdentifier tableIdentifier) {
        return tableIdentifier.namespace().levels().length == 1;
    }

    @Override
    public TableOperations newTableOps(TableIdentifier tableIdentifier) {
        String dbName = tableIdentifier.namespace().level(0);
        String tableName = tableIdentifier.name();
        return new HiveTableOperations(conf, clients, fileIO, name, dbName, tableName);
    }

    @Override
    protected String defaultWarehouseLocation(TableIdentifier tableIdentifier) {
        throw new UnsupportedOperationException("Not implemented");
    }

    @Override
    public List<TableIdentifier> listTables(Namespace namespace) {
        throw new UnsupportedOperationException("Not implemented");
    }

    @Override
    public boolean dropTable(TableIdentifier tableIdentifier, boolean b) {
        throw new UnsupportedOperationException("Not implemented");
    }

    @Override
    public void renameTable(TableIdentifier tableIdentifier, TableIdentifier tableIdentifier1) {
        throw new UnsupportedOperationException("Not implemented");
    }

    @Override
    public String toString() {
        return MoreObjects.toStringHelper(this)
                .add("name", name)
                .add("uri", this.conf == null ? "" : this.conf.get(HiveConf.ConfVars.METASTOREURIS.varname))
                .toString();
    }
}
