#include <ag_clock.h>
#include <gtest/gtest.h>
#include <dnsproxy.h>
#include <ldns/ldns.h>
#include <thread>
#include <memory>
#include <ag_utils.h>
#include <ag_net_consts.h>
#include <cstring>
#include <dns_forwarder.h>
#include <upstream_utils.h>
#include <ag_logger.h>
#include <ag_file.h>

#include "../../upstream/test/test_utils.h"

static constexpr auto DNS64_SERVER_ADDR = "2001:4860:4860::6464";
static constexpr auto IPV4_ONLY_HOST = "ipv4only.arpa.";
static constexpr auto CNAME_BLOCKING_HOST = "test2.meshkov.info";

class dnsproxy_test : public ::testing::Test {
protected:
    ag::dnsproxy proxy;

    void SetUp() override {
        ag::set_default_log_level(ag::TRACE);
    }

    void TearDown() override {
        proxy.deinit();
    }
};

static ag::dnsproxy_settings make_dnsproxy_settings() {
    auto settings = ag::dnsproxy_settings::get_default();
    settings.upstreams = {{ .address = "8.8.8.8" }};
    return settings;
}

static ag::ldns_pkt_ptr create_request(const std::string &domain, ldns_rr_type type, uint16_t flags,
                                       ldns_rr_class cls = LDNS_RR_CLASS_IN) {
    return ag::ldns_pkt_ptr(
            ldns_pkt_query_new(
                    ldns_dname_new_frm_str(domain.c_str()), type, cls, flags));
}

static void perform_request(ag::dnsproxy &proxy, const ag::ldns_pkt_ptr &request, ag::ldns_pkt_ptr &response) {
    // Avoid rate limit
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const std::unique_ptr<ldns_buffer, ag::ftor<ldns_buffer_free>> buffer(
            ldns_buffer_new(ag::REQUEST_BUFFER_INITIAL_CAPACITY));

    ldns_status status = ldns_pkt2buffer_wire(buffer.get(), request.get());
    ASSERT_EQ(status, LDNS_STATUS_OK) << ldns_get_errorstr_by_id(status);

    const auto resp_data = proxy.handle_message({ldns_buffer_at(buffer.get(), 0),
                                                 ldns_buffer_position(buffer.get())}, nullptr);

    ldns_pkt *resp;
    status = ldns_wire2pkt(&resp, resp_data.data(), resp_data.size());
    ASSERT_EQ(status, LDNS_STATUS_OK) << ldns_get_errorstr_by_id(status);
    response = ag::ldns_pkt_ptr(resp);
}

static ag::allocated_ptr<char> make_rr_answer_string(ldns_pkt *pkt) {
    return ag::allocated_ptr<char>{ ldns_rdf2str(ldns_rr_rdf(ldns_rr_list_rr(ldns_pkt_answer(pkt), 0), 0)) };
}

TEST_F(dnsproxy_test, test_dns64) {
    using namespace std::chrono_literals;

    // Assume default settings don't include a DNS64 upstream
    ag::dnsproxy_settings settings = make_dnsproxy_settings();
    settings.dns64 = ag::dns64_settings{
            .upstreams = {{
                    .address = DNS64_SERVER_ADDR,
                    .timeout = 5000ms,
            }},
            .max_tries = 5,
            .wait_time = 1s,
    };

    auto [ret, err] = proxy.init(settings, {});
    ASSERT_TRUE(ret) << *err;

    // This is after proxy.init() to not crash in proxy.deinit()
    if (!test_ipv6_connectivity()) {
        SPDLOG_WARN("IPv6 is NOT available, skipping this test");
        return;
    }

    std::this_thread::sleep_for(5s); // Let DNS64 discovery happen

    ag::ldns_pkt_ptr pkt = create_request(IPV4_ONLY_HOST, LDNS_RR_TYPE_AAAA, LDNS_RD);
    ag::ldns_pkt_ptr response;
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, pkt, response));

    ASSERT_GT(ldns_pkt_ancount(response.get()), 0);
}

TEST_F(dnsproxy_test, test_ipv6_blocking) {
    ag::dnsproxy_settings settings = make_dnsproxy_settings();
    settings.block_ipv6 = true;
    settings.ipv6_available = false;
    settings.filter_params = {{{1, "cname_blocking_test_filter.txt"}}};

    auto [ret, err] = proxy.init(settings, {});
    ASSERT_TRUE(ret) << *err;

    ag::ldns_pkt_ptr pkt = create_request(IPV4_ONLY_HOST, LDNS_RR_TYPE_AAAA, LDNS_RD);
    ag::ldns_pkt_ptr response;
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, pkt, response));

    ASSERT_EQ(ldns_pkt_ancount(response.get()), 0);
    ASSERT_EQ(ldns_pkt_get_rcode(response.get()), LDNS_RCODE_NOERROR);
    ASSERT_EQ(ldns_pkt_nscount(response.get()), 1);

    pkt = create_request("google.com", LDNS_RR_TYPE_AAAA, LDNS_RD);
    response.reset();
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, pkt, response));

    ASSERT_EQ(ldns_pkt_ancount(response.get()), 0);
    ASSERT_EQ(ldns_pkt_get_rcode(response.get()), LDNS_RCODE_NOERROR);
    ASSERT_EQ(ldns_pkt_nscount(response.get()), 1);

    pkt = create_request("example.org", LDNS_RR_TYPE_AAAA, LDNS_RD);
    response.reset();
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, pkt, response));
    ASSERT_EQ(ldns_pkt_ancount(response.get()), 0);
    ASSERT_EQ(ldns_pkt_get_rcode(response.get()), LDNS_RCODE_REFUSED);

    // Long domain name. With "hostmaster." in SOA record it is longer than 253 characters.
    // https://jira.adguard.com/browse/AG-9026
    pkt = create_request("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.", LDNS_RR_TYPE_AAAA, LDNS_RD);
    response.reset();
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, pkt, response));
    ASSERT_EQ(ldns_pkt_ancount(response.get()), 0);
    ASSERT_EQ(ldns_pkt_nscount(response.get()), 1);
    ASSERT_EQ(ldns_pkt_get_rcode(response.get()), LDNS_RCODE_NOERROR);
    // Check that message is correctly serialized
    using ldns_buffer_ptr = std::unique_ptr<ldns_buffer, ag::ftor<&ldns_buffer_free>>;
    ldns_buffer_ptr result(ldns_buffer_new(LDNS_MAX_PACKETLEN));
    ldns_status status = ldns_pkt2buffer_wire(result.get(), response.get());
    ASSERT_EQ(status, LDNS_STATUS_OK);
}

TEST_F(dnsproxy_test, test_cname_blocking) {
    ag::dnsproxy_settings settings = make_dnsproxy_settings();
    settings.filter_params = {{{1, "cname_blocking_test_filter.txt"}}};

    ag::dns_request_processed_event last_event{};
    ag::dnsproxy_events events{
            .on_request_processed = [&last_event](const ag::dns_request_processed_event &event) {
                last_event = event;
            }
    };

    auto [ret, err] = proxy.init(settings, {});
    ASSERT_TRUE(ret) << *err;

    ag::ldns_pkt_ptr response;
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request(CNAME_BLOCKING_HOST, LDNS_RR_TYPE_A, LDNS_RD), response));
    ASSERT_EQ(ldns_pkt_ancount(response.get()), 0);
    ASSERT_EQ(ldns_pkt_get_rcode(response.get()), LDNS_RCODE_REFUSED);
}

