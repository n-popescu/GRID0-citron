// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2025 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <memory>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/settings.h"
#include "common/socket_types.h"
#include "core/core.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/sockets/bsd.h"
#include "core/hle/service/sockets/deferred_poll_waker.h"
#include "core/hle/service/sockets/private_server.h"
#include "core/hle/service/sockets/sockets_translate.h"
#include "core/internal_network/network.h"
#include "core/internal_network/socket_proxy.h"
#include "core/internal_network/sockets.h"
#include "network/network.h"

using Common::Expected;
using Common::Unexpected;

namespace Service::Sockets {

// The private-server connection trace: the socket calls that follow a lookup answered with the
// private server's address, at Warning so that a log taken with the default filter still says
// how far the game got. See ArmPrivateServerTrace.
#define SWITCHNET_TRACE(format, ...)                                                               \
    do {                                                                                           \
        if (TakePrivateServerTrace()) {                                                            \
            LOG_WARNING(Service, "SwitchNet trace: " format, ##__VA_ARGS__);                       \
        }                                                                                          \
    } while (0)

namespace {
/// "fd:events->revents" for each entry of a pollfd array, for the trace.
std::string DescribePollFds(std::span<const u8> pollfds, s32 nfds) {
    std::string out;
    for (s32 i = 0; i < nfds && (i + 1) * sizeof(PollFD) <= pollfds.size(); ++i) {
        PollFD pollfd;
        std::memcpy(&pollfd, pollfds.data() + i * sizeof(PollFD), sizeof(PollFD));
        out += fmt::format("{}{}:{:#x}->{:#x}", i ? " " : "", pollfd.fd,
                           static_cast<u16>(pollfd.events), static_cast<u16>(pollfd.revents));
    }
    return out;
}
} // namespace

namespace {

bool IsConnectionBased(Type type) {
    switch (type) {
    case Type::STREAM:
        return true;
    case Type::DGRAM:
        return false;
    default:
        UNIMPLEMENTED_MSG("Unimplemented type={}", type);
        return false;
    }
}

template <typename T>
T GetValue(std::span<const u8> buffer) {
    T t{};
    std::memcpy(&t, buffer.data(), std::min(sizeof(T), buffer.size()));
    return t;
}

template <typename T>
void PutValue(std::span<u8> buffer, const T& t) {
    std::memcpy(buffer.data(), &t, std::min(sizeof(T), buffer.size()));
}

class OfflineSocket final : public Network::SocketBase {
public:
    Network::Errno Initialize(Network::Domain domain_, Network::Type type_,
                              Network::Protocol protocol_) override {
        domain = domain_;
        type = type_;
        protocol = protocol_;
        return Network::Errno::SUCCESS;
    }

    Network::Errno Close() override {
        opened = false;
        return Network::Errno::SUCCESS;
    }

    std::pair<AcceptResult, Network::Errno> Accept() override {
        return {AcceptResult{}, Network::Errno::NETDOWN};
    }

    Network::Errno Connect(Network::SockAddrIn) override {
        return Network::Errno::NETDOWN;
    }

    std::pair<Network::SockAddrIn, Network::Errno> GetPeerName() override {
        return {{}, Network::Errno::NOTCONN};
    }

    std::pair<Network::SockAddrIn, Network::Errno> GetSockName() override {
        return {{}, Network::Errno::SUCCESS};
    }

    Network::Errno Bind(Network::SockAddrIn) override {
        return Network::Errno::SUCCESS;
    }

    Network::Errno Listen(s32) override {
        return Network::Errno::SUCCESS;
    }

    Network::Errno Shutdown(Network::ShutdownHow) override {
        return Network::Errno::SUCCESS;
    }

    std::pair<s32, Network::Errno> Recv(int, std::span<u8>) override {
        return {-1, Network::Errno::AGAIN};
    }

    std::pair<s32, Network::Errno> RecvFrom(int, std::span<u8>, Network::SockAddrIn*) override {
        return {-1, Network::Errno::AGAIN};
    }

    std::pair<s32, Network::Errno> Send(std::span<const u8> message, int) override {
        return {static_cast<s32>(message.size()), Network::Errno::SUCCESS};
    }

    std::pair<s32, Network::Errno> SendTo(u32, std::span<const u8> message,
                                          const Network::SockAddrIn*) override {
        return {static_cast<s32>(message.size()), Network::Errno::SUCCESS};
    }

    Network::Errno SetLinger(bool, u32) override {
        return Network::Errno::SUCCESS;
    }

    Network::Errno SetReuseAddr(bool) override {
        return Network::Errno::SUCCESS;
    }

    Network::Errno SetKeepAlive(bool) override {
        return Network::Errno::SUCCESS;
    }

    Network::Errno SetNoDelay(bool) override {
        return Network::Errno::SUCCESS;
    }

    std::pair<bool, Network::Errno> GetNoDelay() override {
        return {true, Network::Errno::SUCCESS};
    }

    Network::Errno SetIpOption(Network::IpOption, int) override {
        return Network::Errno::SUCCESS;
    }

    Network::Errno SetBroadcast(bool) override {
        return Network::Errno::SUCCESS;
    }

    Network::Errno SetSndBuf(u32) override {
        return Network::Errno::SUCCESS;
    }

    Network::Errno SetRcvBuf(u32) override {
        return Network::Errno::SUCCESS;
    }

    Network::Errno SetSndTimeo(u32) override {
        return Network::Errno::SUCCESS;
    }

    Network::Errno SetRcvTimeo(u32) override {
        return Network::Errno::SUCCESS;
    }

    Network::Errno SetNonBlock(bool) override {
        return Network::Errno::SUCCESS;
    }

    std::pair<Network::Errno, Network::Errno> GetPendingError() override {
        return {Network::Errno::SUCCESS, Network::Errno::SUCCESS};
    }

    bool IsOpened() const override {
        return opened;
    }

    void HandleProxyPacket(const Network::ProxyPacket&) override {}

private:
    Network::Domain domain = Network::Domain::INET;
    Network::Type type = Network::Type::DGRAM;
    Network::Protocol protocol = Network::Protocol::UDP;
    bool opened = true;
};

} // Anonymous namespace

void BSD::PollWork::Execute(BSD* bsd) {
    std::tie(ret, bsd_errno) = bsd->PollImpl(write_buffer, read_buffer, nfds, timeout);
    SWITCHNET_TRACE("poll nfds={} timeout={} -> {} errno={} [{}]", nfds, timeout, ret,
                    static_cast<u32>(bsd_errno), DescribePollFds(write_buffer, nfds));
}

void BSD::PollWork::Response(HLERequestContext& ctx) {
    if (write_buffer.size() > 0) {
        ctx.WriteBuffer(write_buffer);
    }

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD::AcceptWork::Execute(BSD* bsd) {
    std::tie(ret, bsd_errno) = bsd->AcceptImpl(fd, write_buffer);
}

void BSD::AcceptWork::Response(HLERequestContext& ctx) {
    if (write_buffer.size() > 0) {
        ctx.WriteBuffer(write_buffer);
    }

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
    rb.Push<u32>(static_cast<u32>(write_buffer.size()));
}

void BSD::ConnectWork::Execute(BSD* bsd) {
    bsd_errno = bsd->ConnectImpl(fd, addr);
    if (addr.size() == sizeof(SockAddrIn)) {
        const auto addr_in = GetValue<SockAddrIn>(addr);
        SWITCHNET_TRACE("connect fd={} to {}:{} -> errno={}", fd,
                        Network::IPv4AddressToString(addr_in.ip), addr_in.portno,
                        static_cast<u32>(bsd_errno));
    } else {
        SWITCHNET_TRACE("connect fd={} with a {}-byte address -> errno={}", fd, addr.size(),
                        static_cast<u32>(bsd_errno));
    }
}

void BSD::ConnectWork::Response(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(bsd_errno == Errno::SUCCESS ? 0 : -1);
    rb.PushEnum(bsd_errno);
}

void BSD::RecvWork::Execute(BSD* bsd) {
    std::tie(ret, bsd_errno) = bsd->RecvImpl(fd, flags, message);
    SWITCHNET_TRACE("recv fd={} len={} flags={:#x} -> {} errno={}", fd, message.size(), flags, ret,
                    static_cast<u32>(bsd_errno));
}

void BSD::RecvWork::Response(HLERequestContext& ctx) {
    ctx.WriteBuffer(message);

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD::RecvFromWork::Execute(BSD* bsd) {
    std::tie(ret, bsd_errno) = bsd->RecvFromImpl(fd, flags, message, addr, addr_length);
    SWITCHNET_TRACE("recvfrom fd={} len={} -> {} errno={} addrlen={}", fd, message.size(), ret,
                    static_cast<u32>(bsd_errno), addr_length);
}

void BSD::RecvFromWork::Response(HLERequestContext& ctx) {
    ctx.WriteBuffer(message, 0);
    if (!addr.empty()) {
        ctx.WriteBuffer(addr, 1);
    }

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
    rb.Push<u32>(addr_length);
}

void BSD::SendWork::Execute(BSD* bsd) {
    std::tie(ret, bsd_errno) = bsd->SendImpl(fd, flags, message);
    SWITCHNET_TRACE("send fd={} len={} flags={:#x} -> {} errno={}", fd, message.size(), flags, ret,
                    static_cast<u32>(bsd_errno));
}

void BSD::SendWork::Response(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD::SendToWork::Execute(BSD* bsd) {
    std::tie(ret, bsd_errno) = bsd->SendToImpl(fd, flags, message, addr);
    SWITCHNET_TRACE("sendto fd={} len={} -> {} errno={}", fd, message.size(), ret,
                    static_cast<u32>(bsd_errno));
}

void BSD::SendToWork::Response(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD::RegisterClient(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    // Read LibraryConfigData structure
    struct LibraryConfigData {
        u32 version;
        u32 tcp_tx_buf_size;
        u32 tcp_rx_buf_size;
        u32 tcp_tx_buf_max_size;
        u32 tcp_rx_buf_max_size;
        u32 udp_tx_buf_size;
        u32 udp_rx_buf_size;
        u32 sb_efficiency;
    };

    const auto config = rp.PopRaw<LibraryConfigData>();
    const u64 transfer_memory_size = rp.Pop<u64>();
    [[maybe_unused]] const auto transfer_memory_handle = ctx.GetCopyHandle(0);
    const u64 pid = ctx.GetPID();

    LOG_INFO(Service, "called, version={} pid={} transfer_memory_size={:#x}",
             config.version, pid, transfer_memory_size);
    LOG_DEBUG(Service, "  TCP: tx={:#x} rx={:#x} tx_max={:#x} rx_max={:#x}",
              config.tcp_tx_buf_size, config.tcp_rx_buf_size,
              config.tcp_tx_buf_max_size, config.tcp_rx_buf_max_size);
    LOG_DEBUG(Service, "  UDP: tx={:#x} rx={:#x} sb_efficiency={}",
              config.udp_tx_buf_size, config.udp_rx_buf_size, config.sb_efficiency);

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<s32>(0); // bsd errno
}

void BSD::StartMonitoring(HLERequestContext& ctx) {
    LOG_INFO(Service, "called");

    // StartMonitoring initializes network event monitoring for BSD sockets
    // This command has no documented input parameters in switchbrew
    // It enables proper event handling for socket operations
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void BSD::Socket(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const u32 domain = rp.Pop<u32>();
    const u32 type = rp.Pop<u32>();
    const u32 protocol = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. domain={} type={} protocol={}", domain, type, protocol);

    const auto [fd, bsd_errno] = SocketImpl(static_cast<Domain>(domain), static_cast<Type>(type),
                                            static_cast<Protocol>(protocol));
    SWITCHNET_TRACE("socket domain={} type={:#x} protocol={} -> fd={} errno={}", domain, type,
                    protocol, fd, static_cast<u32>(bsd_errno));

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(fd);
    rb.PushEnum(bsd_errno);
}

void BSD::Select(HLERequestContext& ctx) {
    LOG_DEBUG(Service, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 4};

    rb.Push(ResultSuccess);
    rb.Push<u32>(0); // ret
    rb.Push<u32>(0); // bsd errno
}

void BSD::Poll(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 nfds = rp.Pop<s32>();
    const s32 timeout = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. nfds={} timeout={}", nfds, timeout);

    // A re-run of a deferred poll goes straight back to it: the guest's buffer may no longer hold
    // this poll's descriptors (see PollWithEventFd), so it cannot be looked at again to decide.
    if (deferred_poll_waker &&
        (IsDeferredPoll(ctx) || PollIncludesEventFd(ctx.ReadBuffer(), nfds))) {
        PollWithEventFd(ctx, nfds, timeout);
        return;
    }

    ExecuteWork(ctx, PollWork{
                         .nfds = nfds,
                         .timeout = timeout,
                         .read_buffer = ctx.ReadBuffer(),
                         .write_buffer = std::vector<u8>(ctx.GetWriteBufferSize()),
                     });
}

void BSD::Accept(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={}", fd);

    ExecuteWork(ctx, AcceptWork{
                         .fd = fd,
                         .write_buffer = std::vector<u8>(ctx.GetWriteBufferSize()),
                     });
}

void BSD::Bind(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} addrlen={}", fd, ctx.GetReadBufferSize());
    BuildErrnoResponse(ctx, BindImpl(fd, ctx.ReadBuffer()));
}

void BSD::Connect(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} addrlen={}", fd, ctx.GetReadBufferSize());

    ExecuteWork(ctx, ConnectWork{
                         .fd = fd,
                         .addr = ctx.ReadBuffer(),
                     });
}

void BSD::GetPeerName(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={}", fd);

    std::vector<u8> write_buffer(ctx.GetWriteBufferSize());
    const Errno bsd_errno = GetPeerNameImpl(fd, write_buffer);

    ctx.WriteBuffer(write_buffer);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<s32>(bsd_errno != Errno::SUCCESS ? -1 : 0);
    rb.PushEnum(bsd_errno);
    rb.Push<u32>(static_cast<u32>(write_buffer.size()));
}

void BSD::GetSockName(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={}", fd);

    std::vector<u8> write_buffer(ctx.GetWriteBufferSize());
    const Errno bsd_errno = GetSockNameImpl(fd, write_buffer);

    ctx.WriteBuffer(write_buffer);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<s32>(bsd_errno != Errno::SUCCESS ? -1 : 0);
    rb.PushEnum(bsd_errno);
    rb.Push<u32>(static_cast<u32>(write_buffer.size()));
}

void BSD::GetSockOpt(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const u32 level = rp.Pop<u32>();
    const auto optname = static_cast<OptName>(rp.Pop<u32>());

    std::vector<u8> optval(ctx.GetWriteBufferSize());

    LOG_DEBUG(Service, "called. fd={} level={} optname=0x{:x} len=0x{:x}", fd, level, optname,
              optval.size());

    const Errno err = GetSockOptImpl(fd, level, optname, optval);
    SWITCHNET_TRACE("getsockopt fd={} level={:#x} optname={:#x} -> errno={}", fd, level, optname,
                    static_cast<u32>(err));

    ctx.WriteBuffer(optval);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<s32>(err == Errno::SUCCESS ? 0 : -1);
    rb.PushEnum(err);
    rb.Push<u32>(static_cast<u32>(optval.size()));
}

void BSD::Listen(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const s32 backlog = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} backlog={}", fd, backlog);

    BuildErrnoResponse(ctx, ListenImpl(fd, backlog));
}

void BSD::Fcntl(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const u32 cmd = rp.Pop<u32>();
    const s32 arg = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} cmd={} arg={}", fd, cmd, arg);

    const auto [ret, bsd_errno] = FcntlImpl(fd, static_cast<FcntlCmd>(cmd), arg);
    SWITCHNET_TRACE("fcntl fd={} cmd={} arg={:#x} -> {} errno={}", fd, cmd, arg, ret,
                    static_cast<u32>(bsd_errno));

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD::SetSockOpt(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const u32 level = rp.Pop<u32>();
    const OptName optname = static_cast<OptName>(rp.Pop<u32>());
    const auto optval = ctx.ReadBuffer();

    LOG_DEBUG(Service, "called. fd={} level={} optname=0x{:x} optlen={}", fd, level,
              static_cast<u32>(optname), optval.size());

    const Errno set_errno = SetSockOptImpl(fd, level, optname, optval);
    if (set_errno == Errno::SUCCESS && IsFileDescriptorValid(fd)) {
        file_descriptors[fd]->set_options[(static_cast<u64>(level) << 32) |
                                          static_cast<u32>(optname)]
            .assign(optval.begin(), optval.end());
    }
    SWITCHNET_TRACE("setsockopt fd={} level={:#x} optname={:#x} -> errno={}", fd, level, optname,
                    static_cast<u32>(set_errno));
    BuildErrnoResponse(ctx, set_errno);
}

void BSD::Shutdown(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const s32 how = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} how={}", fd, how);

    BuildErrnoResponse(ctx, ShutdownImpl(fd, how));
}

void BSD::Recv(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const u32 flags = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. fd={} flags=0x{:x} len={}", fd, flags, ctx.GetWriteBufferSize());

    ExecuteWork(ctx, RecvWork{
                         .fd = fd,
                         .flags = flags,
                         .message = std::vector<u8>(ctx.GetWriteBufferSize()),
                     });
}

void BSD::RecvFrom(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const u32 flags = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. fd={} flags=0x{:x} len={} addrlen={}", fd, flags,
              ctx.GetWriteBufferSize(0), ctx.GetWriteBufferSize(1));

    ExecuteWork(ctx, RecvFromWork{
                         .fd = fd,
                         .flags = flags,
                         .message = std::vector<u8>(ctx.GetWriteBufferSize(0)),
                         .addr = std::vector<u8>(ctx.GetWriteBufferSize(1)),
                     });
}

void BSD::Send(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const u32 flags = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. fd={} flags=0x{:x} len={}", fd, flags, ctx.GetReadBufferSize());

    ExecuteWork(ctx, SendWork{
                         .fd = fd,
                         .flags = flags,
                         .message = ctx.ReadBuffer(),
                     });
}

void BSD::SendTo(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const u32 flags = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. fd={} flags=0x{} len={} addrlen={}", fd, flags,
              ctx.GetReadBufferSize(0), ctx.GetReadBufferSize(1));

    ExecuteWork(ctx, SendToWork{
                         .fd = fd,
                         .flags = flags,
                         .message = ctx.ReadBuffer(0),
                         .addr = ctx.ReadBuffer(1),
                     });
}

void BSD::Write(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} len={}", fd, ctx.GetReadBufferSize());

