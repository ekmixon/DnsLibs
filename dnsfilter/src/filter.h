#pragma once


#include <string_view>
#include <memory>
#include <vector>
#include <string>
#include <shared_mutex>
#include <dnsfilter.h>
#include "rule_utils.h"

class filter {
public:
    // Context of domain match
    struct match_context {
        std::string host; // matching domain name
        std::vector<std::string_view> subdomains; // list of subdomains
        std::vector<ag::dnsfilter::rule> matched_rules; // list of matched rules
        ldns_rr_type rr_type; // query RR type
        std::string reverse_lookup_fqdn; // non-empty if the request is a reverse DNS lookup
    };

    static match_context create_match_context(ag::dnsfilter::match_param param);

    filter();
    ~filter();

    filter(filter&&);
    filter &operator=(filter&&);

    filter(const filter&) = delete;
    filter &operator=(const filter&) = delete;

    enum load_result {
        LR_OK, LR_ERROR, LR_MEM_LIMIT_REACHED
    };

    /**
     * Load rule list
     * @param      params    filter parameters
     * @param      mem_limit if not 0, stop loading rules when the approximate memory consumption reaches this limit
     * @return     {load_result, approximate memory consumption}
     */
    std::pair<load_result, size_t> load(const ag::dnsfilter::filter_params &params, size_t mem_limit);

    /**
     * Match domain against rules
     * @param      ctx   match context
     * @return     true if match success, false if filter is outdated
     */
    bool match(match_context &ctx);

    /**
     * Update filter
     * @param   mem_limit    engine memory limit
     */
    void update(std::atomic_size_t &mem_limit);

    // Filter parameters
    ag::dnsfilter::filter_params params;

private:
    class impl;
    std::unique_ptr<impl> pimpl;
};