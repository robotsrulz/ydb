#pragma once
#include "defs.h"
#include "keyvalue_intermediate.h"
#include "keyvalue_request_stat.h"
#include "keyvalue_helpers.h"
#include <ydb/public/lib/base/msgbus.h>
#include <ydb/core/keyvalue/protos/events.pb.h>


namespace NKikimr {

namespace NKeyValue {
    struct TIntermediate;
};

struct TEvKeyValue {
    enum EEv {
        EvRequest = EventSpaceBegin(TKikimrEvents::ES_KEYVALUE),
        EvIntermediate,
        EvNotify,
        EvStoreCollect,
        EvCollect,
        EvEraseCollect,
        EvPeriodicRefresh,
        EvReportWriteLatency,
        EvUpdateWeights,
        EvCompleteGC,
        EvPartitialCompleteGC,
        EvContinueGC,

        EvRead = EvRequest + 16,
        EvReadRange,
        EvExecuteTransaction,
        EvGetStorageChannelStatus,
        EvAcquireLock,

        EvResponse = EvRequest + 512,

        EvReadResponse = EvResponse + 16,
        EvReadRangeResponse,
        EvExecuteTransactionResponse,
        EvGetStorageChannelStatusResponse,
        EvAcquireLockResponse,

        EvEnd
    };

    static_assert(
        EvEnd < EventSpaceEnd(TKikimrEvents::ES_KEYVALUE),
        "expect EvEnd < EventSpaceEnd(TKikimrEvents::ES_KEYVALUE)");

    struct TEvReadResponse;

    struct TEvRead : public TEventPB<TEvRead,
            NKikimrKeyValue::ReadRequest, EvRead> {

        using TResponse = TEvReadResponse;
        TEvRead() { }
    };

    struct TEvReadResponse : public TEventPB<TEvReadResponse,
            NKikimrKeyValue::ReadResult, EvReadResponse> {
        TEvReadResponse() { }
    };

    struct TEvReadRangeResponse;

    struct TEvReadRange : public TEventPB<TEvReadRange,
            NKikimrKeyValue::ReadRangeRequest, EvReadRange> {

        using TResponse = TEvReadRangeResponse;
        TEvReadRange() { }
    };

    struct TEvReadRangeResponse : public TEventPB<TEvReadRangeResponse,
            NKikimrKeyValue::ReadRangeResult, EvReadRangeResponse> {
        TEvReadRangeResponse() { }
    };

    struct TEvExecuteTransactionResponse;

    struct TEvExecuteTransaction : public TEventPB<TEvExecuteTransaction,
            NKikimrKeyValue::ExecuteTransactionRequest, EvExecuteTransaction> {

        using TResponse = TEvExecuteTransactionResponse;
        TEvExecuteTransaction() { }
    };

    struct TEvExecuteTransactionResponse : public TEventPB<TEvExecuteTransactionResponse,
            NKikimrKeyValue::ExecuteTransactionResult, EvExecuteTransactionResponse> {
        TEvExecuteTransactionResponse() { }
    };

    struct TEvGetStorageChannelStatusResponse;

    struct TEvGetStorageChannelStatus : public TEventPB<TEvGetStorageChannelStatus,
            NKikimrKeyValue::GetStorageChannelStatusRequest, EvGetStorageChannelStatus> {

        using TResponse = TEvGetStorageChannelStatusResponse;
        TEvGetStorageChannelStatus() { }
    };

    struct TEvGetStorageChannelStatusResponse : public TEventPB<TEvGetStorageChannelStatusResponse,
            NKikimrKeyValue::GetStorageChannelStatusResult, EvGetStorageChannelStatusResponse> {
        TEvGetStorageChannelStatusResponse() { }
    };

    struct TEvAcquireLockResponse;

    struct TEvAcquireLock : public TEventPB<TEvAcquireLock,
            NKikimrKeyValue::AcquireLockRequest, EvAcquireLock> {

        using TResponse = TEvAcquireLockResponse;
        TEvAcquireLock() { }
    };

