#include <algorithm>
#include <array>
#include <cassert>
#include <ag_logger.h>
#include <ag_utils.h>
#include <ag_regex.h>
#include <ag_socket_address.h>
#include <ag_net_utils.h>
#include "rule_utils.h"


static constexpr int MODIFIERS_MARKER = '$';
static constexpr int MODIFIERS_DELIMITER = ',';
static constexpr std::string_view EXCEPTION_MARKER = "@@";
static constexpr std::string_view SKIPPABLE_PREFIXES[] =
    { "https://", "http://", "http*://", "ws://", "wss://", "ws*://", "://", "//" };
static constexpr std::string_view SPECIAL_SUFFIXES[] =
    { "|", "^", "/" };
static constexpr std::string_view SPECIAL_REGEX_CHARACTERS = "\\^$*+?.()|[]{}";


static const ag::regex SHORTCUT_REGEXES[] =
    {
        // Strip all types of brackets
        ag::regex("([^\\\\]*)\\([^\\\\]*\\)"),
        ag::regex("([^\\\\]*)\\{[^\\\\]*\\}"),
        ag::regex("([^\\\\]*)\\[[^\\\\]*\\]"),
        // Strip some escaped characters
        ag::regex("([^\\\\]*)\\[a-zA-Z]"),
    };

struct supported_modifier_descriptor {
    std::string_view name;
    ag::dnsfilter::adblock_rule_props id;
    /**
     * If non-null, the modifier may have some parameters to parse.
     * E.g., `$dnstype` does it may be `$dnstype=A`,
     * but `$important` does not as it may not be `$important=some`.
     * @param rule the rule
     * @param params_str the parameters string (a slice of `rule`'s text)
     * @param match_info the parsed match info of the rule
     * @return true if successful
     */
    bool (* parse_modifier_params)(rule_utils::rule &rule, std::string_view params_str,
            const rule_utils::match_info &match_info, ag::logger *log) = nullptr;
};

static bool parse_dnstype_modifier(rule_utils::rule &rule, std::string_view params_str,
        const rule_utils::match_info &match_info, ag::logger *log);

static constexpr supported_modifier_descriptor SUPPORTED_MODIFIERS[] = {
    { "important", ag::dnsfilter::DARP_IMPORTANT },
    { "badfilter", ag::dnsfilter::DARP_BADFILTER },
    { "dnstype", ag::dnsfilter::DARP_DNSTYPE, &parse_dnstype_modifier },
    { "dnsrewrite", ag::dnsfilter::DARP_DNSREWRITE, &rule_utils::parse_dnsrewrite_modifier },
};

// RFC1035 $2.3.4 Size limits (https://tools.ietf.org/html/rfc1035#section-2.3.4)
static constexpr size_t MAX_DOMAIN_LENGTH = 255;
// RFC1034 $3.5 Preferred name syntax (https://tools.ietf.org/html/rfc1034#section-3.5)
static constexpr size_t MAX_LABEL_LENGTH = 63;
// INET6_ADDRSTRLEN - 1 (they include the trailing null)
static constexpr size_t MAX_IPADDR_LENTH = 45;


ag::dnsfilter::adblock_rule_info::adblock_rule_info(props_set props)
        : props(props)
{}

ag::dnsfilter::adblock_rule_info::~adblock_rule_info() {
    delete this->params;
}

ag::dnsfilter::adblock_rule_info::adblock_rule_info(adblock_rule_info &&other) {
    *this = std::move(other);
}

ag::dnsfilter::adblock_rule_info &ag::dnsfilter::adblock_rule_info::operator=(adblock_rule_info &&other) {
    this->props = other.props;
    std::swap(this->params, other.params);
    return *this;
}

ag::dnsfilter::adblock_rule_info::adblock_rule_info(const adblock_rule_info &other) {
    *this = other;
}

ag::dnsfilter::adblock_rule_info &ag::dnsfilter::adblock_rule_info::operator=(const adblock_rule_info &other) {
    this->props = other.props;
    this->params = other.params == nullptr ? nullptr : new parameters{ *other.params };
    return *this;
}


static inline bool pattern_exact(int pattern_mode) {
    return pattern_mode == (rule_utils::MPM_LINE_START_ASSERTED | rule_utils::MPM_LINE_END_ASSERTED);
}

