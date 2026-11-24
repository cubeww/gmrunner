#include "extensions/core/extension.h"
#include "extensions/shared/buffer_store.h"

#include "base/encoding.h"
#include "base/hash.h"
#include "base/log.h"
#include "base/zlib_util.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
#elif !defined(__EMSCRIPTEN__)
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
#endif

namespace gm {

namespace {

template <typename T>
T gm_clamp_cast(f64 v) noexcept {
    if (std::isnan(v)) return static_cast<T>(0);
    if constexpr (std::is_floating_point_v<T>) {
        constexpr f64 mx = static_cast<f64>(std::numeric_limits<T>::max());
        return static_cast<T>(std::clamp(v, -mx, mx));
    } else {
        constexpr f64 mn = static_cast<f64>(std::numeric_limits<T>::min());
        constexpr f64 mx = static_cast<f64>(std::numeric_limits<T>::max());
        if (v <= mn) return std::numeric_limits<T>::min();
        if (v >= mx) return std::numeric_limits<T>::max();
        return static_cast<T>(v);
    }
}

void buf_set_length(GmBuffer& b, usize len) {
    const usize old_len = b.data.size();
    b.data.resize(len, 0u);
    if (len > old_len) {
        std::memset(b.data.data() + old_len, 0, len - old_len);
    }
    if (b.pos > len) b.pos = len;
}

void buf_set_pos(GmBuffer& b, usize p) noexcept {
    b.pos = std::min(p, b.data.size());
}

void buf_clear(GmBuffer& b) noexcept {
    b.data.clear();
    b.pos = 0;
    b.error = false;
}

template <typename T>
bool buf_read_le(GmBuffer& b, T& out) noexcept {
    if (b.error || b.pos + sizeof(T) > b.data.size()) {
        b.error = true;
        out = T{};
        return false;
    }
    std::memcpy(&out, b.data.data() + b.pos, sizeof(T));
    b.pos += sizeof(T);
    return true;
}

template <typename T>
void buf_write_le(GmBuffer& b, T val) {
    const usize end = b.pos + sizeof(T);
    if (end > b.data.size()) {
        b.data.resize(end, 0u);
    }
    std::memcpy(b.data.data() + b.pos, &val, sizeof(T));
    b.pos = end;
}

void buf_write_at_pos(GmBuffer& b, const void* src, usize len) {
    if (len == 0) return;
    const usize end = b.pos + len;
    if (end > b.data.size()) {
        b.data.resize(end, 0u);
    }
    std::memcpy(b.data.data() + b.pos, src, len);
    b.pos = end;
}

void buf_append_end(GmBuffer& b, const u8* src, usize len) {
    if (len == 0) return;
    b.data.insert(b.data.end(), src, src + len);
}

std::string encode_base64_gm82(std::span<const u8> bytes) {
    static constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4 + (bytes.size() / 48) * 2);
    usize line_in = 0;
    for (usize i = 0; i < bytes.size(); i += 3) {
        if (line_in == 48) {
            out += "\r\n";
            line_in = 0;
        }
        const u32 b0 = bytes[i];
        const bool has1 = (i + 1 < bytes.size());
        const bool has2 = (i + 2 < bytes.size());
        const u32 b1 = has1 ? bytes[i + 1] : 0u;
        const u32 b2 = has2 ? bytes[i + 2] : 0u;
        const u32 triple = (b0 << 16u) | (b1 << 8u) | b2;
        out.push_back(kB64[(triple >> 18u) & 0x3Fu]);
        out.push_back(kB64[(triple >> 12u) & 0x3Fu]);
        out.push_back(has1 ? kB64[(triple >> 6u) & 0x3Fu] : '=');
        out.push_back(has2 ? kB64[triple & 0x3Fu] : '=');
        line_in += 3;
    }
    return out;
}

std::vector<u8> decode_base64_gm82(std::string_view text) {
    auto val_of = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62;
        if (c == '/' || c == '_') return 63;
        return -1;
    };
    std::vector<u8> out;
    out.reserve((text.size() * 3) / 4);
    int vals[4];
    int count = 0;
    for (unsigned char c : text) {
        if (c == '=') break;
        const int v = val_of(c);
        if (v < 0) continue;
        vals[count++] = v;
        if (count == 4) {
            out.push_back(static_cast<u8>((vals[0] << 2) | (vals[1] >> 4)));
            out.push_back(static_cast<u8>(((vals[1] & 0x0F) << 4) | (vals[2] >> 2)));
            out.push_back(static_cast<u8>(((vals[2] & 0x03) << 6) | vals[3]));
            count = 0;
        }
    }
    if (count == 2) {
        out.push_back(static_cast<u8>((vals[0] << 2) | (vals[1] >> 4)));
    } else if (count == 3) {
        out.push_back(static_cast<u8>((vals[0] << 2) | (vals[1] >> 4)));
        out.push_back(static_cast<u8>(((vals[1] & 0x0F) << 4) | (vals[2] >> 2)));
    }
    return out;
}

void rc4_crypt_inplace(std::span<u8> data, std::span<const u8> key) noexcept {
    if (data.empty() || key.empty()) return;
    u8 s[256];
    for (u32 i = 0; i < 256; ++i) s[i] = static_cast<u8>(i);
    u8 j = 0;
    for (u32 i = 0; i < 256; ++i) {
        j = static_cast<u8>(j + s[i] + key[i % key.size()]);
        std::swap(s[i], s[j]);
    }
    u8 a = 0, b = 0;
    for (u8& byte : data) {
        a = static_cast<u8>(a + 1);
        b = static_cast<u8>(b + s[a]);
        std::swap(s[a], s[b]);
        byte ^= s[static_cast<u8>(s[a] + s[b])];
    }
}

std::string format_crc32_hex(u32 crc) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08x", crc);
    return std::string(buf);
}

// Http Dll 2 variable-length unsigned integer encoding (UIntV) for TCP framed messages.
void encode_uintv(std::vector<u8>& out, u32 v) {
    if (v < 0x80u) {
        out.push_back(static_cast<u8>((v << 1u) | 1u));
    } else if (v < 0x4080u) {
        const u32 n = v - 0x80u;
        const u32 enc = (n << 2u) | 2u;
        out.push_back(static_cast<u8>(enc & 0xFFu));
        out.push_back(static_cast<u8>((enc >> 8u) & 0xFFu));
    } else if (v < 0x204080u) {
        const u32 n = v - 0x4080u;
        const u32 enc = (n << 3u) | 4u;
        out.push_back(static_cast<u8>(enc & 0xFFu));
        out.push_back(static_cast<u8>((enc >> 8u) & 0xFFu));
        out.push_back(static_cast<u8>((enc >> 16u) & 0xFFu));
    } else {
        const u32 n = v - 0x204080u;
        const u32 enc = (n << 3u);
        out.push_back(static_cast<u8>(enc & 0xFFu));
        out.push_back(static_cast<u8>((enc >> 8u) & 0xFFu));
        out.push_back(static_cast<u8>((enc >> 16u) & 0xFFu));
        out.push_back(static_cast<u8>((enc >> 24u) & 0xFFu));
    }
}