    struct TEvAcquireLockResponse : public TEventPB<TEvAcquireLockResponse,
            NKikimrKeyValue::AcquireLockResult, EvAcquireLockResponse> {
        TEvAcquireLockResponse() { }
    };

    struct TEvRequest : public TEventPB<TEvRequest,
            NKikimrClient::TKeyValueRequest, EvRequest> {
        TEvRequest() { }
    };

    struct TEvResponse : public TEventPB<TEvResponse,
            NKikimrClient::TResponse, EvResponse> {
        TEvResponse() { }
    };

    struct TEvIntermediate : public TEventLocal<TEvIntermediate, EvIntermediate> {
        THolder<NKeyValue::TIntermediate> Intermediate;

        TEvIntermediate() { }

        TEvIntermediate(THolder<NKeyValue::TIntermediate>&& intermediate)
            : Intermediate(std::move(intermediate))
        {}
    };

    struct TEvNotify : public TEventLocal<TEvNotify, EvNotify> {
        ui64 RequestUid;
        ui64 Generation;
        ui64 Step;
        NKeyValue::TRequestStat Stat;
        NMsgBusProxy::EResponseStatus Status;

        TEvNotify() { }

        TEvNotify(ui64 requestUid, ui64 generation, ui64 step, const NKeyValue::TRequestStat &stat,
                NMsgBusProxy::EResponseStatus status)
            : RequestUid(requestUid)
            , Generation(generation)
            , Step(step)
            , Stat(stat)
            , Status(status)
        {}

        TEvNotify(ui64 requestUid, ui64 generation, ui64 step, const NKeyValue::TRequestStat &stat,
                NKikimrKeyValue::Statuses::ReplyStatus status)
            : RequestUid(requestUid)
            , Generation(generation)
            , Step(step)
            , Stat(stat)
            , Status(ConvertStatus(status))
        {}

        static NMsgBusProxy::EResponseStatus ConvertStatus(NKikimrKeyValue::Statuses::ReplyStatus status) {
            switch (status) {
            case NKikimrKeyValue::Statuses::RSTATUS_OK:
                return NMsgBusProxy::MSTATUS_OK;
            case NKikimrKeyValue::Statuses::RSTATUS_ERROR:
                return NMsgBusProxy::MSTATUS_ERROR;
            case NKikimrKeyValue::Statuses::RSTATUS_TIMEOUT:
                return NMsgBusProxy::MSTATUS_TIMEOUT;
            case NKikimrKeyValue::Statuses::RSTATUS_INTERNAL_ERROR:
                return NMsgBusProxy::MSTATUS_INTERNALERROR;
            default:
                return NMsgBusProxy::MSTATUS_INTERNALERROR;
            }
        }
    };

    struct TEvStoreCollect : public TEventLocal<TEvStoreCollect, EvStoreCollect> {
        TEvStoreCollect() { }
    };

    struct TEvCollect : public TEventLocal<TEvCollect, EvCollect> {
        TEvCollect() { }
    };

    struct TEvEraseCollect : public TEventLocal<TEvEraseCollect, EvEraseCollect> {
        TEvEraseCollect() { }
    };

    struct TEvPeriodicRefresh : public TEventLocal<TEvPeriodicRefresh, EvPeriodicRefresh> {
        TEvPeriodicRefresh() { }
    };

    struct TEvCompleteGC : public TEventLocal<TEvCompleteGC, EvCompleteGC> {
        TEvCompleteGC() { }
    };

    struct TEvPartitialCompleteGC : public TEventLocal<TEvPartitialCompleteGC, EvPartitialCompleteGC> {
        TMaybe<NKeyValue::THelpers::TGenerationStep> CollectedGenerationStep;
        TVector<TLogoBlobID> CollectedDoNotKeep;

        TEvPartitialCompleteGC() { }
    };

    struct TEvContinueGC : public TEventLocal<TEvContinueGC, EvContinueGC> {
        TVector<TLogoBlobID> Buffer;
        TEvContinueGC(TVector<TLogoBlobID> &&buffer)
            : Buffer(std::move(buffer))
        { }
    };
};

} // NKikimr