TEST_F(dnsproxy_test, test_dnstype_blocking_rule) {
    ag::dnsproxy_settings settings = make_dnsproxy_settings();
    settings.filter_params = {{{1, "example.com$dnstype=A|AAAA", true}}};

    ag::dns_request_processed_event last_event{};
    ag::dnsproxy_events events{
            .on_request_processed = [&last_event](const ag::dns_request_processed_event &event) {
                last_event = event;
            }
    };

    auto [ret, err] = proxy.init(settings, events);
    ASSERT_TRUE(ret) << *err;

    ag::ldns_pkt_ptr response;
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("example.com", LDNS_RR_TYPE_A, LDNS_RD), response));
    ASSERT_EQ(ldns_pkt_ancount(response.get()), 0);
    ASSERT_EQ(ldns_pkt_get_rcode(response.get()), LDNS_RCODE_REFUSED);
    ASSERT_EQ(last_event.rules.size(), 1);
}

TEST_F(dnsproxy_test, test_dnsrewrite_rule) {
    ag::dnsproxy_settings settings = make_dnsproxy_settings();
    settings.filter_params =
            {{{
                1,
                "@@example.com$important\n"
                "example.com$dnsrewrite=1.2.3.4\n"
                "example.com$dnsrewrite=NOERROR;A;100.200.200.100\n"
                "example.com$dnsrewrite=NOERROR;MX;42 example.mail\n"
                "@@example.com$dnsrewrite=1.2.3.4\n",
                true
            }}};

    ag::dns_request_processed_event last_event{};
    ag::dnsproxy_events events{
            .on_request_processed = [&last_event](const ag::dns_request_processed_event &event) {
                last_event = event;
            }
    };

    auto [ret, err] = proxy.init(settings, events);
    ASSERT_TRUE(ret) << *err;

    ag::ldns_pkt_ptr response;
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("example.com", LDNS_RR_TYPE_A, LDNS_RD), response));
    ASSERT_EQ(last_event.rules.size(), 3);
    ASSERT_EQ(ldns_pkt_ancount(response.get()), 2);
    ASSERT_EQ(ldns_pkt_get_rcode(response.get()), LDNS_RCODE_NOERROR);
}

TEST_F(dnsproxy_test, test_dnsrewrite_cname) {
    ag::dnsproxy_settings settings = make_dnsproxy_settings();
    settings.filter_params = {{{ 1, "example.com$dnsrewrite=ietf.org", true }}};

    ag::dns_request_processed_event last_event{};
    ag::dnsproxy_events events{
            .on_request_processed = [&last_event](const ag::dns_request_processed_event &event) {
                last_event = event;
            }
    };

    auto [ret, err] = proxy.init(settings, events);
    ASSERT_TRUE(ret) << *err;

    ag::ldns_pkt_ptr response;
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("example.com", LDNS_RR_TYPE_A, LDNS_RD), response));
    ASSERT_EQ(last_event.rules.size(), 1);

    ag::ldns_pkt_ptr cname_response;
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("ietf.org", LDNS_RR_TYPE_A, LDNS_RD), cname_response));

    size_t num = 0;
    for (size_t i = 0; i < ldns_pkt_ancount(cname_response.get()); ++i) {
        const ldns_rr *rr = ldns_rr_list_rr(ldns_pkt_answer(cname_response.get()), i);
        if (ldns_rr_get_type(rr) == LDNS_RR_TYPE_A) {
            ++num;
        }
    }

    ASSERT_EQ(ldns_pkt_ancount(response.get()), num + 1);
    ASSERT_EQ(ldns_pkt_get_rcode(response.get()), LDNS_RCODE_NOERROR);
}

TEST(dnsproxy_test_static, cname_formatting) {
    const uint8_t packet[] = { 0x00, 0x00, 0x81, 0x80, 0x00, 0x01, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x03, 0x77, 0x77, 0x77, 0x09, 0x6d, 0x69, 0x63, 0x72, 0x6f, 0x73, 0x6f, 0x66, 0x74, 0x03, 0x63, 0x6f, 0x6d, 0x00, 0x00, 0x01, 0x00, 0x01, 0xc0, 0x0c, 0x00, 0x05, 0x00, 0x01, 0x00, 0x00, 0x0c, 0xf5, 0x00, 0x23, 0x03, 0x77, 0x77, 0x77, 0x09, 0x6d, 0x69, 0x63, 0x72, 0x6f, 0x73, 0x6f, 0x66, 0x74, 0x07, 0x63, 0x6f, 0x6d, 0x2d, 0x63, 0x2d, 0x33, 0x07, 0x65, 0x64, 0x67, 0x65, 0x6b, 0x65, 0x79, 0x03, 0x6e, 0x65, 0x74, 0x00, 0xc0, 0x2f, 0x00, 0x05, 0x00, 0x01, 0x00, 0x00, 0x3a, 0x6a, 0x00, 0x37, 0x03, 0x77, 0x77, 0x77, 0x09, 0x6d, 0x69, 0x63, 0x72, 0x6f, 0x73, 0x6f, 0x66, 0x74, 0x07, 0x63, 0x6f, 0x6d, 0x2d, 0x63, 0x2d, 0x33, 0x07, 0x65, 0x64, 0x67, 0x65, 0x6b, 0x65, 0x79, 0x03, 0x6e, 0x65, 0x74, 0x0b, 0x67, 0x6c, 0x6f, 0x62, 0x61, 0x6c, 0x72, 0x65, 0x64, 0x69, 0x72, 0x06, 0x61, 0x6b, 0x61, 0x64, 0x6e, 0x73, 0xc0, 0x4d, 0xc0, 0x5e, 0x00, 0x05, 0x00, 0x01, 0x00, 0x00, 0x01, 0x49, 0x00, 0x19, 0x06, 0x65, 0x31, 0x33, 0x36, 0x37, 0x38, 0x04, 0x64, 0x73, 0x70, 0x62, 0x0a, 0x61, 0x6b, 0x61, 0x6d, 0x61, 0x69, 0x65, 0x64, 0x67, 0x65, 0xc0, 0x4d, 0xc0, 0xa1, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x13, 0x00, 0x04, 0x02, 0x15, 0xc6, 0xe5, };
    ldns_pkt *pkt = nullptr;
    ldns_wire2pkt(&pkt, packet, sizeof(packet));
    ASSERT_NE(pkt, nullptr);
    std::string answer = ag::dns_forwarder_utils::rr_list_to_string(ldns_pkt_answer(pkt));
    std::string expected_answer = "CNAME, www.microsoft.com-c-3.edgekey.net.\n"
                                  "CNAME, www.microsoft.com-c-3.edgekey.net.globalredir.akadns.net.\n"
                                  "CNAME, e13678.dspb.akamaiedge.net.\n"
                                  "A, 2.21.198.229\n";
    ASSERT_EQ(answer, expected_answer);
    ldns_pkt_free(pkt);
}

class dnsproxy_cache_test : public ::testing::Test {
protected:
    ag::dnsproxy proxy;
    ag::dns_request_processed_event last_event{};

    void TearDown() override {
        proxy.deinit();
    }

    void SetUp() override {
        ag::set_default_log_level(ag::TRACE);
        ag::dnsproxy_settings settings = make_dnsproxy_settings();
        settings.dns_cache_size = 1;
        settings.optimistic_cache = false;

        ag::dnsproxy_events events{
            .on_request_processed = [this](ag::dns_request_processed_event event) {
                last_event = std::move(event);
            }
        };

        auto [ret, err] = proxy.init(settings, events);
        ASSERT_TRUE(ret) << *err;
    }
};

