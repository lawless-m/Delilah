// DBISAM login crypto: Blowfish-CBC with IV=0 and key = MD5(encrypt_password).
//
// Plaintext format:  <u8 ulen> <username> <u8 plen> <password>
// zero-padded up to a multiple of 8 (Blowfish block size).
//
// Recovered from dbsys.exe BPL disassembly + dictionary attack against
// the captured login bytes — see DBISAM-PROTOCOL.md §5.
//
// Port of mrsflow-cli/src/exportmaster/crypto.rs.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dbisam {

std::vector<uint8_t> encrypt_login(const uint8_t *username, size_t ulen,
                                   const uint8_t *password, size_t plen,
                                   const uint8_t *encrypt_password, size_t elen);

} // namespace dbisam
