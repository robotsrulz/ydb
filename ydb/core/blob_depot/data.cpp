#include "data.h"
#include "garbage_collection.h"

namespace NKikimr::NBlobDepot {

    using TData = TBlobDepot::TData;

    enum class EUpdateOutcome {
        CHANGE,
        NO_CHANGE,
        DROP
    };

    template<typename T, typename... TArgs>
    bool TData::UpdateKey(TKey key, NTabletFlatExecutor::TTransactionContext& txc, void *cookie, T&& callback, TArgs&&... args) {
        bool underSoft = false, underHard = false;
        auto var = key.AsVariant();
        if (auto *id = std::get_if<TLogoBlobID>(&var)) {
            Self->BarrierServer->GetBlobBarrierRelation(*id, &underSoft, &underHard);
        }
        if (underHard || underSoft) {
            if (const auto it = Data.find(key); it == Data.end()) {
                return false; // no such key existed and will not be created as it hits the barrier
            } else {
                Y_VERIFY_S(!underHard && it->second.KeepState == NKikimrBlobDepot::EKeepState::Keep,
                    "barrier invariant failed Key# " << key.ToString() << " Value# " << it->second.ToString());
            }
        }

        const auto [it, inserted] = Data.try_emplace(std::move(key), std::forward<TArgs>(args)...);
        {
            auto& [key, value] = *it;
            Y_VERIFY(!underHard);
            Y_VERIFY(!underSoft || !inserted);

            std::vector<TLogoBlobID> deleteQ;

            if (!inserted) {
                EnumerateBlobsForValueChain(value.ValueChain, Self->TabletID(), [&](TLogoBlobID id, ui32, ui32) {
                    const auto it = RefCount.find(id);
                    Y_VERIFY(it != RefCount.end());
                    if (!--it->second) {
                        deleteQ.push_back(id);
                    }
                });
            }

            EUpdateOutcome outcome = callback(value, inserted);

            Y_VERIFY(!inserted || outcome != EUpdateOutcome::NO_CHANGE);
            if (underSoft && value.KeepState != NKikimrBlobDepot::EKeepState::Keep) {
                outcome = EUpdateOutcome::DROP;
            }

            EnumerateBlobsForValueChain(value.ValueChain, Self->TabletID(), [&](TLogoBlobID id, ui32, ui32) {
                const auto [it, inserted] = RefCount.try_emplace(id, 1);
                if (inserted) {
                    // first mention of this id
                    auto& record = GetRecordsPerChannelGroup(id);
                    const auto [_, inserted] = record.Used.insert(id);
                    Y_VERIFY(inserted);
                    AccountBlob(id, 1);

                    // blob is first mentioned and deleted as well
                    if (outcome == EUpdateOutcome::DROP) {
                        it->second = 0;
                        deleteQ.push_back(id);
                    }
                } else if (outcome != EUpdateOutcome::DROP) {
                    ++it->second;
                }
            });

            for (const TLogoBlobID& id : deleteQ) {
                const auto it = RefCount.find(id);
                Y_VERIFY(it != RefCount.end());
                if (!it->second) {
                    InFlightTrash.emplace(cookie, id);
                    NIceDb::TNiceDb(txc.DB).Table<Schema::Trash>().Key(id.AsBinaryString()).Update();
                    RefCount.erase(it);
                }
            }

            auto row = NIceDb::TNiceDb(txc.DB).Table<Schema::Data>().Key(key.MakeBinaryKey());
            switch (outcome) {
                case EUpdateOutcome::DROP:
                    Data.erase(it);
                    row.Delete();
                    return true;

                case EUpdateOutcome::CHANGE:
                    row.template Update<Schema::Data::Value>(value.SerializeToString());
                    return true;

                case EUpdateOutcome::NO_CHANGE:
                    return false;
            }
        }
    }

    const TData::TValue *TData::FindKey(const TKey& key) const {
        const auto it = Data.find(key);
        return it != Data.end() ? &it->second : nullptr;
    }

