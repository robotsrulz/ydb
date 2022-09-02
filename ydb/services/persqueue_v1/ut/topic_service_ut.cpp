#include <ydb/public/api/grpc/draft/ydb_topic_tx_v1.grpc.pb.h>

#include <ydb/public/sdk/cpp/client/ydb_driver/driver.h>
#include <ydb/public/sdk/cpp/client/ydb_persqueue_core/ut/ut_utils/test_server.h>
#include <ydb/public/sdk/cpp/client/ydb_table/table.h>
#include <ydb/public/sdk/cpp/client/ydb_types/status_codes.h>

#include <ydb/core/protos/services.pb.h>

#include <util/stream/output.h>
#include <util/string/builder.h>

#include <library/cpp/testing/unittest/registar.h>

namespace NKikimr::NPersQueueTests {

Y_UNIT_TEST_SUITE(TopicService) {

NYdb::NTable::TSession CreateSession(NYdb::TDriver &driver) {
    NYdb::NTable::TClientSettings settings;
    NYdb::NTable::TTableClient client(driver, settings);

    auto result = client.CreateSession().ExtractValueSync();
    UNIT_ASSERT_EQUAL(result.IsTransportError(), false);

    return result.GetSession();
}

NYdb::NTable::TTransaction BeginTransaction(NYdb::NTable::TSession &session) {
    auto result = session.BeginTransaction().ExtractValueSync();
    UNIT_ASSERT_EQUAL(result.IsTransportError(), false);

    return result.GetTransaction();
}

template<class T>
std::unique_ptr<typename T::Stub> CreateServiceStub(const NPersQueue::TTestServer &server) {
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<typename T::Stub> stub;

    channel = grpc::CreateChannel("localhost:" + ToString(server.GrpcPort), grpc::InsecureChannelCredentials());
    stub = T::NewStub(channel);

    return stub;
}

std::unique_ptr<Ydb::Topic::V1::TopicServiceTx::Stub> CreateTopicServiceTxStub(const NPersQueue::TTestServer &server) {
    return CreateServiceStub<Ydb::Topic::V1::TopicServiceTx>(server);
}

struct TOffsetRange {
    ui64 Begin;
    ui64 End;
};

struct TPartition {
    ui64 Id;
    TVector<TOffsetRange> Offsets;
};

struct TTopic {
    TString Path;
    TVector<TPartition> Partitions;
};

void AppendOffsetsRange(const TOffsetRange& r, google::protobuf::RepeatedPtrField<Ydb::Topic::OffsetsRange> *offsets)
{
    auto* range = offsets->Add();

    range->set_start(r.Begin);
    range->set_end(r.End);
}

void AppendPartition(const TPartition& p,
                     google::protobuf::RepeatedPtrField<Ydb::Topic::AddOffsetsToTransactionRequest_TopicOffsets_PartitionOffsets> *partitions)
{
    auto* partition = partitions->Add();

    partition->set_partition_id(p.Id);

    for (auto& r : p.Offsets) {
        AppendOffsetsRange(r, partition->mutable_partition_offsets());
    }
}

void AppendTopic(const TTopic &t,
                 google::protobuf::RepeatedPtrField<Ydb::Topic::AddOffsetsToTransactionRequest_TopicOffsets> *topics)
{
    auto* topic = topics->Add();

    topic->set_path(t.Path);

    for (auto& p : t.Partitions) {
        AppendPartition(p, topic->mutable_partitions());
    }
}

Ydb::Topic::AddOffsetsToTransactionRequest CreateRequest(const TString& session_id,
                                                         const TString& tx_id,
                                                         const TVector<TTopic>& topics)
{
    Ydb::Topic::AddOffsetsToTransactionRequest request;

    request.set_session_id(session_id);
    request.mutable_tx_control()->set_tx_id(tx_id);

    for (auto& t : topics) {
        AppendTopic(t, request.mutable_topics());
    }

    return request;
}

class TAddOffsetToTransactionFixture : public NUnitTest::TBaseFixture {
protected:
    TMaybe<NPersQueue::TTestServer> server;
    TMaybe<NYdb::NTable::TSession> session;
    TMaybe<NYdb::NTable::TTransaction> tx;
    std::unique_ptr<Ydb::Topic::V1::TopicServiceTx::Stub> stub;

    const TString DATABASE = "/Root";
    const TString TOPIC_PARENT = "/Root/PQ";

    const TString VALID_TOPIC_NAME = "rt3.dc1--topic1";
    const TString VALID_SHORT_TOPIC_NAME = "topic1";
    const TString VALID_TOPIC_PATH = TOPIC_PARENT + "/" + VALID_TOPIC_NAME;

    const TString INVALID_TOPIC_NAME = VALID_TOPIC_NAME + "_2";
    const TString INVALID_SHORT_TOPIC_NAME = VALID_SHORT_TOPIC_NAME + "_2";
    const TString INVALID_TOPIC_PATH = TOPIC_PARENT + "/" + INVALID_TOPIC_NAME;

    const TString AUTH_TOKEN = "x-user-x@builtin";

    void SetUp(NUnitTest::TTestContext&) override {
        server = NPersQueue::TTestServer(false);
        server->ServerSettings.PQConfig.SetTopicsAreFirstClassCitizen(true);
        server->StartServer();
        server->EnableLogs({NKikimrServices::PQ_WRITE_PROXY
                           , NKikimrServices::PQ_READ_PROXY
                           , NKikimrServices::TX_PROXY_SCHEME_CACHE
                           , NKikimrServices::KQP_PROXY
                           , NKikimrServices::PERSQUEUE
                           , NKikimrServices::KQP_SESSION}, NActors::NLog::PRI_DEBUG);

        auto partsCount = 5u;
        server->AnnoyingClient->CreateTopicNoLegacy(VALID_TOPIC_PATH, partsCount);

        NACLib::TDiffACL acl;
        acl.AddAccess(NACLib::EAccessType::Allow, NACLib::DescribeSchema, AUTH_TOKEN);
        acl.AddAccess(NACLib::EAccessType::Allow, NACLib::ReadAttributes, AUTH_TOKEN);
        acl.AddAccess(NACLib::EAccessType::Allow, NACLib::WriteAttributes, AUTH_TOKEN);
        server->AnnoyingClient->ModifyACL(TOPIC_PARENT, VALID_TOPIC_NAME, acl.SerializeAsString());

        auto driverCfg = NYdb::TDriverConfig()
            .SetEndpoint(TStringBuilder() << "localhost:" << server->GrpcPort)
            .SetDatabase(DATABASE)
            .SetAuthToken(AUTH_TOKEN);

        auto ydbDriver = std::make_shared<NYdb::TDriver>(driverCfg);

        session = CreateSession(*ydbDriver);
        tx = BeginTransaction(*session);

        stub = CreateTopicServiceTxStub(*server);
    }

    Ydb::Topic::AddOffsetsToTransactionResponse CallAddOffsetsToTransaction(const TVector<TTopic>& topics) {
        grpc::ClientContext rcontext;
        rcontext.AddMetadata("x-ydb-auth-ticket", AUTH_TOKEN);
        rcontext.AddMetadata("x-ydb-database", DATABASE);

        Ydb::Topic::AddOffsetsToTransactionResponse response;

        grpc::Status status = stub->AddOffsetsToTransaction(&rcontext,
                                                            CreateRequest(session->GetId(), tx->GetId(), topics),
                                                            &response);
        UNIT_ASSERT(status.ok());

        return response;
    }

    void TestTopicPaths(const TString& path1, const TString& path2) {
        const auto PARTITION_ID = 1;
        const auto BEGIN = 4;
        const auto END = 7;

        auto response = CallAddOffsetsToTransaction({
            TTopic{.Path=path1, .Partitions={
                TPartition{.Id=PARTITION_ID, .Offsets={
                    TOffsetRange{.Begin=BEGIN, .End=END}
                }}
            }}
        });
        UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::SUCCESS);

        response = CallAddOffsetsToTransaction({
            TTopic{.Path=path2, .Partitions={
                TPartition{.Id=PARTITION_ID, .Offsets={
                    TOffsetRange{.Begin=BEGIN, .End=END}
                }}
            }}
        });
        UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::BAD_REQUEST);
    }
};

