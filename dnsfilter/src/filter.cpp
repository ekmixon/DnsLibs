#include <string_view>
#include <cstdlib>
#include <cinttypes>
#include <algorithm>
#include <cassert>
#include <tuple>
#include <ag_regex.h>
#include <ag_logger.h>
#include <ag_utils.h>
#include <ag_file.h>
#include <ag_sys.h>
#include <dnsfilter.h>
#include <khash.h>
#include "filter.h"
#include "rule_utils.h"


static constexpr size_t APPROX_COMPILED_REGEX_BYTES = 1024; // Empirical

// Any rules besides simple domain rules, which into a somewhat
// contiguous table, cause significant memory fragmentation
// This coefficient was determined empirically
static constexpr double APPROX_FRAGMENTATION_COEF = 1.5;


static constexpr size_t SHORTCUT_LENGTH = 5;

KHASH_MAP_INIT_INT(hash_to_unique_index, uint32_t)
KHASH_MAP_INIT_INT(hash_to_indexes, std::vector<uint32_t>*)


struct match_arg {
    filter::match_context &ctx;
    filter &f;
    ag::file::handle file;
    bool outdated;
};


static void destroy_unique_index_table(kh_hash_to_unique_index_t *table) {
    if (table != nullptr) {
        for (khiter_t i = kh_begin(table); i != kh_end(table); ++i) {
            if (kh_exist(table, i)) {
                kh_del(hash_to_unique_index, table, i);
            }
        }
        kh_destroy(hash_to_unique_index, table);
    }
}

static void destroy_multi_index_table(kh_hash_to_indexes_t *table) {
    if (table != nullptr) {
        for (khiter_t i = kh_begin(table); i != kh_end(table); ++i) {
            if (kh_exist(table, i)) {
                delete kh_value(table, i);
                kh_del(hash_to_indexes, table, i);
            }
        }
        kh_destroy(hash_to_indexes, table);
    }
}

struct leftover_entry {
    // @note: each entry must contain either or both of shortcuts and regex
    std::vector<std::string> shortcuts; // list of extracted shortcuts
    std::optional<ag::regex> regex; // compiled regex
    uint32_t file_idx; // file index
};

class filter::impl {
public:
    impl()
        : unique_domains_table(kh_init(hash_to_unique_index))
        , domains_table(kh_init(hash_to_indexes))
        , shortcuts_table(kh_init(hash_to_indexes))
        , badfilter_table(kh_init(hash_to_unique_index))
        , approx_mem(0)
    {}

    ~impl() {
        destroy_unique_index_table(this->unique_domains_table);
        destroy_multi_index_table(this->domains_table);
        destroy_multi_index_table(this->shortcuts_table);
        destroy_unique_index_table(this->badfilter_table);
    }

    void put_hash_into_tables(uint32_t hash, uint32_t file_idx,
                              kh_hash_to_unique_index_t *unique_table,
                              kh_hash_to_indexes_t *multi_table);

    struct load_line_arg {
        impl *filter;
        size_t approx_mem;  // approximate usage so far
        size_t mem_limit;   // maximum allowed usage, 0 means no limit
        load_result result; // last rule load result
    };

    static bool check_filter_outdated(const filter &filter);
    static bool load_line(uint32_t file_idx, std::string_view line, void *arg);
    static bool match_against_line(match_arg &match, std::string_view line);
    static void match_by_file_position(match_arg &match, size_t idx);

    void search_by_domains(match_arg &match) const;
    void search_by_domains(match_arg &match, std::string_view domain) const;
    void search_by_shortcuts(match_arg &match) const;
    void search_in_leftovers(match_arg &match) const;
    void search_badfilter_rules(match_arg &match) const;

    ag::logger log;

    // unique domain -> rule string file index
    // This table contains indexes of the rules that match exact domains (and their subdomains)
    // (e.g. `example.org`, but for example not `example.org|` or `example.org^` as they
    // match `eeexample.org` as well)
    // As the lion's share of rule domains are unique, using a separate table
    // for such domains saves a lot of memory
    kh_hash_to_unique_index_t *unique_domains_table;
    // non-unique domain -> list of rule string file indexes
    // Similar to the previous one, but contains lists of indexes if the rules that match
    // the same domain.
    kh_hash_to_indexes_t *domains_table;

    // shortcut -> rule string file index
    // Contains indexes of the rules that can be filtered out by checking, if matching domain
    // contains any shortcut
    kh_hash_to_indexes_t *shortcuts_table;

