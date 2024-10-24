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

package org.apache.doris.mtmv;

import org.apache.doris.analysis.StatementBase;
import org.apache.doris.analysis.UserIdentity;
import org.apache.doris.catalog.Env;
import org.apache.doris.catalog.MTMV;
import org.apache.doris.catalog.TableIf;
import org.apache.doris.catalog.TableIf.TableType;
import org.apache.doris.mysql.privilege.Auth;
import org.apache.doris.nereids.NereidsPlanner;
import org.apache.doris.nereids.StatementContext;
import org.apache.doris.nereids.exceptions.ParseException;
import org.apache.doris.nereids.glue.LogicalPlanAdapter;
import org.apache.doris.nereids.parser.NereidsParser;
import org.apache.doris.nereids.properties.PhysicalProperties;
import org.apache.doris.nereids.rules.RuleType;
import org.apache.doris.nereids.trees.plans.Plan;
import org.apache.doris.nereids.trees.plans.commands.ExplainCommand.ExplainLevel;
import org.apache.doris.nereids.trees.plans.commands.info.CreateMTMVInfo;
import org.apache.doris.nereids.trees.plans.logical.LogicalPlan;
import org.apache.doris.nereids.trees.plans.visitor.TableCollector;
import org.apache.doris.nereids.trees.plans.visitor.TableCollector.TableCollectorContext;
import org.apache.doris.qe.ConnectContext;
import org.apache.doris.qe.SessionVariable;

import com.google.common.collect.ImmutableSet;
import com.google.common.collect.Sets;

import java.util.List;
import java.util.Optional;
import java.util.Set;

public class MTMVPlanUtil {

    public static ConnectContext createMTMVContext(MTMV mtmv) {
        ConnectContext ctx = new ConnectContext();
        ctx.setEnv(Env.getCurrentEnv());
        ctx.setQualifiedUser(Auth.ADMIN_USER);
        ctx.setCurrentUserIdentity(UserIdentity.ADMIN);
        ctx.getState().reset();
        ctx.setThreadLocalInfo();
        ctx.getSessionVariable().enableFallbackToOriginalPlanner = false;
        ctx.getSessionVariable().enableNereidsDML = true;
        ctx.getSessionVariable().allowModifyMaterializedViewData = true;
        // Disable add default limit rule to avoid refresh data wrong
        ctx.getSessionVariable().setDisableNereidsRules(
                String.join(",", ImmutableSet.of(RuleType.ADD_DEFAULT_LIMIT.name())));
        Optional<String> workloadGroup = mtmv.getWorkloadGroup();
        if (workloadGroup.isPresent()) {
            ctx.getSessionVariable().setWorkloadGroup(workloadGroup.get());
        }
        return ctx;
    }

    public static MTMVRelation generateMTMVRelation(MTMV mtmv, ConnectContext ctx) {
        // Should not make table without data to empty relation when analyze the related table,
        // so add disable rules
        SessionVariable sessionVariable = ctx.getSessionVariable();
        Set<String> tempDisableRules = sessionVariable.getDisableNereidsRuleNames();
        sessionVariable.setDisableNereidsRules(CreateMTMVInfo.MTMV_PLANER_DISABLE_RULES);
        if (ctx.getStatementContext() != null) {
            ctx.getStatementContext().invalidCache(SessionVariable.DISABLE_NEREIDS_RULES);
        }
        Plan plan;
        try {
            plan = getPlanBySql(mtmv.getQuerySql(), ctx);
        } finally {
            sessionVariable.setDisableNereidsRules(String.join(",", tempDisableRules));
            ctx.getStatementContext().invalidCache(SessionVariable.DISABLE_NEREIDS_RULES);
        }
        return generateMTMVRelation(plan);
    }

    public static MTMVRelation generateMTMVRelation(Plan plan) {
        return new MTMVRelation(getBaseTables(plan, true), getBaseTables(plan, false), getBaseViews(plan));
    }

    private static Set<BaseTableInfo> getBaseTables(Plan plan, boolean expand) {
        TableCollectorContext collectorContext =
                new TableCollector.TableCollectorContext(
                        com.google.common.collect.Sets
                                .newHashSet(TableType.values()), expand);
        plan.accept(TableCollector.INSTANCE, collectorContext);
        Set<TableIf> collectedTables = collectorContext.getCollectedTables();
        return transferTableIfToInfo(collectedTables);
    }

    private static Set<BaseTableInfo> getBaseViews(Plan plan) {
        return Sets.newHashSet();
    }

    private static Set<BaseTableInfo> transferTableIfToInfo(Set<TableIf> tables) {
        Set<BaseTableInfo> result = com.google.common.collect.Sets.newHashSet();
        for (TableIf table : tables) {
            result.add(new BaseTableInfo(table));
        }
        return result;
    }

    private static Plan getPlanBySql(String querySql, ConnectContext ctx) {
        List<StatementBase> statements;
        try {
            statements = new NereidsParser().parseSQL(querySql);
        } catch (Exception e) {
            throw new ParseException("Nereids parse failed. " + e.getMessage());
        }
        StatementBase parsedStmt = statements.get(0);
        LogicalPlan logicalPlan = ((LogicalPlanAdapter) parsedStmt).getLogicalPlan();
        StatementContext original = ctx.getStatementContext();
        ctx.setStatementContext(new StatementContext());
        try {
            NereidsPlanner planner = new NereidsPlanner(ctx.getStatementContext());
            return planner.planWithLock(logicalPlan, PhysicalProperties.ANY, ExplainLevel.NONE);
        } finally {
            ctx.setStatementContext(original);
        }
    }
}
