# Cryptography and TLS

All of Klin's cryptography goes through one small C interface, and only
six of Klin's own files may touch OpenSSL. Read this if you write C against that layer
or want to port it to another library.

## One door to the crypto library

The interface is `klin/crypto.h` (the `kc_` functions); TLS goes
through `klin/tls.h` and the TLS io driver. Only these files call the
crypto library:

| File | What it does |
|---|---|
| `src/libutils/crypto.c` | digests, HMAC, PBKDF2, random bytes, constant-time compare, wiping, ciphers |
| `src/libtls/tls.c` | TLS server contexts |
| `src/libtls/tls_glue.c` | certificates, keys, CA and CRL files |
| `src/libtls/keyproxy.c` | the TLS key manager's provider ([KEYMGR](KEYMGR.md)) |
| `src/libio/iossl.c` | the TLS io driver |
| `klin/tlsprv.h` | their private declarations |

I kept it this narrow so that a second backend, if one is ever needed,
means rewriting six files and not hunting through the tree. The build
checks (test/run_build_checks.py) enforce it: they fail if any other
file under src/, klin/ or webapp/ includes an OpenSSL header. Klin's own
code is also compiled with `OPENSSL_NO_DEPRECATED`
(`OPENSSL_API_COMPAT=30000`), so it uses no API deprecated in OpenSSL
3.0. Site pages are your code and are not held to that.

## The interface (`klin/crypto.h`)

- **Digests.** `kc_digest(KC_MD5 | KC_SHA1 | KC_SHA256 | KC_RIPEMD160, ...)`
  hashes a buffer. `kc_md_from_name("sha256")` turns a configuration value
  into a `KC_*` constant.
- **HMAC.** `kc_hmac()` is one-shot. For a keyed context reused for many
  messages, as client-side sessions do, use `kc_hmac_new`, `init`,
  `update`, `final` and `free`.
- **Passwords, randomness, comparison, wiping.** `kc_pbkdf2_sha256()` makes
  the password hashes for authentication. `kc_random()` is a
  cryptographically secure generator. `kc_memeq()` compares in constant
  time. `kc_cleanse()` wipes memory in a way the compiler cannot optimise
  away.
- **Ciphers.** `kc_cipher_new(KC_AES_256_CBC, encrypt, key, iv)`, then
  `update`, `final` and `free`. The cipher codec (`codec_cipher_create`)
  and `u_cipher_encrypt`/`u_cipher_decrypt` take a `KC_*` cipher.
- **Authenticated encryption.** `kc_aead_seal()` and `kc_aead_open()` do
  AES-256-GCM with additional data (`KC_AEAD_KEY`, `KC_AEAD_NONCE`,
  `KC_AEAD_TAG`). `kc_aead_open()` fails on any change to the key, nonce,
  data or ciphertext. Client-side sessions use it ([SESSIONS](SESSIONS.md)).
- **Sizes.** `KC_MD_MAX`, `KC_KEY_MAX`, `KC_IV_MAX` and `KC_BLOCK_MAX` equal
  OpenSSL's `EVP_MAX_*` values, so key buffers, session cookies and
  encrypted pages keep their formats.

In a build without OpenSSL (`KLIN_WITH_OPENSSL=OFF`) the interface is
still there, but every function that hashes, encrypts or draws random
bytes, `kc_random()` included, returns an error. `kc_memeq()` and
`kc_cleanse()` still work.

For TLS, `tls_load_init_ctx()` returns an opaque `tls_ctx_t` (an
`SSL_CTX` in the OpenSSL backend). Free it with `tls_ctx_free()`, and
hand it to `io_ssl_create()`. `io_ssl_get_SSL()` is declared in
`klin/tlsprv.h` only.

## API changes in 4.0.0

If your code used these directly, four things changed:

- `codec_cipher_create()` and `u_cipher_encrypt/decrypt()` take
  `KC_AES_256_CBC` instead of `EVP_aes_256_cbc()`;
- `tls_load_init_ctx()` returns `tls_ctx_t *`;
- `io_ssl_create()` takes a `tls_ctx_t *`;
- Klin's headers no longer include OpenSSL's. Include them yourself where
  a page uses OpenSSL.

## The backend: OpenSSL only (decision, 20 Nov 2026)

OpenSSL 3 is the only backend. No mbedTLS or wolfSSL backend is to be
added until a real target device cannot take OpenSSL, whether for size,
licence or platform reasons.

If one is needed then, it will be mbedTLS (Apache 2.0 licence), not
wolfSSL (GPL or a commercial licence). It would be written against this
interface: `crypto.c` and the TLS layer above it, with the TLS key manager
either ported to mbedTLS's external-key hooks or turned off.

See also: [TLS](TLS.md) for server settings, [KEYMGR](KEYMGR.md) for the
key manager's provider, and [SESSIONS](SESSIONS.md) for client-side
sessions.