    // Contains indexes of the rules that are not fitting to place in domains and shortcuts tables
    // due to they are any of:
    // - a regex rule for which the shortcut at least with length `SHORTCUT_LENGTH` was not found
    //   (e.g. `/ex.*\.com/`)
    // - a rule with special symbol for which the shortcut at least with length `SHORTCUT_LENGTH`
    //   was not found (e.g. `ex*.com`)
    // - a regex rule with some complicated expression (see `rule_utils::parse` for details)
    std::vector<leftover_entry> leftovers_table;

    // rule text -> badfilter rule file index
    // Contains indexes of the badfilter rules that could be found by rule text without
    // `badfilter` modifier
    kh_hash_to_unique_index_t *badfilter_table;

    size_t approx_mem;
};

filter::filter()
    : pimpl(new impl{})
{}

filter::~filter() = default;

filter::filter(filter &&other) {
    *this = std::move(other);
}

filter &filter::operator=(filter &&other) {
    this->params = std::move(other.params);
    this->pimpl = std::move(other.pimpl);
    return *this;
}

void filter::impl::put_hash_into_tables(uint32_t hash, uint32_t file_idx,
                                        kh_hash_to_unique_index_t *unique_table, kh_hash_to_indexes_t *multi_table) {
    bool already_exists = false;
    size_t stored_idx = 0;

    int ret;
    khiter_t iter = kh_get(hash_to_indexes, multi_table, hash);
    if (iter == kh_end(multi_table)) {
        // there is no such domain in non-unique table
        iter = kh_put(hash_to_unique_index, unique_table, hash, &ret);
        if (ret < 0) {
            errlog(log, "Out of memory");
            return;
        }
        already_exists = ret == 0;
        if (!already_exists) {
            // domain is unique - save index
            kh_value(unique_table, iter) = file_idx;
            return;
        } else {
            // we have one record for this domain - remove from unique table
            stored_idx = kh_value(unique_table, iter);
            kh_del(hash_to_unique_index, unique_table, iter);
        }
    }

    // create record in non-unique table
    iter = kh_put(hash_to_indexes, multi_table, hash, &ret);
    if (ret < 0) {
        errlog(log, "Out of memory");
        return;
    } else if (ret > 0) {
        // the record is a new one
        auto *positions = new(std::nothrow) std::vector<uint32_t>;
        if (positions == nullptr) {
            errlog(log, "Out of memory");
            return;
        }
        kh_value(multi_table, iter) = positions;
    }
    std::vector<uint32_t> *positions = kh_value(multi_table, iter);
    if (already_exists) {
        // put previously stored unique index, if it existed
        positions->reserve(positions->size() + 2);
        positions->push_back(stored_idx);
    }
    positions->push_back(file_idx);
}

struct rules_stat {
    size_t simple_domain_rules;
    size_t shortcut_rules;
    size_t leftover_rules;
    size_t badfilter_rules;
};

static bool count_rules(uint32_t, std::string_view line, void *arg) {
    std::optional<rule_utils::rule> rule = rule_utils::parse(line);
    if (!rule.has_value()) {
        return true;
    }

    auto *stat = (rules_stat *)arg;
    if (const auto *content = std::get_if<ag::dnsfilter::adblock_rule_info>(&rule->public_part.content);
            content != nullptr && content->props.test(ag::dnsfilter::DARP_BADFILTER)) {
        ++stat->badfilter_rules;
        return true;
    }

    switch (rule->match_method) {
    case rule_utils::rule::MMID_EXACT:
    case rule_utils::rule::MMID_SUBDOMAINS:
        stat->simple_domain_rules += rule->matching_parts.size();
        break;
    case rule_utils::rule::MMID_SHORTCUTS:
    case rule_utils::rule::MMID_SHORTCUTS_AND_REGEX:
        ++stat->shortcut_rules;
        break;
    case rule_utils::rule::MMID_REGEX:
        ++stat->leftover_rules;
        break;
    }

    return true;
}

bool filter::impl::check_filter_outdated(const filter &filter) {
    if (filter.params.in_memory) {
        return false;
    }
    time_t file_mtime = ag::file::get_modification_time(filter.params.data.data());
    if (!file_mtime || file_mtime != filter.params.mtime) {
        return true;
    }
    return false;
}

