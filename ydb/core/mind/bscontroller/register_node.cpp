#include "impl.h"

#include <ydb/core/blobstorage/base/utility.h>
#include "config.h"

namespace NKikimr::NBsController {

class TBlobStorageController::TTxUpdateNodeDrives
    : public TTransactionBase<TBlobStorageController>
{
    NKikimrBlobStorage::TEvControllerUpdateNodeDrives Record;
    std::optional<TConfigState> State;

    std::unique_ptr<IEventHandle> Response;

    void UpdateDevicesInfo(TTransactionContext& txc, TEvBlobStorage::TEvControllerNodeServiceSetUpdate* result) {

        const TNodeId nodeId = Record.GetNodeId();

        auto createLog = [&] () {
            TStringStream out;
            bool first = true;
            out << "[";
            for (const auto& data : Record.GetDrivesData()) {
                out << (std::exchange(first, false) ? "" : ", ")
                    << "{"
                    << data.GetPath() << " "
                    << data.GetSerialNumber() << " "
                    << data.GetModelNumber() << " "
                    << NPDisk::DeviceTypeStr(PDiskTypeToPDiskType(data.GetDeviceType()), true) << " "
                    << "}";
            }
            out << "]";
            return out.Str();
        };
        STLOG(PRI_DEBUG, BS_CONTROLLER, BSCTXRN05, "Add devicesData from NodeWarden",
                (NodeId, nodeId), (Devices, createLog()));

        std::map<TString, TString> serialForPath;
        for (const auto& data : Record.GetDrivesData()) {
            serialForPath[data.GetPath()] = data.GetSerialNumber();
        }

        NIceDb::TNiceDb db(txc.DB);
        using T = Schema::PDisk;

        TPDiskId minPDiskId = TPDiskId::MinForNode(nodeId);
        for (auto it = Self->PDisks.lower_bound(minPDiskId); it != Self->PDisks.end() && it->first.NodeId == nodeId; ++it) {
            Y_VERIFY(it->second);
            TPDiskInfo& info = *it->second;
            TPDiskId pdiskId = it->first;

            const T::TKey::Type key(pdiskId.GetKey());
            TString serial;

            if (auto serialIt = serialForPath.find(info.Path); serialIt != serialForPath.end()) {
                serial = serialIt->second;
                if (info.ExpectedSerial != serial) {
                    TStringStream log;
                    auto prio = NLog::PRI_NOTICE;

                    if (!info.ExpectedSerial) {
                        if (auto driveIt = Self->DrivesSerials.find(TSerial{serial}); driveIt != Self->DrivesSerials.end()) {
                            log << "device is managed by HostConfigs and was removed.";
                            if (driveIt->second->LifeStage == NKikimrBlobStorage::TDriveLifeStage::NOT_SEEN) {
                                log << " Drive was added while node was offline, so update ExpectedSerial and"
                                    << " remove fictional row from DriveSerial table";
                                info.ExpectedSerial = serial;
                                Self->DrivesSerials.erase(driveIt);
                                db.Table<Schema::DriveSerial>().Key(TSerial{serial}.GetKey()).Delete();
                            } else if (driveIt->second->LifeStage == NKikimrBlobStorage::TDriveLifeStage::REMOVED) {
                                log << " Drive is still marked as REMOVED, so do not update ExpectedSerial";
                            }
                        } else {
                            // disk has not seen yet
                            info.ExpectedSerial = serial;
                        }
                    } else if (Self->SerialManagementStage == NKikimrBlobStorage::TSerialManagementStage::CHECK_SERIAL) {
                        prio = NLog::PRI_ERROR;
                        log << "new serial mismatched stored pdisk's serial";
                    } else {
                        log << "Set new ExpectedSerial for pdisk";

                        auto [it, emplaced] = Self->DrivesSerials.emplace(serial, MakeHolder<TDriveSerialInfo>(info.BoxId));
                        it->second->Guid = info.Guid;
                        it->second->Kind = info.Kind.Kind();
                        it->second->PDiskType = PDiskTypeToPDiskType(info.Kind.Type());
                        it->second->PDiskConfig = info.PDiskConfig;
                        it->second->LifeStage = NKikimrBlobStorage::TDriveLifeStage::REMOVED;

                        TDriveSerialInfo::Apply(Self, [&, it = it] (auto* adapter) {
                            adapter->IssueUpdateRow(txc, TSerial{serial}, *it->second);
                        });

                        info.ExpectedSerial = serial;
                        db.Table<T>().Key(key).Update<T::ExpectedSerial>(serial);
                    }
                    STLOG(prio, BS_CONTROLLER, BSCTXRN06, log.Str(), (PDiskId, pdiskId), (Path, info.Path),
                            (OldSerial, info.ExpectedSerial), (NewSerial, serial));
                }
            }
            if (info.LastSeenSerial != serial) {
                info.LastSeenSerial = serial;
                db.Table<T>().Key(key).Update<T::LastSeenSerial>(serial);
                if (serial) {
                    Self->ReadPDisk(pdiskId, info, result, NKikimrBlobStorage::RESTART);
                }
            }
        }

        TNodeInfo& nodeInfo = Self->GetNode(nodeId);
        Self->EraseKnownDrivesOnDisconnected(&nodeInfo);

        for (const auto& data : Record.GetDrivesData()) {
            const auto& serial = data.GetSerialNumber();
            if (auto it = Self->NodeForSerial.find(serial); it != Self->NodeForSerial.end() && it->second != nodeId) {
                STLOG(PRI_ERROR, BS_CONTROLLER, BSCTXRN03,
                        "Received drive from NewNodeId, but drive is reported as placed in OldNodeId",
                        (NewNodeId, nodeId), (OldNodeId, it->second), (Serial, serial));
            } else {
                Self->NodeForSerial[serial] = nodeId;
            }
            NPDisk::TDriveData driveData;
            DriveDataToDriveData(data, driveData);
            auto [it, emplaced] = nodeInfo.KnownDrives.emplace(serial, driveData);
            if (it->second.DeviceType == NPDisk::DEVICE_TYPE_NVME) {
                it->second.DeviceType = NPDisk::DEVICE_TYPE_SSD;
            }
        }
    }

public:
    TTxUpdateNodeDrives(NKikimrBlobStorage::TEvControllerUpdateNodeDrives&& rec, TBlobStorageController *controller)
        : TTransactionBase(controller)
        , Record(std::move(rec))
    {}

    TTxType GetTxType() const override { return NBlobStorageController::TXTYPE_UPDATE_NODE_DRIVES; }

    bool Execute(TTransactionContext& txc, const TActorContext&) override {
        const TNodeId nodeId = Record.GetNodeId();

        auto result = std::make_unique<TEvBlobStorage::TEvControllerNodeServiceSetUpdate>(NKikimrProto::OK, nodeId);

        State.emplace(*Self, Self->HostRecords, TActivationContext::Now());
        State->CheckConsistency();

        UpdateDevicesInfo(txc, result.get());

        TNodeInfo& nodeInfo = Self->GetNode(nodeId);

        std::vector<TSerial> serials;
        for (const auto& data : Record.GetDrivesData()) {
            serials.emplace_back(data.GetSerialNumber());
        }

        try {
            Self->FitPDisksForNode(*State, nodeId, serials);
            State->CheckConsistency();
        } catch (const TExError& e) {
            Self->EraseKnownDrivesOnDisconnected(&nodeInfo);
            STLOG(PRI_ERROR, BS_CONTROLLER, BSCTXRN04,
                    "Error during FitPDisks after receiving TEvControllerRegisterNode", (TExError, e.what()));
        }

        result->Record.SetInstanceId(Self->InstanceId);
        result->Record.SetComprehensive(false);
        result->Record.SetAvailDomain(AppData()->DomainsInfo->GetDomainUidByTabletId(Self->TabletID()));
        Response = std::make_unique<IEventHandle>(MakeBlobStorageNodeWardenID(nodeId), Self->SelfId(), result.release(), 0, 0);

        TString error;
        if (State->Changed() && !Self->CommitConfigUpdates(*State, false, false, txc, &error)) {
            State->Rollback();
            State.reset();
        }

        return true;
    }

    void Complete(const TActorContext&) override {
        if (State) {
            // Send new TNodeWardenServiceSet to NodeWarder inside
            State->ApplyConfigUpdates();
            State.reset();
        }
        if (Response) {
            TActivationContext::Send(Response.release());
        }
    }
};

class TBlobStorageController::TTxRegisterNode
    : public TTransactionBase<TBlobStorageController>
{
    TEvBlobStorage::TEvControllerRegisterNode::TPtr Request;
    std::unique_ptr<IEventHandle> Response;
    NKikimrBlobStorage::TEvControllerUpdateNodeDrives UpdateNodeDrivesRecord;


public:
    TTxRegisterNode(TEvBlobStorage::TEvControllerRegisterNode::TPtr& ev, TBlobStorageController *controller)
        : TTransactionBase(controller)
        , Request(ev)
    {}

    TTxType GetTxType() const override { return NBlobStorageController::TXTYPE_REGISTER_NODE; }

    bool Execute(TTransactionContext& txc, const TActorContext&) override {
        Self->TabletCounters->Cumulative()[NBlobStorageController::COUNTER_REGISTER_NODE_COUNT].Increment(1);
        TRequestCounter counter(Self->TabletCounters, NBlobStorageController::COUNTER_REGISTER_NODE_USEC);

        auto request = std::move(Request);
        const auto& record = request->Get()->Record;
        STLOG(PRI_DEBUG, BS_CONTROLLER, BSCTXRN01, "Handle TEvControllerRegisterNode", (Request, record));

        const TNodeId nodeId = record.GetNodeID();
        UpdateNodeDrivesRecord.SetNodeId(nodeId);

        for (const auto& data : record.GetDrivesData()) {
            *UpdateNodeDrivesRecord.AddDrivesData() = data;
        }

        Self->OnRegisterNode(request->Recipient, nodeId);
        Self->ProcessVDiskStatus(record.GetVDiskStatus());

        // create map of group ids to their generations as reported by the node warden
        TMap<ui32, ui32> startedGroups;
        if (record.GroupsSize() == record.GroupGenerationsSize()) {
            for (size_t i = 0; i < record.GroupsSize(); ++i) {
                startedGroups.emplace(record.GetGroups(i), record.GetGroupGenerations(i));
            }
        } else {
            for (ui32 groupId : record.GetGroups()) {
                startedGroups.emplace(groupId, 0);
            }
        }

        auto res = std::make_unique<TEvBlobStorage::TEvControllerNodeServiceSetUpdate>(NKikimrProto::OK, nodeId);

        TSet<ui32> groupIDsToRead;
        const TPDiskId minPDiskId(TPDiskId::MinForNode(nodeId));
        const TVSlotId vslotId = TVSlotId::MinForPDisk(minPDiskId);
        for (auto it = Self->VSlots.lower_bound(vslotId); it != Self->VSlots.end() && it->first.NodeId == nodeId; ++it) {
            Self->ReadVSlot(*it->second, res.get());
            if (!it->second->IsBeingDeleted()) {
                groupIDsToRead.insert(it->second->GroupId);
            }
        }

        TSet<ui32> groupsToDiscard;

        auto processGroup = [&](const auto& p, TGroupInfo *group) {
            auto&& [groupId, generation] = p;
            if (!group) {
                groupsToDiscard.insert(groupsToDiscard.end(), groupId);
            } else if (group->Generation > generation) {
                groupIDsToRead.insert(groupId);
            }
        };

        if (startedGroups.size() <= Self->GroupMap.size() / 10) {
            for (const auto& p : startedGroups) {
                processGroup(p, Self->FindGroup(p.first));
            }
        } else {
            auto started = startedGroups.begin();
            auto groupIt = Self->GroupMap.begin();

            while (started != startedGroups.end()) {
                TGroupInfo *group = nullptr;

                // scan through groups until we find matching one
                for (; groupIt != Self->GroupMap.end() && groupIt->first <= started->first; ++groupIt) {
                    if (groupIt->first == started->first) {
                        group = groupIt->second.Get();
                    }
                }

                processGroup(*started++, group);
            }
        }

        Self->ReadGroups(groupIDsToRead, false, res.get(), nodeId);
        Y_VERIFY(groupIDsToRead.empty());

        Self->ReadGroups(groupsToDiscard, true, res.get(), nodeId);

        for (auto it = Self->PDisks.lower_bound(minPDiskId); it != Self->PDisks.end() && it->first.NodeId == nodeId; ++it) {
            Self->ReadPDisk(it->first, *it->second, res.get(), NKikimrBlobStorage::INITIAL);
        }

        res->Record.SetInstanceId(Self->InstanceId);
        res->Record.SetComprehensive(true);
        res->Record.SetAvailDomain(AppData()->DomainsInfo->GetDomainUidByTabletId(Self->TabletID()));
        Response = std::make_unique<IEventHandle>(request->Sender, Self->SelfId(), res.release(), 0, request->Cookie);

        NIceDb::TNiceDb db(txc.DB);
        auto& node = Self->GetNode(nodeId);
        db.Table<Schema::Node>().Key(nodeId).Update<Schema::Node::LastConnectTimestamp>(node.LastConnectTimestamp);

        return true;
    }

    void Complete(const TActorContext&) override {
        TActivationContext::Send(Response.release());
        Self->Execute(new TTxUpdateNodeDrives(std::move(UpdateNodeDrivesRecord), Self));
    }
};

class TBlobStorageController::TTxUpdateNodeDisconnectTimestamp
    : public TTransactionBase<TBlobStorageController>
{
    TNodeId NodeId;

public:
    TTxUpdateNodeDisconnectTimestamp(TNodeId nodeId, TBlobStorageController *controller)
        : TTransactionBase(controller)
        , NodeId(nodeId)
    {}

    TTxType GetTxType() const override { return NBlobStorageController::TXTYPE_UPDATE_NODE_DISCONNECT_TIMESTAMP; }

    bool Execute(TTransactionContext& txc, const TActorContext&) override {
        NIceDb::TNiceDb db(txc.DB);
        auto& node = Self->GetNode(NodeId);
        db.Table<Schema::Node>().Key(NodeId).Update<Schema::Node::LastDisconnectTimestamp>(node.LastDisconnectTimestamp);
        return true;
    }

    void Complete(const TActorContext&) override {}
};

void TBlobStorageController::ReadGroups(TSet<ui32>& groupIDsToRead, bool discard,
        TEvBlobStorage::TEvControllerNodeServiceSetUpdate *result, TNodeId nodeId) {
    for (auto it = groupIDsToRead.begin(); it != groupIDsToRead.end(); ) {
        const TGroupId groupId = *it;
        TGroupInfo *group = FindGroup(groupId);
        if (group || discard) {
            NKikimrBlobStorage::TNodeWardenServiceSet *serviceSetProto = result->Record.MutableServiceSet();
            NKikimrBlobStorage::TGroupInfo *groupProto = serviceSetProto->AddGroups();
            if (!group) {
                groupProto->SetGroupID(groupId);
                groupProto->SetEntityStatus(NKikimrBlobStorage::DESTROY);
            } else if (group->Listable()) {
                const TStoragePoolInfo& info = StoragePools.at(group->StoragePoolId);

                TMaybe<TKikimrScopeId> scopeId;
                if (info.SchemeshardId && info.PathItemId) {
                    scopeId.ConstructInPlace(*info.SchemeshardId, *info.PathItemId);
                } else {
                    Y_VERIFY(!info.SchemeshardId && !info.PathItemId);
                }

                SerializeGroupInfo(groupProto, *group, info.Name, scopeId);
            } else if (nodeId) {
                // group is not listable, so we have to postpone the request from NW
                group->WaitingNodes.insert(nodeId);
                GetNode(nodeId).WaitingForGroups.insert(group->ID);
            }

            // this group is processed, remove it from the set
            it = groupIDsToRead.erase(it);
        } else {
            ++it; // keep this group in the set as deleted one
        }
    }
}

void TBlobStorageController::ReadPDisk(const TPDiskId& pdiskId, const TPDiskInfo& pdisk,
        TEvBlobStorage::TEvControllerNodeServiceSetUpdate *result, const NKikimrBlobStorage::EEntityStatus entityStatus) {
    NKikimrBlobStorage::TNodeWardenServiceSet *serviceSet = result->Record.MutableServiceSet();
    NKikimrBlobStorage::TNodeWardenServiceSet::TPDisk *pDisk = serviceSet->AddPDisks();
    if (const auto it = StaticPDiskMap.find(pdiskId); it != StaticPDiskMap.end()) {
        pDisk->CopyFrom(it->second);
    } else {
        pDisk->SetNodeID(pdiskId.NodeId);
        pDisk->SetPDiskID(pdiskId.PDiskId);
        if (pdisk.Path) {
            pDisk->SetPath(pdisk.Path);
        } else if (pdisk.LastSeenPath) {
            pDisk->SetPath(pdisk.LastSeenPath);
        }
        pDisk->SetPDiskCategory(pdisk.Kind.GetRaw());
        pDisk->SetPDiskGuid(pdisk.Guid);
        if (pdisk.PDiskConfig && !pDisk->MutablePDiskConfig()->ParseFromString(pdisk.PDiskConfig)) {
            STLOG(PRI_CRIT, BS_CONTROLLER, BSCTXRN02, "PDiskConfig invalid", (NodeId, pdiskId.NodeId),
                (PDiskId, pdiskId.PDiskId));
        }
    }
    pDisk->SetExpectedSerial(pdisk.ExpectedSerial);
    pDisk->SetManagementStage(SerialManagementStage);
    pDisk->SetSpaceColorBorder(PDiskSpaceColorBorder);
    pDisk->SetEntityStatus(entityStatus);
}

void TBlobStorageController::ReadVSlot(const TVSlotInfo& vslot, TEvBlobStorage::TEvControllerNodeServiceSetUpdate *result) {
    NKikimrBlobStorage::TNodeWardenServiceSet *serviceSet = result->Record.MutableServiceSet();
    NKikimrBlobStorage::TNodeWardenServiceSet::TVDisk *vDisk = serviceSet->AddVDisks();
    Serialize(vDisk->MutableVDiskLocation(), vslot);

    VDiskIDFromVDiskID(vslot.GetVDiskId(), vDisk->MutableVDiskID());

    vDisk->SetVDiskKind(vslot.Kind);
    if (vslot.IsBeingDeleted()) {
        vDisk->SetDoDestroy(true);
        vDisk->SetEntityStatus(NKikimrBlobStorage::DESTROY);
    } else {
        vDisk->SetDoWipe(vslot.Mood == TMood::Wipe);
    }

    if (TGroupInfo *group = FindGroup(vslot.GroupId)) {
        const TStoragePoolInfo& info = StoragePools.at(group->StoragePoolId);
        vDisk->SetStoragePoolName(info.Name);
        SerializeDonors(vDisk, vslot, *group);
    } else {
        Y_VERIFY(vslot.Mood != TMood::Donor);
    }
}

void TBlobStorageController::Handle(TEvBlobStorage::TEvControllerRegisterNode::TPtr& ev) {
    Execute(new TTxRegisterNode(ev, this));
}

void TBlobStorageController::Handle(TEvBlobStorage::TEvControllerUpdateNodeDrives::TPtr& ev) {
    Execute(new TTxUpdateNodeDrives(std::move(ev->Get()->Record), this));
}

void TBlobStorageController::Handle(TEvTabletPipe::TEvServerConnected::TPtr& ev) {
    auto&& [it, inserted] = PipeServerToNode.emplace(ev->Get()->ServerId, std::nullopt);
    Y_VERIFY_DEBUG(inserted);
}

void TBlobStorageController::Handle(TEvTabletPipe::TEvServerDisconnected::TPtr& ev) {
    if (auto it = PipeServerToNode.find(ev->Get()->ServerId); it != PipeServerToNode.end()) {
        if (auto&& nodeId = it->second) {
            OnWardenDisconnected(*nodeId);
        }
        PipeServerToNode.erase(it);
    } else {
        Y_VERIFY_DEBUG(false);
    }
}

void TBlobStorageController::OnRegisterNode(const TActorId& serverId, TNodeId nodeId) {
    if (auto it = PipeServerToNode.find(serverId); it != PipeServerToNode.end()) {
        if (!it->second) {
            it->second = nodeId;
            OnWardenConnected(nodeId);
        } else {
            Y_VERIFY_DEBUG(*it->second == nodeId);
        }
    } else {
        Y_VERIFY_DEBUG(false);
    }
}

void TBlobStorageController::OnWardenConnected(TNodeId nodeId) {
    TNodeInfo& node = GetNode(nodeId);
    ++node.ConnectedCount;

    for (auto it = PDisks.lower_bound(TPDiskId::MinForNode(nodeId)); it != PDisks.end() && it->first.NodeId == nodeId; ++it) {
        it->second->UpdateOperational(true);
        SysViewChangedPDisks.insert(it->first);
    }

    node.LastConnectTimestamp = TInstant::Now();
}

void TBlobStorageController::OnWardenDisconnected(TNodeId nodeId) {
    TNodeInfo& node = GetNode(nodeId);
    if (--node.ConnectedCount) {
        return; // there are still some connections from this NW
    }

    for (const TGroupId groupId : std::exchange(node.WaitingForGroups, {})) {
        if (TGroupInfo *group = FindGroup(groupId)) {
            group->WaitingNodes.erase(nodeId);
        }
    }

    const TInstant now = TActivationContext::Now();
    const TMonotonic mono = TActivationContext::Monotonic();
    std::vector<std::pair<TVSlotId, TInstant>> lastSeenReadyQ;
    for (auto it = PDisks.lower_bound(TPDiskId::MinForNode(nodeId)); it != PDisks.end() && it->first.NodeId == nodeId; ++it) {
        it->second->UpdateOperational(false);
        SysViewChangedPDisks.insert(it->first);
    }
    const TVSlotId startingId(nodeId, Min<Schema::VSlot::PDiskID::Type>(), Min<Schema::VSlot::VSlotID::Type>());
    auto sh = MakeHolder<TEvControllerUpdateSelfHealInfo>();
    for (auto it = VSlots.lower_bound(startingId); it != VSlots.end() && it->first.NodeId == nodeId; ++it) {
        if (const TGroupInfo *group = it->second->Group) {
            if (it->second->IsReady) {
                it->second->LastSeenReady = now;
                lastSeenReadyQ.emplace_back(it->second->VSlotId, now);
                NotReadyVSlotIds.insert(it->second->VSlotId);
            }
            it->second->SetStatus(NKikimrBlobStorage::EVDiskStatus::ERROR, mono);
            const_cast<TGroupInfo*>(group)->CalculateGroupStatus();
            sh->VDiskStatusUpdate.emplace_back(it->second->GetVDiskId(), it->second->Status);
            ScrubState.UpdateVDiskState(&*it->second);
        }
    }
    for (auto it = StaticVSlots.lower_bound(startingId); it != StaticVSlots.end() && it->first.NodeId == nodeId; ++it) {
        it->second.VDiskStatus = NKikimrBlobStorage::EVDiskStatus::ERROR;
    }
    if (sh->VDiskStatusUpdate) {
        Send(SelfHealId, sh.Release());
    }
    ScrubState.OnNodeDisconnected(nodeId);
    EraseKnownDrivesOnDisconnected(&node);
    if (!lastSeenReadyQ.empty()) {
        Execute(CreateTxUpdateLastSeenReady(std::move(lastSeenReadyQ)));
    }
    node.LastDisconnectTimestamp = now;
    Execute(new TTxUpdateNodeDisconnectTimestamp(nodeId, this));
}

void TBlobStorageController::EraseKnownDrivesOnDisconnected(TNodeInfo *nodeInfo) {
    for (const auto& [serial, driveData] : nodeInfo->KnownDrives) {
        NodeForSerial.erase(serial);
    }
    nodeInfo->KnownDrives.clear();
}

} // NKikimr::NBsController