    if (const auto eventfd = GetEventFd(fd)) {
        const auto buffer = ctx.ReadBuffer();
        u64 add = 0;
        Errno bsd_errno = Errno::SUCCESS;
        if (buffer.size() < sizeof(u64)) {
            bsd_errno = Errno::INVAL;
        } else {
            std::memcpy(&add, buffer.data(), sizeof(u64));
            std::scoped_lock lock{eventfd->mutex};
            if (add == ~u64{0}) {
                bsd_errno = Errno::INVAL;
            } else if (eventfd->value > ~u64{0} - 1 - add) {
                // Always non-blocking: a blocking write would stall a service thread.
                bsd_errno = Errno::AGAIN;
            } else {
                eventfd->value += add;
            }
        }
        if (bsd_errno == Errno::SUCCESS && deferred_poll_waker) {
            deferred_poll_waker->Wake();
        }
        SWITCHNET_TRACE("eventfd write fd={} +{} -> errno={}", fd, add, static_cast<u32>(bsd_errno));
        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<s32>(bsd_errno == Errno::SUCCESS ? static_cast<s32>(sizeof(u64)) : -1);
        rb.PushEnum(bsd_errno);
        return;
    }

    ExecuteWork(ctx, SendWork{
                         .fd = fd,
                         .flags = 0,
                         .message = ctx.ReadBuffer(),
                     });
}

