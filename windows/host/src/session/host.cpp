#include "session/host.h"

#include <algorithm>

#include <winsock2.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>

#include "core/json.h"
#include "core/log.h"
#include "core/win.h"

namespace dm {

namespace {

const char* mode_name(proto::DisplayMode m) {
    switch (m) {
        case proto::DisplayMode::Extend: return "extend";
        case proto::DisplayMode::Mirror: return "mirror";
        case proto::DisplayMode::Tablet: return "tablet";
    }
    return "?";
}

bool is_elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION e{};
    DWORD len = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &e, sizeof e, &len) && e.TokenIsElevated;
    CloseHandle(token);
    return ok;
}

std::string error_json(const char* message) {
    json::Writer w;
    return w.begin_object().field("ok", false).field("error", message).end_object().str();
}

std::string ok_json() {
    json::Writer w;
    return w.begin_object().field("ok", true).end_object().str();
}

}  // namespace

std::vector<std::string> lan_ipv4_addresses() {
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf(size);
    const ULONG flags =
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS;
    ULONG r = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    if (r == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        r = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (r != NO_ERROR) return {};

    // Hyper-V, WSL, VirtualBox and VMware adapters are up too but lead nowhere; the
    // adapter(s) with a default gateway are the ones on the real network. Windows'
    // preferred route (lowest metric) comes first. Without any gateway (e.g. a router
    // with no internet), fall back to every address.
    struct Candidate {
        std::string ip;
        bool gateway;
        ULONG metric;
    };
    std::vector<Candidate> all;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            char ip[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof ip);
            if (std::string(ip).rfind("169.254.", 0) == 0) continue;  // link-local
            all.push_back({ip, a->FirstGatewayAddress != nullptr, a->Ipv4Metric});
        }
    }
    const bool any_gateway = std::any_of(all.begin(), all.end(), [](const Candidate& c) { return c.gateway; });
    std::stable_sort(all.begin(), all.end(), [](const Candidate& a, const Candidate& b) { return a.metric < b.metric; });
    std::vector<std::string> out;
    for (const auto& c : all)
        if (c.gateway || !any_gateway) out.push_back(c.ip);
    return out;
}

Host::Host(const HostOptions& opts) : opts_(opts), vdm_(std::make_unique<MttVddProvider>()) {
    allow_wifi_ = opts_.allow_wifi.value_or(config_.get_bool("wifi", false));
    opts_.pen_curve = {config_.get_float("pen_min", 0.0f), config_.get_float("pen_max", 1.0f),
                       config_.get_float("pen_gamma", 1.0f)};
}

Host::~Host() {
    stop();
}

bool Host::start_listening() {
    return server_.start(opts_.port, allow_wifi_, [this](std::unique_ptr<Connection> c) {
        std::lock_guard lock(mu_);
        const uint32_t id = next_id_++;
        auto s = std::make_unique<Session>(id, std::move(c), opts_, vdm_, approvals_);
        s->start();
        sessions_.emplace(id, std::move(s));
    });
}

bool Host::start() {
    vdm_.prepare();
    if (!start_listening()) return false;
    update_advertising();
    if (opts_.adb) adb_.start(opts_.port, opts_.adb_auto_launch);
    if (!control_.start([this](const std::string& req) { return handle_control(req); }))
        DM_LOGW("Control API unavailable - the DisplayMaster app can't show status");
    return true;
}

void Host::update_advertising() {
    if (!allow_wifi_) {
        mdns_.stop();
        return;
    }
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD len = ARRAYSIZE(name);
    GetComputerNameW(name, &len);
    mdns_.start(opts_.port, name);
}

void Host::stop() {
    mdns_.stop();
    control_.stop();
    adb_.stop();
    server_.stop();
    std::map<uint32_t, std::unique_ptr<Session>> sessions;
    {
        std::lock_guard lock(mu_);
        sessions.swap(sessions_);
    }
    sessions.clear();  // joins each session's threads
}

void Host::reap() {
    std::lock_guard lock(mu_);
    std::erase_if(sessions_, [](const auto& kv) { return kv.second->finished(); });
}

size_t Host::session_count() {
    std::lock_guard lock(mu_);
    return sessions_.size();
}

