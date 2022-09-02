#pragma once

#include <library/cpp/actors/interconnect/poller_actor.h>
#include "pg_proxy_config.h"
#include "pg_sock64.h"
#include "pg_proxy_ssl.h"

namespace NPG {

struct TEndpointInfo {
    TSslHelpers::TSslHolder<SSL_CTX> SecureContext;
};

class TSocketDescriptor : public NActors::TSharedDescriptor, public TNetworkConfig {
    std::unique_ptr<TNetworkConfig::TSocketType> Socket;
    std::shared_ptr<TEndpointInfo> Endpoint;

public:
    TSocketDescriptor(TSocketType&& s, std::shared_ptr<TEndpointInfo> endpoint)
        : Socket(std::make_unique<TNetworkConfig::TSocketType>(std::move(s)))
        , Endpoint(endpoint)
    {}

    int Listen(int backlog) {
        return Socket->Listen(backlog);
    }

    TIntrusivePtr<TSocketDescriptor> Accept(TSocketAddressType& addr) {
        std::optional<TNetworkConfig::TSocketType> s = Socket->Accept(addr);
        if (!s) {
            return {};
        }
        return new TSocketDescriptor(std::move(s).value(), Endpoint);
    }

    void SetNonBlock() {
        try {
            ::SetNonBlock(*Socket, true);
        }
        catch (const yexception&) {
        }
    }

    ssize_t Send(const void* data, size_t size) {
        return Socket->Send(data, size);
    }

    ssize_t Receive(void* data, size_t size) {
        return Socket->Recv(data, size);
    }

    void RequestPoller(NActors::TPollerToken::TPtr& pollerToken) {
        Socket->RequestPoller(pollerToken);
    }

    int UpgradeToSecure() {
        std::unique_ptr<TNetworkConfig::TSecureSocketType> socket = std::make_unique<TNetworkConfig::TSecureSocketType>(std::move(*Socket));
        int res = socket->SecureAccept(Endpoint->SecureContext.get());
        Socket.reset(socket.release());
        return res;
    }

    void Shutdown() {
        ::shutdown(*Socket, SHUT_RDWR);
    }

    SOCKET GetRawSocket() const {
        return *Socket;
    }

    int GetDescriptor() override {
        return GetRawSocket();
    }

    bool IsSslSupported() const {
        return Endpoint->SecureContext != nullptr;
    }
};

class TSocketBuffer : public TBuffer, public TNetworkConfig {
public:
    TSocketBuffer()
        : TBuffer(BUFFER_SIZE)
    {}

    bool EnsureEnoughSpaceAvailable(size_t need) {
        size_t avail = Avail();
        if (avail < need) {
            Reserve(Capacity() + need);
            return true;
        }
        return true;
    }

    // non-destructive variant of AsString
    TString AsString() const {
        return TString(Data(), Size());
    }
};

}