void BSD::Read(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    if (const auto eventfd = GetEventFd(fd)) {
        Errno bsd_errno = Errno::SUCCESS;
        u64 out = 0;
        if (ctx.GetWriteBufferSize() < sizeof(u64)) {
            bsd_errno = Errno::INVAL;
        } else {
            std::scoped_lock lock{eventfd->mutex};
            if (eventfd->value == 0) {
                // Always non-blocking, for the same reason as Write.
                bsd_errno = Errno::AGAIN;
            } else if (eventfd->semaphore) {
                out = 1;
                --eventfd->value;
            } else {
                out = std::exchange(eventfd->value, 0);
            }
        }
        if (bsd_errno == Errno::SUCCESS) {
            ctx.WriteBuffer(&out, sizeof(out));
        }
        SWITCHNET_TRACE("eventfd read fd={} -> {} errno={}", fd, out, static_cast<u32>(bsd_errno));
        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<s32>(bsd_errno == Errno::SUCCESS ? static_cast<s32>(sizeof(u64)) : -1);
        rb.PushEnum(bsd_errno);
        return;
    }

    LOG_WARNING(Service, "(STUBBED) called. fd={} len={}", fd, ctx.GetWriteBufferSize());

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<u32>(0); // ret
    rb.Push<u32>(0); // bsd errno
}

void BSD::Close(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={}", fd);

    const Errno close_errno = CloseImpl(fd);
    SWITCHNET_TRACE("close fd={} -> errno={}", fd, static_cast<u32>(close_errno));
    BuildErrnoResponse(ctx, close_errno);
}

void BSD::DuplicateSocket(HLERequestContext& ctx) {
    struct InputParameters {
        s32 fd;
        u64 reserved;
    };
    static_assert(sizeof(InputParameters) == 0x10);

    struct OutputParameters {
        s32 ret;
        Errno bsd_errno;
    };
    static_assert(sizeof(OutputParameters) == 0x8);

    IPC::RequestParser rp{ctx};
    auto input = rp.PopRaw<InputParameters>();

    Expected<s32, Errno> res = DuplicateSocketImpl(input.fd);
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.PushRaw(OutputParameters{
        .ret = res.value_or(0),
        .bsd_errno = res ? Errno::SUCCESS : res.error(),
    });
}

void BSD::EventFd(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const u64 initval = rp.Pop<u64>();
    const u32 flags = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. initval={}, flags={}", initval, flags);

    s32 fd;
    {
        std::lock_guard lock(fd_table_mutex);
        fd = FindFreeFileDescriptorHandle();
        if (fd >= 0) {
            auto state = std::make_shared<EventFdState>();
            state->value = initval;
            state->semaphore = (flags & 1) != 0; // EFD_SEMAPHORE
            file_descriptors[fd] = FileDescriptor{};
            file_descriptors[fd]->eventfd = std::move(state);
        }
    }

    SWITCHNET_TRACE("eventfd initval={} flags={:#x} -> fd={}", initval, flags, fd);

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(fd >= 0 ? fd : -1);
    rb.PushEnum(fd >= 0 ? Errno::SUCCESS : Errno::MFILE);
}

void BSD::RegisterClientShared(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called RegisterClientShared");
    IPC::ResponseBuilder rb{ctx, 4}; // Match RegisterClient response style
    rb.Push(ResultSuccess);
    rb.Push<s32>(0); // ret (0 for success)
    rb.Push<s32>(0); // BSD errno (0 for success, consistent with RegisterClient stub)
}

template <typename Work>
void BSD::ExecuteWork(HLERequestContext& ctx, Work work) {
    work.Execute(this);
    work.Response(ctx);
}

std::pair<s32, Errno> BSD::SocketImpl(Domain domain, Type type, Protocol protocol) {
    // Every address this service handles is IPv4 (SockAddrIn), so an IPv6 socket could be created
    // but never bound or connected. Refusing it is the truthful answer, and the one gRPC plans
    // for: its probe_ipv6_once disables AF_INET6 when socket() fails, and it then connects to an
    // IPv4 address -- even one it holds as ::ffff:a.b.c.d -- over an AF_INET socket with a plain
    // sockaddr_in. Splatoon 3's online client otherwise connects with a 28-byte v4-mapped address
    // (NextendoNetwork's capture, facts only), which nothing here can read.
    if (domain == Domain::INET6) {
        LOG_WARNING(Service, "Refusing an IPv6 socket: only IPv4 is emulated");
        return {-1, Errno::AFNOSUPPORT};
    }

    if (type == Type::SEQPACKET) {
        UNIMPLEMENTED_MSG("SOCK_SEQPACKET errno management");
    } else if (type == Type::RAW && (domain != Domain::INET || protocol != Protocol::ICMP)) {
        UNIMPLEMENTED_MSG("SOCK_RAW errno management");
    }

    [[maybe_unused]] const bool unk_flag = (static_cast<u32>(type) & 0x20000000) != 0;
    UNIMPLEMENTED_IF_MSG(unk_flag, "Unknown flag in type");
    type = static_cast<Type>(static_cast<u32>(type) & ~0x20000000);

    const s32 fd = FindFreeFileDescriptorHandle();
    if (fd < 0) {
        LOG_ERROR(Service, "No more file descriptors available");
        return {-1, Errno::MFILE};
    }

    file_descriptors[fd] = FileDescriptor{};
    FileDescriptor& descriptor = *file_descriptors[fd];
    // ENONMEM might be thrown here

    auto room_member = room_network.GetRoomMember().lock();
    const bool using_proxy = room_member && room_member->IsConnected();

    LOG_INFO(Service, "New socket fd={} domain={} type={} protocol={} proxy={}",
             fd, domain, type, protocol, using_proxy);

    // Store socket type information for pooling
    descriptor.domain = Translate(domain);
    descriptor.type = Translate(type);
    descriptor.protocol = Translate(protocol);
    descriptor.is_connection_based = IsConnectionBased(type);

    if (Settings::values.airplane_mode.GetValue()) {
        descriptor.socket = std::make_shared<OfflineSocket>();
        descriptor.socket->Initialize(descriptor.domain, descriptor.type, descriptor.protocol);
        LOG_INFO(Service, "Airplane mode: created offline socket fd={}", fd);
    } else if (using_proxy) {
        descriptor.socket = std::make_shared<Network::ProxySocket>(room_network);
        descriptor.socket->Initialize(descriptor.domain, descriptor.type, descriptor.protocol);
        LOG_DEBUG(Service, "Created new ProxySocket for fd={}", fd);
    } else {
        descriptor.socket = std::make_shared<Network::Socket>();
        descriptor.socket->Initialize(descriptor.domain, descriptor.type, descriptor.protocol);
    }

    return {fd, Errno::SUCCESS};
}

std::pair<s32, Errno> BSD::PollImpl(std::vector<u8>& write_buffer, std::span<const u8> read_buffer,
                                    s32 nfds, s32 timeout) {
    if (nfds <= 0) {
        // When no entries are provided, -1 is returned with errno zero
        return {-1, Errno::SUCCESS};
    }
    if (read_buffer.size() < nfds * sizeof(PollFD)) {
        return {-1, Errno::INVAL};
    }
    if (write_buffer.size() < nfds * sizeof(PollFD)) {
        return {-1, Errno::INVAL};
    }

    std::vector<PollFD> fds(nfds);
    std::memcpy(fds.data(), read_buffer.data(), nfds * sizeof(PollFD));

    // Initialize revents to zero to ensure clean state
    for (PollFD& pollfd : fds) {
        pollfd.revents = PollEvents{};
    }

    if (timeout >= 0) {
        const s64 seconds = timeout / 1000;
        const u64 nanoseconds = 1'000'000 * (static_cast<u64>(timeout) % 1000);

        if (seconds < 0) {
            return {-1, Errno::INVAL};
        }
        if (nanoseconds > 999'999'999) {
            return {-1, Errno::INVAL};
        }
    } else if (timeout != -1) {
        return {-1, Errno::INVAL};
    }

    for (PollFD& pollfd : fds) {
        ASSERT(False(pollfd.revents));

        if (pollfd.fd > static_cast<s32>(MAX_FD) || pollfd.fd < 0) {
            LOG_ERROR(Service, "File descriptor handle={} is invalid", pollfd.fd);
            pollfd.revents = PollEvents{};
            return {0, Errno::SUCCESS};
        }

        const std::optional<FileDescriptor>& descriptor = file_descriptors[pollfd.fd];
        if (!descriptor) {
            LOG_TRACE(Service, "File descriptor handle={} is not allocated", pollfd.fd);
            pollfd.revents = PollEvents::Nval;
            return {0, Errno::SUCCESS};
        }
    }

    std::vector<Network::PollFD> host_pollfds(fds.size());
    std::transform(fds.begin(), fds.end(), host_pollfds.begin(), [this](PollFD pollfd) {
        Network::PollFD result;
        result.socket = file_descriptors[pollfd.fd]->socket.get();
        result.events = Translate(pollfd.events);
        result.revents = Network::PollEvents{};
        return result;
    });

    const auto result = Network::Poll(host_pollfds, timeout);

    const size_t num = host_pollfds.size();
    for (size_t i = 0; i < num; ++i) {
        fds[i].revents = Translate(host_pollfds[i].revents);
    }
    std::memcpy(write_buffer.data(), fds.data(), nfds * sizeof(PollFD));

    return Translate(result);
}

