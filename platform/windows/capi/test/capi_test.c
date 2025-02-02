#include <ag_dns.h>
#include <ldns/ldns.h>
#include <stdio.h>
#include <stdlib.h>

#define ASSERT(cond) do {                                 \
    if (!(cond)) {                                        \
        fprintf(stderr,                                   \
                "\n\t%s:%d:%s() assertion (%s) failed\n", \
                __FILE__, __LINE__, __func__, #cond);     \
        exit(1);                                          \
    }                                                     \
} while (0)

static bool on_req_called = false;
static void on_req(const ag_dns_request_processed_event *event) {
    on_req_called = true;
    ASSERT(event->elapsed > 0);
    ASSERT(0 == strcmp(event->domain, "example.org."));
    ASSERT(event->answer);
    ASSERT(event->error == NULL);
    ASSERT(event->type);
    ASSERT(event->status);
    ASSERT(event->upstream_id);
    ASSERT(*event->upstream_id == 42);
}

static bool on_cert_called = false;
static ag_certificate_verification_result on_cert(const ag_certificate_verification_event *event) {
    on_cert_called = true;
    ASSERT(event->certificate.data);
    ASSERT(event->certificate.size > 0);
    for (size_t i = 0; i < event->chain.size; ++i) {
        ASSERT(event->chain.data[i].data);
        ASSERT(event->chain.data[i].size > 0);
    }
    return AGCVR_OK;
}

static void on_log(void *arg, ag_log_level level, const char *message, uint32_t length) {
    ASSERT((uintptr_t) arg == 42);
    fprintf(stderr, "on_log: (%d) %.*s", (int) level, (int) length, message);
}

static void test_proxy() {
    const char *version = ag_get_capi_version();
    ASSERT(version);
    ASSERT(strlen(version));

    ag_set_log_callback(on_log, (void *) (uintptr_t) 42);

    ag_dnsproxy_settings *settings = ag_dnsproxy_settings_get_default();

    ASSERT(settings->fallback_domains.size > 0);
    ASSERT(settings->fallback_domains.data);

    const char *ugly_hack = settings->upstreams.data[0].address;
    settings->upstreams.data[0].address = "tls://1.1.1.1";
    settings->upstreams.data[0].id = 42;

    ag_dnsproxy_events events = {};
    events.on_request_processed = on_req;
    events.on_certificate_verification = on_cert;

    ag_dnsproxy *proxy = ag_dnsproxy_init(settings, &events);
    ASSERT(proxy);

    ag_dnsproxy_settings *actual_settings = ag_dnsproxy_get_settings(proxy);
    ASSERT(actual_settings);
    ASSERT(actual_settings->upstreams.data[0].id == settings->upstreams.data[0].id);
    ag_dnsproxy_settings_free(actual_settings);

    settings->upstreams.data[0].address = ugly_hack;
    ag_dnsproxy_settings_free(settings);

    ldns_pkt *query = ldns_pkt_query_new(ldns_dname_new_frm_str("example.org"),
                                         LDNS_RR_TYPE_A, LDNS_RR_CLASS_IN, LDNS_RD);
    ag_buffer msg = {};
    size_t out_size;
    ldns_pkt2wire(&msg.data, query, &out_size);
    msg.size = out_size;

    ag_buffer res = ag_dnsproxy_handle_message(proxy, msg);
    ASSERT(on_req_called);
    ASSERT(on_cert_called);

    ldns_pkt *response = NULL;
    ASSERT(LDNS_STATUS_OK == ldns_wire2pkt(&response, res.data, res.size));
    ASSERT(LDNS_RCODE_NOERROR == ldns_pkt_get_rcode(response));
    ASSERT(ldns_pkt_ancount(response) > 0);

    ag_dnsproxy_deinit(proxy);

    ldns_pkt_free(query);
    ldns_pkt_free(response);
    ag_buffer_free(res);
    LDNS_FREE(msg.data);
}

#define TEST_DNS_STAMP "sdns://AgcAAAAAAAAACTEyNy4wLjAuMSDDhGvyS56TymQnTA7GfB7MXgJP_KzS10AZNQ6B_lRq5AtleGFtcGxlLmNvbQovZG5zLXF1ZXJ5"