std::string Host::handle_control(const std::string& request) {
    const auto cmd = json::get_string(request, "cmd").value_or("");
    if (cmd == "status") return status_json();
    if (cmd == "disconnect") {
        const auto id = static_cast<uint32_t>(json::get_int(request, "id").value_or(0));
        std::lock_guard lock(mu_);
        auto it = sessions_.find(id);
        if (it == sessions_.end()) return error_json("no such session");
        it->second->kick("Disconnected from the PC");
        return ok_json();
    }
    if (cmd == "set_wifi") {
        const bool enabled = json::get_bool(request, "enabled").value_or(false);
        config_.set_bool("wifi", enabled);
        if (enabled != allow_wifi_) {
            allow_wifi_ = enabled;
            server_.stop();  // existing sessions keep their own sockets
            if (!start_listening()) return error_json("port in use");
            update_advertising();
            DM_LOGI("Wi-Fi connections %s", enabled ? "enabled" : "disabled");
        }
        return ok_json();
    }
    if (cmd == "approve") {
        const auto id = static_cast<uint32_t>(json::get_int(request, "id").value_or(0));
        const bool allow = json::get_bool(request, "allow").value_or(false);
        const bool remember = json::get_bool(request, "remember").value_or(true);
        return approvals_.decide(id, allow, remember) ? ok_json() : error_json("no such request");
    }
    if (cmd == "forget_device") {
        approvals_.forget(json::get_string(request, "device_id").value_or(""));
        return ok_json();
    }
    if (cmd == "set_pen") {
        // Pressure below `min` is ignored, `max` and above is full pressure, gamma shapes the rest.
        auto num = [&](const char* key, double fallback) {
            const auto v = json::get_number(request, key);
            return static_cast<float>(v.value_or(fallback));
        };
        PressureCurve c{num("min", 0), num("max", 1), num("gamma", 1)};
        c.min_in = std::clamp(c.min_in, 0.0f, 0.9f);
        c.max_in = std::clamp(c.max_in, c.min_in + 0.05f, 1.0f);
        c.gamma = std::clamp(c.gamma, 0.2f, 5.0f);
        config_.set_float("pen_min", c.min_in);
        config_.set_float("pen_max", c.max_in);
        config_.set_float("pen_gamma", c.gamma);
        std::lock_guard lock(mu_);
        opts_.pen_curve = c;  // new sessions
        for (auto& [id, s] : sessions_) s->set_pressure_curve(c);
        return ok_json();
    }
    if (cmd == "shutdown") {
        DM_LOGI("Shutdown requested by the DisplayMaster app");
        if (on_shutdown_requested) on_shutdown_requested();
        return ok_json();
    }
    return error_json("unknown command");
}

std::string Host::status_json() {
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD len = ARRAYSIZE(name);
    GetComputerNameW(name, &len);

    json::Writer w;
    w.begin_object().field("ok", true).field("version", "0.1.0");
    w.key("host").begin_object();
    w.field("name", to_utf8(name)).field("port", static_cast<int>(opts_.port)).field("wifi", allow_wifi_);
    PressureCurve pen;
    {
        std::lock_guard lock(mu_);  // set_pen writes it under this lock
        pen = opts_.pen_curve;
    }
    w.key("pen")
        .begin_object()
        .field("min", static_cast<double>(pen.min_in))
        .field("max", static_cast<double>(pen.max_in))
        .field("gamma", static_cast<double>(pen.gamma))
        .end_object();
    w.field("elevated", is_elevated()).field("vdd", vdm_.available()).field("adb", !adb_.adb_path().empty());
    w.key("addresses").begin_array();
    for (const auto& a : lan_ipv4_addresses()) w.value(a);
    w.end_array();
    const auto adb = adb_.devices();
    w.key("adb_ready").begin_array();
    for (const auto& s : adb.ready) w.value(s);
    w.end_array();
    w.key("adb_unauthorized").begin_array();
    for (const auto& s : adb.unauthorized) w.value(s);
    w.end_array();
    w.end_object();

    w.key("pending").begin_array();
    for (const auto& p : approvals_.pending())
        w.begin_object()
            .field("id", p.session_id)
            .field("name", p.name)
            .field("model", p.model)
            .field("address", p.address)
            .end_object();
    w.end_array();

    w.key("trusted").begin_array();
    for (const auto& [device_id, device_name] : approvals_.trusted())
        w.begin_object().field("device_id", device_id).field("name", device_name).end_object();
    w.end_array();

    w.key("sessions").begin_array();
    std::lock_guard lock(mu_);
    for (const auto& [id, s] : sessions_) {
        if (s->finished()) continue;
        const auto st = s->status();
        if (st.id == 0) continue;  // still in handshake / waiting for approval
        w.begin_object()
            .field("id", st.id)
            .field("name", st.device_name)
            .field("model", st.model)
            .field("transport", st.usb ? "usb" : "wifi")
            .field("streaming", st.streaming)
            .field("mode", mode_name(st.mode))
            .field("codec", codec_name(st.codec))
            .field("width", st.width)
            .field("height", st.height)
            .field("fps", st.fps)
            .field("bitrate_kbps", st.bitrate_kbps)
            .field("encoder", st.encoder)
            .field("gpu", st.adapter)
            .field("monitor", st.monitor)
            .field("pen", st.has_pen)
            .field("sent_fps", st.sent_fps)
            .field("mbps", st.mbps)
            .field("encode_ms", st.work_ms)
            .field("decode_ms", st.client_decode_ms)
            .field("dropped", st.client_dropped);
        w.key("rect")
            .begin_object()
            .field("x", st.monitor_rect.x)
            .field("y", st.monitor_rect.y)
            .field("w", st.monitor_rect.w)
            .field("h", st.monitor_rect.h)
            .end_object();
        w.end_object();
    }
    w.end_array();
    return w.end_object().str();
}

}  // namespace dm