    void TData::UpdateKey(const TKey& key, const NKikimrBlobDepot::TEvCommitBlobSeq::TItem& item,
            NTabletFlatExecutor::TTransactionContext& txc, void *cookie) {
        STLOG(PRI_DEBUG, BLOB_DEPOT, BDT10, "UpdateKey", (Id, Self->GetLogId()), (Key, key), (Item, item));
        UpdateKey(key, txc, cookie, [&](TValue& value, bool inserted) {
            if (!inserted) { // update value items
                value.Meta = item.GetMeta();
                value.Public = false;
                value.Unconfirmed = item.GetUnconfirmed();

                // update it to keep new blob locator
                value.ValueChain.Clear();
                auto *chain = value.ValueChain.Add();
                auto *locator = chain->MutableLocator();
                locator->CopyFrom(item.GetBlobLocator());

                // reset original blob id, if any
                value.OriginalBlobId.reset();
            }

            return EUpdateOutcome::CHANGE;
        }, item);
    }

    TData::TRecordsPerChannelGroup& TData::GetRecordsPerChannelGroup(TLogoBlobID id) {
        TTabletStorageInfo *info = Self->Info();
        const ui32 groupId = info->GroupFor(id.Channel(), id.Generation());
        Y_VERIFY(groupId != Max<ui32>());
        const auto& key = std::make_tuple(id.TabletID(), id.Channel(), groupId);
        const auto [it, _] = RecordsPerChannelGroup.emplace(std::piecewise_construct, key, key);
        return it->second;
    }

    void TData::AddDataOnLoad(TKey key, TString value, NTabletFlatExecutor::TTransactionContext& txc, void *cookie) {
        NKikimrBlobDepot::TValue proto;
        const bool success = proto.ParseFromString(value);
        Y_VERIFY(success);

        UpdateKey(std::move(key), txc, cookie, [&](TValue& value, bool inserted) {
            if (!inserted) { // do some merge logic
                value.KeepState = Max(value.KeepState, proto.GetKeepState());
                if (value.ValueChain.empty() && proto.ValueChainSize()) {
                    value.ValueChain.CopyFrom(proto.GetValueChain());
                    value.OriginalBlobId.reset();
                }
            }

            return EUpdateOutcome::CHANGE;
        }, std::move(proto));
    }

    void TData::AddDataOnDecommit(const TEvBlobStorage::TEvAssimilateResult::TBlob& blob,
            NTabletFlatExecutor::TTransactionContext& txc, void *cookie) {
        UpdateKey(TKey(blob.Id), txc, cookie, [&](TValue& value, bool inserted) {
            STLOG(PRI_DEBUG, BLOB_DEPOT, BDT49, "AddDataOnDecommit", (Id, Self->GetLogId()), (Blob, blob),
                (Value, value), (Inserted, inserted));

            // update keep state if necessary
            if (blob.DoNotKeep && value.KeepState < NKikimrBlobDepot::EKeepState::DoNotKeep) {
                value.KeepState = NKikimrBlobDepot::EKeepState::DoNotKeep;
            } else if (blob.Keep && value.KeepState < NKikimrBlobDepot::EKeepState::Keep) {
                value.KeepState = NKikimrBlobDepot::EKeepState::Keep;
            }

            // if there is not value chain for this blob, map it to the original blob id
            if (value.ValueChain.empty()) {
                value.OriginalBlobId = blob.Id;
            }

            return EUpdateOutcome::CHANGE;
        });
    }

    void TData::AddTrashOnLoad(TLogoBlobID id) {
        auto& record = GetRecordsPerChannelGroup(id);
        record.Trash.insert(id);
        record.EnqueueForCollectionIfPossible(this);
        AccountBlob(id, true);
    }

    void TData::AddGenStepOnLoad(ui8 channel, ui32 groupId, TGenStep issuedGenStep, TGenStep confirmedGenStep) {
        const auto& key = std::make_tuple(Self->TabletID(), channel, groupId);
        const auto [it, _] = RecordsPerChannelGroup.emplace(std::piecewise_construct, key, key);
        auto& record = it->second;
        record.IssuedGenStep = issuedGenStep;
        record.LastConfirmedGenStep = confirmedGenStep;
    }

