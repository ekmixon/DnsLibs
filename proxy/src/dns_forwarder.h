#pragma once


#include <ag_logger.h>
#include <ag_utils.h>
#include <ag_cache.h>
#include <ag_clock.h>
#include <dnsproxy_settings.h>
#include <dnsproxy_events.h>
#include <dnsfilter.h>
#include <dns64.h>
#include <upstream.h>
#include <shared_mutex>
#include <uv.h>
#include <dnsproxy.h>
#include "retransmission_detector.h"

namespace ag {

struct cached_response {
    ldns_pkt_ptr response;
    ag::steady_clock::time_point expires_at;
    std::optional<int32_t> upstream_id;
};

struct cache_result {
    ldns_pkt_ptr response;
    std::optional<int32_t> upstream_id;
    bool expired;
};

struct upstream_exchange_result {
    ldns_pkt_ptr response;
    err_string error;
    upstream *upstream;
};

namespace dns_forwarder_utils {
/**
* Format RR list using the following format:
* <Type>, <RDFs, space separated>\n
* e.g.:
* A, 1.2.3.4
* AAAA, 12::34
* CNAME, google.com.
*/
std::string rr_list_to_string(const ldns_rr_list *rr_list);
} // namespace dns_forwarder_utils

class dns_forwarder {
public:
    dns_forwarder();
    ~dns_forwarder();

    std::pair<bool, err_string> init(const dnsproxy_settings &settings, const dnsproxy_events &events);
    void deinit();

    std::vector<uint8_t> handle_message(uint8_view message, const dns_message_info *info);

private:
    static void async_request_worker(uv_work_t *);
    static void async_request_finalizer(uv_work_t *, int);

    void truncate_response(ldns_pkt *response, const ldns_pkt *request, const dns_message_info *info);

    std::vector<uint8_t> handle_message_internal(uint8_view message, const dns_message_info *info,
                                                 bool fallback_only, uint16_t pkt_id);

    upstream_exchange_result do_upstream_exchange(std::string_view normalized_domain, ldns_pkt *request,
                                                  bool fallback_only, const dns_message_info *info = nullptr);

    cache_result create_response_from_cache(const std::string &key, const ldns_pkt *request);

    void put_response_into_cache(std::string key, ldns_pkt_ptr response, std::optional<int32_t> upstream_id);

    bool apply_fallback_filter(std::string_view hostname, const ldns_pkt *request);

    std::optional<uint8_vector> apply_filter(std::string_view hostname,
                                             const ldns_pkt *request,
                                             const ldns_pkt *original_response,
                                             dns_request_processed_event &event,
                                             std::vector<dnsfilter::rule> &last_effective_rules,
                                             bool fallback_only,
                                             bool fire_event = true, ldns_pkt_rcode *out_rcode = nullptr);

    std::optional<uint8_vector> apply_cname_filter(const ldns_rr *cname_rr, const ldns_pkt *request,
                                                   const ldns_pkt *response, dns_request_processed_event &event,
                                                   std::vector<dnsfilter::rule> &last_effective_rules,
                                                   bool fallback_only);

    std::optional<uint8_vector> apply_ip_filter(const ldns_rr *rr, const ldns_pkt *request,
                                                const ldns_pkt *response, dns_request_processed_event &event,
                                                std::vector<dnsfilter::rule> &last_effective_rules,
                                                bool fallback_only);

    ldns_pkt_ptr try_dns64_aaaa_synthesis(upstream *upstream, const ldns_pkt_ptr &request) const;

    void finalize_processed_event(dns_request_processed_event &event,
        const ldns_pkt *request, const ldns_pkt *response, const ldns_pkt *original_response,
        std::optional<int32_t> upstream_id, err_string error) const;

    bool do_dnssec_log_logic(ldns_pkt *request);
    bool finalize_dnssec_log_logic(ldns_pkt *response, bool is_our_do_bit);

    logger log;
    const dnsproxy_settings *settings = nullptr;
    const dnsproxy_events *events = nullptr;
    std::vector<upstream_ptr> upstreams;
    std::vector<upstream_ptr> fallbacks;
    dnsfilter filter;
    dnsfilter::handle filter_handle = nullptr;
    dnsfilter::handle fallback_filter_handle = nullptr;
    dns64::prefixes dns64_prefixes;
    std::shared_ptr<socket_factory> socket_factory;

    with_mtx<lru_cache<std::string, cached_response>, std::shared_mutex> response_cache;

    retransmission_detector retransmission_detector;

    struct async_request {
        uv_work_t work{};
        dns_forwarder *forwarder{};
        ldns_pkt_ptr request;
        std::string cache_key;
        std::string normalized_domain; // domain name without dot in the end

        async_request() {
            work.data = this;
        }
    };

    // Map of async requests in flight (cache key -> uv work handle)
    std::unordered_map<std::string, async_request> async_reqs;
    std::mutex async_reqs_mtx;
    std::condition_variable async_reqs_cv;
};

} // namespace ag
