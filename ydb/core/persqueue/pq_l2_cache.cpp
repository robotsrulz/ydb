#include "pq_l2_cache.h"
#include <ydb/core/mon/mon.h>

namespace NKikimr {
namespace NPQ {

IActor* CreateNodePersQueueL2Cache(const TCacheL2Parameters& params, TIntrusivePtr<::NMonitoring::TDynamicCounters> counters)
{
    return new TPersQueueCacheL2(params, counters);
}

void TPersQueueCacheL2::Bootstrap(const TActorContext& ctx)
{
    TAppData * appData = AppData(ctx);
    Y_VERIFY(appData);

    auto mon = appData->Mon;
    if (mon) {
        NMonitoring::TIndexMonPage * page = mon->RegisterIndexPage("actors", "Actors");
        mon->RegisterActorPage(page, "pql2", "PersQueue Node Cache", false, ctx.ExecutorThread.ActorSystem, ctx.SelfID);
    }

    Become(&TThis::StateFunc);
}

void TPersQueueCacheL2::Handle(TEvPqCache::TEvCacheL2Request::TPtr& ev, const TActorContext& ctx)
{
    THolder<TCacheL2Request> request(ev->Get()->Data.Release());
    TString topicName = request->TopicName;

    Y_VERIFY(topicName.size(), "PQ L2. Empty topic name in L2");

    TouchBlobs(ctx, topicName, request->RequestedBlobs);
    TouchBlobs(ctx, topicName, request->ExpectedBlobs, false);
    RemoveBlobs(ctx, topicName, request->RemovedBlobs);
    RegretBlobs(ctx, topicName, request->MissedBlobs);

    THashMap<TKey, TCacheValue::TPtr> evicted;
    AddBlobs(ctx, topicName, request->StoredBlobs, evicted);

    SendResponses(ctx, evicted);
}

void TPersQueueCacheL2::SendResponses(const TActorContext& ctx, const THashMap<TKey, TCacheValue::TPtr>& evictedBlobs)
{
    TInstant now = TAppData::TimeProvider->Now();
    THashMap<TActorId, THolder<TCacheL2Response>> responses;

    for (auto rm : evictedBlobs) {
        const TKey& key = rm.first;
        TCacheValue::TPtr evicted = rm.second;

        THolder<TCacheL2Response>& resp = responses[evicted->GetOwner()];
        if (!resp) {
            resp = MakeHolder<TCacheL2Response>();
            resp->TopicName = key.TopicName;
        }

        Y_VERIFY(key.TopicName == resp->TopicName, "PQ L2. Multiple topics in one PQ tablet.");
        resp->Removed.push_back({key.Partition, key.Offset, key.PartNo, evicted});

        RetentionTime = now - evicted->GetAccessTime();
        if (RetentionTime < KeepTime)
            resp->Overload = true;
    }

    for (auto& resp : responses)
        ctx.Send(resp.first, new TEvPqCache::TEvCacheL2Response(resp.second.Release()));

    { // counters
        (*Counters.Retention) = RetentionTime.Seconds();
    }
}

/// @return outRemoved - map of evicted items. L1 should be noticed about them
void TPersQueueCacheL2::AddBlobs(const TActorContext& ctx, TString topic, const TVector<TCacheBlobL2>& blobs,
                                 THashMap<TKey, TCacheValue::TPtr>& outEvicted)
{
    ui32 numUnused = 0;
    for (const TCacheBlobL2& blob : blobs) {
        Y_VERIFY(blob.Value->DataSize(), "Trying to place empty blob into L2 cache");

        TKey key(topic, blob);
        // PQ tablet could send some data twice (if it's restored after die)
        if (Cache.FindWithoutPromote(key) != Cache.End()) {
            LOG_WARN_S(ctx, NKikimrServices::PERSQUEUE, "PQ Cache (L2). Same blob insertion. Topic '" << topic
                << "' partition " << key.Partition << " offset " << key.Offset << " size " << blob.Value->DataSize());
            continue;
        }

        Y_VERIFY(CurrentSize <= Cache.Size() * MAX_BLOB_SIZE);

        CurrentSize += blob.Value->DataSize();

        // manualy manage LRU size
        while (CurrentSize > MaxSize) {
            auto oldest = Cache.FindOldest();
            Y_VERIFY(oldest != Cache.End(), "Topic '%s' count %" PRIu64 " size %" PRIu64
                " maxSize %" PRIu64 " blobSize %" PRIu64 " blobs %" PRIu64 " evicted %" PRIu64,
                topic.data(), Cache.Size(), CurrentSize, MaxSize, blob.Value->DataSize(), blobs.size(), outEvicted.size());

            TCacheValue::TPtr value = oldest.Value();
            outEvicted.insert({oldest.Key(), value});
            if (value->GetAccessCount() == 0)
                ++numUnused;

            LOG_DEBUG_S(ctx, NKikimrServices::PERSQUEUE, "PQ Cache (L2). Evicting blob. Topic '" << topic
                << "' partition " << oldest.Key().Partition << " offset " << oldest.Key().Offset << " size " << value->DataSize());

            CurrentSize -= value->DataSize();
            Cache.Erase(oldest);
        }

        LOG_DEBUG_S(ctx, NKikimrServices::PERSQUEUE, "PQ Cache (L2). Adding blob. Topic '" << topic
            << "' partition " << blob.Partition << " offset " << blob.Offset << " size " << blob.Value->DataSize());

        Cache.Insert(key, blob.Value);
    }

    { // counters
        (*Counters.TotalSize) = CurrentSize;
        (*Counters.TotalCount) = Cache.Size();
        (*Counters.Evictions) += outEvicted.size();
        (*Counters.Unused) += numUnused;
        (*Counters.Used) += outEvicted.size() - numUnused;
    }
}

void TPersQueueCacheL2::RemoveBlobs(const TActorContext& ctx, TString topic, const TVector<TCacheBlobL2>& blobs)
{
    ui32 numEvicted = 0;
    ui32 numUnused = 0;
    for (const TCacheBlobL2& blob : blobs) {
        TKey key(topic, blob);
        auto it = Cache.FindWithoutPromote(key);
        if (it != Cache.End()) {
            CurrentSize -= (*it)->DataSize();
            numEvicted++;
            if ((*it)->GetAccessCount() == 0)
                ++numUnused;
            Cache.Erase(it);
            LOG_DEBUG_S(ctx, NKikimrServices::PERSQUEUE, "PQ Cache (L2). Removed. Topic '" << topic
                << "' partition " << blob.Partition << " offset " << blob.Offset);
        } else {
            LOG_DEBUG_S(ctx, NKikimrServices::PERSQUEUE, "PQ Cache (L2). Miss in remove. Topic '" << topic
                << "' partition " << blob.Partition << " offset " << blob.Offset);
        }
    }

    { // counters
        (*Counters.TotalSize) = CurrentSize;
        (*Counters.TotalCount) = Cache.Size();
        (*Counters.Evictions) += numEvicted;
        (*Counters.Unused) += numUnused;
        (*Counters.Used) += numEvicted - numUnused;
    }
}

void TPersQueueCacheL2::TouchBlobs(const TActorContext& ctx, TString topic, const TVector<TCacheBlobL2>& blobs, bool isHit)
{
    TInstant now = TAppData::TimeProvider->Now();

    for (const TCacheBlobL2& blob : blobs) {
        TKey key(topic, blob);
        auto it = Cache.Find(key);
        if (it != Cache.End()) {
            (*it)->Touch(now);
            LOG_DEBUG_S(ctx, NKikimrServices::PERSQUEUE, "PQ Cache (L2). Touched. Topic '" << topic
                << "' partition " << blob.Partition << " offset " << blob.Offset);
        } else {
            LOG_DEBUG_S(ctx, NKikimrServices::PERSQUEUE, "PQ Cache (L2). Miss in touch. Topic '" << topic
                << "' partition " << blob.Partition << " offset " << blob.Offset);
        }
    }

    { // counters
        (*Counters.Touches) += blobs.size();
        if (isHit)
            (*Counters.Hits) += blobs.size();

        auto oldest = Cache.FindOldest();
        if (oldest != Cache.End())
            RetentionTime = now - oldest.Value()->GetAccessTime();
    }
}

void TPersQueueCacheL2::RegretBlobs(const TActorContext& ctx, TString topic, const TVector<TCacheBlobL2>& blobs)
{
    for (const TCacheBlobL2& blob : blobs) {
        LOG_DEBUG_S(ctx, NKikimrServices::PERSQUEUE, "PQ Cache (L2). Missed blob. Topic '" << topic
            << "' partition " << blob.Partition << " offset " << blob.Offset);
    }

    { // counters
        (*Counters.Misses) += blobs.size();
    }
}

void TPersQueueCacheL2::Handle(NMon::TEvHttpInfo::TPtr& ev, const TActorContext& ctx)
{
    const auto& params = ev->Get()->Request.GetParams();
    if (params.Has("submit")) {
        TString strParam = params.Get("newCacheLimit");
        if (strParam.size()) {
            ui32 valueMb = atoll(strParam.data());
            MaxSize = ClampMinSize(valueMb * 1_MB); // will be applyed at next AddBlobs
        }
    }

    TString html = HttpForm();
    ctx.Send(ev->Sender, new NMon::TEvHttpInfoRes(html));
}

TString TPersQueueCacheL2::HttpForm() const
{
    TStringStream str;
    HTML(str) {
        FORM_CLASS("form-horizontal") {
            DIV_CLASS("row") {
                PRE() {
                        str << "CacheLimit (MB): " << (MaxSize >> 20) << Endl;
                        str << "CacheSize (MB): " << (CurrentSize >> 20) << Endl;
                        str << "Count of blobs: " << Cache.Size() << Endl;
                        str << "Min RetentionTime: " << KeepTime << Endl;
                        str << "RetentionTime: " << RetentionTime << Endl;
                }
            }
            DIV_CLASS("control-group") {
                LABEL_CLASS_FOR("control-label", "inputTo") {str << "New Chache Limit";}
                DIV_CLASS("controls") {
                    str << "<input type=\"number\" id=\"inputTo\" placeholder=\"CacheLimit (MB)\" name=\"newCacheLimit\">";
                }
            }
            DIV_CLASS("control-group") {
                DIV_CLASS("controls") {
                    str << "<button type=\"submit\" name=\"submit\" class=\"btn btn-primary\">Change</button>";
                }
            }
        }
    }
    return str.Str();
}

} // NPQ
} // NKikimr