Y_UNIT_TEST_F(TheRangesDoNotOverlap, TAddOffsetToTransactionFixture) {
    Ydb::Topic::AddOffsetsToTransactionResponse response = CallAddOffsetsToTransaction({
        TTopic{.Path=VALID_TOPIC_PATH, .Partitions={
            TPartition{.Id=4, .Offsets={
                TOffsetRange{.Begin=1, .End=3},
                TOffsetRange{.Begin=5, .End=8}
            }},
            TPartition{.Id=1, .Offsets={
                TOffsetRange{.Begin=2, .End=6}
            }}
        }}
    });
    UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::SUCCESS);

    response = CallAddOffsetsToTransaction({
        TTopic{.Path=VALID_TOPIC_PATH, .Partitions={
            TPartition{.Id=1, .Offsets={
                TOffsetRange{.Begin=8, .End=11}
            }}
        }}
    });
    UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::SUCCESS);
}

Y_UNIT_TEST_F(TheRangesOverlap, TAddOffsetToTransactionFixture) {
    Ydb::Topic::AddOffsetsToTransactionResponse response = CallAddOffsetsToTransaction({
        TTopic{.Path=VALID_TOPIC_PATH, .Partitions={
            TPartition{.Id=4, .Offsets={
                TOffsetRange{.Begin=1, .End=3},
                TOffsetRange{.Begin=5, .End=8}
            }},
            TPartition{.Id=1, .Offsets={
                TOffsetRange{.Begin=2, .End=6}
            }}
        }}
    });
    UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::SUCCESS);

    response = CallAddOffsetsToTransaction({
        TTopic{.Path=VALID_TOPIC_PATH, .Partitions={
            TPartition{.Id=4, .Offsets={
                TOffsetRange{.Begin=4, .End=7}
            }}
        }}
    });
    UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::BAD_REQUEST);
}