static void test_dnsstamp() {
    const char *error = NULL;
    ag_dns_stamp *stamp = ag_dns_stamp_from_str("asdfasdfasdfsdf", &error);
    ASSERT(!stamp);
    ASSERT(error);

    error = NULL;
    const char *doh_str = "sdns://AgMAAAAAAAAADDk0LjE0MC4xNC4xNITK_rq-BN6tvu8PZG5zLmFkZ3VhcmQuY29tCi9kbnMtcXVlcnk";
    stamp = ag_dns_stamp_from_str(doh_str, &error);
    ASSERT(stamp);
    ASSERT(!error);
    ASSERT(0 == strcmp(stamp->provider_name, "dns.adguard.com"));
    ASSERT(0 == strcmp(stamp->path, "/dns-query"));
    ASSERT(stamp->properties & AGSIP_DNSSEC);
    ASSERT(stamp->properties & AGSIP_NO_LOG);
    ASSERT(!(stamp->properties & AGSIP_NO_FILTER));
    ASSERT(stamp->hashes.size == 2);
    ASSERT(0 == strcmp(ag_dns_stamp_pretty_url(stamp), "https://dns.adguard.com/dns-query"));
    ASSERT(0 == strcmp(ag_dns_stamp_prettier_url(stamp), "https://dns.adguard.com/dns-query"));
    ASSERT(0 == strcmp(ag_dns_stamp_to_str(stamp), doh_str));

    static uint8_t BYTES[] = "\xca\xfe\xba\xbe\xde\xad\xbe\xef";
    ag_buffer hash = {.data = BYTES, .size = 4};
    stamp->proto = AGSPT_DOQ;
    stamp->hashes.data = &hash;
    stamp->hashes.size = 1;
    stamp->properties = AGSIP_NO_FILTER;
    stamp->path = NULL;

    ASSERT(0 == strcmp(ag_dns_stamp_pretty_url(stamp), "quic://dns.adguard.com"));
    ASSERT(0 == strcmp(ag_dns_stamp_prettier_url(stamp), "quic://dns.adguard.com"));
    ASSERT(0 == strcmp(ag_dns_stamp_to_str(stamp), "sdns://BAQAAAAAAAAADDk0LjE0MC4xNC4xNATK_rq-D2Rucy5hZGd1YXJkLmNvbQ"));

    stamp->proto = AGSPT_DNSCRYPT;
    stamp->hashes.size = 0;
    stamp->provider_name = "2.dnscrypt-cert.adguard";
    stamp->server_public_key.data = BYTES;
    stamp->server_public_key.size = 8;

    ASSERT(0 == strcmp(ag_dns_stamp_pretty_url(stamp), "sdns://AQQAAAAAAAAADDk0LjE0MC4xNC4xNAjK_rq-3q2-7xcyLmRuc2NyeXB0LWNlcnQuYWRndWFyZA"));
    ASSERT(0 == strcmp(ag_dns_stamp_prettier_url(stamp), "dnscrypt://2.dnscrypt-cert.adguard"));
    ASSERT(0 == strcmp(ag_dns_stamp_to_str(stamp), "sdns://AQQAAAAAAAAADDk0LjE0MC4xNC4xNAjK_rq-3q2-7xcyLmRuc2NyeXB0LWNlcnQuYWRndWFyZA"));
}

static void test_utils() {
    // test_upstream
    ag_upstream_options upstream = {};
    upstream.address = "https://dns.adguard.com/dns-query";
    upstream.bootstrap.size = 1;
    upstream.bootstrap.data = malloc(sizeof(const char *));
    upstream.bootstrap.data[0] = "8.8.8.8";
    upstream.timeout_ms = 5000;
    const char *error = ag_test_upstream(&upstream, false, on_cert);
    ASSERT(error == NULL);
    upstream.address = "1.2.3.4.5.6";
    error = ag_test_upstream(&upstream, false, NULL);
    ASSERT(error);
    upstream.address = "https://asdf.asdf.asdf/asdfdnsqueryasdf";
    error = ag_test_upstream(&upstream, false, NULL);
    ASSERT(error);
    ag_str_free(error);
    free(upstream.bootstrap.data);
}

int main() {
    ag_set_log_level(AGLL_TRACE);

    test_proxy();
    test_utils();
    test_dnsstamp();

#ifdef _WIN32
    // At least check that we don't crash or something
    ag_disable_SetUnhandledExceptionFilter();
    ag_enable_SetUnhandledExceptionFilter();
#endif

    return 0;
}
