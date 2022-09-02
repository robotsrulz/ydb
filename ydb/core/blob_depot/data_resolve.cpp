#include "data.h"
#include "schema.h"

namespace NKikimr::NBlobDepot {

    using TData = TBlobDepot::TData;

    class TData::TTxResolve : public NTabletFlatExecutor::TTransactionBase<TBlobDepot> {
        std::unique_ptr<TEvBlobDepot::TEvResolve::THandle> Request;
        int ItemIndex = 0;
        std::optional<TKey> LastScannedKey;
        ui32 NumKeysRead = 0; // number of keys already read for this item

        // final state
        std::deque<std::unique_ptr<TEvBlobDepot::TEvResolveResult>> Outbox;
        std::unique_ptr<TTxResolve> SuccessorTx;

    public:
        TTxResolve(TBlobDepot *self, TEvBlobDepot::TEvResolve::TPtr request)
            : TTransactionBase(self)
            , Request(request.Release())
        {}

        TTxResolve(TTxResolve& predecessor)
            : TTransactionBase(predecessor.Self)
            , Request(std::move(predecessor.Request))
            , ItemIndex(predecessor.ItemIndex)
            , LastScannedKey(std::move(predecessor.LastScannedKey))
            , NumKeysRead(predecessor.NumKeysRead)
        {}

        bool Execute(TTransactionContext& txc, const TActorContext&) override {
            NIceDb::TNiceDb db(txc.DB);

            STLOG(PRI_DEBUG, BLOB_DEPOT, BDT22, "TTxResolve::Execute", (Id, Self->GetLogId()),
                (Sender, Request->Sender), (Cookie, Request->Cookie), (ItemIndex, ItemIndex),
                (LastScannedKey, LastScannedKey));

            if (Self->Data->Loaded) {
                GenerateResponse();
                return true;
            }

            bool progress = false; // have we made some progress during scan?
            const auto& record = Request->Get()->Record;
            const auto& items = record.GetItems();
            for (; ItemIndex < items.size(); ++ItemIndex, LastScannedKey.reset(), NumKeysRead = 0) {
                const auto& item = items[ItemIndex];

                std::optional<TKey> begin = item.HasBeginningKey()
                    ? std::make_optional(TKey::FromBinaryKey(item.GetBeginningKey(), Self->Config))
                    : std::nullopt;

                std::optional<TKey> end = item.HasEndingKey()
                    ? std::make_optional(TKey::FromBinaryKey(item.GetEndingKey(), Self->Config))
                    : std::nullopt;

                TScanFlags flags;
                if (item.GetIncludeBeginning()) {
                    flags |= EScanFlags::INCLUDE_BEGIN;
                }
                if (item.GetIncludeEnding()) {
                    flags |= EScanFlags::INCLUDE_END;
                }
                if (item.GetReverse()) {
                    flags |= EScanFlags::REVERSE;
                }

                // adjust range according to actually generated data
                if (LastScannedKey) {
                    if (flags & EScanFlags::REVERSE) { // reverse scan
                        end = *LastScannedKey;
                        flags &= ~EScanFlags::INCLUDE_END;
                    } else { // direct scan
                        begin = *LastScannedKey;
                        flags &= ~EScanFlags::INCLUDE_BEGIN;
                    }
                }

                if (end && Self->Data->LastLoadedKey && *end <= Self->Data->LastLoadedKey) {
                    // we have everything we need contained in memory, skip this item
                    continue;
                } else if (Self->Data->LastLoadedKey && (!begin || *begin <= Self->Data->LastLoadedKey)) {
                    // we can scan only some part from memory -- do it
                    auto callback = [&](const TKey& key, const TValue&) {
                        LastScannedKey = key;
                        return ++NumKeysRead != item.GetMaxKeys();
                    };
                    Self->Data->ScanRange(begin ? &begin.value() : nullptr, &Self->Data->LastLoadedKey.value(),
                        flags | EScanFlags::INCLUDE_END, callback);

                    // adjust range beginning
                    begin = Self->Data->LastLoadedKey;
                    flags &= ~EScanFlags::INCLUDE_BEGIN;

                    // check if we have read all the keys requested
                    if (NumKeysRead == item.GetMaxKeys()) {
                        continue;
                    }
                }

                auto processRange = [&](auto&& table) {
                    for (auto rowset = table.Select();; rowset.Next()) {
                        if (!rowset.IsReady()) {
                            return false;
                        } else if (!rowset.IsValid()) {
                            // no more keys in our direction
                            return true;
                        }
                        auto key = TKey::FromBinaryKey(rowset.template GetValue<Schema::Data::Key>(), Self->Config);
                        if (key != LastScannedKey) {
                            LastScannedKey = key;
                            progress = true;
                            Self->Data->AddDataOnLoad(key, rowset.template GetValue<Schema::Data::Value>(), txc, this);

                            const bool matchBegin = !begin || (flags & EScanFlags::INCLUDE_BEGIN ? *begin <= key : *begin < key);
                            const bool matchEnd = !end || (flags & EScanFlags::INCLUDE_END ? key <= *end : key < *end);
                            if (matchBegin && matchEnd && ++NumKeysRead == item.GetMaxKeys()) {
                                // we have hit the MaxItems limit, exit
                                return true;
                            } else if (flags & EScanFlags::REVERSE ? !matchBegin : !matchEnd) {
                                // we have exceeded the opposite boundary, exit
                                return true;
                            }
                        }
                    }
                };

                auto applyEnd = [&](auto&& x) {
                    return end
                        ? processRange(x.LessOrEqual(end->MakeBinaryKey()))
                        : processRange(std::forward<std::decay_t<decltype(x)>>(x));
                };
                auto applyBegin = [&](auto&& x) {
                    return begin
                        ? applyEnd(x.GreaterOrEqual(begin->MakeBinaryKey()))
                        : applyEnd(std::forward<std::decay_t<decltype(x)>>(x));
                };
                auto applyReverse = [&](auto&& x) {
                    return item.GetReverse()
                        ? applyBegin(x.Reverse())
                        : applyBegin(std::forward<std::decay_t<decltype(x)>>(x));
                };
                if (applyReverse(db.Table<Schema::Data>())) {
                    continue; // all work done for this item
                } else if (progress) {
                    // we have already done something, so let's finish this transaction and start a new one, continuing
                    // the job
                    SuccessorTx = std::make_unique<TTxResolve>(*this);
                    return true;
                } else {
                    return false; // we'll have to restart this transaction to fetch some data
                }
            }

            GenerateResponse();
            return true;
        }

