#include <library/cpp/json/json_reader.h>
#include <library/cpp/testing/unittest/registar.h>
#include <util/system/env.h>
#include <ydb/core/base/tablet_pipecache.h>
#include <ydb/core/mon/sync_http_mon.h>
#include <ydb/core/sys_view/service/sysview_service.h>

#include "pq_ut.h"

namespace NKikimr {

namespace {

TVector<std::pair<ui64, TString>> TestData() {
    TVector<std::pair<ui64, TString>> data;
    TString s{32, 'c'};
    ui32 pp = 8 + 4 + 2 + 9;
    for (ui32 i = 0; i < 10; ++i) {
        data.push_back({i + 1, s.substr(pp)});
    }
    return data;
}

struct THttpRequest : NMonitoring::IHttpRequest {
    HTTP_METHOD Method;
    TCgiParameters CgiParameters;
    THttpHeaders HttpHeaders;

    THttpRequest(HTTP_METHOD method)
        : Method(method)
    {
        CgiParameters.emplace("type", TTabletTypes::TypeToStr(TTabletTypes::PersQueue));
        CgiParameters.emplace("json", "");
    }

    ~THttpRequest() {}

    const char* GetURI() const override {
        return "";
    }

    const char* GetPath() const override {
        return "";
    }

    const TCgiParameters& GetParams() const override {
        return CgiParameters;
    }

    const TCgiParameters& GetPostParams() const override {
        return CgiParameters;
    }

    TStringBuf GetPostContent() const override {
        return TString();
    }

    HTTP_METHOD GetMethod() const override {
        return Method;
    }

    const THttpHeaders& GetHeaders() const override {
        return HttpHeaders;
    }