static inline bool pattern_subdomains(int pattern_mode) {
    return pattern_mode == (rule_utils::MPM_DOMAIN_START_ASSERTED | rule_utils::MPM_LINE_END_ASSERTED);
}

static inline bool check_domain_pattern_labels(std::string_view domain) {
    std::vector<std::string_view> labels = ag::utils::split_by(domain, '.');
    for (const std::string_view &label : labels) {
        if (label.length() > MAX_LABEL_LENGTH) {
            return false;
        }
    }
    return true;
}

static inline bool check_domain_pattern_charset(std::string_view domain) {
    return domain.cend() == std::find_if(domain.cbegin(), domain.cend(),
        [] (unsigned char c) -> bool {
            // By RFC1034 $3.5 Preferred name syntax (https://tools.ietf.org/html/rfc1034#section-3.5)
            // plus non-standard:
            //  - '*' for light-weight wildcard regexes
            //  - '_' as it is used by someones
            return !(std::isalpha(c) || std::isdigit(c)
                || c == '.' || c == '-' || c == '*' || c == '_');
        });
}

static inline bool is_valid_domain_pattern(std::string_view domain) {
    return domain.length() <= MAX_DOMAIN_LENGTH
        && check_domain_pattern_charset(domain)
        && check_domain_pattern_labels(domain);
}

static inline bool is_valid_ip_pattern(std::string_view str) {
    if (str.empty() || str.length() > MAX_IPADDR_LENTH) {
        return false;
    }
    return str.cend() == std::find_if(str.cbegin(), str.cend(), [](unsigned char c) {
        return !(std::isxdigit(c) || c == '.' || c == ':' || c == '[' || c == ']' || c == '*');
    });
}

static inline bool is_ip(std::string_view str) {
    return ag::utils::str_to_socket_address(str).valid();
}

bool rule_utils::is_domain_name(std::string_view str) {
    return !str.empty() // Duh
           && !is_ip(str)
           && str.back() != '.' // We consider a domain name ending with '.' a pattern
           && str.front() != '.' // Valid pattern, but not a valid domain
           && is_valid_domain_pattern(str) // This is a bit more general than Go dnsproxy's regex, but yolo
           && str.npos == str.find('*'); // '*' is our special char for pattern matching
}

// https://github.com/AdguardTeam/AdguardHome/wiki/Hosts-Blocklists#-etchosts-syntax
static std::optional<rule_utils::rule> parse_host_file_rule(std::string_view str, ag::logger *log) {
    str = ag::utils::rtrim(str.substr(0, str.find('#')));
    std::vector<std::string_view> parts = ag::utils::split_by_any_of(str, " \t");
    if (parts.size() < 2) {
        return std::nullopt;
    }
    if (!ag::utils::is_valid_ip4(parts[0]) && !ag::utils::is_valid_ip6(parts[0])) {
        return std::nullopt;
    }
    rule_utils::rule r = { .public_part = { .content = ag::dnsfilter::etc_hosts_rule_info{} } };
    r.match_method = rule_utils::rule::MMID_SUBDOMAINS;
    r.matching_parts.reserve(parts.size() - 1);
    for (size_t i = 1; i < parts.size(); ++i) {
        const std::string_view &domain = parts[i];
        if (domain.empty()) {
            continue;
        }
        if (!is_valid_domain_pattern(domain) && domain.npos == domain.find('*')) {
            return std::nullopt;
        }
        r.matching_parts.emplace_back(ag::utils::to_lower(domain));
    }

    if (r.matching_parts.empty()) {
        return std::nullopt;
    }

    r.public_part =
            { 0, std::string(str), ag::dnsfilter::etc_hosts_rule_info{ std::string(parts[0]) } };
    return std::make_optional(std::move(r));
}