Y_UNIT_TEST_F(UnknownTopic, TAddOffsetToTransactionFixture) {
    auto response = CallAddOffsetsToTransaction({
        TTopic{.Path=INVALID_TOPIC_PATH, .Partitions={
            TPartition{.Id=4, .Offsets={
                TOffsetRange{.Begin=4, .End=7}
            }}
        }}
    });
    UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::SCHEME_ERROR);
}

Y_UNIT_TEST_F(UseDoubleSlashInTopicPath, TAddOffsetToTransactionFixture) {
    TestTopicPaths("//Root//PQ//rt3.dc1--topic1", "/Root/PQ/rt3.dc1--topic1");
}

Y_UNIT_TEST_F(RelativePath, TAddOffsetToTransactionFixture) {
    TestTopicPaths("PQ/rt3.dc1--topic1", "/Root/PQ/rt3.dc1--topic1");
}

Y_UNIT_TEST_F(AccessRights, TAddOffsetToTransactionFixture) {
    auto response = CallAddOffsetsToTransaction({
        TTopic{.Path=VALID_TOPIC_PATH, .Partitions={
            TPartition{.Id=4, .Offsets={
                TOffsetRange{.Begin=4, .End=7}
            }}
        }}
    });
    UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::SUCCESS);

    NACLib::TDiffACL acl;
    acl.RemoveAccess(NACLib::EAccessType::Allow, NACLib::ReadAttributes, AUTH_TOKEN);
    server->AnnoyingClient->ModifyACL(TOPIC_PARENT, VALID_TOPIC_NAME, acl.SerializeAsString());

    response = CallAddOffsetsToTransaction({
        TTopic{.Path=VALID_TOPIC_PATH, .Partitions={
            TPartition{.Id=4, .Offsets={
                TOffsetRange{.Begin=14, .End=17}
            }}
        }}
    });
    UNIT_ASSERT_VALUES_EQUAL(response.operation().status(), Ydb::StatusIds::UNAUTHORIZED);
}

}

}