    bool TData::UpdateKeepState(TKey key, NKikimrBlobDepot::EKeepState keepState,
            NTabletFlatExecutor::TTransactionContext& txc, void *cookie) {
        return UpdateKey(std::move(key), txc, cookie, [&](TValue& value, bool inserted) {
             STLOG(PRI_DEBUG, BLOB_DEPOT, BDT51, "UpdateKeepState", (Id, Self->GetLogId()), (Key, key),
                (KeepState, keepState), (Value, value));
             if (inserted) {
                return EUpdateOutcome::CHANGE;
             } else if (value.KeepState < keepState) {
                value.KeepState = keepState;
                return EUpdateOutcome::CHANGE;
             } else {
                return EUpdateOutcome::NO_CHANGE;
             }
        }, keepState);
    }

    void TData::DeleteKey(const TKey& key, NTabletFlatExecutor::TTransactionContext& txc, void *cookie) {
        STLOG(PRI_DEBUG, BLOB_DEPOT, BDT14, "DeleteKey", (Id, Self->GetLogId()), (Key, key));
        UpdateKey(key, txc, cookie, [&](TValue&, bool inserted) {
            Y_VERIFY(!inserted);
            return EUpdateOutcome::DROP;
        });
    }

    void TData::CommitTrash(void *cookie) {
        auto [first, last] = InFlightTrash.equal_range(cookie);
        for (auto it = first; it != last; ++it) {
            auto& record = GetRecordsPerChannelGroup(it->second);
            record.MoveToTrash(this, it->second);
        }
        InFlightTrash.erase(first, last);
    }