    TString GetRemoteAddr() const override {
        return TString();
    }
};

} // anonymous namespace
Y_UNIT_TEST_SUITE(PQCountersSimple) {

Y_UNIT_TEST(Partition) {
    TTestContext tc;
    TFinalizer finalizer(tc);
    bool activeZone{false};
    tc.Prepare("", [](TTestActorRuntime&) {}, activeZone, false, true);
    tc.Runtime->SetScheduledLimit(100);

    PQTabletPrepare({}, {}, tc);
    CmdWrite(0, "sourceid0", TestData(), tc, false, {}, true);
    CmdWrite(0, "sourceid1", TestData(), tc, false);
    CmdWrite(0, "sourceid2", TestData(), tc, false);

    {
        auto counters = tc.Runtime->GetAppData(0).Counters;
        auto dbGroup = GetServiceCounters(counters, "pqproxy");
        TStringStream countersStr;
        dbGroup->OutputHtml(countersStr);
        TString referenceCounters = NResource::Find(TStringBuf("counters_pqproxy.html"));
        UNIT_ASSERT_EQUAL(countersStr.Str() + "\n", referenceCounters);
    }

    {
        auto counters = tc.Runtime->GetAppData(0).Counters;
        auto dbGroup = GetServiceCounters(counters, "datastreams");
        TStringStream countersStr;
        dbGroup->OutputHtml(countersStr);
        UNIT_ASSERT_EQUAL(countersStr.Str(), "<pre></pre>");
    }
}

Y_UNIT_TEST(PartitionFirstClass) {
    TTestContext tc;
    TFinalizer finalizer(tc);
    bool activeZone{false};
    tc.Prepare("", [](TTestActorRuntime&){}, activeZone, true, true);
    tc.Runtime->SetScheduledLimit(100);

    PQTabletPrepare({}, {}, tc);
    CmdWrite(0, "sourceid0", TestData(), tc, false, {}, true);
    CmdWrite(0, "sourceid1", TestData(), tc, false);
    CmdWrite(0, "sourceid2", TestData(), tc, false);

    {
        auto counters = tc.Runtime->GetAppData(0).Counters;
        auto dbGroup = GetServiceCounters(counters, "pqproxy");
        TStringStream countersStr;
        dbGroup->OutputHtml(countersStr);
        TString referenceCounters = NResource::Find(TStringBuf("counters_pqproxy_firstclass.html"));
        UNIT_ASSERT_EQUAL(countersStr.Str() + "\n", referenceCounters);
    }

    {
        auto counters = tc.Runtime->GetAppData(0).Counters;
        auto dbGroup = GetServiceCounters(counters, "datastreams");
        TStringStream countersStr;
        dbGroup->OutputHtml(countersStr);
        const TString referenceCounters = NResource::Find(TStringBuf("counters_datastreams.html"));
        UNIT_ASSERT_EQUAL(countersStr.Str() + "\n", referenceCounters);
    }
}

} // Y_UNIT_TEST_SUITE(PQCountersSimple)

Y_UNIT_TEST_SUITE(PQCountersLabeled) {

void CompareJsons(const TString& inputStr, const TString& referenceStr) {
    NJson::TJsonValue referenceJson;
    UNIT_ASSERT(NJson::ReadJsonTree(TStringBuf(referenceStr), &referenceJson));

    NJson::TJsonValue inputJson;
    UNIT_ASSERT(NJson::ReadJsonTree(TStringBuf(inputStr), &inputJson));

    // Run time of test differs as well as counters below. We check if they are in
    // probable interval [4500; 5500], set it to 5000 and then compare with reference
    // string.
    auto getByPath = [](const NJson::TJsonValue& msg, TStringBuf path) {
        NJson::TJsonValue ret;
        UNIT_ASSERT_C(msg.GetValueByPath(path, ret), path);
        return ret.GetStringSafe();
    };

    for (auto &sensor : inputJson["sensors"].GetArraySafe()) {
        if (getByPath(sensor, "kind") == "GAUGE" &&
            (getByPath(sensor, "labels.sensor") == "PQ/TimeSinceLastReadMs" ||
            getByPath(sensor, "labels.sensor") == "PQ/PartitionLifeTimeMs" ||
            getByPath(sensor, "labels.sensor") == "PQ/WriteTimeLagMsByLastReadOld")) {
            auto value = sensor["value"].GetIntegerSafe();
            UNIT_ASSERT_GT(value, 4500);
            UNIT_ASSERT_LT(value, 5500);
            sensor.SetValueByPath("value", 5000);
        }
    }
    UNIT_ASSERT_VALUES_EQUAL(referenceJson, inputJson);
}

Y_UNIT_TEST(Partition) {
    SetEnv("FAST_UT", "1");
    TTestContext tc;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        TFinalizer finalizer(tc);
        tc.Prepare(dispatchName, setup, activeZone, false, true, true);
        tc.Runtime->SetScheduledLimit(1000);

        PQTabletPrepare({}, {}, tc);

        IActor* actor = CreateTabletCountersAggregator(false);
        auto aggregatorId = tc.Runtime->Register(actor);
        tc.Runtime->EnableScheduleForActor(aggregatorId);

        CmdWrite(0, "sourceid0", TestData(), tc, false, {}, true);
        CmdWrite(0, "sourceid1", TestData(), tc, false);
        CmdWrite(0, "sourceid2", TestData(), tc, false);
        PQGetPartInfo(0, 30, tc);

        {
            TDispatchOptions options;
            options.FinalEvents.emplace_back(TEvTabletCounters::EvTabletAddLabeledCounters);
            auto processedCountersEvent = tc.Runtime->DispatchEvents(options);
            UNIT_ASSERT_VALUES_EQUAL(processedCountersEvent, true);
        }

        IActor* actorX = CreateClusterLabeledCountersAggregatorActor(tc.Edge, TTabletTypes::PersQueue);
        tc.Runtime->Register(actorX);

        TAutoPtr<IEventHandle> handle;
        TEvTabletCounters::TEvTabletLabeledCountersResponse *result;
        result = tc.Runtime->GrabEdgeEvent<TEvTabletCounters::TEvTabletLabeledCountersResponse>(handle);
        UNIT_ASSERT(result);

        THttpRequest httpReq(HTTP_METHOD_GET);
        NMonitoring::TMonService2HttpRequest monReq(nullptr, &httpReq, nullptr, nullptr, "", nullptr);
        tc.Runtime->Send(new IEventHandle(aggregatorId, tc.Edge, new NMon::TEvHttpInfo(monReq)));

        TAutoPtr<IEventHandle> handle1;
        auto resp = tc.Runtime->GrabEdgeEvent<NMon::TEvHttpInfoRes>(handle1);
        const TString countersStr = ((NMon::TEvHttpInfoRes*) resp)->Answer.substr(sizeof("HTTP/1.1 200 Ok Content-Type: application/json Connection: Close "));
        const TString referenceStr = NResource::Find(TStringBuf("counters_labeled.json"));
        CompareJsons(countersStr, referenceStr);
    });
}

Y_UNIT_TEST(PartitionFirstClass) {
    SetEnv("FAST_UT", "1");
    TTestContext tc;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        TFinalizer finalizer(tc);
        activeZone = false;

        tc.Prepare(dispatchName, setup, activeZone, true, true, true);
        tc.Runtime->SetScheduledLimit(1000);

        PQTabletPrepare({}, {}, tc);

        IActor* actor = CreateTabletCountersAggregator(false);
        auto aggregatorId = tc.Runtime->Register(actor);
        tc.Runtime->EnableScheduleForActor(aggregatorId);

        CmdWrite(0, "sourceid0", TestData(), tc, false, {}, true);
        CmdWrite(0, "sourceid1", TestData(), tc, false);
        CmdWrite(0, "sourceid2", TestData(), tc, false);
        PQGetPartInfo(0, 30, tc);

        {
            TDispatchOptions options;
            options.FinalEvents.emplace_back(TEvTabletCounters::EvTabletAddLabeledCounters);
            tc.Runtime->DispatchEvents(options);
        }

        IActor* actorX = CreateClusterLabeledCountersAggregatorActor(tc.Edge, TTabletTypes::PersQueue);
        tc.Runtime->Register(actorX);

        TAutoPtr<IEventHandle> handle;
        TEvTabletCounters::TEvTabletLabeledCountersResponse *result;
        result = tc.Runtime->GrabEdgeEvent<TEvTabletCounters::TEvTabletLabeledCountersResponse>(handle);
        UNIT_ASSERT(result);
        UNIT_ASSERT_VALUES_EQUAL(result->Record.LabeledCountersByGroupSize(), 0);
    });
}

