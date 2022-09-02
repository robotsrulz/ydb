#include <ydb/core/kqp/ut/common/kqp_ut_common.h>

#include <library/cpp/threading/local_executor/local_executor.h>

namespace NKikimr {
namespace NKqp {

using namespace NYdb;
using namespace NYdb::NTable;

Y_UNIT_TEST_SUITE(KqpService) {
    Y_UNIT_TEST_TWIN(Shutdown, UseSessionActor) {
        const ui32 Inflight = 50;
        const TDuration WaitDuration = TDuration::Seconds(1);

        auto settings = TKikimrSettings()
            .SetEnableKqpSessionActor(UseSessionActor);
        auto kikimr = MakeHolder<TKikimrRunner>(settings);

        NPar::LocalExecutor().RunAdditionalThreads(Inflight);
        auto driverConfig = kikimr->GetDriverConfig();

        NYdb::TDriver driver(driverConfig);
        NPar::LocalExecutor().ExecRange([driver](int id) {
            NYdb::NTable::TTableClient db(driver);

            auto sessionResult = db.CreateSession().GetValueSync();
            if (!sessionResult.IsSuccess()) {
                if (!sessionResult.IsTransportError()) {
                    sessionResult.GetIssues().PrintTo(Cerr);
                }

                return;
            }

            auto session = sessionResult.GetSession();

            while (true) {
                auto params = session.GetParamsBuilder()
                    .AddParam("$key").Uint32(id).Build()
                    .AddParam("$value").Int32(id).Build()
                    .Build();

                auto result = session.ExecuteDataQuery(R"(
                    DECLARE $key AS Uint32;
                    DECLARE $value AS Int32;

                    SELECT * FROM `/Root/EightShard`;

                    UPSERT INTO `/Root/TwoShard` (Key, Value2) VALUES
                        ($key, $value);
                )", TTxControl::BeginTx().CommitTx(), params).GetValueSync();

                if (result.IsTransportError()) {
                    return;
                }

                result.GetIssues().PrintTo(Cerr);
            }

        }, 0, Inflight, NPar::TLocalExecutor::MED_PRIORITY);

        Sleep(WaitDuration);
        kikimr.Reset();
        Sleep(WaitDuration);
        driver.Stop(true);
    }

    Y_UNIT_TEST_TWIN(CloseSessionsWithLoad, UseSessionActor) {
        auto settings = TKikimrSettings()
            .SetEnableKqpSessionActor(UseSessionActor);
        auto kikimr = std::make_shared<TKikimrRunner>(settings);
        auto db = kikimr->GetTableClient();

        const ui32 SessionsCount = 50;
        const TDuration WaitDuration = TDuration::Seconds(1);

        TVector<TSession> sessions;
        for (ui32 i = 0; i < SessionsCount; ++i) {
            auto sessionResult = db.CreateSession().GetValueSync();
            UNIT_ASSERT_C(sessionResult.IsSuccess(), sessionResult.GetIssues().ToString());

            sessions.push_back(sessionResult.GetSession());
        }

        NPar::LocalExecutor().RunAdditionalThreads(SessionsCount + 1);
        NPar::LocalExecutor().ExecRange([kikimr, sessions, WaitDuration](int id) mutable {
            if (id == (i32)sessions.size()) {
                Sleep(WaitDuration);

                for (ui32 i = 0; i < sessions.size(); ++i) {
                    sessions[i].Close();
                }

                return;
            }

            auto session = sessions[id];
            TMaybe<TTransaction> tx;

            while (true) {
                if (tx) {
                    auto result = tx->Commit().GetValueSync();
                    if (!result.IsSuccess()) {
                        return;
                    }

                    tx = {};
                    continue;
                }

                auto query = Sprintf(R"(
                    SELECT Key, Text, Data FROM `/Root/EightShard` WHERE Key=%1$d + 0;
                    SELECT Key, Data, Text FROM `/Root/EightShard` WHERE Key=%1$d + 1;
                    SELECT Text, Key, Data FROM `/Root/EightShard` WHERE Key=%1$d + 2;
                    SELECT Text, Data, Key FROM `/Root/EightShard` WHERE Key=%1$d + 3;
                    SELECT Data, Key, Text FROM `/Root/EightShard` WHERE Key=%1$d + 4;
                    SELECT Data, Text, Key FROM `/Root/EightShard` WHERE Key=%1$d + 5;

                    UPSERT INTO `/Root/EightShard` (Key, Text) VALUES
                        (%2$dul, "New");
                )", RandomNumber<ui32>(), RandomNumber<ui32>());

                auto result = session.ExecuteDataQuery(query, TTxControl::BeginTx()).GetValueSync();
                if (!result.IsSuccess()) {
                    return;
                }

                tx = result.GetTransaction();
            }
        }, 0, SessionsCount + 1, NPar::TLocalExecutor::WAIT_COMPLETE | NPar::TLocalExecutor::MED_PRIORITY);
    }

    TVector<TAsyncDataQueryResult> simulateSessionBusy(ui32 count, TSession& session) {
        TVector<TAsyncDataQueryResult> futures;
        for (ui32 i = 0; i < count; ++i) {
            auto query = Sprintf(R"(
                SELECT * FROM `/Root/EightShard` WHERE Key=%1$d;
            )", i);

            auto future = session.ExecuteDataQuery(query, TTxControl::BeginTx().CommitTx());
            futures.push_back(future);
        }
        return futures;
    }

    Y_UNIT_TEST_TWIN(SessionBusy, UseSessionActor) {
        NKikimrConfig::TAppConfig appConfig;
        appConfig.MutableTableServiceConfig()->SetUseSessionBusyStatus(true);

        auto kikimr = KikimrRunnerEnableSessionActor(UseSessionActor, {}, appConfig);
        auto db = kikimr.GetTableClient();
        auto session = db.CreateSession().GetValueSync().GetSession();

        auto futures = simulateSessionBusy(10, session);

        NThreading::WaitExceptionOrAll(futures).GetValueSync();

        for (auto& future : futures) {
            auto result = future.GetValue();
            if (!result.IsSuccess()) {
                UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SESSION_BUSY, result.GetIssues().ToString());
            }
        }
    }