    void TData::HandleTrash() {
        const ui32 generation = Self->Executor()->Generation();
        THashMap<ui32, std::unique_ptr<TEvBlobDepot::TEvPushNotify>> outbox;

        while (RecordsWithTrash) {
            TRecordsPerChannelGroup& record = *RecordsWithTrash.PopFront();

            Y_VERIFY(!record.CollectGarbageRequestInFlight);
            Y_VERIFY(record.TabletId == Self->TabletID());
            Y_VERIFY(!record.Trash.empty());

            Y_VERIFY(record.Channel < Self->Channels.size());
            auto& channel = Self->Channels[record.Channel];

            TGenStep nextGenStep(*--record.Trash.end());

            // step we are going to invalidate (including blobs with this one)
            if (TGenStep(record.LeastExpectedBlobId) <= nextGenStep) {
                const ui32 invalidatedStep = nextGenStep.Step(); // the step we want to invalidate and garbage collect

                // remove invalidated step from allocations
                auto blobSeqId = TBlobSeqId::FromSequentalNumber(record.Channel, generation, channel.NextBlobSeqId);
                Y_VERIFY(record.LastConfirmedGenStep < TGenStep(blobSeqId));
                if (blobSeqId.Step <= invalidatedStep) {
                    blobSeqId.Step = invalidatedStep + 1;
                    blobSeqId.Index = 0;
                    channel.NextBlobSeqId = blobSeqId.ToSequentialNumber();
                }

                // issue notifications to agents
                for (auto& [agentId, agent] : Self->Agents) {
                    if (!agent.AgentId) {
                        continue;
                    }
                    const auto [it, inserted] = agent.InvalidatedStepInFlight.emplace(record.Channel, invalidatedStep);
                    if (inserted || it->second < invalidatedStep) {
                        it->second = invalidatedStep;

                        auto& ev = outbox[agentId];
                        if (!ev) {
                            ev.reset(new TEvBlobDepot::TEvPushNotify);
                        }
                        auto *item = ev->Record.AddInvalidatedSteps();
                        item->SetChannel(record.Channel);
                        item->SetGeneration(generation);
                        item->SetInvalidatedStep(invalidatedStep);
                    }
                }
                
                // adjust the barrier to keep it safe now
                const TLogoBlobID maxId(record.TabletId, record.LeastExpectedBlobId.Generation,
                    record.LeastExpectedBlobId.Step, record.Channel, 0, 0);
                const auto it = record.Trash.lower_bound(maxId);
                if (it != record.Trash.begin()) {
                    nextGenStep = TGenStep(*std::prev(it));
                } else {
                    nextGenStep = {};
                }
            }

            auto keep = std::make_unique<TVector<TLogoBlobID>>();
            auto doNotKeep = std::make_unique<TVector<TLogoBlobID>>();

            for (auto it = record.Trash.begin(); it != record.Trash.end() && TGenStep(*it) <= record.LastConfirmedGenStep; ++it) {
                doNotKeep->push_back(*it);
            }

            const TLogoBlobID keepFrom(record.TabletId, record.LastConfirmedGenStep.Generation(),
                record.LastConfirmedGenStep.Step(), record.Channel, 0, 0);
            for (auto it = record.Used.upper_bound(keepFrom); it != record.Used.end() && TGenStep(*it) <= nextGenStep; ++it) {
                keep->push_back(*it);
            }

            if (keep->empty()) {
                keep.reset();
            }
            if (doNotKeep->empty()) {
                doNotKeep.reset();
            }
            const bool collect = nextGenStep > record.LastConfirmedGenStep;

            if (!keep && !doNotKeep && !collect) {
                continue; // nothing to do here
            }

            auto ev = std::make_unique<TEvBlobStorage::TEvCollectGarbage>(record.TabletId, generation,
                record.PerGenerationCounter, record.Channel, collect, nextGenStep.Generation(), nextGenStep.Step(),
                keep.get(), doNotKeep.get(), TInstant::Max(), true);
            keep.release();
            doNotKeep.release();

            record.CollectGarbageRequestInFlight = true;
            record.PerGenerationCounter += ev->Collect ? ev->PerGenerationCounterStepSize() : 0;
            record.TrashInFlight.insert(record.TrashInFlight.end(), record.Trash.begin(), record.Trash.end());
            record.IssuedGenStep = Max(nextGenStep, record.LastConfirmedGenStep);

            record.TIntrusiveListItem<TRecordsPerChannelGroup, TRecordWithTrash>::Unlink();

            STLOG(PRI_DEBUG, BLOB_DEPOT, BDT11, "issuing TEvCollectGarbage", (Id, Self->GetLogId()),
                (Channel, int(record.Channel)), (GroupId, record.GroupId), (Msg, ev->ToString()),
                (LastConfirmedGenStep, record.LastConfirmedGenStep), (IssuedGenStep, record.IssuedGenStep),
                (TrashInFlight.size, record.TrashInFlight.size()));

            if (collect) {
                ExecuteIssueGC(record.Channel, record.GroupId, record.IssuedGenStep, std::move(ev));
            } else {
                SendToBSProxy(Self->SelfId(), record.GroupId, ev.release(), record.GroupId);
            }
        }

        for (auto& [agentId, ev] : outbox) {
            TAgent& agent = Self->GetAgent(agentId);
            const ui64 id = ++agent.LastRequestId;
            auto& request = agent.InvalidateStepRequests[id];
            for (const auto& item : ev->Record.GetInvalidatedSteps()) {
                request[item.GetChannel()] = item.GetInvalidatedStep();
            }

            Y_VERIFY(agent.AgentId);
            agent.PushCallbacks.emplace(id, std::bind(&TData::OnPushNotifyResult, this, std::placeholders::_1));
            TActivationContext::Send(new IEventHandle(*agent.AgentId, Self->SelfId(), ev.release(), 0, id));
        }
    }

    void TData::Handle(TEvBlobStorage::TEvCollectGarbageResult::TPtr ev) {
        STLOG(PRI_DEBUG, BLOB_DEPOT, BDT12, "TEvCollectGarbageResult", (Id, Self->GetLogId()),
            (Channel, ev->Get()->Channel), (GroupId, ev->Cookie), (Msg, ev->Get()->ToString()));
        const auto& key = std::make_tuple(ev->Get()->TabletId, ev->Get()->Channel, ev->Cookie);
        const auto it = RecordsPerChannelGroup.find(key);
        Y_VERIFY(it != RecordsPerChannelGroup.end());
        auto& record = it->second;
        if (ev->Get()->Status == NKikimrProto::OK) {
            Y_VERIFY(record.CollectGarbageRequestInFlight);
            record.OnSuccessfulCollect(this);
            ExecuteConfirmGC(record.Channel, record.GroupId, std::exchange(record.TrashInFlight, {}),
                record.LastConfirmedGenStep);
        } else {
            record.ClearInFlight(this);
            HandleTrash();
        }
    }