#define CHECK_MEM() do {                                                  \
    if (a->mem_limit && a->mem_limit < a->approx_mem + approx_rule_mem) { \
        a->result = LR_MEM_LIMIT_REACHED;                                 \
        return false;                                                     \
    }                                                                     \
} while (0)
bool filter::impl::load_line(uint32_t file_idx, std::string_view line, void *arg) {
    auto *a = (load_line_arg *) arg;
    filter::impl *self = a->filter;
    std::optional<rule_utils::rule> rule = rule_utils::parse(line, &self->log);

    if (!rule) {
        if (!line.empty() && !rule_utils::is_comment(line)) {
            dbglog(self->log, "Failed to parse rule: {}", line);
        }
        return true;
    }

    size_t approx_rule_mem = 0; // bytes
    const std::string &str = rule->public_part.text;

    if (const auto *content = std::get_if<ag::dnsfilter::adblock_rule_info>(&rule->public_part.content);
            content != nullptr && content->props.test(ag::dnsfilter::DARP_BADFILTER)) {
        std::string text = rule_utils::get_text_without_badfilter(rule->public_part);
        uint32_t hash = ag::utils::hash(text);

        approx_rule_mem = 4 * sizeof(uint32_t); // (k + v) * empty buckets coef
        CHECK_MEM();

        int ret;
        khiter_t iter = kh_put(hash_to_unique_index, self->badfilter_table, hash, &ret);
        if (ret < 0) {
            warnlog(self->log, "Failed to put rule in badfilter table: {}", str);
            return true;
        }
        kh_value(self->badfilter_table, iter) = file_idx;
        tracelog(self->log, "Rule placed in badfilter table: {}", str);
        goto next_line;
    }

    switch (rule->match_method) {
    case rule_utils::rule::MMID_EXACT:
    case rule_utils::rule::MMID_SUBDOMAINS:
        // count * (k + v) * empty buckets coef (assume non-unique domain rules are rare)
        approx_rule_mem = rule->matching_parts.size() * 4 * sizeof(uint32_t);
        CHECK_MEM();
        tracelog(self->log, "Placing a rule in domains table: {}", str);
        for (const std::string &d : rule->matching_parts) {
            self->put_hash_into_tables(ag::utils::hash(d), file_idx, self->unique_domains_table, self->domains_table);
        }
        goto next_line;
    case rule_utils::rule::MMID_SHORTCUTS:
    case rule_utils::rule::MMID_SHORTCUTS_AND_REGEX: {
        std::string_view sc = {};
        for (size_t i = 0; i < rule->matching_parts.size(); ++i) {
            const std::string &part = rule->matching_parts[i];
            if (sc.empty() && part.length() >= SHORTCUT_LENGTH) {
                sc = part;
                break;
            }
        }
        if (!sc.empty()) {
            std::vector<uint32_t> *positions;
            uint32_t hash = ag::utils::hash(sc.substr(0, SHORTCUT_LENGTH));
            khiter_t iter = kh_get(hash_to_indexes, self->shortcuts_table, hash);
            if (iter == kh_end(self->shortcuts_table)) { // add new
                int ret;
                iter = kh_put(hash_to_indexes, self->shortcuts_table, hash, &ret);
                if (ret < 0) {
                    warnlog(self->log, "Failed to put rule in shortcuts table: {}", str);
                    return true;
                }
                positions = new(std::nothrow) std::vector<uint32_t>;
                if (positions == nullptr) {
                    kh_del(hash_to_indexes, self->shortcuts_table, iter);
                    errlog(self->log, "Failed to allocate memory for shortcuts table");
                    return true;
                }
                // (k + v) * empty buckets coef
                approx_rule_mem = 2 * (sizeof(uint32_t) + sizeof(std::vector<uint32_t>) + positions->capacity() * sizeof(uint32_t));
                kh_value(self->shortcuts_table, iter) = positions;
            } else { // update existing
                positions = kh_value(self->shortcuts_table, iter);
            }
            tracelog(self->log, "Placing a rule in shortcuts table: {} ({})", str, hash);
            approx_rule_mem -= positions->capacity() * sizeof(uint32_t);
            positions->push_back(file_idx);
            approx_rule_mem += positions->capacity() * sizeof(uint32_t);
            approx_rule_mem *= APPROX_FRAGMENTATION_COEF;
            CHECK_MEM();
            goto next_line;
        }
        [[fallthrough]];
    }
    case rule_utils::rule::MMID_REGEX: {
        std::vector<std::string> shortcuts = std::move(rule->matching_parts);
        std::transform(shortcuts.begin(), shortcuts.end(), shortcuts.begin(), ag::utils::to_lower);
        std::optional<ag::regex> re = (rule->match_method == rule_utils::rule::MMID_SHORTCUTS)
                                      ? std::nullopt
                                      : std::make_optional(ag::regex(rule_utils::get_regex(*rule)));
        assert(!shortcuts.empty() || re.has_value());
        approx_rule_mem -= self->leftovers_table.capacity() * sizeof(leftover_entry);
        self->leftovers_table.emplace_back(leftover_entry{ std::move(shortcuts), std::move(re), file_idx });
        approx_rule_mem += self->leftovers_table.capacity() * sizeof(leftover_entry);
        tracelog(self->log, "Rule placed in leftovers table: {}", str);
        for (auto &s : self->leftovers_table.back().shortcuts) {
            approx_rule_mem += s.size();
        }
        if (self->leftovers_table.back().regex) {
            approx_rule_mem += APPROX_COMPILED_REGEX_BYTES;
        }
        approx_rule_mem *= APPROX_FRAGMENTATION_COEF;
        CHECK_MEM();
        goto next_line;
    }
    }

next_line:
    a->approx_mem += approx_rule_mem;
    a->result = LR_OK;
    return true;
}
#undef CHECK_MEM

