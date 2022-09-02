#include "dsproxy_mock.h"
#include "model.h"
#include <ydb/core/base/blobstorage.h>
#include <ydb/core/blobstorage/vdisk/common/vdisk_events.h>
#include <ydb/core/util/stlog.h>

namespace NKikimr {

    namespace {

        class TBlobStorageGroupProxyMockActor
            : public TActor<TBlobStorageGroupProxyMockActor>
        {
            TIntrusivePtr<NFake::TProxyDS> Model;

            void Handle(TEvBlobStorage::TEvPut::TPtr& ev) {
                STLOG(PRI_DEBUG, BS_PROXY, BSPM01, "TEvPut", (Msg, ev->Get()->ToString()));
                Send(ev->Sender, Model->Handle(ev->Get()),0, ev->Cookie);
            }

            void Handle(TEvBlobStorage::TEvGet::TPtr& ev) {
                STLOG(PRI_DEBUG, BS_PROXY, BSPM02, "TEvGet", (Msg, ev->Get()->ToString()));
                Send(ev->Sender, Model->Handle(ev->Get()), 0, ev->Cookie);
            }

            void Handle(TEvBlobStorage::TEvBlock::TPtr& ev) {
                STLOG(PRI_DEBUG, BS_PROXY, BSPM03, "TEvBlock", (Msg, ev->Get()->ToString()));
                Send(ev->Sender, Model->Handle(ev->Get()), 0, ev->Cookie);
            }

            void Handle(TEvBlobStorage::TEvDiscover::TPtr& ev) {
                STLOG(PRI_DEBUG, BS_PROXY, BSPM04, "TEvDiscover", (Msg, ev->Get()->ToString()));
                Send(ev->Sender, Model->Handle(ev->Get()), 0, ev->Cookie);
            }

            void Handle(TEvBlobStorage::TEvRange::TPtr& ev) {
                STLOG(PRI_DEBUG, BS_PROXY, BSPM05, "TEvRange", (Msg, ev->Get()->ToString()));
                Send(ev->Sender, Model->Handle(ev->Get()), 0, ev->Cookie);
            }

            void Handle(TEvBlobStorage::TEvCollectGarbage::TPtr& ev) {
                STLOG(PRI_DEBUG, BS_PROXY, BSPM06, "TEvCollectGarbage", (Msg, ev->Get()->ToString()));
                Send(ev->Sender, Model->Handle(ev->Get()), 0, ev->Cookie);
            }

            void Handle(TEvBlobStorage::TEvStatus::TPtr& ev) {
                STLOG(PRI_DEBUG, BS_PROXY, BSPM07, "TEvStatus", (Msg, ev->Get()->ToString()));
                Send(ev->Sender, new TEvBlobStorage::TEvStatusResult(NKikimrProto::OK, Model->GetStorageStatusFlags()), 0,
                        ev->Cookie);
            }

            void Handle(TEvBlobStorage::TEvAssimilate::TPtr& ev) {
                STLOG(PRI_DEBUG, BS_PROXY, BSPM09, "TEvAssimilate", (Msg, ev->Get()->ToString()));
                Send(ev->Sender, new TEvBlobStorage::TEvAssimilateResult(NKikimrProto::ERROR, "not implemented"));
            }

            void HandlePoison(TEvents::TEvPoisonPill::TPtr& ev) {
                STLOG(PRI_DEBUG, BS_PROXY, BSPM08, "TEvPoisonPill");
                Send(ev->Sender, new TEvents::TEvPoisonTaken);
                PassAway();
            }

            STATEFN(StateFunc) {
                switch (const ui32 type = ev->GetTypeRewrite()) {
                    hFunc(TEvBlobStorage::TEvPut, Handle);
                    hFunc(TEvBlobStorage::TEvGet, Handle);
                    hFunc(TEvBlobStorage::TEvBlock, Handle);
                    hFunc(TEvBlobStorage::TEvDiscover, Handle);
                    hFunc(TEvBlobStorage::TEvRange, Handle);
                    hFunc(TEvBlobStorage::TEvCollectGarbage, Handle);
                    hFunc(TEvBlobStorage::TEvStatus, Handle);

                    hFunc(TEvents::TEvPoisonPill, HandlePoison);

                    default:
                        Y_FAIL("unexpected event 0x%08" PRIx32, type);
                }
            }

        public:
            static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
                return NKikimrServices::TActivity::BS_PROXY_ACTOR;
            }

            TBlobStorageGroupProxyMockActor(TIntrusivePtr<NFake::TProxyDS> model)
                : TActor(&TBlobStorageGroupProxyMockActor::StateFunc)
                , Model(model ? std::move(model) : MakeIntrusive<NFake::TProxyDS>())
            {}
        };
    } // anon

    IActor *CreateBlobStorageGroupProxyMockActor(TIntrusivePtr<NFake::TProxyDS> model) {
        return new TBlobStorageGroupProxyMockActor(std::move(model));
    }

    IActor *CreateBlobStorageGroupProxyMockActor() {
        return new TBlobStorageGroupProxyMockActor(nullptr);
    }

} // NKikimr