    Y_UNIT_TEST_TWIN(SessionBusyRetryOperation, UseSessionActor) {
        NKikimrConfig::TAppConfig appConfig;
        appConfig.MutableTableServiceConfig()->SetUseSessionBusyStatus(true);

        auto kikimr = KikimrRunnerEnableSessionActor(UseSessionActor, {}, appConfig);
        auto db = kikimr.GetTableClient();

        ui32 queriesCount = 10;
        ui32 busyResultCount = 0;
        auto status = db.RetryOperation([&queriesCount, &busyResultCount](TSession session) {
            UNIT_ASSERT(queriesCount);
            UNIT_ASSERT(session.GetId());

            auto futures = simulateSessionBusy(queriesCount, session);

            NThreading::WaitExceptionOrAll(futures).GetValueSync();

            for (auto& future : futures) {
                auto result = future.GetValue();
                if (!result.IsSuccess()) {
                    UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SESSION_BUSY, result.GetIssues().ToString());
                    queriesCount--;
                    busyResultCount++;
                    return NThreading::MakeFuture<TStatus>(result);
                }
            }
            return NThreading::MakeFuture<TStatus>(TStatus(EStatus::SUCCESS, NYql::TIssues()));
         }).GetValueSync();
         // Result should be SUCCESS in case of SESSION_BUSY
         UNIT_ASSERT_VALUES_EQUAL_C(status.GetStatus(), EStatus::SUCCESS, status.GetIssues().ToString());
    }