std::pair<filter::load_result, size_t> filter::load(const ag::dnsfilter::filter_params &p, size_t mem_limit) {
    ag::file::handle fd = ag::file::INVALID_HANDLE;
    std::string logger_name = AG_FMT("{}::", p.id);

    if (!p.in_memory) {
        size_t last_slash = p.data.rfind('/');
        logger_name += (last_slash != p.data.npos) ? &p.data[last_slash + 1] : p.data.c_str();
    } else {
        logger_name += "::in_memory";
    }

    this->pimpl->log = ag::create_logger(logger_name);

    if (!p.in_memory) {
        fd = ag::file::open(p.data, ag::file::RDONLY);
        if (!ag::file::is_valid(fd)) {
            errlog(this->pimpl->log, "filter::load failed to read file: {} ({})",
                   p.data, ag::sys::error_string(ag::sys::error_code()));
            return {LR_ERROR, 0};
        }
    }

    rules_stat stat = {};
    if (ag::file::is_valid(fd)) {
        ag::file::for_each_line(fd, &count_rules, &stat);
    } else {
        ag::utils::for_each_line(p.data, &count_rules, &stat);
    }

    impl *f = this->pimpl.get();
    kh_resize(hash_to_unique_index, f->unique_domains_table, stat.simple_domain_rules);
    kh_resize(hash_to_indexes, f->shortcuts_table, kh_size(f->shortcuts_table));
    f->leftovers_table.reserve(stat.leftover_rules);
    kh_resize(hash_to_unique_index, f->badfilter_table, stat.badfilter_rules);

    filter::impl::load_line_arg load_line_arg{};
    load_line_arg.filter = f;
    load_line_arg.mem_limit = mem_limit;

    int rc;

    if (ag::file::is_valid(fd)) {
        ag::file::set_position(fd, 0);
        rc = ag::file::for_each_line(fd, &filter::impl::load_line, &load_line_arg);
        ag::file::close(fd);
    } else {
        rc = ag::utils::for_each_line(p.data, &filter::impl::load_line, &load_line_arg);
    }

    if (rc == 0) {
        this->params = p;
    }
    params.mtime = ag::file::get_modification_time(p.data.data());
    pimpl->approx_mem = load_line_arg.approx_mem;

    tracelog(pimpl->log, "Last modification time: {}", params.mtime);

    kh_resize(hash_to_unique_index, f->unique_domains_table, kh_size(f->unique_domains_table));
    kh_resize(hash_to_indexes, f->domains_table, kh_size(f->domains_table));
    kh_resize(hash_to_indexes, f->shortcuts_table, kh_size(f->shortcuts_table));
    f->leftovers_table.shrink_to_fit();
    kh_resize(hash_to_unique_index, f->badfilter_table, kh_size(f->badfilter_table));

    infolog(pimpl->log, "Unique domains table size: {}", kh_size(f->unique_domains_table));
    infolog(pimpl->log, "Non-unique domains table size: {}", kh_size(f->domains_table));
    infolog(pimpl->log, "Shortcuts table size: {}", kh_size(f->shortcuts_table));
    infolog(pimpl->log, "Leftovers table size: {}", f->leftovers_table.size());
    infolog(pimpl->log, "Badfilter table size: {}", kh_size(f->badfilter_table));
    infolog(pimpl->log, "Approximate memory usage: {}K", (load_line_arg.approx_mem / 1024) + 1);

    return {load_line_arg.result, load_line_arg.approx_mem};
}

