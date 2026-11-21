# TLS settings

An https server block (klind.conf(5), SSL/TLS) needs cert_file and
key_file; the rest has secure defaults.

## Protocols

TLS 1.2 and 1.3. OpenSSL chooses TLS 1.3 whenever the client has it; to
refuse TLS 1.2 clients:

```
tls_min_version  1.3
```

TLS 1.0 and 1.1, compression and renegotiation are always refused (TLS
1.3 has no renegotiation; a TLS 1.2 client asking for one gets a "no
renegotiation" alert).

## Ciphers

TLS 1.2 uses Mozilla's "intermediate" list, in the server's order:

```
ECDHE-ECDSA-AES128-GCM-SHA256  ECDHE-RSA-AES128-GCM-SHA256
ECDHE-ECDSA-AES256-GCM-SHA384  ECDHE-RSA-AES256-GCM-SHA384
ECDHE-ECDSA-CHACHA20-POLY1305  ECDHE-RSA-CHACHA20-POLY1305
DHE-RSA-AES128-GCM-SHA256      DHE-RSA-AES256-GCM-SHA384
DHE-RSA-CHACHA20-POLY1305
```

that is, forward-secret key exchange and AEAD ciphers only. This is new
in 4.0: a client that has only CBC ciphers or RSA key exchange (very old
browsers and libraries) can no longer connect. To change it:

```
tls_ciphers       <OpenSSL cipher list>      # TLS 1.2
tls_ciphersuites  <OpenSSL suite list>       # TLS 1.3 (default: OpenSSL's)
```

With the key manager ([KEYMGR](KEYMGR.md), the default) RSA key exchange is always
left out, whatever the list.

## Session resumption

Session tickets let a returning client skip the full handshake. Their
keys change every `tls_ticket_key_lifetime` (default 12 hours); a ticket
made with the previous key is still accepted, and the client gets a new
one, so a ticket is good for 12 to 24 hours.

How the keys are made:

- Each HTTPS server draws a random secret when klind starts, before the
  workers fork. Every period the secret is replaced by an HMAC-SHA256 of
  itself, and the old one is erased; the period's keys are derived from
  its secret.
- Every process steps its copy forward on its own, so all have the same
  keys without talking to each other: klind every second, a worker
  whenever it accepts a connection (before reading anything the client
  sent). A worker keeps only its own server's secret; the key managers
  keep none.
- A process whose memory is read gives away the keys of the current and
  previous periods, and of later ones until a restart, but not those of
  earlier periods: tickets issued before then stay closed. A restart draws
  a new secret.

The session's own lifetime is three periods, so the ticket's key is what
ends it. A clock set back keeps the keys a process has (processes that
had stepped further than others then disagree, and their tickets mean a
full handshake, until the clock catches up); one set far ahead is caught
up 65536 periods per step at most, so klind does not stall.

```
tls_ticket_key_lifetime  720     # minutes, or e.g. "90s"
tls_ticket_key_lifetime  0       # no tickets (TLS 1.2 session ids only)
```

## Mutual TLS

To ask clients for a certificate signed by a CA of yours:

```
ca_file      /etc/klin/clients-ca.pem
verify_mode  require             # or optional
verify_depth 1                   # CAs between the client and ca_file
crl_file     /etc/klin/clients.crl   # optional
crl_opts     CHECK_ALL               # optional: the whole chain
```

With 'require' a client without a valid certificate fails the handshake;
'optional' accepts it too (pages decide). ca_file is required for both.

## Not supported

OCSP stapling is not done (browsers do not rely on it; short-lived
certificates are the way to limit a stolen key's life).

## Tested

test/run_server_tests.py: TLS 1.3 chosen; a TLS 1.2 client gets an ECDHE
or DHE AEAD cipher, and one offering only RSA key exchange or CBC ciphers
is refused; a TLS 1.2 renegotiation is refused ("no renegotiation"); on a
server with verify_mode require and a 2-second ticket key, a client
without a certificate is refused and one with a certificate served, its
TLS 1.2 and 1.3 sessions are resumed by any worker, with a ticket of the
previous period too, and not after the key has changed twice.