bool decode_uintv(std::span<const u8> in, u32& out_val, usize& out_hdr_len) noexcept {
    if (in.empty()) return false;
    const u8 b0 = in[0];
    if (b0 & 1u) {
        out_val = b0 >> 1u;
        out_hdr_len = 1;
        return true;
    }
    if (b0 & 2u) {
        if (in.size() < 2) return false;
        const u32 enc = static_cast<u32>(b0) | (static_cast<u32>(in[1]) << 8u);
        out_val = (enc >> 2u) + 0x80u;
        out_hdr_len = 2;
        return true;
    }
    if (b0 & 4u) {
        if (in.size() < 3) return false;
        const u32 enc = static_cast<u32>(b0) | (static_cast<u32>(in[1]) << 8u) | (static_cast<u32>(in[2]) << 16u);
        out_val = (enc >> 3u) + 0x4080u;
        out_hdr_len = 3;
        return true;
    }
    if (in.size() < 4) return false;
    const u32 enc = static_cast<u32>(b0) | (static_cast<u32>(in[1]) << 8u) |
                    (static_cast<u32>(in[2]) << 16u) | (static_cast<u32>(in[3]) << 24u);
    out_val = (enc >> 3u) + 0x204080u;
    out_hdr_len = 4;
    return true;
}

#if !defined(__EMSCRIPTEN__)
void close_native_socket(NativeSocket& s) noexcept {
    if (s == kInvalidSocket) return;
#if defined(_WIN32)
    closesocket(s);
#else
    close(s);
#endif
    s = kInvalidSocket;
}

void set_nonblocking(NativeSocket s) noexcept {
#if defined(_WIN32)
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
#else
    const int flags = fcntl(s, F_GETFL, 0);
    if (flags >= 0) fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
}

bool last_sock_would_block() noexcept {
#if defined(_WIN32)
    const int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY;
#else
    return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS;
#endif
}