std::pair<s32, Errno> BSD::AcceptImpl(s32 fd, std::vector<u8>& write_buffer) {
    if (!IsFileDescriptorValid(fd)) {
        return {-1, Errno::BADF};
    }

    const s32 new_fd = FindFreeFileDescriptorHandle();
    if (new_fd < 0) {
        LOG_ERROR(Service, "No more file descriptors available");
        return {-1, Errno::MFILE};
    }

    FileDescriptor& descriptor = *file_descriptors[fd];
    auto [result, bsd_errno] = descriptor.socket->Accept();
    if (bsd_errno != Network::Errno::SUCCESS) {
        return {-1, Translate(bsd_errno)};
    }

    file_descriptors[new_fd] = FileDescriptor{};
    FileDescriptor& new_descriptor = *file_descriptors[new_fd];
    new_descriptor.socket = std::move(result.socket);
    new_descriptor.is_connection_based = descriptor.is_connection_based;

    const SockAddrIn guest_addr_in = Translate(result.sockaddr_in);
    PutValue(write_buffer, guest_addr_in);

    return {new_fd, Errno::SUCCESS};
}

Errno BSD::BindImpl(s32 fd, std::span<const u8> addr) {
    if (!IsFileDescriptorValid(fd)) {
        LOG_ERROR(Service, "Bind failed: Invalid fd={}", fd);
        return Errno::BADF;
    }
    if (!file_descriptors[fd]->socket)
        return Errno::BADF;

    if (addr.size() != sizeof(SockAddrIn)) {
        // Anything but a sockaddr_in cannot be read as one; guessing at it binds somewhere the
        // guest never asked for.
        LOG_WARNING(Service, "Bind fd={} refused: {}-byte address, only sockaddr_in is emulated",
                    fd, addr.size());
        return addr.size() >= 2 && addr[1] == static_cast<u8>(Domain::INET6) ? Errno::AFNOSUPPORT
                                                                              : Errno::INVAL;
    }
    auto addr_in = GetValue<SockAddrIn>(addr);

    LOG_INFO(Service, "Bind fd={} to {}:{}", fd, Network::IPv4AddressToString(addr_in.ip),
             addr_in.portno);

    const auto result = Translate(file_descriptors[fd]->socket->Bind(Translate(addr_in)));
    if (result != Errno::SUCCESS) {
        LOG_ERROR(Service, "Bind fd={} failed with errno={}", fd, static_cast<int>(result));
    }
    return result;
}

Errno BSD::ConnectImpl(s32 fd, std::span<const u8> addr) {
    if (!IsFileDescriptorValid(fd)) {
        LOG_ERROR(Service, "Connect failed: Invalid fd={}", fd);
        return Errno::BADF;
    }
    if (!file_descriptors[fd]->socket)
        return Errno::BADF;
    if (Settings::values.airplane_mode.GetValue()) {
        return Errno::CONNREFUSED;
    }

    if (addr.size() != sizeof(SockAddrIn)) {
        // Read as a sockaddr_in, a sockaddr_in6's flow info became the address: a connect to
        // 0.0.0.0 that looks like the server refusing the game.
        LOG_WARNING(Service, "Connect fd={} refused: {}-byte address, only sockaddr_in is emulated",
                    fd, addr.size());
        return addr.size() >= 2 && addr[1] == static_cast<u8>(Domain::INET6) ? Errno::AFNOSUPPORT
                                                                              : Errno::INVAL;
    }
    auto addr_in = GetValue<SockAddrIn>(addr);

    LOG_INFO(Service, "Connect fd={} to {}:{}", fd, Network::IPv4AddressToString(addr_in.ip),
             addr_in.portno);

    const auto result = Translate(file_descriptors[fd]->socket->Connect(Translate(addr_in)));
    if (result != Errno::SUCCESS) {
        LOG_ERROR(Service, "Connect fd={} failed with errno={}", fd, static_cast<int>(result));
    } else {
        LOG_INFO(Service, "Connect fd={} succeeded", fd);
    }
    return result;
}

Errno BSD::GetPeerNameImpl(s32 fd, std::vector<u8>& write_buffer) {
    if (!IsFileDescriptorValid(fd)) {
        return Errno::BADF;
    }
    if (!file_descriptors[fd]->socket)
        return Errno::BADF;

    const auto [addr_in, bsd_errno] = file_descriptors[fd]->socket->GetPeerName();
    if (bsd_errno != Network::Errno::SUCCESS) {
        return Translate(bsd_errno);
    }
    const SockAddrIn guest_addrin = Translate(addr_in);

    ASSERT(write_buffer.size() >= sizeof(guest_addrin));
    write_buffer.resize(sizeof(guest_addrin));
    PutValue(write_buffer, guest_addrin);
    return Translate(bsd_errno);
}

Errno BSD::GetSockNameImpl(s32 fd, std::vector<u8>& write_buffer) {
    if (!IsFileDescriptorValid(fd)) {
        return Errno::BADF;
    }
    if (!file_descriptors[fd]->socket)
        return Errno::BADF;

    const auto [addr_in, bsd_errno] = file_descriptors[fd]->socket->GetSockName();
    if (bsd_errno != Network::Errno::SUCCESS) {
        return Translate(bsd_errno);
    }
    const SockAddrIn guest_addrin = Translate(addr_in);

    ASSERT(write_buffer.size() >= sizeof(guest_addrin));
    write_buffer.resize(sizeof(guest_addrin));
    PutValue(write_buffer, guest_addrin);
    return Translate(bsd_errno);
}

Errno BSD::ListenImpl(s32 fd, s32 backlog) {
    if (!IsFileDescriptorValid(fd)) {
        return Errno::BADF;
    }
    if (!file_descriptors[fd]->socket)
        return Errno::BADF;
    return Translate(file_descriptors[fd]->socket->Listen(backlog));
}

std::pair<s32, Errno> BSD::FcntlImpl(s32 fd, FcntlCmd cmd, s32 arg) {
    if (!IsFileDescriptorValid(fd)) {
        return {-1, Errno::BADF};
    }
    if (file_descriptors[fd]->eventfd) {
        // Reads and writes on an eventfd never block here, so O_NONBLOCK is only remembered.
        if (cmd == FcntlCmd::GETFL) {
            return {file_descriptors[fd]->flags, Errno::SUCCESS};
        }
        if (cmd == FcntlCmd::SETFL) {
            file_descriptors[fd]->flags = arg;
            return {0, Errno::SUCCESS};
        }
        return {-1, Errno::INVAL};
    }
    if (!file_descriptors[fd]->socket)
        return {-1, Errno::BADF};

    FileDescriptor& descriptor = *file_descriptors[fd];

    switch (cmd) {
    case FcntlCmd::GETFL:
        ASSERT(arg == 0);
        return {descriptor.flags, Errno::SUCCESS};
    case FcntlCmd::SETFL: {
        const bool enable = (arg & Network::FLAG_O_NONBLOCK) != 0;
        const Errno bsd_errno = Translate(descriptor.socket->SetNonBlock(enable));
        if (bsd_errno != Errno::SUCCESS) {
            return {-1, bsd_errno};
        }
        descriptor.flags = arg;
        return {0, Errno::SUCCESS};
    }
    default:
        UNIMPLEMENTED_MSG("Unimplemented cmd={}", cmd);
        return {-1, Errno::SUCCESS};
    }
}

Errno BSD::GetSockOptImpl(s32 fd, u32 level, OptName optname, std::vector<u8>& optval) {
    if (!IsFileDescriptorValid(fd)) {
        return Errno::BADF;
    }
    if (!file_descriptors[fd]->socket)
        return Errno::BADF;

    Network::SocketBase* const socket = file_descriptors[fd]->socket.get();

    // gRPC reads TCP_NODELAY back after setting it and closes the socket if it cannot.
    if (level == static_cast<u32>(SocketLevel::TCP) &&
        static_cast<u32>(optname) == TCP_OPT_NODELAY) {
        auto [enabled, err] = socket->GetNoDelay();
        if (err == Network::Errno::SUCCESS) {
            ASSERT_OR_EXECUTE_MSG(
                optval.size() >= sizeof(u32), { return Errno::INVAL; },
                "Incorrect getsockopt option size");
            optval.resize(sizeof(u32));
            PutValue(optval, static_cast<u32>(enabled ? 1 : 0));
        }
        return Translate(err);
    }

    if (optname != OptName::ERROR_) {
        const auto& set = file_descriptors[fd]->set_options;
        if (const auto it = set.find((static_cast<u64>(level) << 32) | static_cast<u32>(optname));
            it != set.end() && optval.size() >= it->second.size()) {
            optval.assign(it->second.begin(), it->second.end());
            return Errno::SUCCESS;
        }
    }

    if (level != static_cast<u32>(SocketLevel::SOCKET)) {
        LOG_WARNING(Service, "(STUBBED) Unknown getsockopt level={}, returning INVAL", level);
        return Errno::INVAL;
    }

    switch (optname) {
    case OptName::ERROR_: {
        auto [pending_err, getsockopt_err] = socket->GetPendingError();
        if (getsockopt_err == Network::Errno::SUCCESS) {
            Errno translated_pending_err = Translate(pending_err);
            ASSERT_OR_EXECUTE_MSG(
                optval.size() == sizeof(Errno), { return Errno::INVAL; },
                "Incorrect getsockopt option size");
            optval.resize(sizeof(Errno));
            PutValue(optval, translated_pending_err);
        }
        return Translate(getsockopt_err);
    }
    default:
        LOG_WARNING(Service, "(STUBBED) Unimplemented optname={} (0x{:x}), returning INVAL",
                    static_cast<u32>(optname), static_cast<u32>(optname));
        return Errno::INVAL;
    }
}