// https://github.com/AdguardTeam/AdguardHome/wiki/Hosts-Blocklists#dnstype
static bool parse_dnstype_modifier(rule_utils::rule &rule, std::string_view params_str,
        const rule_utils::match_info &, ag::logger *log) {
    if (params_str.empty()
            && !std::get<ag::dnsfilter::adblock_rule_info>(rule.public_part.content)
                    .props.test(ag::dnsfilter::DARP_EXCEPTION)) {
        ru_dbglog(log, "Blocking rule must have some types specified");
        return false;
    }

    std::vector types = ag::utils::split_by(params_str, '|');
    if (types.empty() && !params_str.empty()) {
        ru_dbglog(log, "Malformed modifier parameters: {}", params_str);
        return false;
    }

    using types_list = std::vector<ldns_rr_type>;
    types_list enabled_types;
    enabled_types.reserve(types.size());
    types_list excluded_types;
    excluded_types.reserve(types.size());

    for (std::string_view t : types) {
        bool enabled = t.front() != '~';
        if (!enabled) {
            t.remove_prefix(1);
        }

        ldns_rr_type type = ldns_get_rr_type_by_name(std::string(t).c_str());
        if (type == 0) {
            ru_dbglog(log, "Unexpected DNS type: {}", t);
            return false;
        }

        types_list &list_to_check = enabled ? excluded_types : enabled_types;
        types_list &list_to_insert = enabled ? enabled_types : excluded_types;

        if (list_to_check.end() != std::find(list_to_check.begin(), list_to_check.end(), type)) {
            ru_dbglog(log, "DNS type can't be both enabled and excluded: {}", t);
            return false;
        }

        list_to_insert.emplace_back(type);
        if (list_to_insert.end() != std::unique(list_to_insert.begin(), list_to_insert.end())) {
            ru_dbglog(log, "Duplicated DNS type: {}", t);
            return false;
        }
    }

    rule.dnstype = !enabled_types.empty()
            ? rule_utils::dnstype_info{ std::move(enabled_types),rule_utils::dnstype_info::DTMM_ENABLE }
            : rule_utils::dnstype_info{ std::move(excluded_types), rule_utils::dnstype_info::DTMM_EXCLUDE };

    return true;
}

// https://github.com/AdguardTeam/AdguardHome/wiki/Hosts-Blocklists#rule-modifiers
static inline bool extract_modifiers(rule_utils::rule &rule, std::string_view modifiers_str,
        const rule_utils::match_info &match_info, ag::logger *log) {
    if (modifiers_str.empty()) {
        return true;
    }

    auto &info = std::get<ag::dnsfilter::adblock_rule_info>(rule.public_part.content);
    std::vector<std::string_view> modifiers = ag::utils::split_by(modifiers_str, MODIFIERS_DELIMITER);
    for (const std::string_view &modifier : modifiers) {
        const supported_modifier_descriptor *found = nullptr;
        for (const supported_modifier_descriptor &descr : SUPPORTED_MODIFIERS) {
            if (!ag::utils::starts_with(modifier, descr.name)) {
                continue;
            }
            if (modifier.length() > descr.name.length()) {
                if (modifier[descr.name.length()] != '=') {
                    continue;
                }
                if (descr.parse_modifier_params == nullptr) {
                    ru_dbglog(log, "Modifier can't have parameters: {}", modifier);
                    return false;
                }
            }

            if (descr.parse_modifier_params == nullptr) {
                goto modifier_found;
            }

            if (modifier.length() == descr.name.length() + 1) {
                ru_dbglog(log, "Modifier has empty parameters section: {}", modifier);
                return false;
            }

            if (size_t start_pos = (modifier.length() > descr.name.length()) ? descr.name.length() + 1 : descr.name.length();
                    descr.parse_modifier_params != nullptr
                    && !descr.parse_modifier_params(rule, modifier.substr(start_pos), match_info, log)) {
                return false;
            }

            modifier_found:
            found = &descr;
            break;
        }

        if (found == nullptr) {
            ru_dbglog(log, "Unknown modifier: {}", modifier);
            return false;
        }
        if (info.props.test(found->id)) {
            ru_dbglog(log, "Duplicated modifier: {}", found->name);
            return false;
        }

        info.props.set(found->id);
    }

    return true;
}

static inline bool check_regex(std::string_view str) {
    return str.length() > 1 && str.front() == '/' && str.back() == '/';
}

static int remove_skippable_prefixes(std::string_view &rule) {
    for (std::string_view prefix : SKIPPABLE_PREFIXES) {
        if (ag::utils::starts_with(rule, prefix)) {
            rule.remove_prefix(prefix.length());
            return rule_utils::MPM_DOMAIN_START_ASSERTED;
        }
    }
    return 0;
}

static inline int remove_special_prefixes(std::string_view &rule) {
    if (ag::utils::starts_with(rule, "||")) {
        rule.remove_prefix(2);
        return rule_utils::MPM_DOMAIN_START_ASSERTED;
    }

    if (rule.front() == '|') {
        rule.remove_prefix(1);
        return rule_utils::MPM_LINE_START_ASSERTED;
    }

    return 0;
}