enum adblock_modifiers_match_status {
    /** A rule is not matched because of its modifiers */
    AMMS_NOT_MATCHED,
    /** A domain is matched by rule's modifiers, but it should be checked against rule's pattern as well */
    AMMS_MATCH_CANDIDATE,
    /** A domain is definitely matched by rule's modifiers, no need to check rule's pattern */
    AMMS_MATCHED_SURELY,
};

static adblock_modifiers_match_status match_adblock_modifiers(
        const rule_utils::rule &rule, const filter::match_context &ctx) {
    const auto &info = std::get<ag::dnsfilter::adblock_rule_info>(rule.public_part.content);

    if (info.props.test(ag::dnsfilter::DARP_BADFILTER)) {
        // no need for further checks of $badfilter rules
        return AMMS_MATCHED_SURELY;
    }

    if (info.props.test(ag::dnsfilter::DARP_DNSTYPE)) {
        // match the request by its type against the $dnstype rule
        adblock_modifiers_match_status status;

        switch (const rule_utils::dnstype_info &dnstype = rule.dnstype.value();
                    dnstype.mode) {
        case rule_utils::dnstype_info::DTMM_ENABLE:
            // check if type is enabled by the rule
            status = dnstype.types.end() != std::find(dnstype.types.begin(), dnstype.types.end(), ctx.rr_type)
                    ? AMMS_MATCH_CANDIDATE : AMMS_NOT_MATCHED;
            break;
        case rule_utils::dnstype_info::DTMM_EXCLUDE:
            // check if type is excluded by the rule
            status = dnstype.types.end() == std::find(dnstype.types.begin(), dnstype.types.end(), ctx.rr_type)
                    ? AMMS_MATCH_CANDIDATE : AMMS_NOT_MATCHED;
            break;
        }

        return status;
    } else if (info.props.test(ag::dnsfilter::DARP_DNSREWRITE)) {
        // check if the request's type corresponds to the $dnsrewrite rule's type
        std::optional<rule_utils::dnsrewrite_info> &dnsrewrite = info.params->dnsrewrite;
        if (dnsrewrite.has_value()) {
            if ((dnsrewrite->rrtype == LDNS_RR_TYPE_A && ctx.rr_type != LDNS_RR_TYPE_A)
                    || (dnsrewrite->rrtype == LDNS_RR_TYPE_AAAA && ctx.rr_type != LDNS_RR_TYPE_AAAA)
                    || (dnsrewrite->rrtype == LDNS_RR_TYPE_PTR && ctx.rr_type != LDNS_RR_TYPE_PTR)
                    || (dnsrewrite->rrtype == LDNS_RR_TYPE_CNAME
                            && ctx.rr_type != LDNS_RR_TYPE_A && ctx.rr_type != LDNS_RR_TYPE_AAAA)) {
                return AMMS_NOT_MATCHED;
            }
        }
    }

    return AMMS_MATCH_CANDIDATE;
}

static inline bool match_shortcuts(const std::vector<std::string> &shortcuts, std::string_view domain) {
    size_t seek = 0;
    bool found = false;
    for (const std::string &sc : shortcuts) {
        found = domain.npos != domain.find(sc, seek);
        if (!found) {
            break;
        }
        seek += sc.length();
    }
    return found;
}

