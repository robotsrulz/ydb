#include <ydb/library/folder_service/mock/mock_folder_service.h>
#include <ydb/library/folder_service/events.h>
#include <library/cpp/actors/core/hfunc.h>

namespace NKikimr::NFolderService {

class TFolderServiceMock
    : public NActors::TActor<TFolderServiceMock> {
    using TThis = TFolderServiceMock;
    using TBase = NActors::TActor<TFolderServiceMock>;

    using TEvListFolderRequest = NKikimr::NFolderService::TEvFolderService::TEvGetFolderRequest;
    using TEvListFolderResponse = NKikimr::NFolderService::TEvFolderService::TEvGetFolderResponse;

public:
    TFolderServiceMock()
        : TBase(&TThis::StateWork) {
    }

    void Handle(TEvListFolderRequest::TPtr& ev) {
        auto folderId = ev.Get()->Get()->Request.folder_id();
        auto result = std::make_unique<TEvListFolderResponse>();
        auto* fakeFolder = result->Response.mutable_folder();
        TString cloudId = "mock_cloud";
        auto p = folderId.find('@');
        if (p != folderId.npos) {
            cloudId = folderId.substr(p + 1);
        }
        fakeFolder->set_id(folderId);
        fakeFolder->set_cloud_id(cloudId);
        result->Status = NGrpc::TGrpcStatus();
        Send(ev->Sender, result.release());
    }

    STATEFN(StateWork) {
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvListFolderRequest, Handle)
            cFunc(NActors::TEvents::TEvPoisonPill::EventType, PassAway)
        }
    }
};

NActors::IActor* CreateMockFolderServiceActor(const NKikimrProto::NFolderService::TFolderServiceConfig&) {
    return new TFolderServiceMock();
}
} // namespace NKikimr::NFolderService
