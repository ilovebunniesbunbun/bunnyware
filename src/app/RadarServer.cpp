#include "RadarServer.hpp"

#include "logger.hpp"

#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits.h>
#include <sstream>
#include <unordered_map>

namespace radar {

namespace {

// -----------------------------------------------------------------------------
// Small crypto helpers (SHA-1 + base64) used only for the WebSocket handshake.
// -----------------------------------------------------------------------------
void sha1(const uint8_t* data, size_t len, uint8_t out[20])
{
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
    const uint64_t ml = static_cast<uint64_t>(len) * 8;
    size_t total = len + 1;
    while (total % 64 != 56) total++;
    total += 8;

    std::vector<uint8_t> msg(total, 0);
    if (len) std::memcpy(msg.data(), data, len);
    msg[len] = 0x80;
    for (int i = 0; i < 8; ++i) msg[total - 1 - i] = static_cast<uint8_t>((ml >> (8 * i)) & 0xff);

    for (size_t off = 0; off < total; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(msg[off + 4 * i]) << 24) |
                   (static_cast<uint32_t>(msg[off + 4 * i + 1]) << 16) |
                   (static_cast<uint32_t>(msg[off + 4 * i + 2]) << 8) |
                   static_cast<uint32_t>(msg[off + 4 * i + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            uint32_t v = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = (v << 1) | (v >> 31);
        }
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            const uint32_t tmp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d;
            d = c;
            c = (b << 30) | (b >> 2);
            b = a;
            a = tmp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    const uint32_t hs[5] = { h0, h1, h2, h3, h4 };
    for (int i = 0; i < 5; ++i) {
        out[4 * i] = static_cast<uint8_t>((hs[i] >> 24) & 0xff);
        out[4 * i + 1] = static_cast<uint8_t>((hs[i] >> 16) & 0xff);
        out[4 * i + 2] = static_cast<uint8_t>((hs[i] >> 8) & 0xff);
        out[4 * i + 3] = static_cast<uint8_t>(hs[i] & 0xff);
    }
}

std::string base64(const uint8_t* d, size_t n)
{
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve(((n + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= n) {
        const uint32_t v = (d[i] << 16) | (d[i + 1] << 8) | d[i + 2];
        o += tbl[(v >> 18) & 63];
        o += tbl[(v >> 12) & 63];
        o += tbl[(v >> 6) & 63];
        o += tbl[v & 63];
        i += 3;
    }
    if (n - i == 1) {
        const uint32_t v = d[i] << 16;
        o += tbl[(v >> 18) & 63];
        o += tbl[(v >> 12) & 63];
        o += "==";
    } else if (n - i == 2) {
        const uint32_t v = (d[i] << 16) | (d[i + 1] << 8);
        o += tbl[(v >> 18) & 63];
        o += tbl[(v >> 12) & 63];
        o += tbl[(v >> 6) & 63];
        o += '=';
    }
    return o;
}

// -----------------------------------------------------------------------------
// Socket helpers
// -----------------------------------------------------------------------------
bool send_all(int fd, const void* data, size_t len)
{
    const char* p = static_cast<const char*>(data);
    size_t off = 0;
    while (off < len) {
        const ssize_t n = ::send(fd, p + off, len - off, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

bool recv_exact(int fd, void* data, size_t len)
{
    char* p = static_cast<char*>(data);
    size_t off = 0;
    while (off < len) {
        const ssize_t n = ::recv(fd, p + off, len - off, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

int make_listener(const std::string& host, int port, std::string& err)
{
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    struct addrinfo* res = nullptr;
    const std::string portstr = std::to_string(port);
    const int rc = ::getaddrinfo(host.empty() ? nullptr : host.c_str(), portstr.c_str(), &hints, &res);
    if (rc != 0) {
        err = "getaddrinfo failed for " + host + ":" + portstr + " (" + ::gai_strerror(rc) + ")";
        return -1;
    }
    int fd = -1;
    for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
        fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        const int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (::bind(fd, p->ai_addr, static_cast<socklen_t>(p->ai_addrlen)) == 0 && ::listen(fd, 16) == 0) {
            break;
        }
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(res);
    if (fd < 0) err = "failed to bind/listen on " + host + ":" + portstr;
    return fd;
}

std::string percent_decode(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(in[i + 1]);
            const int lo = hex(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += in[i];
    }
    return out;
}

std::string mime_type(const std::string& path)
{
    const auto dot = path.find_last_of('.');
    const std::string ext = dot == std::string::npos ? std::string() : path.substr(dot + 1);
    static const std::unordered_map<std::string, std::string> types = {
        { "html", "text/html; charset=utf-8" },
        { "css", "text/css; charset=utf-8" },
        { "js", "application/javascript; charset=utf-8" },
        { "json", "application/json" },
        { "json5", "application/json" },
        { "png", "image/png" },
        { "webp", "image/webp" },
        { "svg", "image/svg+xml" },
        { "ico", "image/x-icon" },
        { "jpg", "image/jpeg" },
        { "jpeg", "image/jpeg" },
        { "gif", "image/gif" },
        { "woff", "font/woff" },
        { "woff2", "font/woff2" },
        { "ttf", "font/ttf" },
        { "txt", "text/plain; charset=utf-8" },
    };
    const auto it = types.find(ext);
    return it == types.end() ? "application/octet-stream" : it->second;
}

bool read_file(const std::string& path, std::string& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// -----------------------------------------------------------------------------
// WebSocket framing
// -----------------------------------------------------------------------------
bool ws_send_frame(int fd, uint8_t opcode, const std::string& payload)
{
    std::vector<uint8_t> hdr;
    hdr.push_back(static_cast<uint8_t>(0x80 | opcode));
    const size_t n = payload.size();
    if (n < 126) {
        hdr.push_back(static_cast<uint8_t>(n));
    } else if (n < 65536) {
        hdr.push_back(126);
        hdr.push_back(static_cast<uint8_t>((n >> 8) & 0xff));
        hdr.push_back(static_cast<uint8_t>(n & 0xff));
    } else {
        hdr.push_back(127);
        for (int i = 7; i >= 0; --i) hdr.push_back(static_cast<uint8_t>((n >> (8 * i)) & 0xff));
    }
    if (!send_all(fd, hdr.data(), hdr.size())) return false;
    if (n == 0) return true;
    return send_all(fd, payload.data(), payload.size());
}

// Reads a single (possibly fragmented-ignored) text frame. Returns false on
// disconnect/error. Control frames are handled internally.
bool ws_read_frame(int fd, std::string& out)
{
    out.clear();
    for (;;) {
        uint8_t b[2];
        if (!recv_exact(fd, b, 2)) return false;
        const uint8_t opcode = b[0] & 0x0f;
        const bool masked = (b[1] & 0x80) != 0;
        uint64_t len = b[1] & 0x7f;
        if (len == 126) {
            uint8_t e[2];
            if (!recv_exact(fd, e, 2)) return false;
            len = (static_cast<uint64_t>(e[0]) << 8) | e[1];
        } else if (len == 127) {
            uint8_t e[8];
            if (!recv_exact(fd, e, 8)) return false;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | e[i];
        }
        if (len > 1u << 20) return false;  // guard against absurd frames
        uint8_t mask[4] = { 0, 0, 0, 0 };
        if (masked && !recv_exact(fd, mask, 4)) return false;
        std::string payload;
        payload.resize(static_cast<size_t>(len));
        if (len && !recv_exact(fd, &payload[0], static_cast<size_t>(len))) return false;
        if (masked) {
            for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= static_cast<char>(mask[i % 4]);
        }
        if (opcode == 0x8) return false;           // close
        if (opcode == 0x9) {                        // ping -> pong
            ws_send_frame(fd, 0xA, payload);
            continue;
        }
        if (opcode == 0xA) continue;                // pong
        out = std::move(payload);
        return true;
    }
}

std::string ws_accept_key(const std::string& key)
{
    static const char* kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    const std::string concat = key + kGuid;
    uint8_t digest[20];
    sha1(reinterpret_cast<const uint8_t*>(concat.data()), concat.size(), digest);
    return base64(digest, 20);
}

}  // namespace

// -----------------------------------------------------------------------------
// Implementation state
// -----------------------------------------------------------------------------
struct RadarServer::Impl {
    struct WsClient {
        int fd = -1;
        std::mutex write_mutex;
        std::atomic<bool> alive{ true };
    };

    std::string host;
    std::string asset_root;
    std::string root_canon;
    int http_fd = -1;
    int ws_fd = -1;
    int http_port = 0;
    int ws_port = 0;

    std::thread http_thread;
    std::thread ws_thread;
    std::atomic<bool> stop_flag{ false };

    mutable std::mutex clients_mutex;
    std::vector<std::shared_ptr<WsClient>> clients;

    mutable std::mutex error_mutex;
    std::string last_error;

    std::mutex web_mutex;
    WebConfig web;

    std::mutex page_mutex;
    std::string active_page = "waiting";
    std::string last_sent_page;

    std::atomic<uint64_t> updates{ 0 };
    std::vector<std::string> renderer_scripts;

    void set_error(const std::string& msg)
    {
        std::lock_guard<std::mutex> lk(error_mutex);
        last_error = msg;
    }

    std::string get_error() const
    {
        std::lock_guard<std::mutex> lk(error_mutex);
        return last_error;
    }
};

namespace {

using Impl = RadarServer::Impl;

std::string get_active_page(const std::shared_ptr<Impl>& impl)
{
    std::lock_guard<std::mutex> lk(impl->page_mutex);
    return impl->active_page;
}

void send_response(int fd, int code, const std::string& status, const std::string& content_type,
                   const std::string& body)
{
    std::string head;
    head.reserve(256);
    head += "HTTP/1.1 " + std::to_string(code) + " " + status + "\r\n";
    head += "Content-Type: " + content_type + "\r\n";
    head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    head += "Cache-Control: no-cache, no-store, must-revalidate\r\n";
    head += "Access-Control-Allow-Origin: *\r\n";
    head += "Connection: close\r\n\r\n";
    send_all(fd, head.data(), head.size());
    if (!body.empty()) send_all(fd, body.data(), body.size());
}

// Resolves a request sub-path inside the asset root, refusing traversal.
bool resolve_asset(const std::shared_ptr<Impl>& impl, const std::string& subpath, std::string& resolved)
{
    if (impl->root_canon.empty()) return false;
    const std::string joined = impl->root_canon + "/" + subpath;
    char buf[PATH_MAX];
    if (::realpath(joined.c_str(), buf) == nullptr) return false;
    resolved.assign(buf);
    if (resolved.size() <= impl->root_canon.size()) return false;
    if (resolved.compare(0, impl->root_canon.size(), impl->root_canon) != 0) return false;
    if (resolved[impl->root_canon.size()] != '/') return false;
    struct stat st{};
    if (::stat(resolved.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
    return true;
}

void serve_asset(int fd, const std::shared_ptr<Impl>& impl, const std::string& subpath)
{
    std::string resolved;
    if (!resolve_asset(impl, subpath, resolved)) {
        send_response(fd, 404, "Not Found", "text/plain; charset=utf-8", "404 Not Found");
        return;
    }
    std::string body;
    if (!read_file(resolved, body)) {
        send_response(fd, 500, "Internal Server Error", "text/plain; charset=utf-8", "500");
        return;
    }
    send_response(fd, 200, "OK", mime_type(resolved), body);
}

void serve_root(int fd, const std::shared_ptr<Impl>& impl)
{
    const std::string page = get_active_page(impl);
    std::string resolved;
    if (!resolve_asset(impl, "html/" + page + ".html", resolved)) {
        // Fall back to the map page if the active page is unavailable.
        if (!resolve_asset(impl, "html/map.html", resolved)) {
            send_response(fd, 404, "Not Found", "text/plain; charset=utf-8", "404 Not Found");
            return;
        }
    }
    std::string body;
    if (!read_file(resolved, body)) {
        send_response(fd, 500, "Internal Server Error", "text/plain; charset=utf-8", "500");
        return;
    }
    send_response(fd, 200, "OK", "text/html; charset=utf-8", body);
}

void serve_doorknock(int fd, const std::shared_ptr<Impl>& impl)
{
    nlohmann::json body;
    body["type"] = "fc2observ";
    body["version"] = RadarServer::version();
    body["socket"] = impl->ws_port;
    body["activePage"] = get_active_page(impl);
    body["configPath"] = "";
    send_response(fd, 200, "OK", "application/json", body.dump());
}

void handle_http(const std::shared_ptr<Impl>& impl, int fd)
{
    std::string request;
    request.reserve(1024);
    char buf[2048];
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return;
        }
        request.append(buf, static_cast<size_t>(n));
    }

    // Parse the request line.
    const size_t line_end = request.find("\r\n");
    const std::string line = request.substr(0, line_end == std::string::npos ? request.size() : line_end);
    std::istringstream ls(line);
    std::string method, target;
    ls >> method >> target;

    std::string path = target;
    const auto q = path.find('?');
    if (q != std::string::npos) path = path.substr(0, q);
    path = percent_decode(path);

    if (path == "/" || path.empty()) {
        serve_root(fd, impl);
    } else if (path == "/doorknock") {
        serve_doorknock(fd, impl);
    } else if (path == "/log") {
        send_response(fd, 200, "OK", "text/plain; charset=utf-8", "OK");
    } else if (path == "/favicon.ico") {
        serve_asset(fd, impl, "img/favicon.ico");
    } else if (path.rfind("/css/", 0) == 0) {
        serve_asset(fd, impl, path.substr(1));
    } else if (path.rfind("/renderers/", 0) == 0) {
        serve_asset(fd, impl, path.substr(1));
    } else if (path.rfind("/img/", 0) == 0) {
        serve_asset(fd, impl, path.substr(1));
    } else if (path.rfind("/maps/", 0) == 0) {
        serve_asset(fd, impl, path.substr(1));
    } else {
        send_response(fd, 404, "Not Found", "text/plain; charset=utf-8", "404 Not Found");
    }
}

std::string build_welcome(const std::shared_ptr<Impl>& impl)
{
    WebConfig web;
    {
        std::lock_guard<std::mutex> lk(impl->web_mutex);
        web = impl->web;
    }

    nlohmann::json config;
    config["browser"] = {
        { "transparent", web.transparent },
        { "ports", { { "static", impl->http_port }, { "socket", impl->ws_port } } },
    };
    config["radar"] = {
        { "highestPlayerOnTop", web.highest_player_on_top },
        { "showBuyzones", web.show_buyzones },
        { "showName", web.show_name },
        { "maxNameLength", web.max_name_length },
        { "shooting", web.shooting },
        { "damage", web.damage },
        { "flashes", web.flashes },
        { "projectiles", web.projectiles },
        { "smokeColors", web.smoke_colors },
        { "plainProjectiles", web.plain_projectiles },
        { "playerSmoothing", web.player_smoothing },
        { "projectileSmoothing", web.projectile_smoothing },
        { "tombstoneOpacity", web.tombstone_opacity },
        { "playerDotScale", web.player_dot_scale },
        { "bombDotScale", web.bomb_dot_scale },
    };
    config["autozoom"] = {
        { "enable", web.autozoom },
        { "smoothing", web.autozoom_smoothing },
        { "padding", web.autozoom_padding },
        { "minZoom", web.autozoom_min_zoom },
    };
    config["vertIndicator"] = {
        { "type", web.vert_indicator_type },
        { "colorRange", { { 13, 255, 0 }, { 255, 255, 255 }, { 255, 0, 199 } } },
        { "scaleDelta", web.vert_indicator_scale_delta },
    };

    nlohmann::json msg;
    msg["type"] = "welcome";
    msg["data"]["scripts"] = impl->renderer_scripts;
    msg["data"]["config"] = config;
    return msg.dump();
}

void ws_connection_thread(std::shared_ptr<Impl> impl, std::shared_ptr<Impl::WsClient> client)
{
    const int fd = client->fd;

    // --- Handshake -----------------------------------------------------------
    std::string request;
    request.reserve(1024);
    char buf[2048];
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            goto cleanup;
        }
        request.append(buf, static_cast<size_t>(n));
    }

    {
        std::string key;
        std::istringstream rs(request);
        std::string line;
        while (std::getline(rs, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string name = line.substr(0, colon);
            std::transform(name.begin(), name.end(), name.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (name == "sec-websocket-key") {
                std::string v = line.substr(colon + 1);
                const auto b = v.find_first_not_of(" \t");
                key = b == std::string::npos ? std::string() : v.substr(b);
            }
        }
        if (key.empty()) {
            send_response(fd, 400, "Bad Request", "text/plain; charset=utf-8", "Bad Request");
            goto cleanup;
        }
        std::string resp;
        resp += "HTTP/1.1 101 Switching Protocols\r\n";
        resp += "Upgrade: websocket\r\n";
        resp += "Connection: Upgrade\r\n";
        resp += "Sec-WebSocket-Accept: " + ws_accept_key(key) + "\r\n\r\n";
        if (!send_all(fd, resp.data(), resp.size())) goto cleanup;
    }

    // --- Frame loop ----------------------------------------------------------
    {
        std::string frame;
        while (client->alive.load()) {
            if (!ws_read_frame(fd, frame)) break;
            if (frame == "requestWelcome") {
                const std::string welcome = build_welcome(impl);
                std::lock_guard<std::mutex> lk(client->write_mutex);
                if (!ws_send_frame(fd, 0x1, welcome)) break;
            }
        }
    }

cleanup:
    client->alive.store(false);
    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
    {
        std::lock_guard<std::mutex> lk(impl->clients_mutex);
        impl->clients.erase(
            std::remove_if(impl->clients.begin(), impl->clients.end(),
                           [&](const std::shared_ptr<Impl::WsClient>& c) { return c.get() == client.get(); }),
            impl->clients.end());
    }
}

void http_accept_loop(std::shared_ptr<Impl> impl)
{
    while (!impl->stop_flag.load()) {
        struct sockaddr_storage ss{};
        socklen_t sl = sizeof(ss);
        const int fd = ::accept(impl->http_fd, reinterpret_cast<struct sockaddr*>(&ss), &sl);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        const int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        // Browsers open speculative connections that may never send a request;
        // a read timeout keeps those from lingering.
        struct timeval tv{};
        tv.tv_sec = 5;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        // Serve each request on its own thread so a slow client cannot stall
        // the accept loop. The shared Impl keeps state alive until it returns.
        std::thread([impl, fd]() {
            handle_http(impl, fd);
            ::close(fd);
        }).detach();
    }
}

void ws_accept_loop(std::shared_ptr<Impl> impl)
{
    while (!impl->stop_flag.load()) {
        struct sockaddr_storage ss{};
        socklen_t sl = sizeof(ss);
        const int fd = ::accept(impl->ws_fd, reinterpret_cast<struct sockaddr*>(&ss), &sl);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        const int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        auto client = std::make_shared<Impl::WsClient>();
        client->fd = fd;
        {
            std::lock_guard<std::mutex> lk(impl->clients_mutex);
            impl->clients.push_back(client);
        }
        std::thread(ws_connection_thread, impl, client).detach();
    }
}

std::vector<std::string> enumerate_renderer_scripts(const std::string& asset_root)
{
    std::vector<std::string> scripts;
    std::error_code ec;
    const std::string dir = asset_root + "/renderers";
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        if (name.empty() || name[0] == '_') continue;
        if (name.size() < 3 || name.compare(name.size() - 3, 3, ".js") != 0) continue;
        scripts.push_back(name);
    }
    std::sort(scripts.begin(), scripts.end());
    return scripts;
}

}  // namespace

// -----------------------------------------------------------------------------
// RadarServer
// -----------------------------------------------------------------------------
RadarServer::RadarServer() : impl_(std::make_shared<Impl>()) {}

RadarServer::~RadarServer()
{
    stop();
}

const char* RadarServer::version()
{
    return "bunnyware-1.0";
}

bool RadarServer::start(const std::string& host, int http_port, int ws_port, const std::string& asset_root)
{
    if (running_.load()) return true;

    auto impl = std::make_shared<Impl>();
    impl->host = host;
    impl->asset_root = asset_root;

    char canon[PATH_MAX];
    if (::realpath(asset_root.c_str(), canon) == nullptr) {
        impl->set_error("radar assets not found: " + asset_root);
        impl_ = impl;
        return false;
    }
    impl->root_canon = canon;
    impl->renderer_scripts = enumerate_renderer_scripts(asset_root);

    std::string err;
    impl->http_fd = make_listener(host, http_port, err);
    if (impl->http_fd < 0) {
        impl->set_error(err);
        impl_ = impl;
        return false;
    }
    impl->ws_fd = make_listener(host, ws_port, err);
    if (impl->ws_fd < 0) {
        ::close(impl->http_fd);
        impl->http_fd = -1;
        impl->set_error(err);
        impl_ = impl;
        return false;
    }

    impl->http_port = http_port;
    impl->ws_port = ws_port;
    impl->stop_flag.store(false);

    impl_ = impl;
    running_.store(true, std::memory_order_release);

    impl->http_thread = std::thread(http_accept_loop, impl);
    impl->ws_thread = std::thread(ws_accept_loop, impl);

    BUNNYWARE_LOG_INFO("Radar server listening on http://{}:{} (ws :{})", host, http_port, ws_port);
    return true;
}

void RadarServer::stop()
{
    auto impl = impl_;
    if (!impl) return;

    const bool was_running = running_.exchange(false, std::memory_order_acq_rel);
    impl->stop_flag.store(true);

    if (impl->http_fd >= 0) ::shutdown(impl->http_fd, SHUT_RDWR);
    if (impl->ws_fd >= 0) ::shutdown(impl->ws_fd, SHUT_RDWR);
    if (impl->http_thread.joinable()) impl->http_thread.join();
    if (impl->ws_thread.joinable()) impl->ws_thread.join();
    if (impl->http_fd >= 0) {
        ::close(impl->http_fd);
        impl->http_fd = -1;
    }
    if (impl->ws_fd >= 0) {
        ::close(impl->ws_fd);
        impl->ws_fd = -1;
    }

    std::vector<std::shared_ptr<Impl::WsClient>> clients;
    {
        std::lock_guard<std::mutex> lk(impl->clients_mutex);
        clients = impl->clients;
    }
    for (auto& c : clients) {
        c->alive.store(false);
        ::shutdown(c->fd, SHUT_RDWR);
    }
    {
        std::lock_guard<std::mutex> lk(impl->page_mutex);
        impl->last_sent_page.clear();
    }

    if (was_running) BUNNYWARE_LOG_INFO("Radar server stopped");
}

std::string RadarServer::last_error() const
{
    auto impl = impl_;
    return impl ? impl->get_error() : std::string();
}

ServerStatus RadarServer::status() const
{
    ServerStatus st;
    auto impl = impl_;
    if (!impl) return st;
    st.running = running_.load();
    st.http_port = impl->http_port;
    st.ws_port = impl->ws_port;
    st.updates = impl->updates.load();
    st.last_error = impl->get_error();
    {
        std::lock_guard<std::mutex> lk(impl->clients_mutex);
        st.clients = static_cast<int>(impl->clients.size());
    }
    return st;
}

void RadarServer::set_web_config(const WebConfig& config)
{
    auto impl = impl_;
    if (!impl) return;
    std::lock_guard<std::mutex> lk(impl->web_mutex);
    impl->web = config;
}

void RadarServer::publish(const Snapshot& s)
{
    auto impl = impl_;
    if (!impl || !running_.load()) return;

    using nlohmann::json;
    std::vector<std::string> frames;
    frames.reserve(12);
    auto add = [&frames](const std::string& type, const json& data) {
        json m;
        m["type"] = type;
        m["data"] = data;
        frames.push_back(m.dump());
    };

    add("connection", json{ { "status", "up" } });

    const std::string page = s.in_game ? "map" : "waiting";
    add("map", s.in_game ? s.map : std::string("waiting"));

    bool page_changed = false;
    {
        std::lock_guard<std::mutex> lk(impl->page_mutex);
        if (impl->last_sent_page != page) {
            impl->last_sent_page = page;
            impl->active_page = page;
            page_changed = true;
        }
    }
    if (page_changed) add("pageUpdate", json(nullptr));

    if (!s.phase.empty()) add("round", s.phase);

    add("game_state", json{
        { "t_score", s.t_score },
        { "ct_score", s.ct_score },
        { "round", s.round },
        { "time_remaining", s.time_remaining },
        { "phase", s.phase },
    });

    json players = json::array();
    for (const auto& p : s.players) {
        json pj;
        pj["id"] = std::to_string(p.num);
        pj["num"] = p.num;
        pj["name"] = p.name;
        pj["team"] = (p.team == 3) ? "CT" : "T";
        pj["health"] = p.health;
        pj["active"] = p.active;
        pj["flashed"] = p.flashed;
        pj["bomb"] = p.bomb;
        pj["bombActive"] = false;
        pj["disconnected"] = p.disconnected;
        pj["angle"] = p.angle;
        pj["ammo"] = json::object();
        pj["position"] = { { "x", p.x }, { "y", p.y }, { "z", p.z } };
        if (!p.weapon.empty() && p.ammo >= 0) {
            pj["extended_data"] = { { "active_weapon", p.weapon }, { "active_ammo", p.ammo } };
        } else {
            pj["extended_data"] = nullptr;
        }
        players.push_back(std::move(pj));
    }
    add("players", json{ { "players", players } });

    add("bomb", json{
        { "state", s.bomb.state },
        { "blow_time", s.bomb.blow_time },
        { "defuse_countdown", 0.0 },
        { "defuse_possible", s.bomb.defuse_possible },
        { "is_defusing", s.bomb.is_defusing },
        { "position", { { "x", s.bomb.x }, { "y", s.bomb.y }, { "z", s.bomb.z } } },
    });

    json smokes = json::array();
    for (const auto& sm : s.smokes) {
        smokes.push_back(json{
            { "id", sm.id },
            { "time", sm.time },
            { "team", sm.team },
            { "position", { { "x", sm.x }, { "y", sm.y }, { "z", sm.z } } },
        });
    }
    add("smokes", smokes);

    json infernos = json::array();
    for (const auto& inf : s.infernos) {
        json flames = json::array();
        const size_t count = std::min({ inf.flames_x.size(), inf.flames_y.size(), inf.flames_z.size() });
        for (size_t i = 0; i < count; ++i) {
            flames.push_back(json{ { "x", inf.flames_x[i] }, { "y", inf.flames_y[i] }, { "z", inf.flames_z[i] } });
        }
        infernos.push_back(json{
            { "id", inf.id },
            { "flamesNum", count },
            { "flamesPosition", flames },
        });
    }
    add("infernos", infernos);

    json projectiles = json::array();
    for (const auto& pr : s.projectiles) {
        projectiles.push_back(json{
            { "id", pr.id },
            { "type", pr.type },
            { "team", pr.team },
            { "position", { { "x", pr.x }, { "y", pr.y }, { "z", pr.z } } },
        });
    }
    add("projectiles", projectiles);

    // Nothing to push to nobody.
    std::vector<std::shared_ptr<Impl::WsClient>> clients;
    {
        std::lock_guard<std::mutex> lk(impl->clients_mutex);
        clients = impl->clients;
    }
    if (clients.empty()) {
        impl->updates.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    for (auto& c : clients) {
        if (!c->alive.load()) continue;
        std::lock_guard<std::mutex> lk(c->write_mutex);
        bool ok = true;
        for (const auto& f : frames) {
            if (!ws_send_frame(c->fd, 0x1, f)) {
                ok = false;
                break;
            }
        }
        if (!ok) c->alive.store(false);
    }
    impl->updates.fetch_add(1, std::memory_order_relaxed);
}

bool RadarServer::open_in_browser(int http_port)
{
    const std::string url = "http://127.0.0.1:" + std::to_string(http_port) + "/";
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::setsid();
        // Silence the helper: xdg-open on some desktops prints
        // "kfmclient: command not found" / shell "test: : integer expected"
        // warnings that would otherwise pollute the overlay log.
        const int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) ::close(devnull);
        }
        // Prefer the desktop handler, then common browsers as a fallback so a
        // broken xdg-open still opens the radar.
        static const char* const kOpeners[] = {
            "xdg-open", "firefox", "chromium", "chromium-browser", "google-chrome-stable",
            "google-chrome", "brave", "brave-browser", "microsoft-edge", "vivaldi", "opera",
        };
        for (const char* opener : kOpeners) {
            ::execlp(opener, opener, url.c_str(), static_cast<char*>(nullptr));
        }
        _exit(127);
    }
    return pid > 0;
}

// -----------------------------------------------------------------------------
// Menu <-> App control bridge
// -----------------------------------------------------------------------------
namespace {
RadarServer* g_active_server = nullptr;
std::atomic<bool> g_start_request{ false };
std::atomic<bool> g_stop_request{ false };
}  // namespace

RadarServer* active_server()
{
    return g_active_server;
}

void set_active_server(RadarServer* server)
{
    g_active_server = server;
}

void request_start()
{
    g_start_request.store(true);
}

void request_stop()
{
    g_stop_request.store(true);
}

bool take_start_request()
{
    return g_start_request.exchange(false);
}

bool take_stop_request()
{
    return g_stop_request.exchange(false);
}

}  // namespace radar
