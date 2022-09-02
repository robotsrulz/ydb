#pragma once

#include "events.h"

#include <ydb/core/protos/serverless_proxy_config.pb.h>

#include <ydb/core/protos/serverless_proxy_config.pb.h>
#include <ydb/library/http_proxy/authorization/signature.h>
#include <ydb/public/api/grpc/draft/ydb_datastreams_v1.grpc.pb.h>

#include <library/cpp/actors/core/actorsystem.h>
#include <library/cpp/actors/http/http.h>
#include <library/cpp/grpc/client/grpc_client_low.h>
#include <library/cpp/http/server/http.h>
#include <library/cpp/json/json_value.h>
#include <library/cpp/json/json_reader.h>

#include <util/string/builder.h>


namespace NKikimr::NHttpProxy {

HttpCodes StatusToHttpCode(NYdb::EStatus status);
TString StatusToErrorType(NYdb::EStatus status);

class TRetryCounter {
public:
    bool HasAttemps() const {
        return UsedRetries < MaximumRetries;
    }

    void Void() {
        UsedRetries = 0;
    }

    void Click() {
        ++UsedRetries;
    }

    auto AttempN() const {
        return UsedRetries;
    }
private:
    const ui32 MaximumRetries{3};
    ui32 UsedRetries{0};
};


struct THttpResponseData {
    NYdb::EStatus Status{NYdb::EStatus::SUCCESS};
    NJson::TJsonValue Body;
    TString ErrorText;

    TString DumpBody(MimeTypes contentType);
};

struct THttpRequestContext {
    THttpRequestContext(const NKikimrConfig::TServerlessProxyConfig& config,
                        NHttp::THttpIncomingRequestPtr request,
                        NActors::TActorId sender,
                        NYdb::TDriver* driver,
                        std::shared_ptr<NYdb::ICredentialsProvider> serviceAccountCredentialsProvider);
    const NKikimrConfig::TServerlessProxyConfig& ServiceConfig;
    NHttp::THttpIncomingRequestPtr Request;
    NActors::TActorId Sender;
    NYdb::TDriver* Driver;
    std::shared_ptr<NYdb::ICredentialsProvider> ServiceAccountCredentialsProvider;

    THttpResponseData ResponseData;
    TString ServiceAccountId;
    TString RequestId;
    TString DiscoveryEndpoint;
    TString DatabaseName;
    TString DatabaseId; // not in context
    TString FolderId;   // not in context
    TString CloudId;    // not in context
    TString StreamName; // not in context
    TString SourceAddress;
    TString MethodName; // used once
    TString ApiVersion; // used once
    MimeTypes ContentType{MIME_UNKNOWN};
    TString IamToken;
    TString SerializedUserToken;

    TStringBuilder LogPrefix() const {
        return TStringBuilder() << "http request [" << MethodName << "] requestId [" << RequestId << "]";
    }

    THolder<NKikimr::NSQS::TAwsRequestSignV4> GetSignature();
    void SendBadRequest(NYdb::EStatus status, const TString& errorText, const TActorContext& ctx);
    void DoReply(const TActorContext& ctx);
    void ParseHeaders(TStringBuf headers);
    void RequestBodyToProto(NProtoBuf::Message* request);
};

class IHttpRequestProcessor {
public:
    virtual ~IHttpRequestProcessor() = default;

    virtual const TString& Name() const = 0;
    virtual void Execute(THttpRequestContext&& context,
                         THolder<NKikimr::NSQS::TAwsRequestSignV4> signature,
                         const TActorContext& ctx) = 0;
};

class THttpRequestProcessors {
public:
    using TService = Ydb::DataStreams::V1::DataStreamsService;
    using TServiceConnection = NGrpc::TServiceConnection<TService>;

public:
    void Initialize();
    bool Execute(const TString& name, THttpRequestContext&& params,
                 THolder<NKikimr::NSQS::TAwsRequestSignV4> signature,
                 const TActorContext& ctx);

private:
    THashMap<TString, THolder<IHttpRequestProcessor>> Name2Processor;
};

} // namespace NKinesis::NHttpProxy
