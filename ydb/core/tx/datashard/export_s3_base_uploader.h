#pragma once
#ifndef KIKIMR_DISABLE_S3_OPS

#include "datashard.h"
#include "export_common.h"
#include "export_s3.h"
#include "s3_common.h"

#include <ydb/core/base/appdata.h>
#include <ydb/core/protos/flat_scheme_op.pb.h>
#include <ydb/core/protos/services.pb.h>
#include <ydb/core/wrappers/s3_wrapper.h>
#include <library/cpp/actors/core/actor_bootstrapped.h>
#include <library/cpp/actors/core/hfunc.h>

#include <util/generic/buffer.h>
#include <util/generic/maybe.h>
#include <util/generic/ptr.h>
#include <util/generic/string.h>
#include <util/string/builder.h>
#include <util/string/cast.h>

namespace NKikimr {
namespace NDataShard {

using namespace Aws::S3;
using namespace Aws;

class IProxyOps {
public:
    virtual ~IProxyOps() = default;
    virtual bool NeedToResolveProxy() const = 0;
    virtual void ResolveProxy() = 0;

}; // IProxyOps

template <typename TDerived>
class TS3UploaderBase: public TActorBootstrapped<TDerived>
                     , private NWrappers::TS3User
                     , public IProxyOps
{
    using TEvS3Wrapper = NWrappers::TEvS3Wrapper;
    using TEvBuffer = TEvExportScan::TEvBuffer<TBuffer>;

protected:
    void Restart() {
        Y_VERIFY(ProxyResolved);

        MultiPart = false;
        Last = false;
        Parts.clear();

        if (Attempt) {
            this->Send(std::exchange(Client, TActorId()), new TEvents::TEvPoisonPill());
        }

        Client = this->RegisterWithSameMailbox(NWrappers::CreateS3Wrapper(Settings.GetCredentials(), Settings.GetConfig()));

        if (!SchemeUploaded) {
            this->Become(&TDerived::StateUploadScheme);

            UploadScheme();
        } else {
            this->Become(&TDerived::StateUploadData);

            if (Attempt) {
                this->Send(std::exchange(Scanner, TActorId()), new TEvExportScan::TEvReset());
            } else if (Scanner) {
                this->Send(Scanner, new TEvExportScan::TEvFeed());
            }
        }
    }

    void UploadScheme() {
        Y_VERIFY(!SchemeUploaded);

        if (!Scheme) {
            return Finish(false, "Cannot infer scheme");
        }

        google::protobuf::TextFormat::PrintToString(Scheme.GetRef(), &Buffer);

        auto request = Model::PutObjectRequest()
            .WithBucket(Settings.GetBucket())
            .WithKey(Settings.GetSchemeKey())
            .WithStorageClass(Settings.GetStorageClass());
        this->Send(Client, new TEvS3Wrapper::TEvPutObjectRequest(request, std::move(Buffer)));
    }

    void HandleScheme(TEvS3Wrapper::TEvPutObjectResponse::TPtr& ev) {
        const auto& result = ev->Get()->Result;

        EXPORT_LOG_D("HandleScheme TEvS3Wrapper::TEvPutObjectResponse"
            << ": self# " << this->SelfId()
            << ", result# " << result);

        if (!CheckResult(result, TStringBuf("PutObject (scheme)"))) {
            return;
        }

        SchemeUploaded = true;

        if (Scanner) {
            this->Send(Scanner, new TEvExportScan::TEvFeed());
        }

        this->Become(&TDerived::StateUploadData);
    }

    void Handle(TEvExportScan::TEvReady::TPtr& ev) {
        EXPORT_LOG_D("Handle TEvExportScan::TEvReady"
            << ": self# " << this->SelfId()
            << ", sender# " << ev->Sender);

        Scanner = ev->Sender;

        if (Error) {
            return PassAway();
        }

        if (ProxyResolved && SchemeUploaded) {
            this->Send(Scanner, new TEvExportScan::TEvFeed());
        }
    }

    void Handle(TEvBuffer::TPtr& ev) {
        EXPORT_LOG_D("Handle TEvExportScan::TEvBuffer"
            << ": self# " << this->SelfId()
            << ", sender# " << ev->Sender
            << ", msg# " << ev->Get()->ToString());

        if (ev->Sender != Scanner) {
            EXPORT_LOG_W("Received buffer from unknown scanner"
                << ": self# " << this->SelfId()
                << ", sender# " << ev->Sender
                << ", scanner# " << Scanner);
            return;
        }

        Last = ev->Get()->Last;
        MultiPart = MultiPart || !Last;
        ev->Get()->Buffer.AsString(Buffer);

        UploadData();
    }

    void UploadData() {
        if (!MultiPart) {
            auto request = Model::PutObjectRequest()
                .WithBucket(Settings.GetBucket())
                .WithKey(Settings.GetDataKey(DataFormat, CompressionCodec))
                .WithStorageClass(Settings.GetStorageClass());
            this->Send(Client, new TEvS3Wrapper::TEvPutObjectRequest(request, std::move(Buffer)));
        } else {
            if (!UploadId) {
                this->Send(DataShard, new TEvDataShard::TEvGetS3Upload(this->SelfId(), TxId));
                return;
            }

            auto request = Model::UploadPartRequest()
                .WithBucket(Settings.GetBucket())
                .WithKey(Settings.GetDataKey(DataFormat, CompressionCodec))
                .WithUploadId(*UploadId)
                .WithPartNumber(Parts.size() + 1);
            this->Send(Client, new TEvS3Wrapper::TEvUploadPartRequest(request, std::move(Buffer)));
        }
    }

    void HandleData(TEvS3Wrapper::TEvPutObjectResponse::TPtr& ev) {
        const auto& result = ev->Get()->Result;

        EXPORT_LOG_D("HandleData TEvS3Wrapper::TEvPutObjectResponse"
            << ": self# " << this->SelfId()
            << ", result# " << result);

        if (!CheckResult(result, TStringBuf("PutObject (data)"))) {
            return;
        }

        Finish();
    }

    void Handle(TEvDataShard::TEvS3Upload::TPtr& ev) {
        auto& upload = ev->Get()->Upload;

        EXPORT_LOG_D("Handle TEvDataShard::TEvS3Upload"
            << ": self# " << this->SelfId()
            << ", upload# " << upload);

        if (!upload) {
            auto request = Model::CreateMultipartUploadRequest()
                .WithBucket(Settings.GetBucket())
                .WithKey(Settings.GetDataKey(DataFormat, CompressionCodec))
                .WithStorageClass(Settings.GetStorageClass());
            this->Send(Client, new TEvS3Wrapper::TEvCreateMultipartUploadRequest(request));
        } else {
            UploadId = upload->Id;

            switch (upload->Status) {
                case TS3Upload::EStatus::UploadParts:
                    return UploadData();

                case TS3Upload::EStatus::Complete: {
                    Parts = std::move(upload->Parts);

                    TVector<Model::CompletedPart> parts(Reserve(Parts.size()));
                    for (ui32 partIndex = 0; partIndex < Parts.size(); ++partIndex) {
                        parts.emplace_back(Model::CompletedPart()
                            .WithPartNumber(partIndex + 1)
                            .WithETag(Parts.at(partIndex)));
                    }

                    auto request = Model::CompleteMultipartUploadRequest()
                        .WithBucket(Settings.GetBucket())
                        .WithKey(Settings.GetDataKey(DataFormat, CompressionCodec))
                        .WithUploadId(*UploadId)
                        .WithMultipartUpload(Model::CompletedMultipartUpload().WithParts(std::move(parts)));
                    this->Send(Client, new TEvS3Wrapper::TEvCompleteMultipartUploadRequest(request));
                    break;
                }

                case TS3Upload::EStatus::Abort: {
                    Error = std::move(upload->Error);
                    if (!Error) {
                        Error = "<empty>";
                    }

                    auto request = Model::AbortMultipartUploadRequest()
                        .WithBucket(Settings.GetBucket())
                        .WithKey(Settings.GetDataKey(DataFormat, CompressionCodec))
                        .WithUploadId(*UploadId);
                    this->Send(Client, new TEvS3Wrapper::TEvAbortMultipartUploadRequest(request));
                    break;
                }
            }
        }
    }

    void Handle(TEvS3Wrapper::TEvCreateMultipartUploadResponse::TPtr& ev) {
        const auto& result = ev->Get()->Result;

        EXPORT_LOG_D("Handle TEvS3Wrapper::TEvCreateMultipartUploadResponse"
            << ": self# " << this->SelfId()
            << ", result# " << result);

        if (!CheckResult(result, TStringBuf("CreateMultipartUpload"))) {
            return;
        }

        this->Send(DataShard, new TEvDataShard::TEvStoreS3UploadId(this->SelfId(), TxId, result.GetResult().GetUploadId().c_str()));
    }

    void Handle(TEvS3Wrapper::TEvUploadPartResponse::TPtr& ev) {
        const auto& result = ev->Get()->Result;

        EXPORT_LOG_D("Handle TEvS3Wrapper::TEvUploadPartResponse"
            << ": self# " << this->SelfId()
            << ", result# " << result);

        if (!CheckResult(result, TStringBuf("UploadPart"))) {
            return;
        }

        Parts.push_back(result.GetResult().GetETag().c_str());

        if (Last) {
            return Finish();
        }

        this->Send(Scanner, new TEvExportScan::TEvFeed());
    }

    void Handle(TEvS3Wrapper::TEvCompleteMultipartUploadResponse::TPtr& ev) {
        const auto& result = ev->Get()->Result;

        EXPORT_LOG_D("Handle TEvS3Wrapper::TEvCompleteMultipartUploadResponse"
            << ": self# " << this->SelfId()
            << ", result# " << result);

        if (!result.IsSuccess()) {
            const auto& error = result.GetError();
            if (error.GetErrorType() != S3Errors::NO_SUCH_UPLOAD) {
                Error = error.GetMessage().c_str();
            }
        }

        PassAway();
    }

    void Handle(TEvS3Wrapper::TEvAbortMultipartUploadResponse::TPtr& ev) {
        const auto& result = ev->Get()->Result;

        EXPORT_LOG_D("Handle TEvS3Wrapper::TEvAbortMultipartUploadResponse"
            << ": self# " << this->SelfId()
            << ", result# " << result);

        if (!result.IsSuccess()) {
            Y_VERIFY(Error);
            Error = TStringBuilder() << *Error << " Additionally, 'AbortMultipartUpload' has failed: "
                << result.GetError().GetMessage();
        }

        PassAway();
    }

    template <typename TResult>
    bool CheckResult(const TResult& result, const TStringBuf marker) {
        if (result.IsSuccess()) {
            return true;
        }

        EXPORT_LOG_E("Error at '" << marker << "'"
            << ": self# " << this->SelfId()
            << ", error# " << result);
        RetryOrFinish(result.GetError());

        return false;
    }

    void RetryOrFinish(const S3Error& error) {
        if (Attempt++ < Retries && error.ShouldRetry()) {
            Delay = Min(Delay * Attempt, TDuration::Minutes(10));
            const TDuration random = TDuration::FromValue(TAppData::RandomProvider->GenRand64() % Delay.MicroSeconds());

            this->Schedule(Delay + random, new TEvents::TEvWakeup());
        } else {
            Finish(false, TStringBuilder() << "S3 error: " << error.GetMessage().c_str());
        }
    }

    void Finish(bool success = true, const TString& error = TString()) {
        EXPORT_LOG_I("Finish"
            << ": self# " << this->SelfId()
            << ", success# " << success
            << ", error# " << error
            << ", multipart# " << MultiPart
            << ", uploadId# " << UploadId);

        if (!success) {
            Error = error;
        }

        if (!MultiPart || !UploadId) {
            if (!Scanner) {
                return;
            }

            PassAway();
        } else {
            if (success) {
                this->Send(DataShard, new TEvDataShard::TEvChangeS3UploadStatus(this->SelfId(), TxId,
                    TS3Upload::EStatus::Complete, std::move(Parts)));
            } else {
                this->Send(DataShard, new TEvDataShard::TEvChangeS3UploadStatus(this->SelfId(), TxId,
                    TS3Upload::EStatus::Abort, *Error));
            }
        }
    }

    void PassAway() override {
        if (Scanner) {
            this->Send(Scanner, new TEvExportScan::TEvFinish(Error.Empty(), Error.GetOrElse(TString())));
        }

        this->Send(Client, new TEvents::TEvPoisonPill());

        IActor::PassAway();
    }

public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::EXPORT_S3_UPLOADER_ACTOR;
    }

