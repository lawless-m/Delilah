#include "dbisam/storage/dbisam_attach_options.hpp"

#include <stdexcept>

namespace dbisam {

namespace {

// Split on first occurrence of `sep`. If not found, all goes to `before`.
void split_once(const std::string &s, char sep, std::string &before, std::string &after) {
    auto pos = s.find(sep);
    if (pos == std::string::npos) {
        before = s;
        after.clear();
    } else {
        before = s.substr(0, pos);
        after = s.substr(pos + 1);
    }
}

} // namespace

ConnOpts parse_attach_path(const std::string &path) {
    ConnOpts opts;
    // ConnOpts leaves encrypt_password empty by default; for our
    // Exportmaster deployment it's always "elevatesoft" unless overridden
    // by an ATTACH `(ENCRYPT_PASSWORD '...')` clause.
    opts.encrypt_password = "elevatesoft";

    std::string rest = path;
    if (rest.rfind("em://", 0) == 0) {
        rest = rest.substr(5);
    }

    // Split userinfo (if present) from host/catalog.
    std::string userinfo, hostpath;
    auto at = rest.find('@');
    if (at != std::string::npos) {
        userinfo = rest.substr(0, at);
        hostpath = rest.substr(at + 1);
    } else {
        hostpath = rest;
    }
    if (!userinfo.empty()) {
        split_once(userinfo, ':', opts.user, opts.password);
    }

    // Split host[:port] from catalog.
    std::string hostport;
    split_once(hostpath, '/', hostport, opts.catalog);
    if (hostport.empty()) {
        throw std::invalid_argument("dbisam attach path: missing host");
    }
    std::string host_only, port_str;
    split_once(hostport, ':', host_only, port_str);
    opts.host = host_only;
    if (!port_str.empty()) {
        try {
            int p = std::stoi(port_str);
            if (p <= 0 || p > 65535) {
                throw std::invalid_argument("dbisam attach path: port out of range");
            }
            opts.port = static_cast<uint16_t>(p);
        } catch (const std::exception &) {
            throw std::invalid_argument("dbisam attach path: bad port \"" + port_str + "\"");
        }
    }
    if (opts.catalog.empty()) {
        throw std::invalid_argument("dbisam attach path: missing catalog");
    }
    return opts;
}

} // namespace dbisam