Errno BSD::SetSockOptImpl(s32 fd, u32 level, OptName optname, std::span<const u8> optval) {
    if (!IsFileDescriptorValid(fd))
        return Errno::BADF;
    if (!file_descriptors[fd]->socket)
        return Errno::BADF;

    Network::SocketBase* const socket = file_descriptors[fd]->socket.get();

    // TCP_NODELAY is the only IPPROTO_TCP option a game is known to set. gRPC (Splatoon 3's
    // online client) sets it on every connection and treats a failure as fatal: it closed each
    // socket without ever calling connect, and the game showed a communication error.
    if (level == static_cast<u32>(SocketLevel::TCP)) {
        if (static_cast<u32>(optname) != TCP_OPT_NODELAY || optval.size() != sizeof(u32)) {
            LOG_WARNING(Service, "(STUBBED) Unknown IPPROTO_TCP optname=0x{:x}, returning INVAL",
                        static_cast<u32>(optname));
            return Errno::INVAL;
        }
        return Translate(socket->SetNoDelay(GetValue<u32>(optval) != 0));
    }

    // Pia (Splatoon 3's P2P layer) sets IP_TTL on its socket as it opens a session and gives up
    // when that fails: the room was created on the server, then dropped with a communication
    // error within half a second, before a single packet was sent.
    if (level == static_cast<u32>(SocketLevel::IP)) {
        const u32 name = static_cast<u32>(optname);
        if ((name != IP_OPT_TTL && name != IP_OPT_TOS) || optval.size() != sizeof(u32)) {
            LOG_WARNING(Service, "(STUBBED) Unknown IPPROTO_IP optname=0x{:x}, returning INVAL",
                        name);
            return Errno::INVAL;
        }
        return Translate(socket->SetIpOption(name == IP_OPT_TTL ? Network::IpOption::TTL
                                                                    : Network::IpOption::TOS,
                                             static_cast<int>(GetValue<u32>(optval))));
    }

    if (level != static_cast<u32>(SocketLevel::SOCKET)) {
        LOG_WARNING(Service, "(STUBBED) Unknown setsockopt level={}, returning INVAL", level);
        return Errno::INVAL;
    }

    // Horizon's own socket options live above the BSD range (0x80000000+). They tune Nintendo's
    // socket stack and have no host equivalent; refusing them makes gRPC give up on the socket
    // before connecting, exactly like a failed TCP_NODELAY, so they are accepted and ignored.
    if ((static_cast<u32>(optname) & 0x80000000) != 0) {
        LOG_DEBUG(Service, "Ignoring Horizon socket option 0x{:x}", static_cast<u32>(optname));
        return Errno::SUCCESS;
    }

    if (optname == OptName::LINGER) {
        if (optval.size() != sizeof(Linger)) {
            LOG_WARNING(Service, "LINGER optval size mismatch: expected {}, got {}", sizeof(Linger),
                        optval.size());
            return Errno::INVAL;
        }
        auto linger = GetValue<Linger>(optval);
        if (linger.onoff != 0 && linger.onoff != 1) {
            LOG_WARNING(Service, "Invalid LINGER onoff value: {}", linger.onoff);
            return Errno::INVAL;
        }

        return Translate(socket->SetLinger(linger.onoff != 0, linger.linger));
    }

    if (optval.size() != sizeof(u32)) {
        LOG_WARNING(Service, "optval size mismatch: expected {}, got {} for optname={}", sizeof(u32),
                    optval.size(), static_cast<u32>(optname));
        return Errno::INVAL;
    }
    auto value = GetValue<u32>(optval);

    if (static_cast<u32>(optname) == 0x200 || optname == OptName::BROADCAST) {
        socket->SetBroadcast(value != 0);
        return Errno::SUCCESS;
    }

    switch (optname) {
    case OptName::REUSEADDR:
        if (value != 0 && value != 1) {
            LOG_WARNING(Service, "Invalid REUSEADDR value: {}", value);
            return Errno::INVAL;
        }
        return Translate(socket->SetReuseAddr(value != 0));
    case OptName::KEEPALIVE:
        if (value != 0 && value != 1) {
            LOG_WARNING(Service, "Invalid KEEPALIVE value: {}", value);
            return Errno::INVAL;
        }
        return Translate(socket->SetKeepAlive(value != 0));
    case OptName::SNDBUF:
        return Translate(socket->SetSndBuf(value));
    case OptName::RCVBUF:
        return Translate(socket->SetRcvBuf(value));
    case OptName::SNDTIMEO:
        return Translate(socket->SetSndTimeo(value));
    case OptName::RCVTIMEO:
        return Translate(socket->SetRcvTimeo(value));
    case OptName::NOSIGPIPE:
        LOG_WARNING(Service, "(STUBBED) setting NOSIGPIPE to {}", value);
        return Errno::SUCCESS;
    default:
        LOG_WARNING(Service, "(STUBBED) Unimplemented optname={} (0x{:x}), returning INVAL",
                    static_cast<u32>(optname), static_cast<u32>(optname));
        return Errno::INVAL;
    }
}

Errno BSD::ShutdownImpl(s32 fd, s32 how) {
    if (!IsFileDescriptorValid(fd)) {
        return Errno::BADF;
    }
    if (!file_descriptors[fd]->socket)
        return Errno::BADF;
    const Network::ShutdownHow host_how = Translate(static_cast<ShutdownHow>(how));
    return Translate(file_descriptors[fd]->socket->Shutdown(host_how));
}

std::pair<s32, Errno> BSD::RecvImpl(s32 fd, u32 flags, std::vector<u8>& message) {
    if (!IsFileDescriptorValid(fd)) {
        return {-1, Errno::BADF};
    }

    FileDescriptor& descriptor = *file_descriptors[fd];
    if (Settings::values.airplane_mode.GetValue()) {
        return {-1, Errno::AGAIN};
    }
    // Datagram sockets receive too: see RecvFromImpl.

    // Apply flags
    using Network::FLAG_MSG_DONTWAIT;
    using Network::FLAG_O_NONBLOCK;
    if ((flags & FLAG_MSG_DONTWAIT) != 0) {
        flags &= ~FLAG_MSG_DONTWAIT;
        if ((descriptor.flags & FLAG_O_NONBLOCK) == 0) {
            descriptor.socket->SetNonBlock(true);
        }
    }

    const auto [ret, bsd_errno] = Translate(descriptor.socket->Recv(flags, message));

    // Restore original state
    if ((descriptor.flags & FLAG_O_NONBLOCK) == 0) {
        descriptor.socket->SetNonBlock(false);
    }

    return {ret, bsd_errno};
}

std::pair<s32, Errno> BSD::RecvFromImpl(s32 fd, u32 flags, std::vector<u8>& message,
                                        std::vector<u8>& addr, u32& addr_length) {
    addr_length = 0;
    if (!IsFileDescriptorValid(fd)) {
        return {-1, Errno::BADF};
    }

    FileDescriptor& descriptor = *file_descriptors[fd];
    if (Settings::values.airplane_mode.GetValue()) {
        addr.clear();
        return {-1, Errno::AGAIN};
    }
    // Datagram sockets receive like any other. Returning EAGAIN for every one of them (as this
    // used to) hid every UDP reply from the guest while its poll kept reporting the socket
    // readable: Splatoon 3's latency measurement spun on it for seconds, and Pia -- the match
    // traffic itself -- runs over UDP.

    Network::SockAddrIn addr_in{};
    Network::SockAddrIn* p_addr_in = nullptr;
    if (descriptor.is_connection_based) {
        // Connection based file descriptors (e.g. TCP) zero addr
        addr.clear();
    } else if (!addr.empty()) {
        p_addr_in = &addr_in;
    }

    // Apply flags
    using Network::FLAG_MSG_DONTWAIT;
    using Network::FLAG_O_NONBLOCK;
    if ((flags & FLAG_MSG_DONTWAIT) != 0) {
        flags &= ~FLAG_MSG_DONTWAIT;
        if ((descriptor.flags & FLAG_O_NONBLOCK) == 0) {
            descriptor.socket->SetNonBlock(true);
        }
    }

    const auto [ret, bsd_errno] = Translate(descriptor.socket->RecvFrom(flags, message, p_addr_in));

    // Restore original state
    if ((descriptor.flags & FLAG_O_NONBLOCK) == 0) {
        descriptor.socket->SetNonBlock(false);
    }

    if (p_addr_in) {
        if (ret < 0) {
            addr.clear();
        } else {
            // A sockaddr_storage-sized guest buffer is valid. Report the actual
            // IPv4 address length, not its capacity, and truncate only the copy
            // for a smaller caller buffer, as recvfrom does on the host.
            const SockAddrIn result = Translate(addr_in);
            addr_length = sizeof(result);
            addr.resize(std::min(addr.size(), sizeof(result)));
            PutValue(addr, result);
        }
    }

    return {ret, bsd_errno};
}