void CheckLabeledCountersResponse(TTestContext& tc, ui32 count, TVector<TString> mustHave = {}) {
    IActor* actor = CreateClusterLabeledCountersAggregatorActor(tc.Edge, TTabletTypes::PersQueue);
    tc.Runtime->Register(actor);

    TAutoPtr<IEventHandle> handle;
    TEvTabletCounters::TEvTabletLabeledCountersResponse *result;
    result = tc.Runtime->GrabEdgeEvent<TEvTabletCounters::TEvTabletLabeledCountersResponse>(handle);
    UNIT_ASSERT(result);

    THashSet<TString> groups;

    for (ui32 i = 0; i < result->Record.LabeledCountersByGroupSize(); ++i) {
        auto& c = result->Record.GetLabeledCountersByGroup(i);
        groups.insert(c.GetGroup());
    }
    UNIT_ASSERT_VALUES_EQUAL(groups.size(), count);
    for (auto& g : mustHave) {
        UNIT_ASSERT(groups.contains(g));
    }
}

Y_UNIT_TEST(ImportantFlagSwitching) {
    const TString topicName = "rt3.dc1--asdfgs--topic";

    TTestContext tc;
    RunTestWithReboots(tc.TabletIds, [&]() {
        return tc.InitialEventsFilter.Prepare();
    }, [&](const TString& dispatchName, std::function<void(TTestActorRuntime&)> setup, bool& activeZone) {
        TFinalizer finalizer(tc);
        tc.Prepare(dispatchName, setup, activeZone);
        activeZone = false;
        tc.Runtime->SetScheduledLimit(1000);

        auto MakeTopics = [&] (const TVector<TString>& users) {
            TVector<TString> res;
            for (const auto& u : users) {
                res.emplace_back(NKikimr::JoinPath({u, topicName}));
            }
            return res;
        };

        PQTabletPrepare({}, {}, tc);
        {
            TDispatchOptions options;
            options.FinalEvents.emplace_back(TEvTabletCounters::EvTabletAddLabeledCounters);
            tc.Runtime->DispatchEvents(options);
        }
        // Topic counters only
        CheckLabeledCountersResponse(tc, 8);

        // Topic counters + important
        PQTabletPrepare({}, {{"user", true}}, tc);
        {
            TDispatchOptions options;
            options.FinalEvents.emplace_back(TEvTabletCounters::EvTabletAddLabeledCounters);
            tc.Runtime->DispatchEvents(options);
        }
        CheckLabeledCountersResponse(tc, 8, {NKikimr::JoinPath({"user/1", topicName})});

        PQTabletPrepare({}, {}, tc);
        {
            TDispatchOptions options;
            options.FinalEvents.emplace_back(TEvTabletCounters::EvTabletAddLabeledCounters);
            tc.Runtime->DispatchEvents(options);
        }
        {
            TDispatchOptions options;
            options.FinalEvents.emplace_back(TEvTabletCounters::EvTabletAddLabeledCounters);
            tc.Runtime->DispatchEvents(options);
        }
        // Topic counters + not important
        CheckLabeledCountersResponse(tc, 8, MakeTopics({"user/0"}));

        // Topic counters + not important
        PQTabletPrepare({}, {{"user", true}, {"user2", true}}, tc);
        {
            TDispatchOptions options;
            options.FinalEvents.emplace_back(TEvTabletCounters::EvTabletAddLabeledCounters);
            tc.Runtime->DispatchEvents(options);
        }
        {
            TDispatchOptions options;
            options.FinalEvents.emplace_back(TEvTabletCounters::EvTabletAddLabeledCounters);
            tc.Runtime->DispatchEvents(options);
        }
        CheckLabeledCountersResponse(tc, 11, MakeTopics({"user/1", "user2/1"}));

        PQTabletPrepare({}, {{"user", true}, {"user2", false}}, tc);
        {
            TDispatchOptions options;
            options.FinalEvents.emplace_back(TEvTabletCounters::EvTabletAddLabeledCounters);
            tc.Runtime->DispatchEvents(options);
        }
        {
            TDispatchOptions options;
            options.FinalEvents.emplace_back(TEvTabletCounters::EvTabletAddLabeledCounters);
            tc.Runtime->DispatchEvents(options);
        }
        CheckLabeledCountersResponse(tc, 12, MakeTopics({"user/1", "user2/0"}));

        PQTabletPrepare({}, {{"user", true}}, tc);
        {
            TDispatchOptions options;
            options.FinalEvents.emplace_back(TEvTabletCounters::EvTabletAddLabeledCounters);
            tc.Runtime->DispatchEvents(options);
        }
        {
            TDispatchOptions options;
            options.FinalEvents.emplace_back(TEvTabletCounters::EvTabletAddLabeledCounters);
            tc.Runtime->DispatchEvents(options);
        }
        CheckLabeledCountersResponse(tc, 8, MakeTopics({"user/1"}));
    });
}
} // Y_UNIT_TEST_SUITE(PQCountersLabeled)

} // namespace NKikimr