static inline int remove_special_suffixes(std::string_view &rule) {
    int r = 0;

    std::vector<std::string_view> suffixes_to_remove(SPECIAL_SUFFIXES, SPECIAL_SUFFIXES + std::size(SPECIAL_SUFFIXES));
    std::vector<std::string_view>::iterator iter;
    while (suffixes_to_remove.end() != (iter = std::find_if(suffixes_to_remove.begin(), suffixes_to_remove.end(),
            [&rule] (std::string_view suffix) {
                return ag::utils::ends_with(rule, suffix);
            }))) {
        rule.remove_suffix(iter->length());
        r = rule_utils::MPM_LINE_END_ASSERTED;
        suffixes_to_remove.erase(iter);
    }

    return r;
}

static inline bool is_valid_port(std::string_view p) {
    return p.length() <= 5
            && (p.cend() == std::find_if_not(p.cbegin(), p.cend(), [](unsigned char c) { return std::isdigit(c); }));
}

static inline int remove_port(std::string_view &rule) {
    size_t rpos = rule.rfind(':');
    if (rpos == std::string_view::npos) {
        return 0;
    }
    size_t fpos = rule.find(':');
    if (fpos == rpos && (fpos != rule.length() - 1) && is_valid_port(rule.substr(fpos + 1))) {
        rule = rule.substr(0, fpos);
        return rule_utils::MPM_LINE_END_ASSERTED;
    } else if (fpos > 0 && rule[fpos - 1] == ']' && rule[0] == '[') { // IPv6
        rule = rule.substr(1, rpos - 2);
        return rule_utils::MPM_LINE_START_ASSERTED | rule_utils::MPM_LINE_END_ASSERTED;
    }
    return 0;
}

// https://github.com/AdguardTeam/AdguardHome/wiki/Hosts-Blocklists#adblock-style
static rule_utils::match_info extract_match_info(std::string_view rule) {
    rule_utils::match_info info = {.text = rule, .is_regex_rule = check_regex(rule), .has_wildcard = false, .pattern_mode = 0};

    if (info.is_regex_rule) {
        info.text.remove_prefix(1); // begin slash
        info.text.remove_suffix(1); // end slash
        return info;
    }

    // rules with wrong special and skippable prefixes and suffixes will be dropped by
    // domain validity check

    // special prefixes come before skippable ones (e.g. `||http://example.org`)
    // so for the first we should check special ones
    info.pattern_mode |= remove_special_prefixes(info.text);
    info.pattern_mode |= remove_skippable_prefixes(info.text);
    if ((info.pattern_mode & rule_utils::MPM_DOMAIN_START_ASSERTED)
            && (info.pattern_mode & rule_utils::MPM_LINE_START_ASSERTED)) {
        info.pattern_mode ^= rule_utils::MPM_DOMAIN_START_ASSERTED;
    }

    info.pattern_mode |= remove_special_suffixes(info.text);
    info.pattern_mode |= remove_port(info.text);

    info.has_wildcard = info.text.npos != info.text.find('*');

    return info;
}

static inline bool is_host_rule(std::string_view str) {
    std::vector<std::string_view> parts = ag::utils::split_by_any_of(str, " \t");
    return parts.size() > 1
            && (ag::utils::is_valid_ip4(parts[0]) || ag::utils::is_valid_ip6(parts[0]));
}

// https://github.com/AdguardTeam/AdguardHome/wiki/Hosts-Blocklists#domains-only
static inline rule_utils::rule make_exact_domain_name_rule(std::string_view name) {
    rule_utils::rule r = { .public_part = { .content = ag::dnsfilter::adblock_rule_info{} } };
    r.public_part.text = std::string{name};
    r.match_method = rule_utils::rule::MMID_EXACT;
    r.matching_parts = {ag::utils::to_lower(name)};
    return r;
}