        void Complete(const TActorContext&) override {
            STLOG(PRI_DEBUG, BLOB_DEPOT, BDT30, "TTxResolve::Complete", (Id, Self->GetLogId()),
                (Sender, Request->Sender), (Cookie, Request->Cookie), (SuccessorTx, bool(SuccessorTx)),
                (Outbox.size, Outbox.size()));

            Self->Data->CommitTrash(this);

            if (SuccessorTx) {
                Self->Execute(std::move(SuccessorTx));
            } else {
                if (Outbox.empty()) {
                    Outbox.push_back(std::make_unique<TEvBlobDepot::TEvResolveResult>(NKikimrProto::OK, std::nullopt));
                }
                for (auto& ev : Outbox) {
                    auto handle = std::make_unique<IEventHandle>(Request->Sender, Self->SelfId(), ev.release(), 0, Request->Cookie);
                    if (Request->InterconnectSession) {
                        handle->Rewrite(TEvInterconnect::EvForward, Request->InterconnectSession);
                    }
                    TActivationContext::Send(handle.release());
                }
            }
        }

        void GenerateResponse() {
            size_t lastResponseSize;

            for (const auto& item : Request->Get()->Record.GetItems()) {
                std::optional<ui64> cookie = item.HasCookie() ? std::make_optional(item.GetCookie()) : std::nullopt;

                std::optional<TKey> begin = item.HasBeginningKey()
                    ? std::make_optional(TKey::FromBinaryKey(item.GetBeginningKey(), Self->Config))
                    : std::nullopt;

                std::optional<TKey> end = item.HasEndingKey()
                    ? std::make_optional(TKey::FromBinaryKey(item.GetEndingKey(), Self->Config))
                    : std::nullopt;

                TScanFlags flags;
                if (item.GetIncludeBeginning()) {
                    flags |= EScanFlags::INCLUDE_BEGIN;
                }
                if (item.GetIncludeEnding()) {
                    flags |= EScanFlags::INCLUDE_END;
                }
                if (item.GetReverse()) {
                    flags |= EScanFlags::REVERSE;
                }

                ui64 count = item.GetMaxKeys();

                auto callback = [&](const TKey& key, const TValue& value) {
                    IssueResponseItem(cookie, key, value, lastResponseSize);
                    return --count != 0;
                };

                Self->Data->ScanRange(begin ? &begin.value() : nullptr, end ? &end.value() : nullptr, flags, callback);
            }
        }