TEST_F(dnsproxy_cache_test, cache_works) {
    ag::ldns_pkt_ptr pkt = create_request("google.com.", LDNS_RR_TYPE_A, LDNS_RD);
    ag::ldns_pkt_ptr res;
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, pkt, res));
    ASSERT_FALSE(last_event.cache_hit);
    auto first_upstream_id = last_event.upstream_id;
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, pkt, res));
    ASSERT_TRUE(last_event.cache_hit);
    ASSERT_TRUE(last_event.domain == "google.com.");
    ASSERT_EQ(last_event.upstream_id, first_upstream_id);
}

TEST_F(dnsproxy_cache_test, cached_response_ttl_decreases) {
    ag::ldns_pkt_ptr pkt = create_request("example.org.", LDNS_RR_TYPE_SOA, LDNS_RD);
    ag::ldns_pkt_ptr res;
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, pkt, res));
    ASSERT_FALSE(last_event.cache_hit);
    ASSERT_GT(ldns_pkt_ancount(res.get()), 0);

    const uint32_t ttl = ldns_rr_ttl(ldns_rr_list_rr(ldns_pkt_answer(res.get()), 0));
    ASSERT_GT(ttl, 1);
    ag::steady_clock::add_time_shift(std::chrono::seconds((ttl / 2) + 1));

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, pkt, res));
    ASSERT_TRUE(last_event.cache_hit);
    const uint32_t cached_ttl = ldns_rr_ttl(ldns_rr_list_rr(ldns_pkt_answer(res.get()), 0));
    ASSERT_LE(cached_ttl, ttl / 2);
}

TEST_F(dnsproxy_cache_test, cached_response_expires) {
    ag::ldns_pkt_ptr pkt = create_request("example.org.", LDNS_RR_TYPE_A, LDNS_RD);
    ag::ldns_pkt_ptr res;
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, pkt, res));
    ASSERT_FALSE(last_event.cache_hit);
    ASSERT_GT(ldns_pkt_ancount(res.get()), 0);

    const uint32_t ttl = ldns_rr_ttl(ldns_rr_list_rr(ldns_pkt_answer(res.get()), 0));
    ASSERT_GT(ttl, 0);
    ag::steady_clock::add_time_shift(std::chrono::seconds(ttl + 1));

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, pkt, res));
    ASSERT_FALSE(last_event.cache_hit);
}

TEST_F(dnsproxy_cache_test, cached_response_question_matches_request) {
    ag::ldns_pkt_ptr pkt = create_request("GoOGLe.CoM", LDNS_RR_TYPE_A, LDNS_RD);
    ag::ldns_pkt_ptr res;
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, pkt, res));
    ASSERT_FALSE(last_event.cache_hit);
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, pkt, res));
    ASSERT_TRUE(last_event.cache_hit);

    ldns_rr *resp_question = ldns_rr_list_rr(ldns_pkt_question(res.get()), 0);
    ag::allocated_ptr<char> resp_question_domain(ldns_rdf2str(ldns_rr_owner(resp_question)));
    ag::allocated_ptr<char> req_question_domain(ldns_rdf2str(ldns_rr_owner(ldns_rr_list_rr(ldns_pkt_question(pkt.get()), 0))));

    ASSERT_EQ(0, std::strcmp(req_question_domain.get(), resp_question_domain.get()));
    ASSERT_EQ(LDNS_RR_TYPE_A, ldns_rr_get_type(resp_question));
}

TEST_F(dnsproxy_cache_test, cache_size_is_set) {
    // Cache size is 1 for this test
    ag::ldns_pkt_ptr res;
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("google.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_FALSE(last_event.cache_hit);
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("yandex.ru", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_FALSE(last_event.cache_hit);
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("yandex.ru", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_TRUE(last_event.cache_hit);
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("google.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_FALSE(last_event.cache_hit);
}

TEST_F(dnsproxy_cache_test, cache_key_test) {
    ag::ldns_pkt_ptr res;

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("google.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_FALSE(last_event.cache_hit);

    // Check case doesn't matter
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("GoOgLe.CoM", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_TRUE(last_event.cache_hit);

    // Check class matters
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("google.com", LDNS_RR_TYPE_A, LDNS_RD, LDNS_RR_CLASS_CH), res));
    ASSERT_FALSE(last_event.cache_hit);

    // Check type matters
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("google.com", LDNS_RR_TYPE_AAAA, LDNS_RD), res));
    ASSERT_FALSE(last_event.cache_hit);

    // Check CD flag matters
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("google.com", LDNS_RR_TYPE_A, LDNS_RD | LDNS_CD), res));
    ASSERT_FALSE(last_event.cache_hit);

    // Check DO flag matters
    ag::ldns_pkt_ptr req = create_request("google.com", LDNS_RR_TYPE_A, LDNS_RD);
    ldns_pkt_set_edns_do(req.get(), true);
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, req, res));
    ASSERT_FALSE(last_event.cache_hit);
}