static std::string_view skip_special_chars(std::string_view str) {
    if (str.empty()) {
        return str;
    }

    // @todo: handle hex (like \xhh), unicode (like \uhh...), and octal number (like \nnn) sequences
    static constexpr std::string_view SPEC_SEQS[] = {
            // escape sequences
            "\\n", "\\r", "\\t",
            // metacharacters
            "\\d", "\\D", "\\w", "\\W", "\\s", "\\S",
            // position anchors
            "\\b", "\\B", "\\<", "\\>", "\\A", "\\Z",
        };

    std::string_view seq;
    for (std::string_view i : SPEC_SEQS) {
        if (ag::utils::starts_with(str, i)) {
            seq = i;
            break;
        }
    }

    str.remove_prefix(std::max(seq.length(), (size_t)1));
    return str;

}

static std::vector<std::string_view> extract_regex_shortcuts(std::string_view text) {
    std::vector<std::string_view> shortcuts;
    while (!text.empty()) {
        size_t seek = text.find_first_of(SPECIAL_REGEX_CHARACTERS);
        if (seek > 0) {
            shortcuts.emplace_back(text.substr(0, seek));
        }

        std::string_view tail = text.substr(std::min(text.length(), seek));
        text = skip_special_chars(tail);
    }

    return shortcuts;
}

static bool is_too_wide_rule(const ag::dnsfilter::adblock_rule_info &rule_info,
        const rule_utils::match_info &match_info) {
    return !rule_info.props.test(ag::dnsfilter::DARP_DNSTYPE)
            && !rule_info.props.test(ag::dnsfilter::DARP_DNSREWRITE)
            && (match_info.text.length() < 3
                    || match_info.text.find_first_not_of(".*") == match_info.text.npos);
}

static std::optional<rule_utils::rule> parse_adblock_rule(std::string_view str, ag::logger *log) {
    using rule = rule_utils::rule;

    std::string_view orig_str = str;
    bool is_exception = ag::utils::starts_with(str, EXCEPTION_MARKER);
    if (is_exception) {
        str.remove_prefix(EXCEPTION_MARKER.length());
    }

    std::array<std::string_view, 2> parts = { str, {} };
    if (!check_regex(str)) {
        parts = ag::utils::rsplit2_by(str, MODIFIERS_MARKER);
        str = parts[0];
    }

    rule_utils::match_info match_info = extract_match_info(str);
    str = match_info.text;

    if (!match_info.is_regex_rule && !is_valid_domain_pattern(str) && !is_valid_ip_pattern(str)) {
        ru_dbglog(log, "Invalid domain name: {}", str);
        return std::nullopt;
    }

    rule r = { .public_part = { .content = ag::dnsfilter::adblock_rule_info{} } };
    auto &rule_info = std::get<ag::dnsfilter::adblock_rule_info>(r.public_part.content);
    rule_info.props.set(ag::dnsfilter::DARP_EXCEPTION, is_exception);
    rule_info.params = new ag::dnsfilter::adblock_rule_info::parameters{};
    if (!extract_modifiers(r, parts[1], match_info, log)) {
        return std::nullopt;
    }

    if (is_too_wide_rule(rule_info, match_info)) {
        ru_dbglog(log, "Too wide rule: {}", str);
        return std::nullopt;
    }

    r.public_part.text = std::string(orig_str);

    if (rule_info.props.test(ag::dnsfilter::DARP_BADFILTER)) {
        return std::make_optional(std::move(r));
    }

    bool exact_pattern = pattern_exact(match_info.pattern_mode);
    bool subdomains_pattern = pattern_subdomains(match_info.pattern_mode);
    ag::socket_address addr{match_info.text, 0};
    if (!match_info.is_regex_rule && exact_pattern && addr.valid()) { // match_info.text is a valid IP address
        r.match_method = rule::MMID_EXACT;
        r.matching_parts.emplace_back(ag::utils::addr_to_str(addr.addr())); // strip port, compress
    } else if (!match_info.is_regex_rule && !match_info.has_wildcard && (exact_pattern || subdomains_pattern)) {
        r.match_method = exact_pattern ? rule::MMID_EXACT : rule::MMID_SUBDOMAINS;
        r.matching_parts.emplace_back(ag::utils::to_lower(str));
    } else if (!match_info.is_regex_rule && match_info.pattern_mode == 0) {
        r.match_method = rule::MMID_SHORTCUTS;
        std::vector<std::string_view> shortcuts = ag::utils::split_by(str, '*');
        r.matching_parts.reserve(shortcuts.size());
        for (const std::string_view &sc : shortcuts) {
            r.matching_parts.emplace_back(ag::utils::to_lower(sc));
        }
    } else {
        if (str.find('?') != str.npos) {
            r.match_method = rule::MMID_REGEX;
        } else {
            #define SPECIAL_CHAR_PLACEHOLDER "..."
            std::string text(str);
            for (const ag::regex &re : SHORTCUT_REGEXES) {
                if (re.is_valid()) {
                    text = re.replace(text, "$1" SPECIAL_CHAR_PLACEHOLDER);
                }
            }

            std::vector<std::string_view> shortcuts = extract_regex_shortcuts(text);
            if (!shortcuts.empty()) {
                r.match_method = rule::MMID_SHORTCUTS_AND_REGEX;
                r.matching_parts.reserve(shortcuts.size());
                for (const std::string_view &sc : shortcuts) {
                    r.matching_parts.emplace_back(ag::utils::to_lower(sc));
                }
            } else {
                r.match_method = rule::MMID_REGEX;
            }
        }

        std::string re = rule_utils::get_regex(r);
        if (!ag::regex(re).is_valid()) {
            ru_dbglog(log, "Invalid regex: {}", re);
            return std::nullopt;
        }
    }

    return std::make_optional(std::move(r));
}