    static constexpr TStringBuf LogPrefix() {
        return "s3"sv;
    }

    explicit TS3UploaderBase(
            const TActorId& dataShard, ui64 txId,
            const NKikimrSchemeOp::TBackupTask& task,
            TMaybe<Ydb::Table::CreateTableRequest>&& scheme)
        : Settings(TS3Settings::FromBackupTask(task))
        , DataFormat(NBackupRestoreTraits::EDataFormat::Csv)
        , CompressionCodec(NBackupRestoreTraits::CodecFromTask(task))
        , DataShard(dataShard)
        , TxId(txId)
        , Scheme(std::move(scheme))
        , Retries(task.GetNumberOfRetries())
        , Attempt(0)
        , Delay(TDuration::Minutes(1))
        , SchemeUploaded(task.GetShardNum() == 0 ? false : true)
    {
    }

    void Bootstrap() {
        EXPORT_LOG_D("Bootstrap"
            << ": self# " << this->SelfId()
            << ", attempt# " << Attempt);

        ProxyResolved = !NeedToResolveProxy();
        if (!ProxyResolved) {
            ResolveProxy();
        } else {
            Restart();
        }
    }

    STATEFN(StateBase) {
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvExportScan::TEvReady, Handle);

            sFunc(TEvents::TEvWakeup, Bootstrap);
            sFunc(TEvents::TEvPoisonPill, PassAway);
        }
    }

    STATEFN(StateUploadScheme) {
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvS3Wrapper::TEvPutObjectResponse, HandleScheme);
        default:
            return StateBase(ev, TlsActivationContext->AsActorContext());
        }
    }

    STATEFN(StateUploadData) {
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvBuffer, Handle);
            hFunc(TEvDataShard::TEvS3Upload, Handle);

            hFunc(TEvS3Wrapper::TEvPutObjectResponse, HandleData);
            hFunc(TEvS3Wrapper::TEvCreateMultipartUploadResponse, Handle);
            hFunc(TEvS3Wrapper::TEvUploadPartResponse, Handle);
            hFunc(TEvS3Wrapper::TEvCompleteMultipartUploadResponse, Handle);
            hFunc(TEvS3Wrapper::TEvAbortMultipartUploadResponse, Handle);
        default:
            return StateBase(ev, TlsActivationContext->AsActorContext());
        }
    }

protected:
    TS3Settings Settings;
    const NBackupRestoreTraits::EDataFormat DataFormat;
    const NBackupRestoreTraits::ECompressionCodec CompressionCodec;
    bool ProxyResolved;

private:
    const TActorId DataShard;
    const ui64 TxId;
    const TMaybe<Ydb::Table::CreateTableRequest> Scheme;

    const ui32 Retries;
    ui32 Attempt;

    TActorId Client;
    TDuration Delay;
    bool SchemeUploaded;
    bool MultiPart;
    bool Last;

    TActorId Scanner;
    TString Buffer;

    TMaybe<TString> UploadId;
    TVector<TString> Parts;
    TMaybe<TString> Error;

}; // TS3UploaderBase

} // NDataShard
} // NKikimr

#endif // KIKIMR_DISABLE_S3_OPS