TEST_F(dnsproxy_test, blocking_mode_default) {
    ag::dnsproxy_settings settings = make_dnsproxy_settings();
    settings.filter_params = {{{1, "blocking_modes_test_filter.txt"}}};

    ASSERT_EQ(ag::dnsproxy_blocking_mode::REFUSED, settings.adblock_rules_blocking_mode);
    ASSERT_EQ(ag::dnsproxy_blocking_mode::ADDRESS, settings.hosts_rules_blocking_mode);

    auto [ret, err] = proxy.init(settings, {});
    ASSERT_TRUE(ret) << *err;

    ag::ldns_pkt_ptr res;

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("adb-style.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_REFUSED, ldns_pkt_get_rcode(res.get()));

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("adb-style.com", LDNS_RR_TYPE_AAAA, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_REFUSED, ldns_pkt_get_rcode(res.get()));

    // Check weird qtype
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("privacy-policy.truste.com", (ldns_rr_type) 65, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_REFUSED, ldns_pkt_get_rcode(res.get()));
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style.truste.com", (ldns_rr_type) 65, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_EQ(1, ldns_pkt_nscount(res.get()));

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-unspec.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("0.0.0.0", make_rr_answer_string(res.get()).get());

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-unspec-6.com", LDNS_RR_TYPE_AAAA, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("::", make_rr_answer_string(res.get()).get());

    // Check loopback is equivalent to unspec
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-loopback.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("0.0.0.0", make_rr_answer_string(res.get()).get());

    // Check loopback is equivalent to unspec for IPv6
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-loopback-6.com", LDNS_RR_TYPE_AAAA, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("::", make_rr_answer_string(res.get()).get());

    // Check custom IP works
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-custom.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("1.2.3.4", make_rr_answer_string(res.get()).get());

    // Check custom IP works for IPv6
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-custom-6.com", LDNS_RR_TYPE_AAAA, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("12::34", make_rr_answer_string(res.get()).get());

    // Check custom (from rule!) IP works
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-4-and-6.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("4.5.6.7", make_rr_answer_string(res.get()).get());

    // Check custom (from rule!) IP works for IPv6
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-4-and-6.com", LDNS_RR_TYPE_AAAA, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("45::67", make_rr_answer_string(res.get()).get());
}

TEST_F(dnsproxy_test, blocking_mode_nxdomain) {
    ag::dnsproxy_settings settings = make_dnsproxy_settings();
    settings.filter_params = {{{1, "blocking_modes_test_filter.txt"}}};
    settings.adblock_rules_blocking_mode = ag::dnsproxy_blocking_mode::NXDOMAIN;
    settings.hosts_rules_blocking_mode = ag::dnsproxy_blocking_mode::NXDOMAIN;

    auto [ret, err] = proxy.init(settings, {});
    ASSERT_TRUE(ret) << *err;

    ag::ldns_pkt_ptr res;

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("adb-style.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NXDOMAIN, ldns_pkt_get_rcode(res.get()));
    ASSERT_EQ(ldns_pkt_nscount(res.get()), 1);

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("adb-style.com", LDNS_RR_TYPE_AAAA, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NXDOMAIN, ldns_pkt_get_rcode(res.get()));
    ASSERT_EQ(ldns_pkt_nscount(res.get()), 1);

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-unspec.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NXDOMAIN, ldns_pkt_get_rcode(res.get()));
    ASSERT_EQ(ldns_pkt_nscount(res.get()), 1);

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-unspec-6.com", LDNS_RR_TYPE_AAAA, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NXDOMAIN, ldns_pkt_get_rcode(res.get()));
    ASSERT_EQ(ldns_pkt_nscount(res.get()), 1);

    // Check loopback is equivalent to unspec
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-loopback.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NXDOMAIN, ldns_pkt_get_rcode(res.get()));
    ASSERT_EQ(ldns_pkt_nscount(res.get()), 1);

    // Check loopback is equivalent to unspec for IPv6
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-loopback-6.com", LDNS_RR_TYPE_AAAA, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NXDOMAIN, ldns_pkt_get_rcode(res.get()));
    ASSERT_EQ(ldns_pkt_nscount(res.get()), 1);

    // Check weird qtype
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("privacy-policy.truste.com", (ldns_rr_type) 65, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NXDOMAIN, ldns_pkt_get_rcode(res.get()));
    ASSERT_EQ(ldns_pkt_nscount(res.get()), 1);

    // Check custom IP works
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-custom.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("1.2.3.4", make_rr_answer_string(res.get()).get());

    // Check custom IP works for IPv6
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-custom-6.com", LDNS_RR_TYPE_AAAA, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("12::34", make_rr_answer_string(res.get()).get());

    // Check custom (from rule!) IP works
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-4-and-6.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("4.5.6.7", make_rr_answer_string(res.get()).get());

    // Check custom (from rule!) IP works for IPv6
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-4-and-6.com", LDNS_RR_TYPE_AAAA, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("45::67", make_rr_answer_string(res.get()).get());
}

TEST_F(dnsproxy_test, blocking_mode_refused) {
    ag::dnsproxy_settings settings = make_dnsproxy_settings();
    settings.filter_params = {{{1, "blocking_modes_test_filter.txt"}}};
    settings.adblock_rules_blocking_mode = ag::dnsproxy_blocking_mode::REFUSED;
    settings.hosts_rules_blocking_mode = ag::dnsproxy_blocking_mode::REFUSED;

    auto [ret, err] = proxy.init(settings, {});
    ASSERT_TRUE(ret) << *err;

    ag::ldns_pkt_ptr res;

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("adb-style.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_REFUSED, ldns_pkt_get_rcode(res.get()));

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("adb-style.com", LDNS_RR_TYPE_AAAA, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_REFUSED, ldns_pkt_get_rcode(res.get()));

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-unspec.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_REFUSED, ldns_pkt_get_rcode(res.get()));

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-unspec-6.com", LDNS_RR_TYPE_AAAA, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_REFUSED, ldns_pkt_get_rcode(res.get()));

    // Check loopback is equivalent to unspec
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-loopback.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_REFUSED, ldns_pkt_get_rcode(res.get()));

    // Check loopback is equivalent to unspec for IPv6
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-loopback-6.com", LDNS_RR_TYPE_AAAA, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_REFUSED, ldns_pkt_get_rcode(res.get()));

    // Check weird qtype
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("privacy-policy.truste.com", (ldns_rr_type) 65, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_REFUSED, ldns_pkt_get_rcode(res.get()));

    // Check weird qtype (hosts-style rule)
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-custom.com", (ldns_rr_type) 65, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_EQ(0, ldns_pkt_ancount(res.get()));
    ASSERT_EQ(1, ldns_pkt_nscount(res.get()));

    // Check rule IP works
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-custom.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("1.2.3.4", make_rr_answer_string(res.get()).get());

    // Check rule IP works
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-custom-6.com", LDNS_RR_TYPE_AAAA, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("12::34", make_rr_answer_string(res.get()).get());

    // Check rule IP works
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-4-and-6.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("4.5.6.7", make_rr_answer_string(res.get()).get());

    // Check rule IP works
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-4-and-6.com", LDNS_RR_TYPE_AAAA, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("45::67", make_rr_answer_string(res.get()).get());
}

