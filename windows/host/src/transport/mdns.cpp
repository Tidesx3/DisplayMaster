#include "transport/mdns.h"

#include <ws2tcpip.h>

#include "core/log.h"
#include "core/win.h"

namespace dm {
namespace {

void WINAPI on_registered(DWORD status, PVOID, PDNS_SERVICE_INSTANCE instance) {
    if (status == ERROR_SUCCESS)
        DM_LOGI("mDNS: advertising %s", to_utf8(instance ? instance->pszInstanceName : L"?").c_str());
    else
        DM_LOGW("mDNS: registration failed (%lu) - devices must enter the PC address manually", status);
    if (instance) DnsServiceFreeInstance(instance);
}

void WINAPI on_deregistered(DWORD, PVOID context, PDNS_SERVICE_INSTANCE instance) {
    if (instance) DnsServiceFreeInstance(instance);
    if (context) SetEvent(static_cast<HANDLE>(context));
}

}  // namespace

bool MdnsAdvertiser::start(uint16_t port, const std::wstring& pc_name, const std::string& ipv4) {
    stop();
    instance_name_ = pc_name + L"." + kServiceType;
    host_name_ = pc_name + L".local";
    // TXT record: protocol version + a friendly name (instance names can't hold every character).
    const std::wstring version = L"1";
    PCWSTR keys[] = {L"v", L"name"};
    PCWSTR values[] = {version.c_str(), pc_name.c_str()};
    IP4_ADDRESS addr = 0;  // network byte order
    const bool have_addr = !ipv4.empty() && inet_pton(AF_INET, ipv4.c_str(), &addr) == 1;
    instance_ = DnsServiceConstructInstance(instance_name_.c_str(), host_name_.c_str(), have_addr ? &addr : nullptr,
                                            nullptr, port, 0, 0, 2, keys, values);
    if (!instance_) return false;
    request_ = {};
    request_.Version = DNS_QUERY_REQUEST_VERSION1;
    request_.InterfaceIndex = 0;  // all interfaces
    request_.pServiceInstance = instance_;
    request_.pRegisterCompletionCallback = on_registered;
    const DWORD r = DnsServiceRegister(&request_, nullptr);
    if (r != DNS_REQUEST_PENDING) {
        DM_LOGW("mDNS: DnsServiceRegister failed (%lu)", r);
        DnsServiceFreeInstance(instance_);
        instance_ = nullptr;
        return false;
    }
    return true;
}

void MdnsAdvertiser::stop() {
    if (!instance_) return;
    // Deregistration is asynchronous and reads the request; wait for it before freeing.
    UniqueHandle done(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    request_.pRegisterCompletionCallback = on_deregistered;
    request_.pQueryContext = done.get();
    if (DnsServiceDeRegister(&request_, nullptr) == DNS_REQUEST_PENDING) WaitForSingleObject(done.get(), 2000);
    DnsServiceFreeInstance(instance_);
    instance_ = nullptr;
}

}  // namespace dm