    void TData::OnPushNotifyResult(TEvBlobDepot::TEvPushNotifyResult::TPtr ev) {
        TAgent& agent = Self->GetAgent(ev->Recipient);

        const auto it = agent.InvalidateStepRequests.find(ev->Cookie);
        Y_VERIFY(it != agent.InvalidateStepRequests.end());
        auto items = std::move(it->second);
        agent.InvalidateStepRequests.erase(it);

        const ui32 generation = Self->Executor()->Generation();

        std::set<TBlobSeqId> writesInFlight;
        for (const auto& item : ev->Get()->Record.GetWritesInFlight()) {
            writesInFlight.insert(TBlobSeqId::FromProto(item));
        }

        for (const auto& [channel, invalidatedStep] : items) {
            const ui32 channel_ = channel;
            const ui32 invalidatedStep_ = invalidatedStep;
            auto& agentGivenIdRanges = agent.GivenIdRanges[channel];
            auto& givenIdRanges = Self->Channels[channel].GivenIdRanges;

            auto begin = writesInFlight.lower_bound(TBlobSeqId{channel, 0, 0, 0});
            auto end = writesInFlight.upper_bound(TBlobSeqId{channel, Max<ui32>(), Max<ui32>(), TBlobSeqId::MaxIndex});

            auto makeWritesInFlight = [&] {
                TStringStream s;
                s << "[";
                for (auto it = begin; it != end; ++it) {
                    s << (it != begin ? " " : "") << it->ToString();
                }
                s << "]";
                return s.Str();
            };

            STLOG(PRI_DEBUG, BLOB_DEPOT, BDT13, "Trim", (Id, Self->GetLogId()), (AgentId, agent.ConnectedNodeId),
                (Id, ev->Cookie), (Channel, channel_), (InvalidatedStep, invalidatedStep_),
                (GivenIdRanges, Self->Channels[channel_].GivenIdRanges),
                (Agent.GivenIdRanges, agent.GivenIdRanges[channel_]),
                (WritesInFlight, makeWritesInFlight()));

            for (auto it = begin; it != end; ++it) {
                Y_VERIFY_S(agentGivenIdRanges.GetPoint(it->ToSequentialNumber()), "blobSeqId# " << it->ToString());
                Y_VERIFY_S(givenIdRanges.GetPoint(it->ToSequentialNumber()), "blobSeqId# " << it->ToString());
            }

            const TBlobSeqId trimmedBlobSeqId{channel, generation, invalidatedStep, TBlobSeqId::MaxIndex};
            const ui64 validSince = trimmedBlobSeqId.ToSequentialNumber() + 1;
            givenIdRanges.Subtract(agentGivenIdRanges.Trim(validSince));

            for (auto it = begin; it != end; ++it) {
                agentGivenIdRanges.AddPoint(it->ToSequentialNumber());
                givenIdRanges.AddPoint(it->ToSequentialNumber());
            }

            OnLeastExpectedBlobIdChange(channel);
        }

        HandleTrash();
    }

    void TData::OnCommitConfirmedGC(ui8 channel, ui32 groupId) {
        const auto& key = std::make_tuple(Self->TabletID(), channel, groupId);
        const auto it = RecordsPerChannelGroup.find(key);
        Y_VERIFY(it != RecordsPerChannelGroup.end());
        it->second.ClearInFlight(this);
    }

    bool TData::OnBarrierShift(ui64 tabletId, ui8 channel, bool hard, TGenStep previous, TGenStep current, ui32& maxItems,
            NTabletFlatExecutor::TTransactionContext& txc, void *cookie) {
        const TData::TKey first(TLogoBlobID(tabletId, previous.Generation(), previous.Step(), channel, 0, 0));
        const TData::TKey last(TLogoBlobID(tabletId, current.Generation(), current.Step(), channel,
            TLogoBlobID::MaxBlobSize, TLogoBlobID::MaxCookie, TLogoBlobID::MaxPartId, TLogoBlobID::MaxCrcMode));

        bool finished = true;
        Self->Data->ScanRange(&first, &last, TData::EScanFlags::INCLUDE_END, [&](auto& key, auto& value) {
            if (value.KeepState != NKikimrBlobDepot::EKeepState::Keep || hard) {
                if (maxItems) {
                    Self->Data->DeleteKey(key, txc, cookie);
                    --maxItems;
                } else {
                    finished = false;
                    return false;
                }
            }
            return true;
        });

        return finished;
    }

