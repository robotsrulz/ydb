#include "keyvalue_storage_read_request.h"
#include "keyvalue_const.h"

#include <ydb/core/util/stlog.h>
#include <library/cpp/actors/protos/services_common.pb.h>


namespace NKikimr {
namespace NKeyValue {

#define STLOG_WITH_ERROR_DESCRIPTION(VARIABLE, PRIO, COMP, MARKER, TEXT, ...) \
    do { \
        struct MARKER {}; \
        VARIABLE = (TStringBuilder() << VARIABLE << Endl \
                << ::NKikimr::NStLog::TMessage<MARKER>(__FILE__, __LINE__, #MARKER, TStringBuilder() << TEXT) \
                        STLOG_PARAMS(__VA_ARGS__)); \
        STLOG(PRIO, COMP, MARKER, TEXT, __VA_ARGS__); \
    } while(false) \
// STLOG_WITH_ERROR_DESCRIPTION


class TKeyValueStorageReadRequest : public TActorBootstrapped<TKeyValueStorageReadRequest> {
    struct TGetBatch {
        TStackVec<ui32, 1> ReadItemIndecies;
        ui32 GroupId;
        ui32 Cookie;
        TInstant SentTime;

        TGetBatch(ui32 groupId, ui32 cookie)
            : GroupId(groupId)
            , Cookie(cookie)
        {}
    };

    struct TReadItemInfo {
        TIntermediate::TRead *Read;
        TIntermediate::TRead::TReadItem *ReadItem;
    };

    THolder<TIntermediate> IntermediateResult;
    const TTabletStorageInfo *TabletInfo;
    TStackVec<TGetBatch, 1> Batches;

    ui32 ReceivedGetResults = 0;
    TString ErrorDescription;

    TStackVec<TReadItemInfo, 1> ReadItems;

public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::KEYVALUE_ACTOR;
    }

    std::variant<TIntermediate::TRead, TIntermediate::TRangeRead>& GetCommand() const {
        return *IntermediateResult->ReadCommand;
    }

    bool IsRead() const {
        return std::holds_alternative<TIntermediate::TRead>(GetCommand());
    }

    bool IsRangeRead() const {
        return std::holds_alternative<TIntermediate::TRangeRead>(GetCommand());
    }

    void AddRead(TIntermediate::TRead &read) {
        for (auto &readItem : read.ReadItems) {
            ReadItems.push_back({&read, &readItem});
        }
    }

    NKikimrBlobStorage::EGetHandleClass GetHandleClass() const {
        auto visitor = [&] (auto &request) {
            return request.HandleClass;
        };
        return std::visit(visitor, GetCommand());
    }

    void Bootstrap() {
        if (IntermediateResult->Deadline != TInstant::Max()) {
            TInstant now = TActivationContext::Now();
            if (IntermediateResult->Deadline <= now) {
                STLOG_WITH_ERROR_DESCRIPTION(ErrorDescription, NLog::PRI_ERROR, NKikimrServices::KEYVALUE, KV313,
                        "Deadline reached before processing request.",
                        (KeyValue, TabletInfo->TabletID),
                        (Deadline, IntermediateResult->Deadline.MilliSeconds()),
                        (Now, now.MilliSeconds()),
                        (GotAt, IntermediateResult->Stat.IntermediateCreatedAt.MilliSeconds()),
                        (EnqueuedAs, IntermediateResult->Stat.EnqueuedAs));
                ReplyErrorAndPassAway(NKikimrKeyValue::Statuses::RSTATUS_TIMEOUT);
                return;
            }

            const TDuration timeout = IntermediateResult->Deadline - now;
            Schedule(timeout, new TEvents::TEvWakeup());
        }

        ui32 readCount = 0;
        auto addReadItems = [&](auto &request) {
            using Type = std::decay_t<decltype(request)>;
            if constexpr (std::is_same_v<Type, TIntermediate::TRead>) {
                AddRead(request);
                readCount++;
            } else {
                for (auto &read : request.Reads) {
                    AddRead(read);
                    readCount++;
                }
            }
        };
        std::visit(addReadItems, GetCommand());

        if (ReadItems.empty()) {
            auto getStatus = [&](auto &request) {
                return request.Status;
            };
            NKikimrProto::EReplyStatus status = std::visit(getStatus, GetCommand());

            STLOG(NLog::PRI_INFO, NKikimrServices::KEYVALUE, KV320, "Inline read request",
                    (KeyValue, TabletInfo->TabletID),
                    (Status, status));
            bool isError = status != NKikimrProto::OK
                    && status != NKikimrProto::UNKNOWN
                    && status != NKikimrProto::NODATA
                    && status != NKikimrProto::OVERRUN;
            if (isError) {
                STLOG_WITH_ERROR_DESCRIPTION(ErrorDescription, NLog::PRI_ERROR, NKikimrServices::KEYVALUE, KV321,
                    "Expected OK, UNKNOWN, NODATA or OVERRUN but given " << NKikimrProto::EReplyStatus_Name(status));
                ReplyErrorAndPassAway(NKikimrKeyValue::Statuses::RSTATUS_INTERNAL_ERROR);
            } else {
                STLOG(NLog::PRI_DEBUG, NKikimrServices::KEYVALUE, KV322,
                    "Expected OK or UNKNOWN and given " << NKikimrProto::EReplyStatus_Name(status)
                    << " readCount# " << readCount);

                NKikimrKeyValue::Statuses::ReplyStatus replyStatus;
                if (status == NKikimrProto::UNKNOWN || status == NKikimrProto::NODATA) {
                    replyStatus = NKikimrKeyValue::Statuses::RSTATUS_OK;
                } else {
                    replyStatus = ConvertStatus(status);
                }

                SendResponseAndPassAway(replyStatus);
            }
        }

        Become(&TThis::StateWait);
        SendGets();
    }

    void SendGets() {
        THashMap<ui32, ui32> mapFromGroupToBatch;

        for (ui32 readItemIdx = 0; readItemIdx < ReadItems.size(); ++readItemIdx) {
            TIntermediate::TRead::TReadItem &readItem = *ReadItems[readItemIdx].ReadItem;
            TLogoBlobID &id = readItem.LogoBlobId;
            ui32 group = TabletInfo->GroupFor(id.Channel(), id.Generation());

            // INVALID GROUP
            if (group == Max<ui32>()) {
                STLOG_WITH_ERROR_DESCRIPTION(ErrorDescription, NLog::PRI_ERROR, NKikimrServices::KEYVALUE, KV315,
                        "InternalError can't find correct group",
                        (KeyValue, TabletInfo->TabletID),
                        (Channel, id.Channel()),
                        (Generation, id.Generation()));
                ReplyErrorAndPassAway(NKikimrKeyValue::Statuses::RSTATUS_INTERNAL_ERROR);
                return;
            }

            auto it = mapFromGroupToBatch.find(group);
            if (it == mapFromGroupToBatch.end()) {
                it = mapFromGroupToBatch.emplace(group, Batches.size()).first;
                Batches.emplace_back(group, Batches.size());
            }
            TGetBatch &batch = Batches[it->second];
            batch.ReadItemIndecies.push_back(readItemIdx);
        }

        NKikimrBlobStorage::EGetHandleClass handleClass = GetHandleClass();

        for (TGetBatch &batch : Batches) {
            TArrayHolder<TEvBlobStorage::TEvGet::TQuery> readQueries(
                    new TEvBlobStorage::TEvGet::TQuery[batch.ReadItemIndecies.size()]);
            for (ui32 readQueryIdx = 0; readQueryIdx < batch.ReadItemIndecies.size(); ++readQueryIdx) {
                ui32 readItemIdx = batch.ReadItemIndecies[readQueryIdx];
                TIntermediate::TRead::TReadItem &readItem = *ReadItems[readItemIdx].ReadItem;
                readQueries[readQueryIdx].Set(readItem.LogoBlobId, readItem.BlobOffset, readItem.BlobSize);
                readItem.InFlight = true;
            }

            std::unique_ptr<TEvBlobStorage::TEvGet> get = std::make_unique<TEvBlobStorage::TEvGet>(
                    readQueries, batch.ReadItemIndecies.size(), IntermediateResult->Deadline, handleClass, false);

            SendToBSProxy(TActivationContext::AsActorContext(), batch.GroupId, get.release(),
                    batch.Cookie);
            batch.SentTime = TActivationContext::Now();
        }
    }

    void Handle(TEvBlobStorage::TEvGetResult::TPtr &ev) {
        TEvBlobStorage::TEvGetResult *result = ev->Get();
        STLOG(NLog::PRI_INFO, NKikimrServices::KEYVALUE, KV20, "Received GetResult",
                (KeyValue, TabletInfo->TabletID),
                (GroupId, result->GroupId),
                (Status, result->Status),
                (ResponseSz, result->ResponseSz),
                (ErrorReason, result->ErrorReason),
                (ReadRequestCookie, IntermediateResult->Cookie));

        if (ev->Cookie >= Batches.size()) {
            STLOG_WITH_ERROR_DESCRIPTION(ErrorDescription, NLog::PRI_ERROR, NKikimrServices::KEYVALUE, KV319,
                    "Received EvGetResult with an unexpected cookie.",
                    (KeyValue, TabletInfo->TabletID),
                    (Cookie, ev->Cookie),
                    (SentGets, Batches.size()),
                    (GroupId, result->GroupId),
                    (Status, result->Status),
                    (Deadline, IntermediateResult->Deadline.MilliSeconds()),
                    (Now, TActivationContext::Now().MilliSeconds()),
                    (GotAt, IntermediateResult->Stat.IntermediateCreatedAt.MilliSeconds()),
                    (ErrorReason, result->ErrorReason));
            ReplyErrorAndPassAway(NKikimrKeyValue::Statuses::RSTATUS_INTERNAL_ERROR);
            return;
        }

        TGetBatch &batch = Batches[ev->Cookie];

        if (result->GroupId != batch.GroupId) {
            STLOG_WITH_ERROR_DESCRIPTION(ErrorDescription, NLog::PRI_ERROR, NKikimrServices::KEYVALUE, KV318,
                    "Received EvGetResult from an unexpected storage group.",
                    (KeyValue, TabletInfo->TabletID),
                    (GroupId, result->GroupId),
                    (ExpecetedGroupId, batch.GroupId),
                    (Status, result->Status),
                    (Deadline, IntermediateResult->Deadline.MilliSeconds()),
                    (Now, TActivationContext::Now().MilliSeconds()),
                    (SentAt, batch.SentTime),
                    (GotAt, IntermediateResult->Stat.IntermediateCreatedAt.MilliSeconds()),
                    (ErrorReason, result->ErrorReason));
            ReplyErrorAndPassAway(NKikimrKeyValue::Statuses::RSTATUS_INTERNAL_ERROR);
            return;
        }

        if (result->Status != NKikimrProto::OK) {
            STLOG_WITH_ERROR_DESCRIPTION(ErrorDescription, NLog::PRI_ERROR, NKikimrServices::KEYVALUE, KV316,
                    "Unexpected EvGetResult.",
                    (KeyValue, TabletInfo->TabletID),
                    (Status, result->Status),
                    (Deadline, IntermediateResult->Deadline.MilliSeconds()),
                    (Now, TActivationContext::Now().MilliSeconds()),
                    (SentAt, batch.SentTime),
                    (GotAt, IntermediateResult->Stat.IntermediateCreatedAt.MilliSeconds()),
                    (ErrorReason, result->ErrorReason));
            ReplyErrorAndPassAway(NKikimrKeyValue::Statuses::RSTATUS_INTERNAL_ERROR);
            return;
        }


        bool hasErrorResponses = false;
        for (ui32 readQueryIdx = 0; readQueryIdx < batch.ReadItemIndecies.size(); ++readQueryIdx) {
            ui32 readItemIdx = batch.ReadItemIndecies[readQueryIdx];
            TEvBlobStorage::TEvGetResult::TResponse &response = ev->Get()->Responses[readQueryIdx];
            TIntermediate::TRead &read = *ReadItems[readItemIdx].Read;
            TIntermediate::TRead::TReadItem &readItem = *ReadItems[readItemIdx].ReadItem;
            read.Status = response.Status;

            if (response.Status == NKikimrProto::OK) {
                if (read.Value.size() != read.ValueSize) {
                    read.Value.resize(read.ValueSize);
                }
                Y_VERIFY_S(response.Buffer.size() == readItem.BlobSize,
                        "response.Buffer.size()# " << response.Buffer.size()
                        << " readItem.BlobSize# " << readItem.BlobSize);
                Y_VERIFY_S(readItem.ValueOffset + readItem.BlobSize <= read.ValueSize,
                        "readItem.ValueOffset# " << readItem.ValueOffset
                        << " readItem.BlobSize# " << readItem.BlobSize
                        << " read.ValueSize# " << read.ValueSize);
                memcpy(const_cast<char *>(read.Value.data()) + readItem.ValueOffset, response.Buffer.data(), response.Buffer.size());
                IntermediateResult->Stat.GroupReadBytes[std::make_pair(response.Id.Channel(), batch.GroupId)] += response.Buffer.size();
                // FIXME: count distinct blobs?" keyvalue_storage_request.cpp:279
                IntermediateResult->Stat.GroupReadIops[std::make_pair(response.Id.Channel(), batch.GroupId)] += 1;
            } else {
                STLOG_WITH_ERROR_DESCRIPTION(ErrorDescription, NLog::PRI_ERROR, NKikimrServices::KEYVALUE, KV317,
                        "Unexpected EvGetResult.",
                        (KeyValue, TabletInfo->TabletID),
                        (Status, result->Status),
                        (Id, response.Id),
                        (ResponseStatus, response.Status),
                        (Deadline, IntermediateResult->Deadline),
                        (Now, TActivationContext::Now()),
                        (SentAt, batch.SentTime),
                        (GotAt, IntermediateResult->Stat.IntermediateCreatedAt),
                        (ErrorReason, result->ErrorReason));
                hasErrorResponses = true;
            }

            Y_VERIFY(response.Status != NKikimrProto::UNKNOWN);
            readItem.Status = response.Status;
            readItem.InFlight = false;
        }
        if (hasErrorResponses) {
            ReplyErrorAndPassAway(NKikimrKeyValue::Statuses::RSTATUS_INTERNAL_ERROR);
            return;
        }

        ReceivedGetResults++;
        if (ReceivedGetResults == Batches.size()) {
            SendResponseAndPassAway(IntermediateResult->IsTruncated ?
                    NKikimrKeyValue::Statuses::RSTATUS_OVERRUN :
                    NKikimrKeyValue::Statuses::RSTATUS_OK);
        }
    }

    void SendNotify(NKikimrKeyValue::Statuses::ReplyStatus status) {
        IntermediateResult->UpdateStat();
        Send(IntermediateResult->KeyValueActorId, new TEvKeyValue::TEvNotify(
            IntermediateResult->RequestUid,
            IntermediateResult->CreatedAtGeneration, IntermediateResult->CreatedAtStep,
            IntermediateResult->Stat, status));
    }

    std::unique_ptr<TEvKeyValue::TEvReadResponse> CreateReadResponse(NKikimrKeyValue::Statuses::ReplyStatus status,
            const TString &errorDescription)
    {
        auto response = std::make_unique<TEvKeyValue::TEvReadResponse>();
        response->Record.set_status(status);
        if (errorDescription) {
            response->Record.set_msg(errorDescription);
        }
        if (IntermediateResult->HasCookie) {
            response->Record.set_cookie(IntermediateResult->Cookie);
        }
        return response;
    }

    std::unique_ptr<TEvKeyValue::TEvReadRangeResponse> CreateReadRangeResponse(
            NKikimrKeyValue::Statuses::ReplyStatus status, const TString &errorDescription)
    {
        auto response = std::make_unique<TEvKeyValue::TEvReadRangeResponse>();
        response->Record.set_status(status);
        if (errorDescription) {
            response->Record.set_msg(errorDescription);
        }
        return response;
    }

    std::unique_ptr<IEventBase> MakeErrorResponse(NKikimrKeyValue::Statuses::ReplyStatus status) {
        if (IsRead()) {
            return CreateReadResponse(status, ErrorDescription);
        } else {
            return CreateReadRangeResponse(status, ErrorDescription);
        }
    }

    void ReplyErrorAndPassAway(NKikimrKeyValue::Statuses::ReplyStatus status) {
        std::unique_ptr<IEventBase> response = MakeErrorResponse(status);
        Send(IntermediateResult->RespondTo, response.release());
        IntermediateResult->IsReplied = true;
        SendNotify(status);
        PassAway();
    }

    TString MakeErrorMsg(const TString &msg) const {
        TStringBuilder builder;
        if (ErrorDescription) {
            builder << ErrorDescription << ';';
        }
        if (msg) {
            builder << "Message# " << msg << ';';
        }
        return builder;
    }

    std::unique_ptr<TEvKeyValue::TEvReadResponse> MakeReadResponse(NKikimrKeyValue::Statuses::ReplyStatus status) {
        auto &cmd = GetCommand();
        Y_VERIFY(std::holds_alternative<TIntermediate::TRead>(cmd));
        TIntermediate::TRead &interRead = std::get<TIntermediate::TRead>(cmd);

        TString errorMsg = MakeErrorMsg(interRead.Message);
        std::unique_ptr<TEvKeyValue::TEvReadResponse> response = CreateReadResponse(status, errorMsg);

        response->Record.set_requested_key(interRead.Key);
        response->Record.set_requested_offset(interRead.Offset);
        response->Record.set_requested_size(interRead.RequestedSize);
        response->Record.set_value(interRead.Value);

        if (IntermediateResult->RespondTo.NodeId() != SelfId().NodeId()) {
            response->Record.set_node_id(SelfId().NodeId());
        }

        return response;
    }

    NKikimrKeyValue::Statuses::ReplyStatus ConvertStatus(NKikimrProto::EReplyStatus status) {
        if (status == NKikimrProto::OK) {
            return NKikimrKeyValue::Statuses::RSTATUS_OK;
        } else if (status == NKikimrProto::OVERRUN) {
            return NKikimrKeyValue::Statuses::RSTATUS_OVERRUN;
        } else {
            return NKikimrKeyValue::Statuses::RSTATUS_INTERNAL_ERROR;
        }
    }

    std::unique_ptr<TEvKeyValue::TEvReadRangeResponse> MakeReadRangeResponse(NKikimrKeyValue::Statuses::ReplyStatus status) {
        auto &cmd = GetCommand();
        Y_VERIFY(std::holds_alternative<TIntermediate::TRangeRead>(cmd));
        TIntermediate::TRangeRead &interRange = std::get<TIntermediate::TRangeRead>(cmd);

        TStringBuilder msgBuilder;
        if (ErrorDescription) {
            msgBuilder << ErrorDescription << ';';
        }
        for (ui32 idx = 0; idx < interRange.Reads.size(); ++idx) {
            auto &interRead = interRange.Reads[idx];
            if (interRead.Message) {
                msgBuilder << "Messages[" << idx << "]# " << interRead.Message << ';';
            }
        }

        std::unique_ptr<TEvKeyValue::TEvReadRangeResponse> response = CreateReadRangeResponse(status, msgBuilder);
        NKikimrKeyValue::ReadRangeResult &readRangeResult = response->Record;

        for (auto &interRead : interRange.Reads) {
            auto *kvp = readRangeResult.add_pair();
            kvp->set_key(interRead.Key);
            kvp->set_value(interRead.Value);
            kvp->set_value_size(interRead.ValueSize);
            kvp->set_creation_unix_time(interRead.CreationUnixTime);
            ui32 storageChannel = MainStorageChannelInPublicApi;
            if (interRead.StorageChannel == NKikimrClient::TKeyValueRequest::INLINE) {
                storageChannel = InlineStorageChannelInPublicApi;
            } else {
                storageChannel = interRead.StorageChannel + MainStorageChannelInPublicApi;
            }
            kvp->set_storage_channel(storageChannel);
            kvp->set_status(NKikimrKeyValue::Statuses::RSTATUS_OK);
        }
        readRangeResult.set_status(status);

        if (IntermediateResult->RespondTo.NodeId() != SelfId().NodeId()) {
            readRangeResult.set_node_id(SelfId().NodeId());
        }

        return response;
    }

    std::unique_ptr<IEventBase> MakeResponse(NKikimrKeyValue::Statuses::ReplyStatus status) {
        if (IsRead()) {
            return MakeReadResponse(status);
        } else {
            return MakeReadRangeResponse(status);
        }
    }

    void SendResponseAndPassAway(NKikimrKeyValue::Statuses::ReplyStatus status = NKikimrKeyValue::Statuses::RSTATUS_OK) {
        STLOG(NLog::PRI_INFO, NKikimrServices::KEYVALUE, KV34, "Send respose",
                (KeyValue, TabletInfo->TabletID),
                (Status, NKikimrKeyValue::Statuses_ReplyStatus_Name(status)),
                (ReadRequestCookie, IntermediateResult->Cookie));
        std::unique_ptr<IEventBase> response = MakeResponse(status);
        Send(IntermediateResult->RespondTo, response.release());
        IntermediateResult->IsReplied = true;
        SendNotify(status);
        PassAway();
    }

    STATEFN(StateWait) {
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvBlobStorage::TEvGetResult, Handle);
        default:
            Y_FAIL();
        }
   }

    TKeyValueStorageReadRequest(THolder<TIntermediate> &&intermediate,
            const TTabletStorageInfo *tabletInfo)
        : IntermediateResult(std::move(intermediate))
        , TabletInfo(tabletInfo)
    {}
};


IActor* CreateKeyValueStorageReadRequest(THolder<TIntermediate>&& intermediate,
        const TTabletStorageInfo *tabletInfo)
{
    return new TKeyValueStorageReadRequest(std::move(intermediate), tabletInfo);
}

} // NKeyValue

} // NKikimr
