#pragma once

#include "blob.h"

#include <ydb/core/tx/ctor_logger.h>
#include <ydb/core/base/logoblob.h>
#include <ydb/core/base/events.h>
#include <ydb/core/base/blobstorage.h>

#include <library/cpp/monlib/dynamic_counters/counters.h>
#include <library/cpp/actors/core/actorid.h>
#include <library/cpp/actors/core/event_local.h>

#include <util/generic/vector.h>

namespace NKikimr::NBlobCache {

using NOlap::TUnifiedBlobId;
using NOlap::TBlobRange;

using TLogThis = TCtorLogger<NKikimrServices::BLOB_CACHE>;


struct TEvBlobCache {
    enum EEv {
        EvReadBlobRange = EventSpaceBegin(TKikimrEvents::ES_BLOB_CACHE),
        EvReadBlobRangeBatch,
        EvReadBlobRangeResult,
        EvCacheBlobRange,
        EvForgetBlob,

        EvEnd
    };

    static_assert(EvEnd < EventSpaceEnd(TKikimrEvents::ES_BLOB_CACHE), "Unexpected TEvBlobCache event range");

    struct TEvReadBlobRange : public NActors::TEventLocal<TEvReadBlobRange, EvReadBlobRange> {
        TBlobRange BlobRange;
        bool CacheAfterRead;
        bool Fallback;

        explicit TEvReadBlobRange(const TBlobRange& blobRange, bool cacheResult, bool fallback)
            : BlobRange(blobRange)
            , CacheAfterRead(cacheResult)
            , Fallback(fallback)
        {}
    };

    // Read a batch of ranges from the same DS group
    // This is usefull to save IOPs when reading multiple columns from the same blob
    struct TEvReadBlobRangeBatch : public NActors::TEventLocal<TEvReadBlobRangeBatch, EvReadBlobRangeBatch> {
        std::vector<TBlobRange> BlobRanges;
        bool CacheAfterRead;
        bool Fallback;

        explicit TEvReadBlobRangeBatch(std::vector<TBlobRange>&& blobRanges, bool cacheResult, bool fallback)
            : BlobRanges(std::move(blobRanges))
            , CacheAfterRead(cacheResult)
            , Fallback(fallback)
        {
            if (fallback) {
                for (const auto& blobRange : BlobRanges) {
                    Y_VERIFY(blobRange.BlobId == BlobRanges[0].BlobId);
                }
            }
        }
    };

    struct TEvReadBlobRangeResult : public NActors::TEventLocal<TEvReadBlobRangeResult, EvReadBlobRangeResult> {
        TBlobRange BlobRange;
        NKikimrProto::EReplyStatus Status;
        TString Data;

        TEvReadBlobRangeResult(const TBlobRange& blobRange, NKikimrProto::EReplyStatus status, const TString& data)
            : BlobRange(blobRange)
            , Status(status)
            , Data(data)
        {}
    };

    // Put a blob range data into cache. This helps to reduce number of reads from disks done by indexing, compactions
    // and queries that read recent data
    struct TEvCacheBlobRange : public NActors::TEventLocal<TEvCacheBlobRange, EvCacheBlobRange> {
        TBlobRange BlobRange;
        TString Data;

        TEvCacheBlobRange(const TBlobRange& blobRange, const TString& data)
            : BlobRange(blobRange)
            , Data(data)
        {}
    };

    // Notify the cache that this blob will not be requested any more
    // (e.g. when it was deleted after indexing or compaction)
    struct TEvForgetBlob : public NActors::TEventLocal<TEvForgetBlob, EvForgetBlob> {
        TUnifiedBlobId BlobId;

        explicit TEvForgetBlob(const TUnifiedBlobId& blobId)
            : BlobId(blobId)
        {}
    };
};

inline
NActors::TActorId MakeBlobCacheServiceId() {
    static_assert(TActorId::MaxServiceIDLength == 12, "Unexpected actor id length");
    const char x[12] = "blob_cache";
    return TActorId(0, TStringBuf(x, 12));
}

NActors::IActor* CreateBlobCache(ui64 maxBytes, TIntrusivePtr<::NMonitoring::TDynamicCounters>);

// Explicitly add and remove data from cache. This is usefull for newly written data that is likely to be read by
// indexing, compaction and user queries and for the data that has been compacted and will not be read again.
void AddRangeToCache(const TBlobRange& blobRange, const TString& data);
void ForgetBlob(const TUnifiedBlobId& blobId);

}