std::pair<s32, Errno> BSD::SendImpl(s32 fd, u32 flags, std::span<const u8> message) {
    if (!IsFileDescriptorValid(fd)) {
        return {-1, Errno::BADF};
    }
    if (!file_descriptors[fd]->socket)
        return {-1, Errno::BADF};
    if (Settings::values.airplane_mode.GetValue()) {
        return {static_cast<s32>(message.size()), Errno::SUCCESS};
    }
    FileDescriptor& descriptor = *file_descriptors[fd];
    if (!descriptor.is_connection_based) {
        LOG_DEBUG(Service, "Dropping datagram send without destination fd={}", fd);
        return {static_cast<s32>(message.size()), Errno::SUCCESS};
    }
    return Translate(descriptor.socket->Send(message, flags));
}

std::pair<s32, Errno> BSD::SendToImpl(s32 fd, u32 flags, std::span<const u8> message,
                                      std::span<const u8> addr) {
    if (!IsFileDescriptorValid(fd)) {
        return {-1, Errno::BADF};
    }
    if (!file_descriptors[fd]->socket)
        return {-1, Errno::BADF};
    if (Settings::values.airplane_mode.GetValue()) {
        return {static_cast<s32>(message.size()), Errno::SUCCESS};
    }

    FileDescriptor& descriptor = *file_descriptors[fd];

    // For datagram sockets (UDP), a destination address is required
    if (!descriptor.is_connection_based && addr.empty()) {
        LOG_DEBUG(Service, "Dropping datagram sendto without destination fd={}", fd);
        return {static_cast<s32>(message.size()), Errno::SUCCESS};
    }

    Network::SockAddrIn addr_in;
    Network::SockAddrIn* p_addr_in = nullptr;
    if (!addr.empty()) {
        ASSERT(addr.size() == sizeof(SockAddrIn));
        auto guest_addr_in = GetValue<SockAddrIn>(addr);
        addr_in = Translate(guest_addr_in);
        p_addr_in = &addr_in;
    }

    return Translate(file_descriptors[fd]->socket->SendTo(flags, message, p_addr_in));
}

Errno BSD::CloseImpl(s32 fd) {
    if (!IsFileDescriptorValid(fd)) {
        return Errno::BADF;
    }

    std::shared_ptr<Network::SocketBase> socket_to_close;

    {
        std::lock_guard lock(fd_table_mutex);
        if (file_descriptors[fd]->eventfd) {
            file_descriptors[fd].reset();
            return Errno::SUCCESS;
        }
        if (!file_descriptors[fd]->socket)
            return Errno::BADF;
        socket_to_close = file_descriptors[fd]->socket;
        file_descriptors[fd].reset();
    }

    const Errno bsd_errno = Translate(socket_to_close->Close());
    LOG_INFO(Service, "Close socket fd={}", fd);

    return bsd_errno;
}

Expected<s32, Errno> BSD::DuplicateSocketImpl(s32 fd) {
    if (!IsFileDescriptorValid(fd)) {
        return Unexpected(Errno::BADF);
    }

    const s32 new_fd = FindFreeFileDescriptorHandle();
    if (new_fd < 0) {
        LOG_ERROR(Service, "No more file descriptors available");
        return Unexpected(Errno::MFILE);
    }

    file_descriptors[new_fd] = file_descriptors[fd];
    return new_fd;
}

std::optional<std::shared_ptr<Network::SocketBase>> BSD::GetSocket(s32 fd) {
    if (!IsFileDescriptorValid(fd)) {
        return std::nullopt;
    }
    return file_descriptors[fd]->socket;
}

s32 BSD::FindFreeFileDescriptorHandle() noexcept {
    for (s32 fd = 0; fd < static_cast<s32>(file_descriptors.size()); ++fd) {
        if (!file_descriptors[fd]) {
            return fd;
        }
    }
    return -1;
}

bool BSD::IsFileDescriptorValid(s32 fd) const noexcept {
    if (fd > static_cast<s32>(MAX_FD) || fd < 0) {
        LOG_ERROR(Service, "Invalid file descriptor handle={}", fd);
        return false;
    }
    if (!file_descriptors[fd]) {
        LOG_ERROR(Service, "File descriptor handle={} is not allocated", fd);
        return false;
    }
    return true;
}

void BSD::SetDeferredPollWaker(std::shared_ptr<DeferredPollWaker> waker) {
    deferred_poll_waker = std::move(waker);
}

std::shared_ptr<BSD::EventFdState> BSD::GetEventFd(s32 fd) {
    if (fd < 0 || fd >= static_cast<s32>(MAX_FD)) {
        return nullptr;
    }
    std::lock_guard lock(fd_table_mutex);
    return file_descriptors[fd] ? file_descriptors[fd]->eventfd : nullptr;
}

bool BSD::IsDeferredPoll(const HLERequestContext& ctx) {
    std::scoped_lock lock{deferred_polls_mutex};
    return deferred_polls.contains(&ctx);
}

bool BSD::PollIncludesEventFd(std::span<const u8> read_buffer, s32 nfds) {
    if (nfds <= 0 || read_buffer.size() < nfds * sizeof(PollFD)) {
        return false;
    }
    for (s32 i = 0; i < nfds; ++i) {
        PollFD pollfd;
        std::memcpy(&pollfd, read_buffer.data() + i * sizeof(PollFD), sizeof(PollFD));
        if (GetEventFd(pollfd.fd)) {
            return true;
        }
    }
    return false;
}

// One non-blocking pass over a pollfd set, following POSIX: a closed descriptor reports POLLNVAL
// for its own entry instead of failing the whole call, and the count is of entries with any
// revents set. gRPC depends on both.
std::pair<s32, Errno> BSD::PollOnce(std::vector<u8>& write_buffer, std::span<const u8> read_buffer,
                                    s32 nfds) {
    std::vector<PollFD> fds(nfds);
    std::memcpy(fds.data(), read_buffer.data(), nfds * sizeof(PollFD));

    std::vector<Network::PollFD> host_pollfds;
    std::vector<size_t> host_index;

    for (size_t i = 0; i < fds.size(); ++i) {
        PollFD& pollfd = fds[i];
        pollfd.revents = PollEvents{};

        if (pollfd.fd < 0) {
            continue; // POSIX: a negative fd is skipped
        }
        if (pollfd.fd >= static_cast<s32>(MAX_FD) || !file_descriptors[pollfd.fd]) {
            pollfd.revents = PollEvents::Nval;
            continue;
        }

        const FileDescriptor& descriptor = *file_descriptors[pollfd.fd];
        if (descriptor.eventfd) {
            // gRPC polls its wakeup eventfd with no events requested while a connect is in
            // flight, and expects the poll to return once the eventfd is written; so an empty
            // request is read as a request for input.
            const bool wants_input =
                pollfd.events == PollEvents{} || True(pollfd.events & PollEvents::In);
            std::scoped_lock lock{descriptor.eventfd->mutex};
            if (wants_input && descriptor.eventfd->value > 0) {
                pollfd.revents |= PollEvents::In;
                // Consumed as it is reported. gRPC writes its wakeup eventfd but never reads it
                // back, so left set it would stay readable for good: every poll would return at
                // once on the wakeup and the event loop would never get to the socket it is
                // waiting on. NextendoNetwork found exactly that on the same game (facts only).
                // A guest that does read it gets EAGAIN, which gRPC's wakeup consumer accepts.
                if (descriptor.eventfd->semaphore) {
                    --descriptor.eventfd->value;
                } else {
                    descriptor.eventfd->value = 0;
                }
            }
            if (True(pollfd.events & PollEvents::Out)) {
                pollfd.revents |= PollEvents::Out;
            }
            continue;
        }
        if (!descriptor.socket) {
            pollfd.revents = PollEvents::Nval;
            continue;
        }

        Network::PollFD host;
        host.socket = descriptor.socket.get();
        host.events = Translate(pollfd.events);
        host.revents = Network::PollEvents{};
        host_pollfds.push_back(host);
        host_index.push_back(i);
    }

    if (!host_pollfds.empty()) {
        const auto [result, err] = Network::Poll(host_pollfds, 0);
        if (result < 0) {
            return {-1, Translate(err)};
        }
        for (size_t j = 0; j < host_pollfds.size(); ++j) {
            fds[host_index[j]].revents = Translate(host_pollfds[j].revents);
        }
    }

    s32 ready = 0;
    for (const PollFD& pollfd : fds) {
        if (pollfd.revents != PollEvents{}) {
            ++ready;
        }
    }

    std::memcpy(write_buffer.data(), fds.data(), nfds * sizeof(PollFD));
    return {ready, Errno::SUCCESS};
}