static bool match_pattern(const rule_utils::rule &rule,
        std::string_view host, const std::vector<std::string_view> &subdomains) {
    bool matched = false;

    switch (rule.match_method) {
    case rule_utils::rule::MMID_EXACT:
        matched = rule.matching_parts.end() != std::find(rule.matching_parts.begin(), rule.matching_parts.end(), host);
        break;
    case rule_utils::rule::MMID_SUBDOMAINS: {
        for (auto &part : rule.matching_parts) {
            for (auto &subdomain : subdomains) { // assert `subdomains` also contains the full host
                if ((matched = (subdomain == part))) {
                    goto loopexit;
                }
            }
        }
        loopexit:
        break;
    }
    case rule_utils::rule::MMID_SHORTCUTS:
        matched = match_shortcuts(rule.matching_parts, host);
        break;
    case rule_utils::rule::MMID_SHORTCUTS_AND_REGEX:
        assert(!rule.matching_parts.empty());
        if (match_shortcuts(rule.matching_parts, host)) {
            ag::regex re(rule_utils::get_regex(rule));
            matched = re.match(host);
        }
        break;
    case rule_utils::rule::MMID_REGEX: {
        ag::regex re = ag::regex(rule_utils::get_regex(rule));
        matched = subdomains.end() != std::find_if(subdomains.begin(), subdomains.end(),
                [&re] (std::string_view subdomain) { return re.match(subdomain); });
        break;
    }
    }

    return matched;
}

bool filter::impl::match_against_line(match_arg &match, std::string_view line) {
    bool matched = false;
    std::optional<rule_utils::rule> rule = rule_utils::parse(line);

    if (!rule.has_value()) {
        matched = false;
        goto exit;
    }

    if (nullptr != std::get_if<ag::dnsfilter::adblock_rule_info>(&rule->public_part.content)) {
        switch (match_adblock_modifiers(rule.value(), match.ctx)) {
        case AMMS_NOT_MATCHED:
            matched = false;
            goto exit;
        case AMMS_MATCH_CANDIDATE:
            break;
        case AMMS_MATCHED_SURELY:
            matched = true;
            goto exit;
        }
    }

    matched = match_pattern(rule.value(), match.ctx.host, match.ctx.subdomains);

exit:
    if (matched) {
        dbglog(match.f.pimpl->log, "Domain '{}' matched against rule '{}'", match.ctx.host, line);
        match.ctx.matched_rules.emplace_back(std::move(rule->public_part));
    }
    return matched;
}

static inline bool is_unique_rule(const std::vector<ag::dnsfilter::rule> &rules, std::string_view line) {
    return rules.end() == std::find_if(rules.begin(), rules.end(),
        [&line] (const ag::dnsfilter::rule &rule) { return line == rule.text; });
}

void filter::impl::match_by_file_position(match_arg &match, size_t idx) {
    std::optional<std::string> file_line;
    std::string_view line;

    if (!match.f.params.in_memory) {
        if (match.outdated || check_filter_outdated(match.f)) {
            match.outdated = true;
            return;
        }

        if (!ag::file::is_valid(match.file)) {
            match.file = ag::file::open(match.f.params.data, ag::file::RDONLY);
            if (!ag::file::is_valid(match.file)) {
                SPDLOG_ERROR("failed to open file to match a domain: {}", match.f.params.data);
                return;
            }
        }

        file_line = ag::file::read_line(match.file, idx);
        if (!file_line.has_value()) {
            return;
        }

        line = file_line.value();
    } else {
        std::optional<std::string_view> opt_line = ag::utils::read_line(match.f.params.data, idx);

        if (!opt_line.has_value()) {
            return;
        }

        line = opt_line.value();
    }

    if (!is_unique_rule(match.ctx.matched_rules, line)) {
        return;
    }

    match_against_line(match, line);
}

void filter::impl::search_by_domains(match_arg &match) const {
    if (match.outdated) {
        return;
    }
    for (const std::string_view &domain : match.ctx.subdomains) {
        uint32_t hash = ag::utils::hash(domain);
        khiter_t iter = kh_get(hash_to_unique_index, this->unique_domains_table, hash);
        if (iter != kh_end(this->unique_domains_table)) {
            uint32_t position = kh_value(this->unique_domains_table, iter);
            match_by_file_position(match, position);
            continue;
        }

        iter = kh_get(hash_to_indexes, this->domains_table, hash);
        if (iter != kh_end(this->domains_table)) {
            const std::vector<uint32_t> &positions = *kh_value(this->domains_table, iter);
            for (uint32_t p : positions) {
                match_by_file_position(match, p);
            }
        }
    }
}

void filter::impl::search_by_shortcuts(match_arg &match) const {
    if ((match.ctx.host.length() < SHORTCUT_LENGTH) || match.outdated) {
        return;
    }

    for (size_t i = 0; i <= match.ctx.host.length() - SHORTCUT_LENGTH; ++i) {
        size_t hash = ag::utils::hash({ &match.ctx.host[i], SHORTCUT_LENGTH });
        khiter_t iter = kh_get(hash_to_indexes, this->shortcuts_table, hash);
        if (iter != kh_end(this->shortcuts_table)) {
            const std::vector<uint32_t> &positions = *kh_value(this->shortcuts_table, iter);
            for (uint32_t p : positions) {
                match_by_file_position(match, p);
            }
        }
    }
}