    void TData::AccountBlob(TLogoBlobID id, bool add) {
        // account record
        const ui32 groupId = Self->Info()->GroupFor(id.Channel(), id.Generation());
        auto& groupStat = Self->Groups[groupId];
        if (add) {
            groupStat.AllocatedBytes += id.BlobSize();
        } else {
            groupStat.AllocatedBytes -= id.BlobSize();
        }
    }

    bool TData::CanBeCollected(ui32 groupId, TBlobSeqId id) const {
        const auto it = RecordsPerChannelGroup.find(std::make_tuple(Self->TabletID(), id.Channel, groupId));
        return it != RecordsPerChannelGroup.end() && TGenStep(id) <= it->second.IssuedGenStep;
    }

    void TData::OnLeastExpectedBlobIdChange(ui8 channel) {
        auto& ch = Self->Channels[channel];
        const ui64 minSequenceNumber = ch.GivenIdRanges.IsEmpty()
            ? ch.NextBlobSeqId
            : ch.GivenIdRanges.GetMinimumValue();
        const TBlobSeqId leastExpectedBlobId = TBlobSeqId::FromSequentalNumber(channel, Self->Executor()->Generation(),
            minSequenceNumber);

        const TTabletStorageInfo *info = Self->Info();
        const TTabletChannelInfo *storageChannel = info->ChannelInfo(leastExpectedBlobId.Channel);
        Y_VERIFY(storageChannel);
        for (const auto& entry : storageChannel->History) {
            const auto& key = std::make_tuple(info->TabletID, storageChannel->Channel, entry.GroupID);
            auto [it, _] = RecordsPerChannelGroup.emplace(std::piecewise_construct, key, key);
            auto& record = it->second;
            record.OnLeastExpectedBlobIdChange(this, leastExpectedBlobId);
        }
    }

    void TData::TRecordsPerChannelGroup::MoveToTrash(TData *self, TLogoBlobID id) {
        const auto usedIt = Used.find(id);
        Y_VERIFY(usedIt != Used.end());
        Trash.insert(Used.extract(usedIt));
        EnqueueForCollectionIfPossible(self);
    }

    void TData::TRecordsPerChannelGroup::OnSuccessfulCollect(TData *self) {
        auto it = Trash.begin();
        for (const TLogoBlobID& id : TrashInFlight) {
            for (; it != Trash.end() && *it < id; ++it) {}
            Y_VERIFY(it != Trash.end() && *it == id);
            it = Trash.erase(it);
            self->AccountBlob(id, false);
        }
        LastConfirmedGenStep = IssuedGenStep;
        EnqueueForCollectionIfPossible(self);
    }

    void TData::TRecordsPerChannelGroup::OnLeastExpectedBlobIdChange(TData *self, TBlobSeqId leastExpectedBlobId) {
        Y_VERIFY_S(LeastExpectedBlobId <= leastExpectedBlobId, "Prev# " << LeastExpectedBlobId.ToString()
            << " Next# " << leastExpectedBlobId.ToString());
        if (LeastExpectedBlobId < leastExpectedBlobId) {
            LeastExpectedBlobId = leastExpectedBlobId;
            EnqueueForCollectionIfPossible(self);
        }
    }

    void TData::TRecordsPerChannelGroup::ClearInFlight(TData *self) {
        Y_VERIFY(CollectGarbageRequestInFlight);
        CollectGarbageRequestInFlight = false;
        EnqueueForCollectionIfPossible(self);
    }

    void TData::TRecordsPerChannelGroup::EnqueueForCollectionIfPossible(TData *self) {
        if (!CollectGarbageRequestInFlight && TabletId == self->Self->TabletID() && Empty() && !Trash.empty()) {
            self->RecordsWithTrash.PushBack(this);
        }
    }

} // NKikimr::NBlobDepot