// poll() for a set that includes an eventfd. It never blocks a service thread: when nothing is
// ready yet the reply is deferred, and DeferredPollWaker has the server manager re-run this until
// something is or the timeout passes. See DeferredPollWaker for why.
void BSD::PollWithEventFd(HLERequestContext& ctx, s32 nfds, s32 timeout) {
    const size_t pollfds_size = nfds > 0 ? static_cast<size_t>(nfds) * sizeof(PollFD) : 0;

    // The descriptors are read from the guest once, when the request first arrives, and every
    // re-run checks that copy. The guest's buffer is only certain to hold them while the request
    // is being received: the game's socket library reuses it for its next bsd call, and a re-run
    // that read it again took that call's data (a connect's address, an eventfd write) for
    // descriptors -- EBADF or POLLNVAL on a poll the guest never made, and gRPC's event loop
    // stopped for good. NextendoNetwork found this on the same game (facts only).
    std::vector<u8> pollfds;
    {
        std::scoped_lock lock{deferred_polls_mutex};
        const auto it = deferred_polls.find(&ctx);
        // The size check stops a destroyed session's leftover entry, whose context address has
        // since been reused, from standing in for a new request's descriptors.
        if (it != deferred_polls.end() && it->second.pollfds.size() == pollfds_size) {
            pollfds = it->second.pollfds;
        }
    }
    if (pollfds.empty()) {
        const auto read_buffer = ctx.ReadBuffer();
        pollfds.assign(read_buffer.begin(), read_buffer.end());
    }
    std::vector<u8> write_buffer(ctx.GetWriteBufferSize());

    s32 ret = -1;
    Errno bsd_errno = Errno::INVAL;
    if (pollfds.size() >= pollfds_size && write_buffer.size() >= pollfds_size && timeout >= -1) {
        std::tie(ret, bsd_errno) = PollOnce(write_buffer, pollfds, nfds);
    }

    const auto now = std::chrono::steady_clock::now();
    bool finished = true;
    {
        std::scoped_lock lock{deferred_polls_mutex};

        // A deferred poll whose session went away is never re-run; do not keep waking for it.
        for (auto it = deferred_polls.begin(); it != deferred_polls.end();) {
            if (it->first != &ctx && now - it->second.last_run > std::chrono::seconds(5)) {
                it = deferred_polls.erase(it);
                deferred_poll_waker->RemoveWaiter();
            } else {
                ++it;
            }
        }

        if (ret == 0 && bsd_errno == Errno::SUCCESS && timeout != 0) {
            const auto deadline = timeout < 0 ? std::chrono::steady_clock::time_point::max()
                                              : now + std::chrono::milliseconds(timeout);
            auto [it, inserted] = deferred_polls.try_emplace(&ctx, DeferredPoll{deadline, now});
            if (inserted) {
                it->second.pollfds = pollfds;
                deferred_poll_waker->AddWaiter();
            }
            it->second.last_run = now;
            finished = now >= it->second.deadline;
        }

        if (finished && deferred_polls.erase(&ctx) != 0) {
            deferred_poll_waker->RemoveWaiter();
        }
    }

    if (!finished) {
        ctx.SetIsDeferred();
        return;
    }

    if (ret >= 0 && !write_buffer.empty()) {
        ctx.WriteBuffer(write_buffer);
    }
    SWITCHNET_TRACE("poll(eventfd) nfds={} timeout={} -> {} errno={} [{}]", nfds, timeout, ret,
                    static_cast<u32>(bsd_errno),
                    DescribePollFds(ret >= 0 ? std::span<const u8>{write_buffer}
                                             : std::span<const u8>{pollfds},
                                    nfds));
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD::BuildErrnoResponse(HLERequestContext& ctx, Errno bsd_errno) const noexcept {
    IPC::ResponseBuilder rb{ctx, 4};

    rb.Push(ResultSuccess);
    rb.Push<s32>(bsd_errno == Errno::SUCCESS ? 0 : -1);
    rb.PushEnum(bsd_errno);
}

void BSD::OnProxyPacketReceived(const Network::ProxyPacket& packet) {
    // Lock the table so CloseImpl doesn't delete a socket while we are iterating
    std::lock_guard lock(fd_table_mutex);

    // We must ensure we only deliver the packet ONCE
    std::vector<Network::SocketBase*> processed_sockets;

    for (auto& optional_desc : file_descriptors) {
        if (optional_desc.has_value() && optional_desc->socket) {
            Network::SocketBase* socket_ptr = optional_desc->socket.get();

            // If we haven't given this specific socket the packet yet...
            if (std::find(processed_sockets.begin(), processed_sockets.end(), socket_ptr) == processed_sockets.end()) {
                socket_ptr->HandleProxyPacket(packet);
                processed_sockets.push_back(socket_ptr);
            }
        }
    }
}

BSD::BSD(Core::System& system_, const char* name)
    : ServiceFramework{system_, name}, room_network{system_.GetRoomNetwork()} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, &BSD::RegisterClient, "RegisterClient"},
        {1, &BSD::StartMonitoring, "StartMonitoring"},
        {2, &BSD::Socket, "Socket"},
        {3, &BSD::SocketExempt, "SocketExempt"},
        {4, &BSD::Open, "Open"},
        {5, &BSD::Select, "Select"},
        {6, &BSD::Poll, "Poll"},
        {7, &BSD::Sysctl, "Sysctl"},
        {8, &BSD::Recv, "Recv"},
        {9, &BSD::RecvFrom, "RecvFrom"},
        {10, &BSD::Send, "Send"},
        {11, &BSD::SendTo, "SendTo"},
        {12, &BSD::Accept, "Accept"},
        {13, &BSD::Bind, "Bind"},
        {14, &BSD::Connect, "Connect"},
        {15, &BSD::GetPeerName, "GetPeerName"},
        {16, &BSD::GetSockName, "GetSockName"},
        {17, &BSD::GetSockOpt, "GetSockOpt"},
        {18, &BSD::Listen, "Listen"},
        {19, &BSD::Ioctl, "Ioctl"},
        {20, &BSD::Fcntl, "Fcntl"},
        {21, &BSD::SetSockOpt, "SetSockOpt"},
        {22, &BSD::Shutdown, "Shutdown"},
        {23, &BSD::ShutdownAllSockets, "ShutdownAllSockets"},
        {24, &BSD::Write, "Write"},
        {25, &BSD::Read, "Read"},
        {26, &BSD::Close, "Close"},
        {27, &BSD::DuplicateSocket, "DuplicateSocket"},
        {28, &BSD::GetResourceStatistics, "GetResourceStatistics"},
        {29, &BSD::RecvMMsg, "RecvMMsg"},
        {30, &BSD::SendMMsg, "SendMMsg"},
        {31, &BSD::EventFd, "EventFd"},
        {32, &BSD::RegisterResourceStatisticsName, "RegisterResourceStatisticsName"},
        {33, &BSD::RegisterClientShared, "RegisterClientShared"},
        {34, &BSD::GetSocketStatistics, "GetSocketStatistics"},
        {35, &BSD::NifIoctl, "NifIoctl"},
        {36, &BSD::Unknown36, "Unknown36"},
        {37, &BSD::Unknown37, "Unknown37"},
        {38, &BSD::Unknown38, "Unknown38"},
        {39, &BSD::Unknown39, "Unknown39"},
        {40, &BSD::Unknown40, "Unknown40"},
        {200, &BSD::SetThreadCoreMask, "SetThreadCoreMask"},
        {201, &BSD::GetThreadCoreMask, "GetThreadCoreMask"},
    };
    // clang-format on

    RegisterHandlers(functions);

    if (auto room_member = room_network.GetRoomMember().lock()) {
        proxy_packet_received = room_member->BindOnProxyPacketReceived(
            [this](const Network::ProxyPacket& packet) { OnProxyPacketReceived(packet); });
    } else {
        LOG_ERROR(Service, "Network isn't initialized");
    }
}

BSD::~BSD() {
    if (auto room_member = room_network.GetRoomMember().lock()) {
        room_member->Unbind(proxy_packet_received);
    }
}

std::unique_lock<std::mutex> BSD::LockService() {
    // Do not lock socket IClient instances.
    return {};
}