        void IssueResponseItem(std::optional<ui64> cookie, const TKey& key, const TValue& value, size_t& lastResponseSize) {
            NKikimrBlobDepot::TEvResolveResult::TResolvedKey item;

            if (cookie) {
                item.SetCookie(*cookie);
            }
            item.SetKey(key.MakeBinaryKey());
            EnumerateBlobsForValueChain(value.ValueChain, Self->TabletID(), [&](const TLogoBlobID& id, ui32 begin, ui32 end) {
                if (begin != end) {
                    auto *out = item.AddValueChain();
                    out->SetGroupId(Self->Info()->GroupFor(id.Channel(), id.Generation()));
                    LogoBlobIDFromLogoBlobID(id, out->MutableBlobId());
                    if (begin) {
                        out->SetSubrangeBegin(begin);
                    }
                    if (end != id.BlobSize()) {
                        out->SetSubrangeEnd(end);
                    }
                }
            });
            if (value.OriginalBlobId) {
                auto *out = item.AddValueChain();
                out->SetGroupId(Self->Config.GetDecommitGroupId());
                LogoBlobIDFromLogoBlobID(*value.OriginalBlobId, out->MutableBlobId());
            }
            if (value.Meta) {
                item.SetMeta(value.Meta.data(), value.Meta.size());
            }

            if (!item.ValueChainSize()) {
                STLOG(PRI_WARN, BLOB_DEPOT, BDT48, "empty ValueChain on Resolve", (Id, Self->GetLogId()),
                    (Key, key), (Value, value), (Item, item), (Sender, Request->Sender), (Cookie, Request->Cookie));
            }

            size_t itemSize = item.ByteSizeLong();
            if (Outbox.empty() || lastResponseSize + itemSize > EventMaxByteSize) {
                if (!Outbox.empty()) {
                    auto& lastEvent = Outbox.back();
                    lastEvent->Record.SetStatus(NKikimrProto::OVERRUN);
                }
                auto ev = std::make_unique<TEvBlobDepot::TEvResolveResult>(NKikimrProto::OK, std::nullopt);
                lastResponseSize = ev->CalculateSerializedSize();
                Outbox.push_back(std::move(ev));
            }

            auto& lastEvent = Outbox.back();
            item.Swap(lastEvent->Record.AddResolvedKeys());
            lastResponseSize += itemSize;
        }
    };

