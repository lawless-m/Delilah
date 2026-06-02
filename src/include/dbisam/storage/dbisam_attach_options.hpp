// Parse an ATTACH path into ConnOpts.
//
// Supported forms:
//   em://user:pw@host[:port]/catalog
//   user:pw@host[:port]/catalog
//   host[:port]/catalog        (user/password supplied via attach options)
//
// Additional options come from the ATTACH `(...)` clause:
//   USER 'name', PASSWORD 'pw', ENCRYPT_PASSWORD 'elevatesoft',
//   PORT 12005, COMPRESSION true

#pragma once

#include "dbisam/framing.hpp"  // for ConnOpts deps
#include "dbisam/client.hpp"

#include <stdexcept>
#include <string>

namespace dbisam {

// Parse `path` (the string given to ATTACH) into ConnOpts. Throws
// std::invalid_argument on malformed input. `port` and `encrypt_password`
// default to the same values as ConnOpts unless overridden in `path`.
ConnOpts parse_attach_path(const std::string &path);

} // namespace dbisam