BSDCFG::BSDCFG(Core::System& system_) : ServiceFramework{system_, "bsdcfg"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, &BSDCFG::SetIfUp, "SetIfUp"},
        {1, &BSDCFG::SetIfUpWithEvent, "SetIfUpWithEvent"},
        {2, &BSDCFG::CancelIf, "CancelIf"},
        {3, &BSDCFG::SetIfDown, "SetIfDown"},
        {4, &BSDCFG::GetIfState, "GetIfState"},
        {5, &BSDCFG::DhcpRenew, "DhcpRenew"},
        {6, &BSDCFG::AddStaticArpEntry, "AddStaticArpEntry"},
        {7, &BSDCFG::RemoveArpEntry, "RemoveArpEntry"},
        {8, &BSDCFG::LookupArpEntry, "LookupArpEntry"},
        {9, &BSDCFG::LookupArpEntry2, "LookupArpEntry2"},
        {10, &BSDCFG::ClearArpEntries, "ClearArpEntries"},
        {11, &BSDCFG::ClearArpEntries2, "ClearArpEntries2"},
        {12, &BSDCFG::PrintArpEntries, "PrintArpEntries"},
        {13, &BSDCFG::Unknown13, "Unknown13"},
        {14, &BSDCFG::Unknown14, "Unknown14"},
        {15, &BSDCFG::Unknown15, "Unknown15"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

BSDCFG::~BSDCFG() = default;

// BSDCFG Service Method Stubs
void BSDCFG::SetIfUp(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called SetIfUp");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSDCFG::SetIfUpWithEvent(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called SetIfUpWithEvent");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSDCFG::CancelIf(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called CancelIf");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSDCFG::SetIfDown(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called SetIfDown");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSDCFG::GetIfState(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called GetIfState");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSDCFG::DhcpRenew(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called DhcpRenew");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSDCFG::AddStaticArpEntry(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called AddStaticArpEntry");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSDCFG::RemoveArpEntry(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called RemoveArpEntry");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSDCFG::LookupArpEntry(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called LookupArpEntry");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSDCFG::LookupArpEntry2(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called LookupArpEntry2");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSDCFG::ClearArpEntries(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called ClearArpEntries");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSDCFG::ClearArpEntries2(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called ClearArpEntries2");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSDCFG::PrintArpEntries(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called PrintArpEntries");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSDCFG::Unknown13(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called Unknown13 (Cmd13)");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSDCFG::Unknown14(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called Unknown14 (Cmd14)");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSDCFG::Unknown15(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called Unknown15 (Cmd15)");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSD::GetResourceStatistics(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called GetResourceStatistics");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSD::GetSocketStatistics(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called GetSocketStatistics");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSD::GetThreadCoreMask(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called GetThreadCoreMask");
    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<u64>(0);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSD::Ioctl(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called Ioctl");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(ENOTTY));
}

void BSD::NifIoctl(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called NifIoctl");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(ENOTTY));
}

void BSD::Open(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called Open");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EACCES));
}

// RecvMMsg(u32 fd, u32 vlen, u32 flags, u32 reserved, TimeVal timeout) -> (i32 ret, u32 bsd_errno),
// with the in/out buffer serialised as in SendMMsg. Each iov's data is space to receive into:
// received bytes are written there in place, each message's length is set, and the buffer is
// written back.
//
// gRPC's recvmsg reaches the service as a RecvMMsg of one message. Left as an EOPNOTSUPP stub,
// Splatoon 3's online client closed its connection to the game server the first time the
// socket became readable, before reading the TLS handshake reply.
void BSD::RecvMMsg(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const u32 vlen = rp.Pop<u32>();
    const u32 flags = rp.Pop<u32>();

    auto respond = [&ctx](s32 ret, Errno err) {
        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<s32>(ret);
        rb.PushEnum(err);
    };

    if (ctx.BufferDescriptorB().empty()) {
        respond(-1, Errno::INVAL);
        return;
    }
    const auto& desc = ctx.BufferDescriptorB()[0];
    std::vector<u8> buf(desc.Size());
    ctx.GetMemory().ReadBlock(desc.Address(), buf.data(), buf.size());

    struct Space {
        std::size_t offset;
        std::size_t size;
    };
    struct Message {
        std::vector<Space> iovs;
        std::size_t length_offset;
    };
    std::vector<Message> messages;

    std::size_t pos = 1; // header byte, ignored as Horizon does
    auto skip = [&](std::size_t n) {
        if (pos + n > buf.size()) {
            return false;
        }
        pos += n;
        return true;
    };
    auto read_u32 = [&](u32& v) {
        if (pos + sizeof(v) > buf.size()) {
            return false;
        }
        std::memcpy(&v, buf.data() + pos, sizeof(v));
        pos += sizeof(v);
        return true;
    };

    bool parsed = true;
    for (u32 i = 0; i < vlen && parsed; ++i) {
        Message message;
        u32 name_len = 0, iov_count = 0, control_len = 0, msg_flags = 0;
        parsed = read_u32(name_len) && skip(name_len) && read_u32(iov_count);
        for (u32 j = 0; j < iov_count && parsed; ++j) {
            u64 len = 0;
            parsed = pos + sizeof(len) <= buf.size();
            if (parsed) {
                std::memcpy(&len, buf.data() + pos, sizeof(len));
                pos += sizeof(len);
                message.iovs.push_back({pos, static_cast<std::size_t>(len)});
                parsed = skip(static_cast<std::size_t>(len));
            }
        }
        parsed = parsed && read_u32(control_len) && skip(control_len) && read_u32(msg_flags);
        message.length_offset = pos;
        parsed = parsed && skip(sizeof(u32));
        if (parsed) {
            messages.push_back(std::move(message));
        }
    }
    if (!parsed) {
        respond(-1, Errno::INVAL);
        return;
    }

    std::size_t capacity = 0;
    for (const auto& message : messages) {
        for (const auto& iov : message.iovs) {
            capacity += iov.size;
        }
    }
    std::vector<u8> received(capacity);
    const auto [ret, err] = RecvImpl(fd, flags, received);
    SWITCHNET_TRACE("recvmmsg fd={} vlen={} capacity={} -> {} errno={}", fd, vlen, capacity, ret,
                    static_cast<u32>(err));
    if (err != Errno::SUCCESS || ret < 0) {
        respond(-1, err);
        return;
    }

    // A stream socket's bytes fill the messages' buffers in order.
    std::size_t left = static_cast<std::size_t>(ret);
    std::size_t from = 0;
    s32 filled_messages = 0;
    for (const auto& message : messages) {
        u32 length = 0;
        for (const auto& iov : message.iovs) {
            const std::size_t n = std::min(left, iov.size);
            std::memcpy(buf.data() + iov.offset, received.data() + from, n);
            from += n;
            left -= n;
            length += static_cast<u32>(n);
        }
        std::memcpy(buf.data() + message.length_offset, &length, sizeof(length));
        if (length == 0 && filled_messages > 0) {
            break;
        }
        ++filled_messages;
        if (left == 0) {
            break;
        }
    }

    ctx.GetMemory().WriteBlock(desc.Address(), buf.data(), buf.size());
    respond(filled_messages, Errno::SUCCESS);
}

void BSD::RegisterResourceStatisticsName(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called RegisterResourceStatisticsName");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

// SendMMsg(u32 fd, u32 vlen, u32 flags) -> (i32 ret, u32 bsd_errno), with an in/out buffer
// holding the messages serialised as Horizon does: one ignored header byte, then per message
// u32 name_len + name, u32 iov_count + (u64 len + data) per iov, u32 control_len + control,
// u32 flags, u32 length. The length of each message sent is written back in place.
//
// gRPC's sendmsg reaches the service as a SendMMsg of one message. Left as an EOPNOTSUPP stub,
// Splatoon 3's online client retried it without end once connected (a million calls in 40 s),
// so the TLS handshake to the game server was never sent.
void BSD::SendMMsg(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const u32 vlen = rp.Pop<u32>();
    const u32 flags = rp.Pop<u32>();

    auto respond = [&ctx](s32 ret, Errno err) {
        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<s32>(ret);
        rb.PushEnum(err);
    };

    if (ctx.BufferDescriptorB().empty()) {
        respond(-1, Errno::INVAL);
        return;
    }
    const auto& desc = ctx.BufferDescriptorB()[0];
    std::vector<u8> buf(desc.Size());
    ctx.GetMemory().ReadBlock(desc.Address(), buf.data(), buf.size());

    std::size_t pos = 1; // header byte, ignored as Horizon does
    auto take = [&](std::size_t n) -> std::optional<std::span<const u8>> {
        if (pos + n > buf.size()) {
            return std::nullopt;
        }
        std::span<const u8> out{buf.data() + pos, n};
        pos += n;
        return out;
    };
    auto take_u32 = [&]() -> std::optional<u32> {
        const auto b = take(sizeof(u32));
        if (!b) {
            return std::nullopt;
        }
        u32 v;
        std::memcpy(&v, b->data(), sizeof(v));
        return v;
    };

    s32 sent_messages = 0;
    Errno first_error = Errno::SUCCESS;
    for (u32 i = 0; i < vlen; ++i) {
        const auto name_len = take_u32();
        const auto name = name_len ? take(*name_len) : std::nullopt;
        const auto iov_count = take_u32();
        if (!name_len || !name || !iov_count) {
            first_error = Errno::INVAL;
            break;
        }
        std::vector<u8> data;
        bool ok = true;
        for (u32 j = 0; j < *iov_count && ok; ++j) {
            const auto len_bytes = take(sizeof(u64));
            if (!len_bytes) {
                ok = false;
                break;
            }
            u64 len;
            std::memcpy(&len, len_bytes->data(), sizeof(len));
            const auto piece = take(static_cast<std::size_t>(len));
            ok = piece.has_value();
            if (ok) {
                data.insert(data.end(), piece->begin(), piece->end());
            }
        }
        const auto control_len = ok ? take_u32() : std::nullopt;
        if (!control_len || !take(*control_len) || !take_u32() /* flags */) {
            first_error = Errno::INVAL;
            break;
        }
        const std::size_t length_pos = pos;
        if (!take(sizeof(u32))) {
            first_error = Errno::INVAL;
            break;
        }

        const auto [ret, err] = name->empty() ? SendImpl(fd, flags, data)
                                              : SendToImpl(fd, flags, data, *name);
        if (err != Errno::SUCCESS || ret < 0) {
            first_error = err;
            break;
        }
        const u32 length = static_cast<u32>(ret);
        std::memcpy(buf.data() + length_pos, &length, sizeof(length));
        ++sent_messages;
        if (static_cast<std::size_t>(ret) < data.size()) {
            break; // a short send ends the batch, as sendmmsg does
        }
    }

    ctx.GetMemory().WriteBlock(desc.Address(), buf.data(), buf.size());
    SWITCHNET_TRACE("sendmmsg fd={} vlen={} -> {} errno={}", fd, vlen, sent_messages,
                    static_cast<u32>(first_error));
    if (sent_messages == 0 && first_error != Errno::SUCCESS) {
        respond(-1, first_error);
        return;
    }
    respond(sent_messages, Errno::SUCCESS);
}

void BSD::SetThreadCoreMask(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called SetThreadCoreMask [15.0.0+]");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSD::ShutdownAllSockets(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called ShutdownAllSockets");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSD::SocketExempt(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called SocketExempt");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1); // fd
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSD::Unknown36(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called Unknown36 [18.0.0+]");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSD::Unknown37(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called Unknown37 [18.0.0+]");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSD::Unknown38(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called Unknown38 [18.0.0+]");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSD::Unknown39(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called Unknown39 [20.0.0+]");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSD::Unknown40(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called Unknown40 [20.0.0+]");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

void BSD::Sysctl(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called Sysctl");
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(-1);
    rb.PushEnum(static_cast<Errno>(EOPNOTSUPP));
}

} // namespace Service::Sockets