TEST_F(dnsproxy_test, blocking_mode_unspecified_address) {
    ag::dnsproxy_settings settings = make_dnsproxy_settings();
    settings.filter_params = {{{1, "blocking_modes_test_filter.txt"}}};
    settings.adblock_rules_blocking_mode = ag::dnsproxy_blocking_mode::ADDRESS;
    settings.hosts_rules_blocking_mode = ag::dnsproxy_blocking_mode::ADDRESS;

    auto [ret, err] = proxy.init(settings, {});
    ASSERT_TRUE(ret) << *err;

    ag::ldns_pkt_ptr res;

    // Check weird qtype
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("privacy-policy.truste.com", (ldns_rr_type) 65, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_EQ(1, ldns_pkt_nscount(res.get()));

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("adb-style.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("0.0.0.0", make_rr_answer_string(res.get()).get());

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("adb-style.com", LDNS_RR_TYPE_AAAA, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("::", make_rr_answer_string(res.get()).get());

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-unspec.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("0.0.0.0", make_rr_answer_string(res.get()).get());

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-unspec-6.com", LDNS_RR_TYPE_AAAA, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("::", make_rr_answer_string(res.get()).get());

    // Check loopback is equivalent to unspec
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-loopback.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("0.0.0.0", make_rr_answer_string(res.get()).get());

    // Check loopback is equivalent to unspec for IPv6
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-loopback-6.com", LDNS_RR_TYPE_AAAA, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("::", make_rr_answer_string(res.get()).get());

    // Check custom IP works
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-custom.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("1.2.3.4", make_rr_answer_string(res.get()).get());

    // Check custom IP works for IPv6
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-custom-6.com", LDNS_RR_TYPE_AAAA, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("12::34", make_rr_answer_string(res.get()).get());

    // Check custom (from rule!) IP works
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-4-and-6.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("4.5.6.7", make_rr_answer_string(res.get()).get());

    // Check custom (from rule!) IP works for IPv6
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-4-and-6.com", LDNS_RR_TYPE_AAAA, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("45::67", make_rr_answer_string(res.get()).get());
}

TEST_F(dnsproxy_test, blocking_mode_custom_address) {
    ag::dnsproxy_settings settings = make_dnsproxy_settings();
    settings.filter_params = {{{1, "blocking_modes_test_filter.txt"}}};
    settings.adblock_rules_blocking_mode = ag::dnsproxy_blocking_mode::ADDRESS;
    settings.hosts_rules_blocking_mode = ag::dnsproxy_blocking_mode::ADDRESS;
    settings.custom_blocking_ipv4 = "4.3.2.1";
    settings.custom_blocking_ipv6 = "43::21";

    auto [ret, err] = proxy.init(settings, {});
    ASSERT_TRUE(ret) << *err;

    ag::ldns_pkt_ptr res;

    // Check weird qtype
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("privacy-policy.truste.com", (ldns_rr_type) 65, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_EQ(0, ldns_pkt_ancount(res.get()));
    ASSERT_EQ(1, ldns_pkt_nscount(res.get()));

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("adb-style.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("4.3.2.1", make_rr_answer_string(res.get()).get());

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("adb-style.com", LDNS_RR_TYPE_AAAA, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("43::21", make_rr_answer_string(res.get()).get());

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-unspec.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("4.3.2.1", make_rr_answer_string(res.get()).get());

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-unspec-6.com", LDNS_RR_TYPE_AAAA, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("43::21", make_rr_answer_string(res.get()).get());

    // Check loopback is equivalent to unspec
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-loopback.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("4.3.2.1", make_rr_answer_string(res.get()).get());

    // Check loopback is equivalent to unspec for IPv6
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-loopback-6.com", LDNS_RR_TYPE_AAAA, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("43::21", make_rr_answer_string(res.get()).get());

    // Check custom (from rule!) IP works
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-custom.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("1.2.3.4", make_rr_answer_string(res.get()).get());

    // Check custom (from rule!) IP works for IPv6
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-custom-6.com", LDNS_RR_TYPE_AAAA, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("12::34", make_rr_answer_string(res.get()).get());

    // Check custom (from rule!) IP works
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-4-and-6.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("4.5.6.7", make_rr_answer_string(res.get()).get());

    // Check custom (from rule!) IP works for IPv6
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-4-and-6.com", LDNS_RR_TYPE_AAAA, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("45::67", make_rr_answer_string(res.get()).get());
}

TEST_F(dnsproxy_test, blocking_mode_custom_address_ipv4_only) {
    ag::dnsproxy_settings settings = make_dnsproxy_settings();
    settings.filter_params = {{{1, "blocking_modes_test_filter.txt"}}};
    settings.adblock_rules_blocking_mode = ag::dnsproxy_blocking_mode::ADDRESS;
    settings.hosts_rules_blocking_mode = ag::dnsproxy_blocking_mode::ADDRESS;
    settings.custom_blocking_ipv4 = "4.3.2.1";

    auto [ret, err] = proxy.init(settings, {});
    ASSERT_TRUE(ret) << *err;

    ag::ldns_pkt_ptr res;

    // Check weird qtype
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("privacy-policy.truste.com", (ldns_rr_type) 65, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_EQ(0, ldns_pkt_ancount(res.get()));
    ASSERT_EQ(1, ldns_pkt_nscount(res.get()));

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("adb-style.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("4.3.2.1", make_rr_answer_string(res.get()).get());

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("adb-style.com", LDNS_RR_TYPE_AAAA, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_EQ(0, ldns_pkt_ancount(res.get()));
    ASSERT_EQ(1, ldns_pkt_nscount(res.get()));

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-unspec.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("4.3.2.1", make_rr_answer_string(res.get()).get());

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-unspec-6.com", LDNS_RR_TYPE_AAAA, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_EQ(0, ldns_pkt_ancount(res.get()));
    ASSERT_EQ(1, ldns_pkt_nscount(res.get()));

    // Check loopback is equivalent to unspec
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-loopback.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("4.3.2.1", make_rr_answer_string(res.get()).get());

    // Check loopback is equivalent to unspec for IPv6
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-loopback-6.com", LDNS_RR_TYPE_AAAA, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_EQ(0, ldns_pkt_ancount(res.get()));
    ASSERT_EQ(1, ldns_pkt_nscount(res.get()));

    // Check custom (from rule!) IP works
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-custom.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("1.2.3.4", make_rr_answer_string(res.get()).get());

    // Check custom (from rule!) IP works for IPv6
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-custom-6.com", LDNS_RR_TYPE_AAAA, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("12::34", make_rr_answer_string(res.get()).get());

    // Check custom (from rule!) IP works
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-4-and-6.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("4.5.6.7", make_rr_answer_string(res.get()).get());

    // Check custom (from rule!) IP works for IPv6
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-4-and-6.com", LDNS_RR_TYPE_AAAA, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("45::67", make_rr_answer_string(res.get()).get());
}

TEST_F(dnsproxy_test, blocking_mode_custom_address_ipv6_only) {
    ag::dnsproxy_settings settings = make_dnsproxy_settings();
    settings.filter_params = {{{1, "blocking_modes_test_filter.txt"}}};
    settings.adblock_rules_blocking_mode = ag::dnsproxy_blocking_mode::ADDRESS;
    settings.hosts_rules_blocking_mode = ag::dnsproxy_blocking_mode::ADDRESS;
    settings.custom_blocking_ipv6 = "43::21";

    auto [ret, err] = proxy.init(settings, {});
    ASSERT_TRUE(ret) << *err;

    ag::ldns_pkt_ptr res;

    // Check weird qtype
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("privacy-policy.truste.com", (ldns_rr_type) 65, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_EQ(0, ldns_pkt_ancount(res.get()));
    ASSERT_EQ(1, ldns_pkt_nscount(res.get()));

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("adb-style.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_EQ(0, ldns_pkt_ancount(res.get()));
    ASSERT_EQ(1, ldns_pkt_nscount(res.get()));

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("adb-style.com", LDNS_RR_TYPE_AAAA, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("43::21", make_rr_answer_string(res.get()).get());

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-unspec.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_EQ(0, ldns_pkt_ancount(res.get()));
    ASSERT_EQ(1, ldns_pkt_nscount(res.get()));

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-unspec-6.com", LDNS_RR_TYPE_AAAA, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("43::21", make_rr_answer_string(res.get()).get());

    // Check loopback is equivalent to unspec
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-loopback.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_EQ(0, ldns_pkt_ancount(res.get()));
    ASSERT_EQ(1, ldns_pkt_nscount(res.get()));

    // Check loopback is equivalent to unspec for IPv6
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-loopback-6.com", LDNS_RR_TYPE_AAAA, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("43::21", make_rr_answer_string(res.get()).get());

    // Check custom (from rule!) IP works
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-custom.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("1.2.3.4", make_rr_answer_string(res.get()).get());

    // Check custom (from rule!) IP works for IPv6
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-custom-6.com", LDNS_RR_TYPE_AAAA, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("12::34", make_rr_answer_string(res.get()).get());

    // Check custom (from rule!) IP works
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-4-and-6.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("4.5.6.7", make_rr_answer_string(res.get()).get());

    // Check custom (from rule!) IP works for IPv6
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("hosts-style-4-and-6.com", LDNS_RR_TYPE_AAAA, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_STREQ("45::67", make_rr_answer_string(res.get()).get());
}

TEST_F(dnsproxy_test, custom_blocking_address_validation_1) {
    ag::dnsproxy_settings settings = make_dnsproxy_settings();
    auto [ret, err] = proxy.init(settings, {});
    ASSERT_TRUE(ret) << *err;
}

TEST_F(dnsproxy_test, custom_blocking_address_validation_2) {
    ag::dnsproxy_settings settings = make_dnsproxy_settings();
    settings.custom_blocking_ipv4 = "abracadabra";
    settings.custom_blocking_ipv6 = "::1";
    auto [ret, _] = proxy.init(settings, {});
    ASSERT_FALSE(ret);
}

TEST_F(dnsproxy_test, custom_blocking_address_validation_3) {
    ag::dnsproxy_settings settings = make_dnsproxy_settings();
    settings.custom_blocking_ipv4 = "127.0.0.1";
    settings.custom_blocking_ipv6 = "abracadabra";
    auto [ret, _] = proxy.init(settings, {});
    ASSERT_FALSE(ret);
}

TEST_F(dnsproxy_test, correct_filter_ids_in_event) {
    ag::dnsproxy_settings settings = make_dnsproxy_settings();
    settings.filter_params = {{
        {15, "cname_blocking_test_filter.txt"},
        {-3, "blocking_modes_test_filter.txt"},
    }};

    ag::dns_request_processed_event last_event{};
    ag::dnsproxy_events events{
        .on_request_processed = [&last_event](const ag::dns_request_processed_event &event) {
            last_event = event;
        }
    };

    auto [ret, err] = proxy.init(settings, events);
    ASSERT_TRUE(ret) << *err;

    ag::ldns_pkt_ptr res;

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request(CNAME_BLOCKING_HOST, LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(1, last_event.filter_list_ids.size());
    ASSERT_EQ(15, last_event.filter_list_ids[0]);

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("adb-style.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(1, last_event.filter_list_ids.size());
    ASSERT_EQ(-3, last_event.filter_list_ids[0]);
}

TEST_F(dnsproxy_test, whitelisting) {
    ag::dnsproxy_settings settings = make_dnsproxy_settings();
    settings.filter_params = {{
        {15, "whitelist_test_filter.txt"},
    }};

    ag::dns_request_processed_event last_event{};
    ag::dnsproxy_events events{
        .on_request_processed = [&last_event](const ag::dns_request_processed_event &event) {
            last_event = event;
        }
    };

    auto [ret, err] = proxy.init(settings, events);
    ASSERT_TRUE(ret) << *err;

    ag::ldns_pkt_ptr res;

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("example.org", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(1, last_event.filter_list_ids.size());
    ASSERT_TRUE(last_event.whitelist);

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request(CNAME_BLOCKING_HOST, LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(2, last_event.filter_list_ids.size()); // Whitelisted by both domain and CNAME
    ASSERT_TRUE(last_event.whitelist);

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request(IPV4_ONLY_HOST, LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(2, last_event.filter_list_ids.size()); // Whitelisted by domain,
    ASSERT_FALSE(last_event.whitelist); // then blocked by IP, because of $important

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("google.com", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(0, last_event.filter_list_ids.size()); // Not blocked
    ASSERT_FALSE(last_event.whitelist); // Neither whitelisted

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("sync.datamind.ru", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_GT(ldns_pkt_ancount(res.get()), 0);
    ASSERT_EQ(ldns_pkt_get_rcode(res.get()), LDNS_RCODE_NOERROR);
    ASSERT_TRUE(last_event.whitelist);
}

TEST_F(dnsproxy_test, fallbacks_ignore_proxy_socks) {
    ag::dnsproxy_settings settings = make_dnsproxy_settings();
    settings.fallbacks = settings.upstreams;
    // some nonexistent proxy
    settings.outbound_proxy = { { ag::outbound_proxy_protocol::SOCKS5_UDP, "255.255.255.255", 1 } };

    ag::dns_request_processed_event last_event{};

    auto [ret, err] = proxy.init(settings, {});
    ASSERT_TRUE(ret) << *err;

    ag::ldns_pkt_ptr res;
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("example.org", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
}

TEST_F(dnsproxy_test, fallbacks_ignore_proxy_http) {
    ag::dnsproxy_settings settings = make_dnsproxy_settings();
    settings.upstreams = {{ .address = "tcp://94.140.14.140" }};
    settings.fallbacks = {{ .address = "tcp://94.140.14.140" }};
    // some nonexistent proxy
    settings.outbound_proxy = { { ag::outbound_proxy_protocol::HTTP_CONNECT, "255.255.255.255", 1 } };

    ag::dns_request_processed_event last_event{};

    auto [ret, err] = proxy.init(settings, {});
    ASSERT_TRUE(ret) << *err;

    ag::ldns_pkt_ptr res;
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("example.org", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
}

TEST_F(dnsproxy_test, bad_filter_file_does_not_crash) {
    ag::dnsproxy_settings settings = make_dnsproxy_settings();
    settings.filter_params = {{ {111, "bad_test_filter.txt"}, }};
    auto [ret, err] = proxy.init(settings, {});
    ASSERT_TRUE(ret) << *err;
}

TEST_F(dnsproxy_test, rules_load_from_memory) {
    ag::dnsproxy_settings settings = make_dnsproxy_settings();

    std::string filter_data;
    ag::file::handle file_handle = ag::file::open("bad_test_filter.txt", ag::file::RDONLY);
    ag::file::for_each_line(file_handle, [](uint32_t, std::string_view line, void *arg) -> bool {
        auto &s = *(std::string *) arg;
        s += line;
        s += "\r\n";
        return true;
    }, &filter_data);

    settings.filter_params = {{ {42, filter_data, true}, }};
    auto [ret, err] = proxy.init(settings, {});
    ASSERT_TRUE(ret) << *err;
}

TEST_F(dnsproxy_test, ip_blocking_regress) {
    ag::dnsproxy_settings settings = make_dnsproxy_settings();
    settings.filter_params = {{ {15, "crash_regress_test_filter.txt"}, }};

    ag::dns_request_processed_event last_event{};
    ag::dnsproxy_events events{
            .on_request_processed = [&last_event](const ag::dns_request_processed_event &event) {
                last_event = event;
            }
    };

    auto [ret, err] = proxy.init(settings, events);
    ASSERT_TRUE(ret) << *err;

    ag::ldns_pkt_ptr res;

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("example.org", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request(IPV4_ONLY_HOST, LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_EQ(1, last_event.filter_list_ids.size()); // Whitelisted by both domain and CNAME
    ASSERT_FALSE(last_event.whitelist);

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("dns.adguard.com", LDNS_RR_TYPE_AAAA, LDNS_RD), res));
    ASSERT_EQ(1, last_event.filter_list_ids.size()); // Whitelisted by both domain and CNAME
    ASSERT_FALSE(last_event.whitelist);
}

TEST_F(dnsproxy_test, warnings) {
    ag::dnsproxy_settings settings = make_dnsproxy_settings();

    settings.filter_params = {{ {15, "blocking_modes_test_filter.txt"}, }};
    {
        auto [ret, err_or_warn] = proxy.init(settings, {});
        ASSERT_TRUE(ret) << *err_or_warn;
        ASSERT_FALSE(err_or_warn); // No warning
        proxy.deinit();
    }

    settings.filter_params.mem_limit = 1;
    {
        auto [ret, err_or_warn] = proxy.init(settings, {});
        ASSERT_TRUE(ret) << *err_or_warn;
        ASSERT_TRUE(err_or_warn); // Mem usage warning
    }
}

TEST_F(dnsproxy_test, optimistic_cache) {
    ag::dnsproxy_settings settings = make_dnsproxy_settings();
    settings.optimistic_cache = true;
    settings.dns_cache_size = 100;

    ag::dns_request_processed_event last_event{};
    ag::dnsproxy_events events{
            .on_request_processed = [&last_event](const ag::dns_request_processed_event &event) {
                last_event = event;
            }
    };

    auto [ret, err] = proxy.init(settings, events);
    ASSERT_TRUE(ret) << *err;

    ag::ldns_pkt_ptr res;
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("example.org", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_FALSE(last_event.cache_hit);
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_GT(ldns_pkt_ancount(res.get()), 0);

    uint32_t max_ttl = 0;
    for (int i = 0; i < ldns_pkt_ancount(res.get()); ++i) {
        max_ttl = std::max(max_ttl, ldns_rr_ttl(ldns_rr_list_rr(ldns_pkt_answer(res.get()), i)));
    }

    ag::steady_clock::add_time_shift(std::chrono::seconds(2 * max_ttl));

    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("example.org", LDNS_RR_TYPE_A, LDNS_RD), res));
    ASSERT_TRUE(last_event.cache_hit);
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_GT(ldns_pkt_ancount(res.get()), 0);
    for (int i = 0; i < ldns_pkt_ancount(res.get()); ++i) {
        ASSERT_EQ(1, ldns_rr_ttl(ldns_rr_list_rr(ldns_pkt_answer(res.get()), i)));
    }
}

TEST_F(dnsproxy_test, dnssec_simple_test) {
    ag::dnsproxy_settings settings = make_dnsproxy_settings();
    settings.enable_dnssec_ok = true;

    std::vector<std::string> dnssecSupport = { "cloudflare.com", "example.org" };
    std::vector<std::string> dnssecNotSupport = { "adguard.com", "google.com" };
    ldns_enum_rr_type arrOfTypes[] = { LDNS_RR_TYPE_AAAA, LDNS_RR_TYPE_A, LDNS_RR_TYPE_TXT };

    ag::dns_request_processed_event last_event{};
    ag::dnsproxy_events events{
            .on_request_processed = [&last_event](const ag::dns_request_processed_event &event) {
                last_event = event;
            }
    };

    auto [ret, err] = proxy.init(settings, events);
    ASSERT_TRUE(ret) << *err;

    for (auto &curAddress : dnssecSupport) {
        for (auto curType : arrOfTypes) {
            ag::ldns_pkt_ptr res;
            ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request(curAddress, curType, LDNS_RD), res));
            ASSERT_TRUE(last_event.dnssec);
            ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
            ASSERT_GT(ldns_pkt_ancount(res.get()), 0);
            // check that RRSIG section does not exist cuz the request haven't DO bit
            ASSERT_TRUE(last_event.answer.find("RRSIG") == std::string::npos);
            auto ptr = ldns_pkt_rr_list_by_type(res.get(), LDNS_RR_TYPE_RRSIG, LDNS_SECTION_ANSWER);
            ASSERT_EQ(nullptr, ptr);
            ldns_rr_list_deep_free(ptr);
        }
    }

    for (auto &curAddress : dnssecNotSupport) {
        for (auto curType : arrOfTypes) {
            ag::ldns_pkt_ptr res;
            ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request(curAddress, curType, LDNS_RD), res));
            ASSERT_FALSE(last_event.dnssec);
            ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
            ASSERT_GT(ldns_pkt_ancount(res.get()), 0);
            // check that RRSIG section does not exist cuz the request haven't DO bit
            ASSERT_TRUE(last_event.answer.find("RRSIG") == std::string::npos);
            auto ptr = ldns_pkt_rr_list_by_type(res.get(), LDNS_RR_TYPE_RRSIG, LDNS_SECTION_ANSWER);
            ASSERT_EQ(nullptr, ptr);
            ldns_rr_list_deep_free(ptr);
        }
    }
}

TEST_F(dnsproxy_test, dnssec_request_with_do_bit) {
    ag::dnsproxy_settings settings = make_dnsproxy_settings();
    settings.enable_dnssec_ok = true;

    ag::dns_request_processed_event last_event{};
    ag::dnsproxy_events events{
            .on_request_processed = [&last_event](const ag::dns_request_processed_event &event) {
                last_event = event;
            }
    };

    auto [ret, err] = proxy.init(settings, events);
    ASSERT_TRUE(ret) << *err;

    ag::ldns_pkt_ptr res;
    auto req = create_request("cloudflare.com", LDNS_RR_TYPE_A, LDNS_RD);
    ldns_pkt_set_edns_do(req.get(), true);
    ldns_pkt_set_edns_udp_size(req.get(), 4096);
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, req, res));
    ASSERT_TRUE(last_event.dnssec);
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_GT(ldns_pkt_ancount(res.get()), 0);
    // check that response not modified
    ASSERT_FALSE(last_event.answer.find("RRSIG") == std::string::npos);
    auto ptr = ldns_pkt_rr_list_by_type(res.get(), LDNS_RR_TYPE_RRSIG, LDNS_SECTION_ANSWER);
    ASSERT_NE(nullptr, ptr);
    ldns_rr_list_deep_free(ptr);
}

TEST_F(dnsproxy_test, dnssec_ds_request) {
    ag::dnsproxy_settings settings = make_dnsproxy_settings();
    settings.enable_dnssec_ok = true;

    ag::dns_request_processed_event last_event{};
    ag::dnsproxy_events events{
            .on_request_processed = [&last_event](const ag::dns_request_processed_event &event) {
                last_event = event;
            }
    };

    auto [ret, err] = proxy.init(settings, events);
    ASSERT_TRUE(ret) << *err;

    ag::ldns_pkt_ptr res;
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("cloudflare.com", LDNS_RR_TYPE_DS, LDNS_RD), res));
    ASSERT_TRUE(last_event.dnssec);
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_GT(ldns_pkt_ancount(res.get()), 0);
    // check that response was modified cuz DO bit we added
    ASSERT_TRUE(last_event.answer.find("RRSIG") == std::string::npos);
    auto ptr = ldns_pkt_rr_list_by_type(res.get(), LDNS_RR_TYPE_RRSIG, LDNS_SECTION_ANSWER);
    ASSERT_EQ(nullptr, ptr);
    ldns_rr_list_deep_free(ptr);
    // but type of request here is in response
    ptr = ldns_pkt_rr_list_by_type(res.get(), LDNS_RR_TYPE_DS, LDNS_SECTION_ANSWER);
    ASSERT_NE(nullptr, ptr);
    ldns_rr_list_deep_free(ptr);
}

TEST_F(dnsproxy_test, dnssec_the_same_qtype_request) {
    ag::dnsproxy_settings settings = make_dnsproxy_settings();
    // dns.adguard.com answers SERVFAIL
    settings.upstreams = {{ .address = "1.1.1.1" }};
    settings.enable_dnssec_ok = true;

    ag::dns_request_processed_event last_event{};
    ag::dnsproxy_events events{
            .on_request_processed = [&last_event](const ag::dns_request_processed_event &event) {
                last_event = event;
            }
    };

    auto [ret, err] = proxy.init(settings, events);
    ASSERT_TRUE(ret) << *err;

    ag::ldns_pkt_ptr res;
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("example.org", LDNS_RR_TYPE_RRSIG, LDNS_RD), res));
    ASSERT_EQ(LDNS_RCODE_NOERROR, ldns_pkt_get_rcode(res.get()));
    ASSERT_GT(ldns_pkt_ancount(res.get()), 0);
    // check that response not modified
    ASSERT_FALSE(last_event.answer.find("RRSIG") == std::string::npos);
    auto ptr = ldns_pkt_rr_list_by_type(res.get(), LDNS_RR_TYPE_RRSIG, LDNS_SECTION_ANSWER);
    ASSERT_NE(nullptr, ptr);
    ldns_rr_list_deep_free(ptr);
}

TEST_F(dnsproxy_test, dnssec_regress_does_not_scrub_cname) {
    ag::dnsproxy_settings settings = make_dnsproxy_settings();
    settings.upstreams = {{ .address = "1.1.1.1" }};
    settings.enable_dnssec_ok = true;

    ag::dns_request_processed_event last_event{};
    ag::dnsproxy_events events{
            .on_request_processed = [&last_event](const ag::dns_request_processed_event &event) {
                last_event = event;
            }
    };

    auto [ret, err] = proxy.init(settings, {});
    ASSERT_TRUE(ret) << *err;

    ag::ldns_pkt_ptr response;
    ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request(CNAME_BLOCKING_HOST, LDNS_RR_TYPE_A, LDNS_RD), response));
    ASSERT_GT(ldns_pkt_ancount(response.get()), 0);
    ASSERT_EQ(ldns_pkt_get_rcode(response.get()), LDNS_RCODE_NOERROR);

    ldns_rr_list *rrs = ldns_pkt_rr_list_by_type(response.get(), LDNS_RR_TYPE_CNAME, LDNS_SECTION_ANSWER);
    ASSERT_NE(rrs, nullptr);
    ASSERT_GT(ldns_rr_list_rr_count(rrs), 0);
    ldns_rr_list_deep_free(rrs);

    rrs = ldns_pkt_rr_list_by_type(response.get(), LDNS_RR_TYPE_A, LDNS_SECTION_ANSWER);
    ASSERT_NE(rrs, nullptr);
    ASSERT_GT(ldns_rr_list_rr_count(rrs), 0);
    ldns_rr_list_deep_free(rrs);
}

TEST_F(dnsproxy_test, dnssec_autority_section) {
    ag::dnsproxy_settings settings = make_dnsproxy_settings();
    settings.enable_dnssec_ok = true;

    ag::dns_request_processed_event last_event{};
    ag::dnsproxy_events events{
            .on_request_processed = [&last_event](const ag::dns_request_processed_event &event) {
                last_event = event;
            }
    };

    static const ldns_enum_rr_type SPECIAL_TYPES_DNSSEC_LOG_LOGIC[] = {
            LDNS_RR_TYPE_DS,
            LDNS_RR_TYPE_DNSKEY,
            LDNS_RR_TYPE_NSEC,
            LDNS_RR_TYPE_NSEC3,
            LDNS_RR_TYPE_RRSIG
    };

    auto [ret, err] = proxy.init(settings, events);
    ASSERT_TRUE(ret) << *err;

    for (auto cur : SPECIAL_TYPES_DNSSEC_LOG_LOGIC) {
        ag::ldns_pkt_ptr res;
        ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request("actuallythissitedoesnotexist.fuu", cur, LDNS_RD), res));
        ASSERT_EQ(LDNS_RCODE_NXDOMAIN, ldns_pkt_get_rcode(res.get()));
        auto ptr = ldns_pkt_rr_list_by_type(res.get(), LDNS_RR_TYPE_SIG, LDNS_SECTION_ANSWER);
        ASSERT_EQ(nullptr, ptr);
        ldns_rr_list_deep_free(ptr);
        for (auto cur : SPECIAL_TYPES_DNSSEC_LOG_LOGIC) {
            auto ptr = ldns_pkt_rr_list_by_type(res.get(), cur, LDNS_SECTION_AUTHORITY);
            ASSERT_EQ(nullptr, ptr);
            ldns_rr_list_deep_free(ptr);
        }
    }
}

TEST_F(dnsproxy_test, fallback_filter_works_and_defaults_are_correct) {
    static constexpr int32_t UPSTREAM_ID = 42;
    static constexpr int32_t FALLBACK_ID = 4242;
    ag::dnsproxy_settings settings = make_dnsproxy_settings();
    settings.upstreams = {{.address = "8.8.8.8", .id = UPSTREAM_ID}};
    settings.fallbacks = {{.address = "8.8.8.8", .id = FALLBACK_ID}};
    ag::dns_request_processed_event last_event{};
    ag::dnsproxy_events events{
            .on_request_processed = [&last_event](const ag::dns_request_processed_event &event) {
                last_event = event;
            }
    };
    auto [ret, err] = proxy.init(settings, events);
    ASSERT_TRUE(ret) << *err;
    for (const std::string &host : {"epdg.epc.aptg.com.tw",
                                    "epdg.epc.att.net",
                                    "epdg.mobileone.net.sg",
                                    "primgw.vowifina.spcsdns.net",
                                    "swu-loopback-epdg.qualcomm.com",
                                    "vowifi.jio.com",
                                    "weconnect.globe.com.ph",
                                    "wlan.three.com.hk",
                                    "wo.vzwwo.com",
                                    "epdg.epc.mncXXX.mccYYY.pub.3gppnetwork.org",
                                    "ss.epdg.epc.mncXXX.mccYYY.pub.3gppnetwork.org",
                                    }) {
        ag::ldns_pkt_ptr res;
        ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request(host, LDNS_RR_TYPE_A, LDNS_RD), res));
        ASSERT_TRUE(last_event.upstream_id.has_value()) << last_event.error;
        ASSERT_EQ(FALLBACK_ID, *last_event.upstream_id) << last_event.domain;
    }
    for (const std::string &host : {"a.epdg.epc.aptg.com.tw",
                                    "b.epdg.epc.att.net",
                                    "c.epdg.mobileone.net.sg",
                                    "d.primgw.vowifina.spcsdns.net",
                                    "e.swu-loopback-epdg.qualcomm.com",
                                    "f.vowifi.jio.com",
                                    "g.weconnect.globe.com.ph",
                                    "h.wlan.three.com.hk",
                                    "i.wo.vzwwo.com",
                                    "pub.3gppnetwork.org",
                                    "xyz.pub.3gppnetwork.org",
                                    }) {
        ag::ldns_pkt_ptr res;
        ASSERT_NO_FATAL_FAILURE(perform_request(proxy, create_request(host, LDNS_RR_TYPE_A, LDNS_RD), res));
        ASSERT_TRUE(last_event.upstream_id.has_value()) << last_event.error;
        ASSERT_EQ(UPSTREAM_ID, *last_event.upstream_id) << last_event.domain;
    }
}

// TEST_F(dnsproxy_test, fallback_domains_bad) {
//     ag::dnsproxy_settings settings = make_dnsproxy_settings();
//     for (const std::string &pattern : {"...",
//                                        "*",
//                                        "***",
//                                        "@@||example.org$important",
//                                        }) {
//         settings.fallback_domains = {pattern};
//         auto [ret, err] = proxy.init(settings, {});
//         ASSERT_FALSE(ret) << pattern;
//         ASSERT_TRUE(err) << pattern;
//         ASSERT_TRUE(strstr(err->c_str(), pattern.c_str())) << *err;
//     }
// }

TEST_F(dnsproxy_test, fallback_domains_good) {
    ag::dnsproxy_settings settings = make_dnsproxy_settings();
    for (const std::string &pattern : {"*.example.org",
                                       "*exampl",
                                       "exa*mp*l.com",
                                       "mygateway",
                                       "*.local",
                                       "*.company.local",
                                       }) {
        settings.fallback_domains = {pattern};
        auto [ret, err] = proxy.init(settings, {});
        ASSERT_TRUE(ret) << pattern;
        ASSERT_FALSE(err) << pattern;
        proxy.deinit();
    }
}
