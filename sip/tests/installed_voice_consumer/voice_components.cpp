#include <sip-subscribe.h>
#include <sip-registration.h>
#include <sip-agent.h>
#include <turbo_audio_codec.h>

#include <atomic>
#include <cstring>

static int via(void*, const char*, char protocol[16], char local[128], char dns[128]) {
    std::strcpy(protocol, "UDP");
    std::strcpy(local, "127.0.0.1:5062");
    std::strcpy(dns, local);
    return 0;
}

static int send(void* context, const void* data, size_t bytes) {
    const char method[] = "REGISTER ";
    if (bytes < sizeof(method) - 1 || std::memcmp(data, method, sizeof(method) - 1) != 0) return -1;
    static_cast<std::atomic<unsigned>*>(context)->fetch_add(1, std::memory_order_relaxed);
    return 0;
}

int main() {
    sip_uas_handler_t handler{};
    auto* agent = sip_agent_create(&handler);
    if (!agent) return 1;
    std::atomic<unsigned> requests{0};
    const sip_transport_t transport{via, send};
    sip_registration_config_t config{};
    config.agent = agent;
    config.transport = &transport;
    config.transport_context = &requests;
    config.address_of_record = "sip:alice@example.test";
    config.registrar = "sip:example.test";
    config.expires_seconds = 60;
    auto* registration = sip_registration_create(&config);
    sip_registration_status_t status{};
    int result = !registration || sip_registration_poll(registration, 1000, &status) != 0 ||
                 status.state != SIP_REGISTRATION_PENDING || status.error ||
                 requests.load(std::memory_order_relaxed) == 0;
    sip_registration_destroy(registration);
    if (sip_agent_destroy(agent) != 0) return 2;
    return result || turbo_audio_codec_rtp_clock_rate("pcmu") != 8000;
}