    Y_UNIT_TEST_TWIN(SessionBusyRetryOperationSync, UseSessionActor) {
        NKikimrConfig::TAppConfig appConfig;
        appConfig.MutableTableServiceConfig()->SetUseSessionBusyStatus(true);

        auto kikimr = KikimrRunnerEnableSessionActor(UseSessionActor, {}, appConfig);
        auto db = kikimr.GetTableClient();

        ui32 queriesCount = 10;
        ui32 busyResultCount = 0;
        auto status = db.RetryOperationSync([&queriesCount, &busyResultCount](TSession session) {
            UNIT_ASSERT(queriesCount);
            UNIT_ASSERT(session.GetId());

            auto futures = simulateSessionBusy(queriesCount, session);

            NThreading::WaitExceptionOrAll(futures).GetValueSync();

            for (auto& future : futures) {
                auto result = future.GetValue();
                if (!result.IsSuccess()) {
                    UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), EStatus::SESSION_BUSY, result.GetIssues().ToString());
                    queriesCount--;
                    busyResultCount++;
                    return (TStatus)result;
                }
            }
            return TStatus(EStatus::SUCCESS, NYql::TIssues());
         });
         // Result should be SUCCESS in case of SESSION_BUSY
         UNIT_ASSERT_VALUES_EQUAL_C(status.GetStatus(), EStatus::SUCCESS, status.GetIssues().ToString());
    }

    Y_UNIT_TEST_TWIN(PatternCache, UseCache) {
        auto settings = TKikimrSettings()
            .SetWithSampleTables(false);
        settings.FeatureFlags.SetEnableKqpPatternCacheLiteral(UseCache);
        auto kikimr = TKikimrRunner{settings};
        auto driver = kikimr.GetDriver();

        size_t InFlight = 10;
        NPar::LocalExecutor().RunAdditionalThreads(InFlight);
        NPar::LocalExecutor().ExecRange([&driver](int /*id*/) {
            TTimer t;
            NYdb::NTable::TTableClient db(driver);
            auto session = db.CreateSession().GetValueSync().GetSession();
                for (ui32 i = 0; i < 500; ++i) {
                ui64 total = 100500;
                TString request = (TStringBuilder() << R"_(
                    $data = AsList(
                        AsStruct("aaa" AS Key,)_" << i / 5 << R"_(u AS Value),
                        AsStruct("aaa" AS Key,)_" << i / 5 << R"_(u AS Value),
                        AsStruct("aaa" AS Key,)_" << i / 5 << R"_(u AS Value),
                        AsStruct("aaa" AS Key,)_" << i / 5 << R"_(u AS Value),
                        AsStruct("aaa" AS Key,)_" << i / 5 << R"_(u AS Value),

                        AsStruct("aaa" AS Key,)_" << i / 5 << R"_(u AS Value),
                        AsStruct("aaa" AS Key,)_" << i / 5 << R"_(u AS Value),
                        AsStruct("aaa" AS Key,)_" << i / 5 << R"_(u AS Value),
                        AsStruct("aaa" AS Key,)_" << i / 5 << R"_(u AS Value),
                        AsStruct("aaa" AS Key,)_" << i / 5 << R"_(u AS Value),

                        AsStruct("aaa" AS Key,)_" << total - 10 * (i / 5) << R"_(u AS Value),

                        AsStruct("bbb" AS Key,)_" << i / 5 << R"_(u AS Value),
                        AsStruct("bbb" AS Key,)_" << i / 5 << R"_(u AS Value),
                        AsStruct("bbb" AS Key,)_" << i / 5 << R"_(u AS Value),
                        AsStruct("bbb" AS Key,)_" << i / 5 << R"_(u AS Value),
                        AsStruct("bbb" AS Key,)_" << i / 5 << R"_(u AS Value),

                        AsStruct("bbb" AS Key,)_" << i / 5 << R"_(u AS Value),
                        AsStruct("bbb" AS Key,)_" << i / 5 << R"_(u AS Value),
                        AsStruct("bbb" AS Key,)_" << i / 5 << R"_(u AS Value),
                        AsStruct("bbb" AS Key,)_" << i / 5 << R"_(u AS Value),
                        AsStruct("bbb" AS Key,)_" << i / 5 << R"_(u AS Value)
                    );

                    SELECT * FROM (
                        SELECT Key, SUM(Value) as Sum FROM (
                            SELECT * FROM AS_TABLE($data)
                        ) GROUP BY Key
                    ) WHERE Key == "aaa";
                )_");

                NYdb::NTable::TExecDataQuerySettings execSettings;
                execSettings.KeepInQueryCache(true);

                auto result = session.ExecuteDataQuery(request,
                    TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx(), execSettings).ExtractValueSync();
                AssertSuccessResult(result);

                CompareYson(R"( [ ["aaa";100500u] ])", FormatResultSetYson(result.GetResultSet(0)));
            }
        }, 0, InFlight, NPar::TLocalExecutor::WAIT_COMPLETE | NPar::TLocalExecutor::MED_PRIORITY);
    }
}

} // namspace NKqp
} // namespace NKikimr