std::optional<rule_utils::rule> rule_utils::parse(std::string_view str, ag::logger *log) {
    if (is_comment(str)) {
        return std::nullopt;
    }

    str = ag::utils::trim(str);

    if (str.empty()) {
        return std::nullopt;
    }

    if (is_domain_name(str)) {
        return make_exact_domain_name_rule(str);
    }

    if (is_host_rule(str)) {
        return parse_host_file_rule(str, log);
    }

    return parse_adblock_rule(str, log);
}

std::string rule_utils::get_regex(const rule &r) {
    assert(r.match_method == rule::MMID_REGEX || r.match_method == rule::MMID_SHORTCUTS_AND_REGEX);

    std::string_view text = r.public_part.text;
    if (ag::utils::starts_with(text, EXCEPTION_MARKER)) {
        text.remove_prefix(EXCEPTION_MARKER.length());
    }

    if (text.front() != '/' || text.back() != '/') {
        std::array<std::string_view, 2> parts = ag::utils::rsplit2_by(text, MODIFIERS_MARKER);
        text = parts[0];
    }

    match_info info = extract_match_info(text);
    if (info.is_regex_rule) {
        return std::string(info.text);
    }

    bool assert_line_start = info.pattern_mode & MPM_LINE_START_ASSERTED;
    bool assert_domain_start = info.pattern_mode & MPM_DOMAIN_START_ASSERTED;
    bool assert_end = info.pattern_mode & MPM_LINE_END_ASSERTED;

    std::string re = AG_FMT("{}{}{}"
            , assert_line_start ? "^" : (assert_domain_start ? "^(*.)?" : "")
            , info.text
            , assert_end ? "$" : "");
    size_t n = std::count_if(re.begin(), re.end(), [] (int ch) { return ch == '*' || ch == '.'; });
    if (n > 0) {
        std::string tmp;
        tmp.reserve(re.length() + n);
        for (size_t i = 0; i < re.length(); ++i) {
            int ch = re[i];
            switch (ch) {
            case '*':
                tmp.push_back('.');
                break;
            case '.':
                tmp.push_back('\\');
                break;
            }
            tmp.push_back(ch);
        }
        std::swap(tmp, re);
    }

    return re;
}

std::string rule_utils::get_text_without_badfilter(const ag::dnsfilter::rule &r) {
    constexpr std::string_view BADFILTER_MODIFIER = "badfilter";

    std::array<std::string_view, 2> parts = ag::utils::rsplit2_by(r.text, MODIFIERS_MARKER);
    size_t bf_pos = parts[1].find(BADFILTER_MODIFIER.data());
    size_t after_bf_pos = bf_pos + BADFILTER_MODIFIER.length();

    std::string_view prefix = { parts[0].data(), parts[0].length() + 1 + bf_pos };
    std::string_view suffix = { parts[1].data() + after_bf_pos, parts[1].length() - after_bf_pos };
    if (prefix.back() == ','
            || (suffix.length() == 0 && prefix.back() == '$')) {
        prefix.remove_suffix(1);
    } else if (suffix.front() == ',' && prefix.back() == '$') {
        suffix.remove_prefix(1);
    }

    return AG_FMT("{}{}", prefix, suffix);
}