void configure_tcp_socket(NativeSocket s) noexcept {
    set_nonblocking(s);
    const int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
#if defined(SO_NOSIGPIPE) && !defined(MSG_NOSIGNAL)
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

std::string sockaddr_to_ip(const sockaddr_storage& ss) {
    char buf[INET6_ADDRSTRLEN]{};
    if (ss.ss_family == AF_INET) {
        const auto* s4 = reinterpret_cast<const sockaddr_in*>(&ss);
        if (inet_ntop(AF_INET, &s4->sin_addr, buf, sizeof(buf))) return std::string(buf);
    } else if (ss.ss_family == AF_INET6) {
        const auto* s6 = reinterpret_cast<const sockaddr_in6*>(&ss);
        if (inet_ntop(AF_INET6, &s6->sin6_addr, buf, sizeof(buf))) return std::string(buf);
    }
    return {};
}

u16 sockaddr_to_port(const sockaddr_storage& ss) noexcept {
    if (ss.ss_family == AF_INET) {
        return ntohs(reinterpret_cast<const sockaddr_in*>(&ss)->sin_port);
    }
    if (ss.ss_family == AF_INET6) {
        return ntohs(reinterpret_cast<const sockaddr_in6*>(&ss)->sin6_port);
    }
    return 0;
}
#endif

struct TcpListener {
#if !defined(__EMSCRIPTEN__)
    NativeSocket sock = kInvalidSocket;
#endif
    bool listening = false;
};

struct TcpSocket {
#if !defined(__EMSCRIPTEN__)
    NativeSocket sock = kInvalidSocket;
    sockaddr_storage peer{};
    bool has_peer = false;
#endif
    i32 state = 0; // 0=offline, 1=connecting, 2=connected, 4=closed, 5=error
    bool should_shut_down = false;
    std::vector<u8> read_buf;
    std::vector<u8> write_buf;
};

struct UdpSocket {
#if !defined(__EMSCRIPTEN__)
    NativeSocket sock = kInvalidSocket;
    sockaddr_storage dest{};
    int dest_len = 0;
    sockaddr_storage last_from{};
    bool has_last_from = false;
#endif
    bool ipv6 = false;
    i32 state = 0; // 0=notstarted, 1=started, 2=error
};

class Gm82BufExtension final : public IExtension {
public:
    Gm82BufExtension() {
#if defined(_WIN32)
        WSADATA wsa{};
        wsa_ok_ = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#endif
    }

    ~Gm82BufExtension() override {
        close_all_sockets();
#if defined(_WIN32)
        if (wsa_ok_) {
            WSACleanup();
            wsa_ok_ = false;
        }
#endif
    }

    std::string_view canonical_name() const noexcept override { return "gm82buf"; }

    void on_register(ExtensionRegistrar& reg) override {
        reg.add_dll_aliases({"gm82buf", "gm82net"});
        buffers_ = &reg.services().get_or_create<GmBufferStore>();

        // ---- Buffer lifecycle & metadata ----
        reg.bind({"__gm82buf_buffer_create", "buffer_create"}, [this]() {
            return static_cast<f64>(buffers_->create());
        });
        reg.bind("buffer_destroy", [this](const CallContext& ctx) {
            return RValue(buffers_->destroy(static_cast<u32>(ctx.i32_arg(0, 0))) ? 1.0 : 0.0);
        });
        reg.bind("buffer_exists", [this](const CallContext& ctx) {
            return RValue(buffers_->exists(static_cast<u32>(ctx.i32_arg(0, 0))) ? 1.0 : 0.0);
        });
        reg.bind("buffer_get_pos", [this](const CallContext& ctx) {
            const auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(0, 0)));
            return RValue(b ? static_cast<f64>(b->pos) : 0.0);
        });
        reg.bind({"buffer_get_length", "buffer_get_size"}, [this](const CallContext& ctx) {
            const auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(0, 0)));
            return RValue(b ? static_cast<f64>(b->data.size()) : 0.0);
        });
        reg.bind("buffer_at_end", [this](const CallContext& ctx) {
            const auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(0, 0)));
            return RValue((b && b->pos >= b->data.size()) ? 1.0 : 0.0);
        });
        reg.bind("buffer_get_error", [this](const CallContext& ctx) {
            const auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(0, 0)));
            return RValue((b && b->error) ? 1.0 : 0.0);
        });
        reg.bind("buffer_clear_error", [this](const CallContext& ctx) {
            auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (!b) return RValue(0.0);
            b->error = false;
            return RValue(1.0);
        });
        reg.bind("buffer_clear", [this](const CallContext& ctx) {
            auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (!b) return RValue(0.0);
            buf_clear(*b);
            return RValue(1.0);
        });
        reg.bind("buffer_set_pos", [this](const CallContext& ctx) {
            auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (!b) return RValue(0.0);
            buf_set_pos(*b, static_cast<usize>(gm_clamp_cast<u32>(ctx.real(1, 0.0))));
            return RValue(1.0);
        });
        reg.bind("buffer_set_size", [this](const CallContext& ctx) {
            auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (!b) return RValue(0.0);
            buf_set_length(*b, static_cast<usize>(gm_clamp_cast<u32>(ctx.real(1, 0.0))));
            return RValue(1.0);
        });
        reg.bind("buffer_get_address", [this](const CallContext& ctx) {
            return RValue(static_cast<f64>(buffers_->pseudo_address(static_cast<u32>(ctx.i32_arg(0, 0)))));
        });
        reg.bind("buffer_get_address_string", [this](const CallContext& ctx) {
            const u32 addr = buffers_->pseudo_address(static_cast<u32>(ctx.i32_arg(0, 0)));
            char tmp[32];
            if (ctx.boolean(1, false)) {
                std::snprintf(tmp, sizeof(tmp), "%x", addr);
            } else {
                std::snprintf(tmp, sizeof(tmp), "%u", addr);
            }
            return RValue(std::string(tmp));
        });
        reg.bind("buffer_peek", [this](const CallContext& ctx) {
            auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (!b) return RValue(0.0);
            const usize old_pos = b->pos;
            buf_set_pos(*b, static_cast<usize>(gm_clamp_cast<u32>(ctx.real(1, 0.0))));
            u8 v = 0;
            (void)buf_read_le(*b, v);
            b->pos = old_pos;
            return RValue(static_cast<f64>(v));
        });
        reg.bind("buffer_poke", [this](const CallContext& ctx) {
            auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (!b) return RValue(0.0);
            const usize old_pos = b->pos;
            buf_set_pos(*b, static_cast<usize>(gm_clamp_cast<u32>(ctx.real(1, 0.0))));
            buf_write_le<u8>(*b, gm_clamp_cast<u8>(ctx.real(2, 0.0)));
            b->pos = std::min(old_pos, b->data.size());
            return RValue(1.0);
        });

        // ---- Typed Little-Endian Reads ----
        reg.bind<&Gm82BufExtension::read_int<i8>>(this, {"buffer_read_int8", "buffer_read_i8"});
        reg.bind<&Gm82BufExtension::read_int<u8>>(this, {"buffer_read_uint8", "buffer_read_u8"});
        reg.bind<&Gm82BufExtension::read_int<i16>>(this, {"buffer_read_int16", "buffer_read_i16"});
        reg.bind<&Gm82BufExtension::read_int<u16>>(this, {"buffer_read_uint16", "buffer_read_u16"});
        reg.bind<&Gm82BufExtension::read_int<i32>>(this, {"buffer_read_int32", "buffer_read_i32"});
        reg.bind<&Gm82BufExtension::read_int<u32>>(this, {"buffer_read_uint32", "buffer_read_u32"});
        reg.bind<&Gm82BufExtension::read_int<i64>>(this, {"buffer_read_int64", "buffer_read_i64"});
        reg.bind<&Gm82BufExtension::read_int<u64>>(this, {"buffer_read_uint64", "buffer_read_u64"});
        reg.bind<&Gm82BufExtension::read_float<f32>>(this, {"buffer_read_float32", "buffer_read_float"});
        reg.bind<&Gm82BufExtension::read_float<f64>>(this, {"buffer_read_float64", "buffer_read_double"});

        // ---- Typed Little-Endian Writes ----
        reg.bind<&Gm82BufExtension::write_clamped<i8>>(this, {"buffer_write_int8", "buffer_write_i8"});
        reg.bind<&Gm82BufExtension::write_clamped<u8>>(this, {"buffer_write_uint8", "buffer_write_u8"});
        reg.bind<&Gm82BufExtension::write_clamped<i16>>(this, {"buffer_write_int16", "buffer_write_i16"});
        reg.bind<&Gm82BufExtension::write_clamped<u16>>(this, {"buffer_write_uint16", "buffer_write_u16"});
        reg.bind<&Gm82BufExtension::write_clamped<i32>>(this, {"buffer_write_int32", "buffer_write_i32"});
        reg.bind<&Gm82BufExtension::write_clamped<u32>>(this, {"buffer_write_uint32", "buffer_write_u32"});
        reg.bind<&Gm82BufExtension::write_clamped<i64>>(this, {"buffer_write_int64", "buffer_write_i64"});
        reg.bind<&Gm82BufExtension::write_clamped<u64>>(this, {"buffer_write_uint64", "buffer_write_u64"});
        reg.bind<&Gm82BufExtension::write_clamped<f32>>(this, {"buffer_write_float32", "buffer_write_float"});
        reg.bind<&Gm82BufExtension::write_f32_vec<2>>(this, {"buffer_write_float32x2", "buffer_write_float2"});
        reg.bind<&Gm82BufExtension::write_f32_vec<3>>(this, {"buffer_write_float32x3", "buffer_write_float3"});
        reg.bind<&Gm82BufExtension::write_f32_vec<4>>(this, {"buffer_write_float32x4", "buffer_write_float4"});
        reg.bind<&Gm82BufExtension::write_f32_vec<12>>(this, {"buffer_write_float32x12", "buffer_write_float12"});
        reg.bind({"buffer_write_float64", "buffer_write_double"}, [this](const CallContext& ctx) {
            auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (!b) return RValue(0.0);
            buf_write_le<f64>(*b, ctx.real(1));
            return RValue(1.0);
        });

        // ---- Strings, Raw Data, Hex, Base64, Copy ----
        reg.bind("buffer_read_string", [this](const CallContext& ctx) {
            auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (!b || b->error) return RValue(std::string{});
            for (usize i = b->pos; i < b->data.size(); ++i) {
                if (b->data[i] == 0u) {
                    std::string s(reinterpret_cast<const char*>(b->data.data() + b->pos), i - b->pos);
                    b->pos = i + 1;
                    return RValue(std::move(s));
                }
            }
            b->error = true;
            return RValue(std::string{});
        });
        reg.bind("buffer_write_string", [this](const CallContext& ctx) {
            auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (!b) return RValue(0.0);
            const std::string s = ctx.str(1);
            const usize cstr_len = std::strlen(s.c_str());
            buf_write_at_pos(*b, s.c_str(), cstr_len + 1);
            return RValue(1.0);
        });
        reg.bind("buffer_read_data", [this](const CallContext& ctx) {
            auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(0, 0)));
            const usize len = static_cast<usize>(gm_clamp_cast<u32>(ctx.real(1, 0.0)));
            if (!b || b->error || b->pos + len > b->data.size()) {
                if (b) b->error = true;
                return RValue(std::string{});
            }
            std::string s(reinterpret_cast<const char*>(b->data.data() + b->pos), len);
            b->pos += len;
            return RValue(std::move(s));
        });
        reg.bind("buffer_write_data", [this](const CallContext& ctx) {
            auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (!b) return RValue(0.0);
            const std::string s = ctx.str(1);
            buf_write_at_pos(*b, s.data(), s.size());
            return RValue(1.0);
        });
        reg.bind("buffer_read_hex", [this](const CallContext& ctx) {
            auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(0, 0)));
            const usize len = static_cast<usize>(gm_clamp_cast<u32>(ctx.real(1, 0.0)));
            if (!b || b->error || b->pos + len > b->data.size()) {
                if (b) b->error = true;
                return RValue(std::string{});
            }
            std::string hex = hex_encode(std::span<const u8>(b->data.data() + b->pos, len), false);
            b->pos += len;
            return RValue(std::move(hex));
        });
        reg.bind("buffer_write_hex", [this](const CallContext& ctx) {
            auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (!b) return RValue(0.0);
            const std::string s = ctx.str(1);
            auto nib = [](char c) -> u8 {
                if (c >= '0' && c <= '9') return static_cast<u8>(c - '0');
                if (c >= 'a' && c <= 'f') return static_cast<u8>(c - 'a' + 10);
                if (c >= 'A' && c <= 'F') return static_cast<u8>(c - 'A' + 10);
                return 0u;
            };
            std::vector<u8> bytes;
            bytes.reserve(s.size() / 2);
            for (usize i = 0; i + 1 < s.size(); i += 2) {
                bytes.push_back(static_cast<u8>((nib(s[i]) << 4u) | nib(s[i + 1])));
            }
            // Exact gm82buf behavior: appends at end and does NOT change pos.
            buf_append_end(*b, bytes.data(), bytes.size());
            return RValue(1.0);
        });
        reg.bind("buffer_encode_base64", [this](const CallContext& ctx) {
            auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(0, 0)));
            const usize len = static_cast<usize>(gm_clamp_cast<u32>(ctx.real(1, 0.0)));
            if (!b || b->error || b->pos + len > b->data.size()) {
                if (b) b->error = true;
                return RValue(std::string{});
            }
            std::string out = encode_base64_gm82(std::span<const u8>(b->data.data() + b->pos, len));
            b->pos += len;
            return RValue(std::move(out));
        });
        reg.bind("buffer_decode_base64", [this](const CallContext& ctx) {
            auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (!b) return RValue(0.0);
            const std::vector<u8> dec = decode_base64_gm82(ctx.str(1));
            // Exact gm82buf behavior: appends at end and does NOT change pos.
            buf_append_end(*b, dec.data(), dec.size());
            return RValue(1.0);
        });
        reg.bind({"buffer_write_buffer", "buffer_copy"}, [this](const CallContext& ctx) {
            auto* dst = buffers_->find(static_cast<u32>(ctx.i32_arg(0, 0)));
            const auto* src = buffers_->find(static_cast<u32>(ctx.i32_arg(1, 0)));
            if (!dst || !src) return RValue(0.0);
            std::vector<u8> copy = src->data;
            buf_append_end(*dst, copy.data(), copy.size());
            dst->pos = dst->data.size();
            return RValue(1.0);
        });
        reg.bind({"buffer_write_buffer_part", "buffer_copy_part"}, [this](const CallContext& ctx) {
            auto* dst = buffers_->find(static_cast<u32>(ctx.i32_arg(0, 0)));
            const auto* src = buffers_->find(static_cast<u32>(ctx.i32_arg(1, 0)));
            if (!dst || !src) return RValue(0.0);
            usize pos = static_cast<usize>(gm_clamp_cast<u32>(ctx.real(2, 0.0)));
            usize len = static_cast<usize>(gm_clamp_cast<u32>(ctx.real(3, 0.0)));
            if (pos > src->data.size()) pos = src->data.size();
            if (pos + len > src->data.size()) len = src->data.size() - pos;
            std::vector<u8> slice(src->data.begin() + static_cast<std::ptrdiff_t>(pos),
                                  src->data.begin() + static_cast<std::ptrdiff_t>(pos + len));
            buf_append_end(*dst, slice.data(), slice.size());
            dst->pos = dst->data.size();
            return RValue(1.0);
        });

        // ---- File I/O, RC4, Zlib ----
        reg.bind({"buffer_read_from_file", "buffer_load"}, [this](const CallContext& ctx) {
            auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (!b) return RValue(0.0);
            buf_clear(*b);
            auto res = ctx.host.ext_vfs().read_all_bytes(ctx.str(1));
            if (!res) return RValue(0.0);
            b->data = std::move(res.value());
            b->pos = 0;
            b->error = false;
            return RValue(1.0);
        });
        reg.bind({"buffer_read_from_file_part", "buffer_load_part"}, [this](const CallContext& ctx) {
            auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (!b) return RValue(0.0);
            buf_clear(*b);
            auto res = ctx.host.ext_vfs().read_all_bytes(ctx.str(1));
            if (!res) return RValue(0.0);
            const usize pos = static_cast<usize>(gm_clamp_cast<u32>(ctx.real(2, 0.0)));
            const usize len = static_cast<usize>(gm_clamp_cast<u32>(ctx.real(3, 0.0)));
            if (pos > res.value().size() || pos + len > res.value().size()) {
                return RValue(0.0);
            }
            b->data.assign(res.value().begin() + static_cast<std::ptrdiff_t>(pos),
                           res.value().begin() + static_cast<std::ptrdiff_t>(pos + len));
            b->pos = 0;
            b->error = false;
            return RValue(1.0);
        });
        reg.bind({"buffer_write_to_file", "buffer_save",
                  "buffer_write_to_temp_file", "buffer_save_temp"}, [this](const CallContext& ctx) {
            const auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (!b) return RValue(0.0);
            return RValue(ctx.host.ext_vfs().write_all_bytes(ctx.str(1), b->data) ? 1.0 : 0.0);
        });
        reg.bind({"buffer_append_to_file", "buffer_save_append"}, [this](const CallContext& ctx) {
            const auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (!b) return RValue(0.0);
            const std::string path = ctx.str(1);
            std::vector<u8> combined;
            if (auto existing = ctx.host.ext_vfs().read_all_bytes(path)) {
                combined = std::move(existing.value());
            }
            combined.insert(combined.end(), b->data.begin(), b->data.end());
            return RValue(ctx.host.ext_vfs().write_all_bytes(path, combined) ? 1.0 : 0.0);
        });
        reg.bind({"buffer_rc4_crypt", "buffer_rc4"}, [this](const CallContext& ctx) {
            auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (!b) return RValue(0.0);
            const std::string key = ctx.str(1);
            rc4_crypt_inplace(b->data, std::span<const u8>(reinterpret_cast<const u8*>(key.data()), key.size()));
            return RValue(1.0);
        });
        reg.bind({"buffer_rc4_crypt_buffer", "buffer_rc4_buffer"}, [this](const CallContext& ctx) {
            auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(0, 0)));
            const auto* k = buffers_->find(static_cast<u32>(ctx.i32_arg(1, 0)));
            if (!b || !k) return RValue(0.0);
            std::vector<u8> key_copy = k->data;
            rc4_crypt_inplace(b->data, key_copy);
            return RValue(1.0);
        });
        reg.bind({"buffer_zlib_compress", "buffer_deflate"}, [this](const CallContext& ctx) {
            auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (!b) return RValue(0.0);
            auto z = zlib_compress(b->data);
            if (!z) return RValue(0.0);
            b->data = std::move(z.value());
            b->pos = 0;
            b->error = false;
            return RValue(1.0);
        });
        reg.bind({"buffer_zlib_uncompress", "buffer_inflate"}, [this](const CallContext& ctx) {
            auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (!b) return RValue(0.0);
            auto z = zlib_decompress(b->data);
            if (!z) return RValue(0.0);
            b->data = std::move(z.value());
            b->pos = 0;
            b->error = false;
            return RValue(1.0);
        });

        // ---- Virtual Named Pipe (cross-platform replacement for \\.\pipe\gm82netpipe.*) ----
        reg.bind("buffer_make_pipe", [this](const CallContext& ctx) {
            const auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (!b) return RValue(std::string{});
            const std::string pipe_name = "\\\\.\\pipe\\gm82netpipe." + std::to_string(next_pipe_id_++);
            ctx.host.ext_vfs().register_virtual_pipe(pipe_name, b->data, /*one_shot=*/true);
            return RValue(pipe_name);
        });

        // ---- Hashing (MD5 / SHA-1 / CRC-32) ----
        reg.bind("md5_string", [](std::string_view s) {
            const auto d = md5_bytes(std::span<const u8>(reinterpret_cast<const u8*>(s.data()), s.size()));
            return hex_encode(d, false);
        });
        reg.bind("md5_file", [](IExtensionHost& host, std::string_view path) {
            std::vector<u8> bytes;
            if (auto r = host.ext_vfs().read_all_bytes(path)) bytes = std::move(r.value());
            return hex_encode(md5_bytes(bytes), false);
        });
        reg.bind({"md5_buffer", "md5_buffer_part"}, [this](const CallContext& ctx) {
            const auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (!b) return RValue(std::string{});
            std::span<const u8> slice = b->data;
            if (ctx.func_name == "md5_buffer_part") {
                const usize pos = std::min<usize>(b->data.size(), gm_clamp_cast<u32>(ctx.real(1, 0.0)));
                const usize len = std::min<usize>(b->data.size() - pos, gm_clamp_cast<u32>(ctx.real(2, 0.0)));
                slice = std::span<const u8>(b->data.data() + pos, len);
            }
            return RValue(hex_encode(md5_bytes(slice), false));
        });
        reg.bind("sha1_string", [](std::string_view s) {
            const auto d = sha1_bytes(std::span<const u8>(reinterpret_cast<const u8*>(s.data()), s.size()));
            return hex_encode(d, false);
        });
        reg.bind("sha1_file", [](IExtensionHost& host, std::string_view path) {
            std::vector<u8> bytes;
            if (auto r = host.ext_vfs().read_all_bytes(path)) bytes = std::move(r.value());
            return hex_encode(sha1_bytes(bytes), false);
        });
        reg.bind({"sha1_buffer", "sha1_buffer_part"}, [this](const CallContext& ctx) {
            const auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (!b) return RValue(std::string{});
            std::span<const u8> slice = b->data;
            if (ctx.func_name == "sha1_buffer_part") {
                const usize pos = std::min<usize>(b->data.size(), gm_clamp_cast<u32>(ctx.real(1, 0.0)));
                const usize len = std::min<usize>(b->data.size() - pos, gm_clamp_cast<u32>(ctx.real(2, 0.0)));
                slice = std::span<const u8>(b->data.data() + pos, len);
            }
            return RValue(hex_encode(sha1_bytes(slice), false));
        });
        reg.bind("crc32_string", [](std::string_view s) {
            return format_crc32_hex(crc32_ieee(std::span<const u8>(reinterpret_cast<const u8*>(s.data()), s.size())));
        });
        reg.bind("crc32_file", [](IExtensionHost& host, std::string_view path) {
            std::vector<u8> bytes;
            if (auto r = host.ext_vfs().read_all_bytes(path)) bytes = std::move(r.value());
            return format_crc32_hex(crc32_ieee(bytes));
        });
        reg.bind({"crc32_buffer", "crc32_buffer_part"}, [this](const CallContext& ctx) {
            const auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (!b) return RValue(std::string{});
            std::span<const u8> slice = b->data;
            if (ctx.func_name == "crc32_buffer_part") {
                const usize pos = std::min<usize>(b->data.size(), gm_clamp_cast<u32>(ctx.real(1, 0.0)));
                const usize len = std::min<usize>(b->data.size() - pos, gm_clamp_cast<u32>(ctx.real(2, 0.0)));
                slice = std::span<const u8>(b->data.data() + pos, len);
            }
            return RValue(format_crc32_hex(crc32_ieee(slice)));
        });

        // ---- TCP Listeners ----
        reg.bind("listener_create", [this]() {
            const u32 id = next_listener_id_++;
            listeners_.emplace(id, TcpListener{});
            return static_cast<f64>(id);
        });
        reg.bind("listener_destroy", [this](const CallContext& ctx) {
            auto it = listeners_.find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (it == listeners_.end()) return RValue(0.0);
#if !defined(__EMSCRIPTEN__)
            close_native_socket(it->second.sock);
#endif
            listeners_.erase(it);
            return RValue(1.0);
        });
        reg.bind("listener_exists", [this](const CallContext& ctx) {
            return RValue(listeners_.contains(static_cast<u32>(ctx.i32_arg(0, 0))) ? 1.0 : 0.0);
        });
        reg.bind("listener_is_active", [this](const CallContext& ctx) {
            auto it = listeners_.find(static_cast<u32>(ctx.i32_arg(0, 0)));
            return RValue((it != listeners_.end() && it->second.listening) ? 1.0 : 0.0);
        });
        reg.bind("listener_start", [this](const CallContext& ctx) {
            auto it = listeners_.find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (it == listeners_.end()) return RValue(0.0);
#if !defined(__EMSCRIPTEN__)
            close_native_socket(it->second.sock);
            it->second.listening = false;
            const bool ipv6 = ctx.boolean(1, false);
            const u16 port = gm_clamp_cast<u16>(ctx.real(2, 0.0));
            const bool local = ctx.boolean(3, false);
            addrinfo hints{}, *res = nullptr;
            hints.ai_family = ipv6 ? AF_INET6 : AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_protocol = IPPROTO_TCP;
            hints.ai_flags = local ? 0 : AI_PASSIVE;
            const std::string port_s = std::to_string(port);
            if (getaddrinfo(local ? "localhost" : nullptr, port_s.c_str(), &hints, &res) == 0 && res) {
                NativeSocket s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
                if (s != kInvalidSocket) {
                    int opt = 1;
                    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
                    if (bind(s, res->ai_addr, static_cast<int>(res->ai_addrlen)) == 0 && listen(s, SOMAXCONN) == 0) {
                        set_nonblocking(s);
                        it->second.sock = s;
                        it->second.listening = true;
                    } else {
                        close_native_socket(s);
                    }
                }
                freeaddrinfo(res);
            }
#endif
            return RValue(1.0);
        });
        reg.bind("listener_stop", [this](const CallContext& ctx) {
            auto it = listeners_.find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (it == listeners_.end()) return RValue(0.0);
#if !defined(__EMSCRIPTEN__)
            close_native_socket(it->second.sock);
#endif
            it->second.listening = false;
            return RValue(1.0);
        });
        reg.bind("listener_pending", [this](const CallContext& ctx) {
            auto it = listeners_.find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (it == listeners_.end() || !it->second.listening) return RValue(0.0);
#if !defined(__EMSCRIPTEN__)
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(it->second.sock, &rfds);
            timeval tv{0, 0};
            const int r = select(static_cast<int>(it->second.sock) + 1, &rfds, nullptr, nullptr, &tv);
            if (r < 0) {
                close_native_socket(it->second.sock);
                it->second.listening = false;
                return RValue(0.0);
            }
            return RValue(r > 0 ? 1.0 : 0.0);
#else
            return RValue(0.0);
#endif
        });
        reg.bind("listener_accept", [this](const CallContext& ctx) {
            auto lit = listeners_.find(static_cast<u32>(ctx.i32_arg(0, 0)));
            auto sit = sockets_.find(static_cast<u32>(ctx.i32_arg(1, 0)));
            if (lit == listeners_.end() || sit == sockets_.end() || !lit->second.listening) return RValue(0.0);
#if !defined(__EMSCRIPTEN__)
            reset_tcp_socket(sit->second);
            sockaddr_storage ss{};
            socklen_t slen = sizeof(ss);
            NativeSocket ns = accept(lit->second.sock, reinterpret_cast<sockaddr*>(&ss), &slen);
            if (ns == kInvalidSocket) {
                sit->second.state = 5;
                return RValue(1.0);
            }
            configure_tcp_socket(ns);
            sit->second.sock = ns;
            sit->second.peer = ss;
            sit->second.has_peer = true;
            sit->second.state = 2;
            return RValue(1.0);
#else
            return RValue(0.0);
#endif
        });

        // ---- TCP Sockets ----
        reg.bind("socket_create", [this]() {
            const u32 id = next_socket_id_++;
            sockets_.emplace(id, TcpSocket{});
            return static_cast<f64>(id);
        });
        reg.bind("socket_destroy", [this](const CallContext& ctx) {
            auto it = sockets_.find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (it == sockets_.end()) return RValue(0.0);
            reset_tcp_socket(it->second);
            sockets_.erase(it);
            return RValue(1.0);
        });
        reg.bind("socket_exists", [this](const CallContext& ctx) {
            return RValue(sockets_.contains(static_cast<u32>(ctx.i32_arg(0, 0))) ? 1.0 : 0.0);
        });
        reg.bind("socket_get_state", [this](const CallContext& ctx) {
            auto it = sockets_.find(static_cast<u32>(ctx.i32_arg(0, 0)));
            return RValue(it != sockets_.end() ? static_cast<f64>(it->second.state) : 0.0);
        });
        reg.bind("socket_reset", [this](const CallContext& ctx) {
            auto it = sockets_.find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (it == sockets_.end()) return RValue(0.0);
            reset_tcp_socket(it->second);
            return RValue(1.0);
        });
        reg.bind("socket_connect", [this](const CallContext& ctx) {
            auto it = sockets_.find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (it == sockets_.end()) return RValue(0.0);
#if !defined(__EMSCRIPTEN__)
            auto& sock = it->second;
            if (sock.state != 0) reset_tcp_socket(sock);
            const std::string addr = ctx.str(1);
            const u16 port = gm_clamp_cast<u16>(ctx.real(2, 0.0));
            addrinfo hints{}, *res = nullptr;
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_protocol = IPPROTO_TCP;
            const std::string port_s = std::to_string(port);
            if (getaddrinfo(addr.c_str(), port_s.c_str(), &hints, &res) == 0 && res) {
                NativeSocket s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
                if (s != kInvalidSocket) {
                    configure_tcp_socket(s);
                    const int r = connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen));
                    if (r == 0) {
                        sock.sock = s;
                        std::memcpy(&sock.peer, res->ai_addr, std::min<usize>(sizeof(sock.peer), res->ai_addrlen));
                        sock.has_peer = true;
                        sock.state = 2;
                    } else if (last_sock_would_block()) {
                        sock.sock = s;
                        std::memcpy(&sock.peer, res->ai_addr, std::min<usize>(sizeof(sock.peer), res->ai_addrlen));
                        sock.has_peer = true;
                        sock.state = 1;
                    } else {
                        close_native_socket(s);
                        sock.state = 5;
                    }
                } else {
                    sock.state = 5;
                }
                freeaddrinfo(res);
            } else {
                sock.state = 5;
            }