    void TData::Handle(TEvBlobDepot::TEvResolve::TPtr ev) {
        STLOG(PRI_DEBUG, BLOB_DEPOT, BDT21, "TEvResolve", (Id, Self->GetLogId()), (Msg, ev->Get()->ToString()),
            (Sender, ev->Sender), (Cookie, ev->Cookie), (LastAssimilatedBlobId, LastAssimilatedBlobId));

        if (Self->Config.HasDecommitGroupId() && Self->DecommitState <= EDecommitState::BlobsFinished) {
            std::vector<std::tuple<ui64, bool, TLogoBlobID, TLogoBlobID>> queries;

            for (const auto& item : ev->Get()->Record.GetItems()) {
                if (!item.HasTabletId()) {
                   STLOG(PRI_CRIT, BLOB_DEPOT, BDT42, "incorrect request", (Id, Self->GetLogId()), (Item, item));
                   auto [response, record] = TEvBlobDepot::MakeResponseFor(*ev, Self->SelfId(), NKikimrProto::ERROR,
                        "incorrect request");
                   TActivationContext::Send(response.release());
                   return;
                }

                const ui64 tabletId = item.GetTabletId();
                if (LastAssimilatedBlobId && tabletId < LastAssimilatedBlobId->TabletID()) {
                    continue; // fast path
                }

                TLogoBlobID minId(tabletId, 0, 0, 0, 0, 0);
                TLogoBlobID maxId(tabletId, Max<ui32>(), Max<ui32>(), TLogoBlobID::MaxChannel, TLogoBlobID::MaxBlobSize,
                    TLogoBlobID::MaxCookie, TLogoBlobID::MaxPartId, TLogoBlobID::MaxCrcMode);

                if (item.HasBeginningKey()) {
                    minId = TKey::FromBinaryKey(item.GetBeginningKey(), Self->Config).GetBlobId();
                }
                if (item.HasEndingKey()) {
                    maxId = TKey::FromBinaryKey(item.GetEndingKey(), Self->Config).GetBlobId();
                }

                Y_VERIFY_DEBUG(minId.TabletID() == tabletId);
                Y_VERIFY_DEBUG(maxId.TabletID() == tabletId);

                if (!LastAssimilatedBlobId || *LastAssimilatedBlobId < maxId) {
                    if (LastAssimilatedBlobId && minId < *LastAssimilatedBlobId) {
                        minId = *LastAssimilatedBlobId;
                    }
                    if (minId == maxId) {
                        const auto it = Data.find(TKey(minId));
                        if (it != Data.end() && !it->second.ValueChain.empty() || it->second.OriginalBlobId) {
                            continue; // fast path for extreme queries
                        }
                    }
                    queries.emplace_back(tabletId, item.GetMustRestoreFirst(), minId, maxId);
                }
            }

            if (!queries.empty()) {
                const ui64 id = ++LastRangeId;
                for (const auto& [tabletId, mustRestoreFirst, minId, maxId] : queries) {
                    auto ev = std::make_unique<TEvBlobStorage::TEvRange>(tabletId, minId, maxId, mustRestoreFirst,
                        TInstant::Max(), true);
                    ev->Decommission = true;

                    const auto& tabletId_ = tabletId;
                    const auto& minId_ = minId;
                    const auto& maxId_ = maxId;
                    const auto& mustRestoreFirst_ = mustRestoreFirst;
                    STLOG(PRI_DEBUG, BLOB_DEPOT, BDT46, "going to TEvRange", (Id, Self->GetLogId()), (TabletId, tabletId_),
                        (MinId, minId_), (MaxId, maxId_), (MustRestoreFirst, mustRestoreFirst_));
                    SendToBSProxy(Self->SelfId(), Self->Config.GetDecommitGroupId(), ev.release(), id);
                }
                ResolveDecommitContexts[id] = {ev, (ui32)queries.size()};
                return;
            }
        }

        Self->Execute(std::make_unique<TTxResolve>(Self, ev));
    }

    void TData::Handle(TEvBlobStorage::TEvRangeResult::TPtr ev) {
        class TTxCommitRange : public NTabletFlatExecutor::TTransactionBase<TBlobDepot> {
            TEvBlobStorage::TEvRangeResult::TPtr Ev;

        public:
            TTxCommitRange(TBlobDepot *self, TEvBlobStorage::TEvRangeResult::TPtr ev)
                : TTransactionBase(self)
                , Ev(ev)
            {}

            bool Execute(TTransactionContext& txc, const TActorContext&) override {
                if (Ev->Get()->Status == NKikimrProto::OK) {
                    for (const auto& response : Ev->Get()->Responses) {
                        Self->Data->AddDataOnDecommit({
                            .Id = response.Id,
                            .Keep = response.Keep,
                            .DoNotKeep = response.DoNotKeep
                        }, txc, this);
                    }
                }
                return true;
            }

            void Complete(const TActorContext&) override {
                Self->Data->CommitTrash(this);

                auto& contexts = Self->Data->ResolveDecommitContexts;
                if (const auto it = contexts.find(Ev->Cookie); it != contexts.end()) {
                    TResolveDecommitContext& context = it->second;
                    if (Ev->Get()->Status != NKikimrProto::OK) {
                        context.Errors = true;
                    }
                    if (!--context.NumRangesInFlight) {
                        if (context.Errors) {
                           auto [response, record] = TEvBlobDepot::MakeResponseFor(*context.Ev, Self->SelfId(),
                                NKikimrProto::ERROR, "errors in range queries");
                           TActivationContext::Send(response.release());
                        } else {
                            Self->Execute(std::make_unique<TTxResolve>(Self, context.Ev));
                        }
                    }
                    contexts.erase(it);
                }
            }
        };
        STLOG(PRI_DEBUG, BLOB_DEPOT, BDT50, "TEvRangeResult", (Id, Self->GetLogId()), (Msg, *ev->Get()));
        Self->Execute(std::make_unique<TTxCommitRange>(Self, ev));
    }

} // NKikimr::NBlobDepot
