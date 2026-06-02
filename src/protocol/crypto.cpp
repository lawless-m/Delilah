#include "dbisam/crypto.hpp"

#include "blowfish.hpp"
#include "md5.hpp"

namespace dbisam {

std::vector<uint8_t> encrypt_login(const uint8_t *username, size_t ulen,
                                   const uint8_t *password, size_t plen,
                                   const uint8_t *encrypt_password, size_t elen) {
    std::vector<uint8_t> pt;
    pt.reserve(2 + ulen + plen + 8);
    pt.push_back(static_cast<uint8_t>(ulen));
    pt.insert(pt.end(), username, username + ulen);
    pt.push_back(static_cast<uint8_t>(plen));
    pt.insert(pt.end(), password, password + plen);
    size_t pad = (8 - (pt.size() % 8)) % 8;
    pt.insert(pt.end(), pad, 0);

    uint8_t key[16];
    dbisam_md5::compute(encrypt_password, elen, key);

    dbisam_blowfish::Context bf{};
    dbisam_blowfish::key_setup(bf, key, sizeof(key));

    // CBC encrypt with IV = 0.
    uint8_t prev[8] = {0};
    for (size_t off = 0; off < pt.size(); off += 8) {
        uint8_t block[8];
        for (int i = 0; i < 8; ++i) {
            block[i] = pt[off + i] ^ prev[i];
        }
        dbisam_blowfish::encrypt_block(bf, block);
        for (int i = 0; i < 8; ++i) {
            pt[off + i] = block[i];
            prev[i] = block[i];
        }
    }
    return pt;
}

} // namespace dbisam
