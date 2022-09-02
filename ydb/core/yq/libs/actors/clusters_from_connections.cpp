#include "clusters_from_connections.h"

#include <ydb/library/yql/providers/common/provider/yql_provider_names.h>
#include <ydb/library/yql/utils/url_builder.h>

#include <util/generic/hash.h>
#include <util/string/builder.h>
#include <util/system/env.h>

#include <library/cpp/string_utils/quote/quote.h>

namespace NYq {

using namespace NYql;

namespace {

template <typename TClusterConfig>
void FillClusterAuth(TClusterConfig& clusterCfg,
        const YandexQuery::IamAuth& auth, const TString& authToken,
        const THashMap<TString, TString>& accountIdSignatures) {
    switch (auth.identity_case()) {
    case YandexQuery::IamAuth::kNone:
        break;
    case YandexQuery::IamAuth::kCurrentIam:
        clusterCfg.SetToken(authToken);
        break;
    case YandexQuery::IamAuth::kServiceAccount:
        clusterCfg.SetServiceAccountId(auth.service_account().id());
        clusterCfg.SetServiceAccountIdSignature(accountIdSignatures.at(auth.service_account().id()));
        break;
    // Do not replace with default. Adding a new auth item should cause a compilation error
    case YandexQuery::IamAuth::IDENTITY_NOT_SET:
        break;
    }
}

void FillPqClusterConfig(NYql::TPqClusterConfig& clusterConfig,
        const TString& name, bool useBearerForYdb,
        const TString& authToken, const THashMap<TString, TString>& accountIdSignatures,
        const YandexQuery::DataStreams& ds) {
    clusterConfig.SetName(name);
    if (ds.endpoint()) {
        clusterConfig.SetEndpoint(ds.endpoint());
    }
    clusterConfig.SetDatabase(ds.database());
    clusterConfig.SetDatabaseId(ds.database_id());
    clusterConfig.SetUseSsl(ds.secure());
    clusterConfig.SetAddBearerToToken(useBearerForYdb);
    clusterConfig.SetClusterType(TPqClusterConfig::CT_DATA_STREAMS);
    FillClusterAuth(clusterConfig, ds.auth(), authToken, accountIdSignatures);
}

void FillS3ClusterConfig(NYql::TS3ClusterConfig& clusterConfig,
        const TString& name, const TString& authToken,
        const TString& objectStorageEndpoint,
        const THashMap<TString, TString>& accountIdSignatures,
        const YandexQuery::ObjectStorageConnection& s3) {
    clusterConfig.SetName(name);
    TString objectStorageUrl;


    if (objectStorageEndpoint == "https://s3.mds.yandex.net") {
        TUrlBuilder builder{"https://"};
        objectStorageUrl = builder.AddPathComponent(s3.bucket() + ".s3.mds.yandex.net/").Build();
    } else {
        TUrlBuilder builder{UrlEscapeRet(objectStorageEndpoint, true)};
        objectStorageUrl = builder.AddPathComponent(s3.bucket() + "/").Build();
    }
    clusterConfig.SetUrl(objectStorageUrl);
    FillClusterAuth(clusterConfig, s3.auth(), authToken, accountIdSignatures);
}

void FillSolomonClusterConfig(NYql::TSolomonClusterConfig& clusterConfig,
        const TString& name, const TString& authToken,
        const THashMap<TString, TString>& accountIdSignatures,
        const YandexQuery::Monitoring& monitoring) {
        clusterConfig.SetName(name);

        // TODO: move Endpoint to yq config
        auto solomonEndpointForTest = GetEnv("SOLOMON_ENDPOINT");
        auto solomonEndpoint = solomonEndpointForTest ? TString(solomonEndpointForTest) : TString();
        if (solomonEndpoint.empty()) {
            if (name.StartsWith("pre")) {
                solomonEndpoint = "monitoring.api.cloud-preprod.yandex.net";
                clusterConfig.SetUseSsl(true);
            } else if (name.StartsWith("so")) {
                solomonEndpoint = "solomon.yandex.net";
            } else {
                solomonEndpoint = "monitoring.api.cloud.yandex.net";
                clusterConfig.SetUseSsl(true);
            }
        }

        clusterConfig.SetCluster(solomonEndpoint);
        clusterConfig.SetClusterType(TSolomonClusterConfig::SCT_MONITORING);
        clusterConfig.MutablePath()->SetProject(monitoring.project());
        clusterConfig.MutablePath()->SetCluster(monitoring.cluster());
        FillClusterAuth(clusterConfig, monitoring.auth(), authToken, accountIdSignatures);
}

} //namespace

NYql::TPqClusterConfig CreatePqClusterConfig(const TString& name,
        bool useBearerForYdb, const TString& authToken,
        const TString& accountSignature, const YandexQuery::DataStreams& ds) {
    NYql::TPqClusterConfig cluster;
    THashMap<TString, TString> accountIdSignatures;
    if (ds.auth().has_service_account()) {
        accountIdSignatures[ds.auth().service_account().id()] = accountSignature;
    }
    FillPqClusterConfig(cluster, name, useBearerForYdb, authToken, accountIdSignatures, ds);
    return cluster;
}

NYql::TS3ClusterConfig CreateS3ClusterConfig(const TString& name,
        const TString& authToken, const TString& objectStorageEndpoint,
        const TString& accountSignature, const YandexQuery::ObjectStorageConnection& s3) {
    NYql::TS3ClusterConfig cluster;
    THashMap<TString, TString> accountIdSignatures;
    accountIdSignatures[s3.auth().service_account().id()] = accountSignature;
    FillS3ClusterConfig(cluster, name, authToken, objectStorageEndpoint, accountIdSignatures, s3);
    return cluster;
}

NYql::TSolomonClusterConfig CreateSolomonClusterConfig(const TString& name,
        const TString& authToken, const TString& accountSignature,
        const YandexQuery::Monitoring& monitoring) {
    NYql::TSolomonClusterConfig cluster;
    THashMap<TString, TString> accountIdSignatures;
    accountIdSignatures[monitoring.auth().service_account().id()] = accountSignature;
    FillSolomonClusterConfig(cluster, name, authToken, accountIdSignatures, monitoring);
    return cluster;
}

void AddClustersFromConnections(const THashMap<TString, YandexQuery::Connection>& connections,
    bool useBearerForYdb,
    const TString& objectStorageEndpoint,
    const TString& authToken,
    const THashMap<TString, TString>& accountIdSignatures,
    TGatewaysConfig& gatewaysConfig,
    THashMap<TString, TString>& clusters) {
    for (const auto&[_, conn] : connections) {
        auto connectionName = conn.content().name();
        switch (conn.content().setting().connection_case()) {
        case YandexQuery::ConnectionSetting::kYdbDatabase: {
            const auto& db = conn.content().setting().ydb_database();
            auto* clusterCfg = gatewaysConfig.MutableYdb()->AddClusterMapping();
            clusterCfg->SetName(connectionName);
            clusterCfg->SetId(db.database_id());
            if (db.database())
                clusterCfg->SetDatabase(db.database());
            if (db.endpoint())
                clusterCfg->SetEndpoint(db.endpoint());
            clusterCfg->SetSecure(db.secure());
            clusterCfg->SetAddBearerToToken(useBearerForYdb);
            FillClusterAuth(*clusterCfg, db.auth(), authToken, accountIdSignatures);
            clusters.emplace(connectionName, YdbProviderName);
            break;
        }
        case YandexQuery::ConnectionSetting::kClickhouseCluster: {
            const auto& ch = conn.content().setting().clickhouse_cluster();
            auto* clusterCfg = gatewaysConfig.MutableClickHouse()->AddClusterMapping();
            clusterCfg->SetName(connectionName);
            clusterCfg->SetId(ch.database_id());
            if (ch.host())
                clusterCfg->SetCluster(ch.host());
            clusterCfg->SetNativeHostPort(9440);
            clusterCfg->SetNativeSecure(true);
            clusterCfg->SetCHToken(TStringBuilder() << "basic#" << ch.login() << "#" << ch.password());
            clusters.emplace(connectionName, ClickHouseProviderName);
            break;
        }
        case YandexQuery::ConnectionSetting::kObjectStorage: {
            const auto& s3 = conn.content().setting().object_storage();
            auto* clusterCfg = gatewaysConfig.MutableS3()->AddClusterMapping();
            FillS3ClusterConfig(*clusterCfg, connectionName, authToken, objectStorageEndpoint, accountIdSignatures, s3);
            clusters.emplace(connectionName, S3ProviderName);
            break;
        }
        case YandexQuery::ConnectionSetting::kDataStreams: {
            const auto& ds = conn.content().setting().data_streams();
            auto* clusterCfg = gatewaysConfig.MutablePq()->AddClusterMapping();
            FillPqClusterConfig(*clusterCfg, connectionName, useBearerForYdb, authToken, accountIdSignatures, ds);
            clusters.emplace(connectionName, PqProviderName);
            break;
        }
        case YandexQuery::ConnectionSetting::kMonitoring: {
            const auto& monitoring = conn.content().setting().monitoring();
            auto* clusterCfg = gatewaysConfig.MutableSolomon()->AddClusterMapping();
            FillSolomonClusterConfig(*clusterCfg, connectionName, authToken, accountIdSignatures, monitoring);
            clusters.emplace(connectionName, SolomonProviderName);
            break;
        }

        // Do not replace with default. Adding a new connection should cause a compilation error
        case YandexQuery::ConnectionSetting::CONNECTION_NOT_SET:
            break;
        }
    }
}
} //NYq