#else
            it->second.state = 5;
#endif
            return RValue(1.0);
        });
        reg.bind("socket_receive", [this](const CallContext& ctx) {
            auto it = sockets_.find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (it == sockets_.end()) return RValue(0.0);
#if !defined(__EMSCRIPTEN__)
            auto& sock = it->second;
            if (sock.state == 1 && sock.sock != kInvalidSocket) {
                fd_set wfds, efds;
                FD_ZERO(&wfds); FD_ZERO(&efds);
                FD_SET(sock.sock, &wfds); FD_SET(sock.sock, &efds);
                timeval tv{0, 0};
                if (select(static_cast<int>(sock.sock) + 1, nullptr, &wfds, &efds, &tv) > 0) {
                    if (FD_ISSET(sock.sock, &efds)) {
                        close_native_socket(sock.sock);
                        sock.state = 5;
                    } else if (FD_ISSET(sock.sock, &wfds)) {
                        int err = 0;
                        socklen_t elen = sizeof(err);
                        getsockopt(sock.sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &elen);
                        if (err == 0) sock.state = 2;
                        else { close_native_socket(sock.sock); sock.state = 5; }
                    }
                }
            }
            if (sock.state == 2 && sock.sock != kInvalidSocket) {
                u8 tmp[10240];
                for (;;) {
                    const int n = static_cast<int>(recv(sock.sock, reinterpret_cast<char*>(tmp), sizeof(tmp), 0));
                    if (n > 0) {
                        sock.read_buf.insert(sock.read_buf.end(), tmp, tmp + n);
                        if (static_cast<usize>(n) < sizeof(tmp)) break;
                    } else if (n == 0) {
                        sock.state = 4;
                        break;
                    } else {
                        if (!last_sock_would_block()) {
                            close_native_socket(sock.sock);
                            sock.state = 5;
                        }
                        break;
                    }
                }
            }
#endif
            return RValue(1.0);
        });
        reg.bind("socket_send", [this](const CallContext& ctx) {
            auto it = sockets_.find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (it == sockets_.end()) return RValue(0.0);
#if !defined(__EMSCRIPTEN__)
            auto& sock = it->second;
            // tcp_closed records the peer's FIN. Our write half can still send a response.
            if ((sock.state == 2 || sock.state == 4) && sock.sock != kInvalidSocket && !sock.write_buf.empty()) {
#if defined(MSG_NOSIGNAL)
                constexpr int flags = MSG_NOSIGNAL;
#else
                constexpr int flags = 0;
#endif
                const int n = static_cast<int>(send(
                    sock.sock, reinterpret_cast<const char*>(sock.write_buf.data()),
                    static_cast<int>(sock.write_buf.size()), flags));
                if (n > 0) {
                    sock.write_buf.erase(sock.write_buf.begin(), sock.write_buf.begin() + n);
                } else if (n < 0 && !last_sock_would_block()) {
                    close_native_socket(sock.sock);
                    sock.state = 5;
                }
                if (sock.should_shut_down && sock.write_buf.empty() && sock.sock != kInvalidSocket) {
#if defined(_WIN32)
                    shutdown(sock.sock, SD_SEND);
#else
                    shutdown(sock.sock, SHUT_WR);
#endif
                }
            }
#endif
            return RValue(1.0);
        });
        reg.bind("socket_close", [this](const CallContext& ctx) {
            auto it = sockets_.find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (it == sockets_.end()) return RValue(0.0);
            it->second.should_shut_down = true;
            return RValue(1.0);
        });
        reg.bind("socket_get_peer_address", [this](const CallContext& ctx) {
            auto it = sockets_.find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (it == sockets_.end() || (it->second.state != 2 && it->second.state != 4)) return RValue(std::string{});
#if !defined(__EMSCRIPTEN__)
            return RValue(it->second.has_peer ? sockaddr_to_ip(it->second.peer) : std::string{});
#else
            return RValue(std::string{});
#endif
        });
        reg.bind("socket_read_data", [this](const CallContext& ctx) {
            auto sit = sockets_.find(static_cast<u32>(ctx.i32_arg(0, 0)));
            auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(1, 0)));
            const usize bytes = static_cast<usize>(gm_clamp_cast<u32>(ctx.real(2, 0.0)));
            if (sit == sockets_.end() || !b || bytes > sit->second.read_buf.size()) return RValue(0.0);
            buf_clear(*b);
            b->data.assign(sit->second.read_buf.begin(), sit->second.read_buf.begin() + static_cast<std::ptrdiff_t>(bytes));
            sit->second.read_buf.erase(sit->second.read_buf.begin(), sit->second.read_buf.begin() + static_cast<std::ptrdiff_t>(bytes));
            return RValue(1.0);
        });
        reg.bind("socket_write_data", [this](const CallContext& ctx) {
            auto sit = sockets_.find(static_cast<u32>(ctx.i32_arg(0, 0)));
            const auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(1, 0)));
            if (sit == sockets_.end() || !b) return RValue(0.0);
            if (!sit->second.should_shut_down && sit->second.state != 5) {
                sit->second.write_buf.insert(sit->second.write_buf.end(), b->data.begin(), b->data.end());
            }
            return RValue(1.0);
        });
        reg.bind("socket_read_message", [this](const CallContext& ctx) {
            auto sit = sockets_.find(static_cast<u32>(ctx.i32_arg(0, 0)));
            auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(1, 0)));
            if (sit == sockets_.end() || !b) return RValue(0.0);
            u32 msg_len = 0;
            usize hdr_len = 0;
            if (!decode_uintv(sit->second.read_buf, msg_len, hdr_len)) return RValue(0.0);
            if (hdr_len + msg_len > sit->second.read_buf.size()) return RValue(0.0);
            buf_clear(*b);
            b->data.assign(sit->second.read_buf.begin() + static_cast<std::ptrdiff_t>(hdr_len),
                           sit->second.read_buf.begin() + static_cast<std::ptrdiff_t>(hdr_len + msg_len));
            sit->second.read_buf.erase(sit->second.read_buf.begin(),
                                       sit->second.read_buf.begin() + static_cast<std::ptrdiff_t>(hdr_len + msg_len));
            return RValue(1.0);
        });
        reg.bind("socket_write_message", [this](const CallContext& ctx) {
            auto sit = sockets_.find(static_cast<u32>(ctx.i32_arg(0, 0)));
            const auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(1, 0)));
            if (sit == sockets_.end() || !b) return RValue(0.0);
            if (!sit->second.should_shut_down && sit->second.state != 5) {
                encode_uintv(sit->second.write_buf, static_cast<u32>(b->data.size()));
                sit->second.write_buf.insert(sit->second.write_buf.end(), b->data.begin(), b->data.end());
            }
            return RValue(1.0);
        });
        reg.bind("socket_read_message_delimiter", [this](const CallContext& ctx) {
            auto sit = sockets_.find(static_cast<u32>(ctx.i32_arg(0, 0)));
            auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(1, 0)));
            const std::string delim = ctx.str(2);
            if (sit == sockets_.end() || !b) return RValue(0.0);
            const u8 d = delim.empty() ? 0u : static_cast<u8>(delim[0]);
            const auto& rb = sit->second.read_buf;
            const auto pos = std::find(rb.begin(), rb.end(), d);
            if (pos == rb.end()) return RValue(0.0);
            buf_clear(*b);
            b->data.assign(rb.begin(), pos);
            sit->second.read_buf.erase(rb.begin(), pos + 1);
            return RValue(1.0);
        });
        reg.bind("socket_write_message_delimiter", [this](const CallContext& ctx) {
            auto sit = sockets_.find(static_cast<u32>(ctx.i32_arg(0, 0)));
            const auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(1, 0)));
            const std::string delim = ctx.str(2);
            if (sit == sockets_.end() || !b) return RValue(0.0);
            if (!sit->second.should_shut_down && sit->second.state != 5) {
                const u8 d = delim.empty() ? 0u : static_cast<u8>(delim[0]);
                sit->second.write_buf.insert(sit->second.write_buf.end(), b->data.begin(), b->data.end());
                sit->second.write_buf.push_back(d);
            }
            return RValue(1.0);
        });
        reg.bind("socket_get_receiving_size", [this](const CallContext& ctx) {
            auto sit = sockets_.find(static_cast<u32>(ctx.i32_arg(0, 0)));
            return RValue(sit != sockets_.end() ? static_cast<f64>(sit->second.read_buf.size()) : 0.0);
        });
        reg.bind("socket_get_sending_size", [this](const CallContext& ctx) {
            auto sit = sockets_.find(static_cast<u32>(ctx.i32_arg(0, 0)));
            return RValue(sit != sockets_.end() ? static_cast<f64>(sit->second.write_buf.size()) : 0.0);
        });

        // ---- UDP Sockets ----
        reg.bind("udpsocket_create", [this]() {
            const u32 id = next_udp_id_++;
            udpsockets_.emplace(id, UdpSocket{});
            return static_cast<f64>(id);
        });
        reg.bind("udpsocket_destroy", [this](const CallContext& ctx) {
            auto it = udpsockets_.find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (it == udpsockets_.end()) return RValue(0.0);
#if !defined(__EMSCRIPTEN__)
            close_native_socket(it->second.sock);
#endif
            udpsockets_.erase(it);
            return RValue(1.0);
        });
        reg.bind("udpsocket_exists", [this](const CallContext& ctx) {
            return RValue(udpsockets_.contains(static_cast<u32>(ctx.i32_arg(0, 0))) ? 1.0 : 0.0);
        });
        reg.bind("udpsocket_get_state", [this](const CallContext& ctx) {
            auto it = udpsockets_.find(static_cast<u32>(ctx.i32_arg(0, 0)));
            return RValue(it != udpsockets_.end() ? static_cast<f64>(it->second.state) : 0.0);
        });
        reg.bind("udpsocket_reset", [this](const CallContext& ctx) {
            auto it = udpsockets_.find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (it == udpsockets_.end()) return RValue(0.0);
#if !defined(__EMSCRIPTEN__)
            close_native_socket(it->second.sock);
            it->second.dest_len = 0;
            it->second.has_last_from = false;
#endif
            it->second.state = 0;
            return RValue(1.0);
        });
        reg.bind("udpsocket_start", [this](const CallContext& ctx) {
            auto it = udpsockets_.find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (it == udpsockets_.end()) return RValue(0.0);
#if !defined(__EMSCRIPTEN__)
            close_native_socket(it->second.sock);
            const bool ipv6 = ctx.boolean(1, false);
            const u16 port = gm_clamp_cast<u16>(ctx.real(2, 0.0));
            it->second.ipv6 = ipv6;
            NativeSocket s = socket(ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (s == kInvalidSocket) {
                it->second.state = 2;
                return RValue(1.0);
            }
            if (port != 0) {
                sockaddr_storage ss{};
                int slen = 0;
                if (ipv6) {
                    auto* s6 = reinterpret_cast<sockaddr_in6*>(&ss);
                    s6->sin6_family = AF_INET6;
                    s6->sin6_port = htons(port);
                    s6->sin6_addr = in6addr_any;
                    slen = sizeof(sockaddr_in6);
                } else {
                    auto* s4 = reinterpret_cast<sockaddr_in*>(&ss);
                    s4->sin_family = AF_INET;
                    s4->sin_port = htons(port);
                    s4->sin_addr.s_addr = INADDR_ANY;
                    slen = sizeof(sockaddr_in);
                }
                if (bind(s, reinterpret_cast<const sockaddr*>(&ss), slen) != 0) {
                    close_native_socket(s);
                    it->second.state = 2;
                    return RValue(1.0);
                }
            }
            set_nonblocking(s);
            it->second.sock = s;
            it->second.state = 1;
#else
            it->second.state = 2;
#endif
            return RValue(1.0);
        });
        reg.bind("udpsocket_set_destination", [this](const CallContext& ctx) {
            auto it = udpsockets_.find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (it == udpsockets_.end()) return RValue(0.0);
#if !defined(__EMSCRIPTEN__)
            const std::string addr = ctx.str(1);
            const u16 port = gm_clamp_cast<u16>(ctx.real(2, 0.0));
            addrinfo hints{}, *res = nullptr;
            hints.ai_family = it->second.ipv6 ? AF_INET6 : AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            hints.ai_protocol = IPPROTO_UDP;
            const std::string port_s = std::to_string(port);
            if (getaddrinfo(addr.c_str(), port_s.c_str(), &hints, &res) == 0 && res) {
                std::memcpy(&it->second.dest, res->ai_addr, std::min<usize>(sizeof(it->second.dest), res->ai_addrlen));
                it->second.dest_len = static_cast<int>(res->ai_addrlen);
                freeaddrinfo(res);
            } else {
                close_native_socket(it->second.sock);
                it->second.state = 2;
            }
#endif
            return RValue(1.0);
        });
        reg.bind("udpsocket_receive", [this](const CallContext& ctx) {
            auto it = udpsockets_.find(static_cast<u32>(ctx.i32_arg(0, 0)));
            auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(1, 0)));
            if (it == udpsockets_.end() || !b || it->second.state != 1) return RValue(0.0);
#if !defined(__EMSCRIPTEN__)
            u8 pkt[65536];
            sockaddr_storage from{};
            socklen_t flen = sizeof(from);
            const int n = static_cast<int>(recvfrom(
                it->second.sock, reinterpret_cast<char*>(pkt), sizeof(pkt), 0,
                reinterpret_cast<sockaddr*>(&from), &flen));
            if (n < 0) return RValue(0.0);
            buf_clear(*b);
            b->data.assign(pkt, pkt + n);
            it->second.last_from = from;
            it->second.has_last_from = true;
            return RValue(1.0);
#else
            return RValue(0.0);
#endif
        });
        reg.bind("udpsocket_send", [this](const CallContext& ctx) {
            auto it = udpsockets_.find(static_cast<u32>(ctx.i32_arg(0, 0)));
            const auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(1, 0)));
            if (it == udpsockets_.end() || !b || it->second.state != 1) return RValue(0.0);