void filter::impl::search_in_leftovers(match_arg &match) const {
    if (match.outdated) {
        return;
    }
    for (const leftover_entry &entry : this->leftovers_table) {
        const std::vector<std::string> &shortcuts = entry.shortcuts;
        if (!shortcuts.empty() && !match_shortcuts(shortcuts, match.ctx.host)) {
            continue;
        }

        const std::optional<ag::regex> &re = entry.regex;
        if (!re.has_value() || re->match(match.ctx.host)) {
            match_by_file_position(match, entry.file_idx);
        }
    }
}

void filter::impl::search_badfilter_rules(match_arg &match) const {
    if (match.outdated) {
        return;
    }
    for (const ag::dnsfilter::rule &rule : match.ctx.matched_rules) {
        khiter_t iter = kh_get(hash_to_unique_index, this->badfilter_table, ag::utils::hash(rule.text));
        if (iter != kh_end(this->badfilter_table)) {
            match_by_file_position(match, kh_value(this->badfilter_table, iter));
        }
    }
}

bool filter::match(match_context &ctx) {
    match_arg m = { ctx, *this, ag::file::INVALID_HANDLE, false };

    size_t matched_rule_pos = m.ctx.matched_rules.size();

    this->pimpl->search_by_domains(m);
    this->pimpl->search_by_shortcuts(m);
    this->pimpl->search_in_leftovers(m);
    this->pimpl->search_badfilter_rules(m);

    for (; matched_rule_pos < m.ctx.matched_rules.size(); ++matched_rule_pos) {
        m.ctx.matched_rules[matched_rule_pos].filter_id = this->params.id;
    }

    ag::file::close(m.file);

    return !m.outdated;
}

void filter::update(std::atomic_size_t &mem_limit) {
    infolog(pimpl->log, "Updating {}...", params.data);
    size_t freed_mem = pimpl->approx_mem;
    pimpl.reset();
    mem_limit += freed_mem;
    pimpl = std::make_unique<impl>();
    auto [res, mem] = load(params, mem_limit);
    mem_limit -= mem;
    if (res == LR_ERROR) {
        errlog(pimpl->log, "Filter {} was not updated because of an error", params.id);
    } else if (res == LR_MEM_LIMIT_REACHED) {
        warnlog(pimpl->log, "Filter {} updated partially (reached memory limit)", params.id);
    }
    infolog(pimpl->log, "Update {} successful", params.id);
}

filter::match_context filter::create_match_context(ag::dnsfilter::match_param param) {
    match_context ctx = { ag::utils::to_lower(param.domain), {}, {}, param.rr_type, {} };

    size_t n = std::count(ctx.host.begin(), ctx.host.end(), '.');
    if (n > 0) {
        // all except tld
        --n;
    }

    ctx.subdomains.reserve(n + 1);
    ctx.subdomains.emplace_back(ctx.host);
    for (size_t i = 0; i < n; ++i) {
        std::array<std::string_view, 2> parts = ag::utils::split2_by(ctx.subdomains[i], '.');
        ctx.subdomains.emplace_back(parts[1]);
    }

    static constexpr std::string_view REVERSE_DNS_DOMAIN_SUFFIX =
            rule_utils::REVERSE_DNS_DOMAIN_SUFFIX
                    .substr(0, rule_utils::REVERSE_DNS_DOMAIN_SUFFIX.length() - 1);
    static constexpr std::string_view REVERSE_IPV6_DNS_DOMAIN_SUFFIX =
            rule_utils::REVERSE_IPV6_DNS_DOMAIN_SUFFIX
                    .substr(0, rule_utils::REVERSE_IPV6_DNS_DOMAIN_SUFFIX.length() - 1);
    if (ctx.rr_type == LDNS_RR_TYPE_PTR
            && ctx.host.back() != '.'
            && (ag::utils::ends_with(ctx.host, REVERSE_DNS_DOMAIN_SUFFIX)
                    || ag::utils::ends_with(ctx.host, REVERSE_IPV6_DNS_DOMAIN_SUFFIX))) {
        ctx.reverse_lookup_fqdn = AG_FMT("{}.", ctx.host);
    }

    return ctx;
}
