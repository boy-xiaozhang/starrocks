// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/fe/fe-core/src/test/java/org/apache/doris/analysis/ShowAlterStmtTest.java

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

package com.starrocks.analysis;

import com.starrocks.analysis.BinaryPredicate.Operator;
import com.starrocks.catalog.FakeGlobalStateMgr;
import com.starrocks.common.AnalysisException;
import com.starrocks.common.UserException;
import com.starrocks.qe.ConnectContext;
import com.starrocks.server.GlobalStateMgr;
import com.starrocks.system.SystemInfoService;
import mockit.Expectations;
import org.junit.Assert;
import org.junit.Before;
import org.junit.Test;

public class ShowAlterStmtTest {
    private Analyzer analyzer;
    private GlobalStateMgr globalStateMgr;
    private SystemInfoService systemInfo;

    private static FakeGlobalStateMgr fakeGlobalStateMgr;

    @Before
    public void setUp() {
        fakeGlobalStateMgr = new FakeGlobalStateMgr();
        globalStateMgr = AccessTestUtil.fetchAdminCatalog();

        FakeGlobalStateMgr.setGlobalStateMgr(globalStateMgr);

        analyzer = new Analyzer(globalStateMgr, new ConnectContext(null));
        new Expectations(analyzer) {
            {
                analyzer.getDefaultDb();
                minTimes = 0;
                result = "testDb";

                analyzer.getQualifiedUser();
                minTimes = 0;
                result = "testUser";

                analyzer.getCatalog();
                minTimes = 0;
                result = globalStateMgr;

                analyzer.getClusterName();
                minTimes = 0;
                result = "testCluster";
            }
        };
    }

    @Test
    public void testAlterStmt1() throws UserException, AnalysisException {
        ShowAlterStmt stmt = new ShowAlterStmt(ShowAlterStmt.AlterType.COLUMN, null, null,
                null, null);
        stmt.analyzeSyntax(analyzer);
        Assert.assertEquals("SHOW ALTER TABLE COLUMN FROM `testDb`", stmt.toString());
    }

    @Test
    public void testAlterStmt2() throws UserException, AnalysisException {
        SlotRef slotRef = new SlotRef(null, "TableName");
        StringLiteral stringLiteral = new StringLiteral("abc");
        BinaryPredicate binaryPredicate = new BinaryPredicate(Operator.EQ, slotRef, stringLiteral);
        ShowAlterStmt stmt = new ShowAlterStmt(ShowAlterStmt.AlterType.COLUMN, null, binaryPredicate, null,
                new LimitElement(1, 2));
        stmt.analyzeSyntax(analyzer);
        Assert.assertEquals("SHOW ALTER TABLE COLUMN FROM `testDb` WHERE `TableName` = \'abc\' LIMIT 1, 2",
                stmt.toString());
    }

    @Test
    public void testAlterStmt3() throws UserException, AnalysisException {
        SlotRef slotRef = new SlotRef(null, "CreateTime");
        StringLiteral stringLiteral = new StringLiteral("2019-12-04");
        BinaryPredicate binaryPredicate = new BinaryPredicate(Operator.EQ, slotRef, stringLiteral);
        ShowAlterStmt stmt = new ShowAlterStmt(ShowAlterStmt.AlterType.COLUMN, null, binaryPredicate, null, null);
        stmt.analyzeSyntax(analyzer);
        Assert.assertEquals("SHOW ALTER TABLE COLUMN FROM `testDb` WHERE `CreateTime` = \'2019-12-04 00:00:00\'",
                stmt.toString());
    }
}