#if !defined(__EMSCRIPTEN__)
            if (it->second.dest_len > 0) {
                (void)sendto(it->second.sock, reinterpret_cast<const char*>(b->data.data()),
                             static_cast<int>(b->data.size()), 0,
                             reinterpret_cast<const sockaddr*>(&it->second.dest), it->second.dest_len);
            }
#endif
            return RValue(1.0);
        });
        reg.bind("udpsocket_get_last_address", [this](const CallContext& ctx) {
#if !defined(__EMSCRIPTEN__)
            auto it = udpsockets_.find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (it != udpsockets_.end() && it->second.has_last_from) {
                return RValue(sockaddr_to_ip(it->second.last_from));
            }
#endif
            return RValue(std::string{});
        });
        reg.bind("udpsocket_get_last_port", [this](const CallContext& ctx) {
#if !defined(__EMSCRIPTEN__)
            auto it = udpsockets_.find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (it != udpsockets_.end() && it->second.has_last_from) {
                return RValue(static_cast<f64>(sockaddr_to_port(it->second.last_from)));
            }
#endif
            return RValue(0.0);
        });
        reg.bind("udpsocket_get_ttl", [this](const CallContext& ctx) {
#if !defined(__EMSCRIPTEN__)
            auto it = udpsockets_.find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (it != udpsockets_.end() && it->second.sock != kInvalidSocket) {
                int ttl = 64;
                socklen_t len = sizeof(ttl);
                if (getsockopt(it->second.sock, IPPROTO_IP, IP_TTL, reinterpret_cast<char*>(&ttl), &len) == 0) {
                    return RValue(static_cast<f64>(ttl));
                }
            }
#endif
            return RValue(0.0);
        });
        reg.bind("udpsocket_set_ttl", [this](const CallContext& ctx) {
            auto it = udpsockets_.find(static_cast<u32>(ctx.i32_arg(0, 0)));
            if (it == udpsockets_.end()) return RValue(0.0);
#if !defined(__EMSCRIPTEN__)
            if (it->second.sock != kInvalidSocket) {
                int ttl = std::clamp(ctx.i32_arg(1, 64), 1, 255);
                setsockopt(it->second.sock, IPPROTO_IP, IP_TTL, reinterpret_cast<const char*>(&ttl), sizeof(ttl));
            }
#endif
            return RValue(1.0);
        });
    }

    void on_unload(IExtensionHost&) override {
        if (buffers_) buffers_->clear_all();
        close_all_sockets();
        next_pipe_id_ = 0;
    }

