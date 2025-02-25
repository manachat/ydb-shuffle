#pragma once

#include <ydb/core/protos/table_stats.pb.h>
#include <ydb/core/tablet_flat/tablet_flat_executor.h>
#include <ydb/core/tx/columnshard/counters/counters_manager.h>
#include <ydb/core/tx/columnshard/engines/column_engine.h>

namespace NKikimr::NColumnShard {

class TTableStatsBuilder {
private:
    TCountersManager& Counters;
    const NTabletFlatExecutor::NFlatExecutorSetup::IExecutor& Executor;
    NOlap::IColumnEngine& ColumnEngine;

public:
    TTableStatsBuilder(
        TCountersManager& counters, const NTabletFlatExecutor::NFlatExecutorSetup::IExecutor* executor, NOlap::IColumnEngine& columnEngine)
        : Counters(counters)
        , Executor(*executor)
        , ColumnEngine(columnEngine) {
    }

    void FillTableStats(ui64 pathId, ::NKikimrTableStats::TTableStats& tableStats) {
        Counters.FillTableStats(pathId, tableStats);

        auto columnEngineStats = ColumnEngine.GetStats().FindPtr(pathId);
        if (columnEngineStats && *columnEngineStats) {
            auto activeStats = (*columnEngineStats)->Active();
            tableStats.SetRowCount(activeStats.Rows);
            tableStats.SetDataSize(activeStats.Bytes);
        }
    }

    void FillTotalTableStats(::NKikimrTableStats::TTableStats& tableStats) {
        Counters.FillTotalTableStats(tableStats);

        tableStats.SetInFlightTxCount(Executor.GetStats().TxInFly);
        tableStats.SetHasLoanedParts(Executor.HasLoanedParts());

        auto activeStats = ColumnEngine.GetTotalStats().Active();
        tableStats.SetRowCount(activeStats.Rows);
        tableStats.SetDataSize(activeStats.Bytes);
        for (ui32 ch = 0; ch < activeStats.ByChannel.size(); ch++) {
            ui64 dataSize = activeStats.ByChannel[ch];
            if (dataSize > 0) {
                auto item = tableStats.AddChannels();
                item->SetChannel(ch);
                item->SetDataSize(dataSize);
            }
        }
    }
};

} // namespace NKikimr::NColumnShard
