#pragma once

#include <string>
#include <string_view>

namespace supabase::auth::pkce
{

/// RFC 7636 code verifier: 64 unreserved characters from a random source.
std::string generateVerifier();

/// BASE64URL(SHA256(verifier)) without padding (the `S256` challenge method).
std::string challengeS256(std::string_view verifier);

}