private:
    template <typename T>
    f64 read_int(const CallContext& ctx) {
        auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(0, 0)));
        T v = 0;
        return (b && buf_read_le(*b, v)) ? static_cast<f64>(v) : 0.0;
    }

    template <typename T>
    f64 read_float(const CallContext& ctx) {
        auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(0, 0)));
        T v = static_cast<T>(0.0);
        if (!b || !buf_read_le(*b, v) || !std::isfinite(v)) return 0.0;
        return static_cast<f64>(v);
    }

    template <typename T>
    f64 write_clamped(const CallContext& ctx) {
        auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(0, 0)));
        if (!b) return 0.0;
        buf_write_le<T>(*b, gm_clamp_cast<T>(ctx.real(1)));
        return 1.0;
    }

    template <usize Count>
    f64 write_f32_vec(const CallContext& ctx) {
        auto* b = buffers_->find(static_cast<u32>(ctx.i32_arg(0, 0)));
        if (!b) return 0.0;
        for (usize i = 1; i <= Count; ++i) {
            buf_write_le<f32>(*b, gm_clamp_cast<f32>(ctx.real(i)));
        }
        return 1.0;
    }

    static void reset_tcp_socket(TcpSocket& s) noexcept {
#if !defined(__EMSCRIPTEN__)
        close_native_socket(s.sock);
        s.has_peer = false;
#endif
        s.state = 0;
        s.should_shut_down = false;
        s.read_buf.clear();
        s.write_buf.clear();
    }

    void close_all_sockets() noexcept {
#if !defined(__EMSCRIPTEN__)
        for (auto& [_, l] : listeners_) close_native_socket(l.sock);
        for (auto& [_, s] : sockets_) close_native_socket(s.sock);
        for (auto& [_, u] : udpsockets_) close_native_socket(u.sock);
#endif
        listeners_.clear();
        sockets_.clear();
        udpsockets_.clear();
        next_listener_id_ = 1;
        next_socket_id_ = 1;
        next_udp_id_ = 1;
    }

    GmBufferStore* buffers_ = nullptr;
#if defined(_WIN32)
    bool wsa_ok_ = false;
#endif
    u32 next_pipe_id_ = 0;
    std::unordered_map<u32, TcpListener> listeners_;
    std::unordered_map<u32, TcpSocket> sockets_;
    std::unordered_map<u32, UdpSocket> udpsockets_;
    u32 next_listener_id_ = 1;
    u32 next_socket_id_ = 1;
    u32 next_udp_id_ = 1;
};

} // namespace

std::unique_ptr<IExtension> create_gm82buf_extension() {
    return std::make_unique<Gm82BufExtension>();
}

} // namespace gm
