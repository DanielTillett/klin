# Security headers

klind adds a set of security headers to every response, pages and error
pages alike. Each can be changed or left out per server or virtual host
in a `security_headers` block. The value you give replaces the default,
and `no` leaves the header out:

```conf
security_headers
{
    x_content_type_options     nosniff                # default
    x_frame_options            SAMEORIGIN             # default
    referrer_policy            strict-origin-when-cross-origin  # default
    content_security_policy    default-src 'self'     # default: not sent
    strict_transport_security  max-age=31536000       # default, HTTPS only
}
```

| Header | Default | What it does |
|---|---|---|
| X-Content-Type-Options | `nosniff` | Stops browsers from guessing a content type other than the one sent. |
| X-Frame-Options | `SAMEORIGIN` | Lets only the site's own pages frame it, against clickjacking. `DENY` forbids framing. |
| Referrer-Policy | `strict-origin-when-cross-origin` | Sends other sites only the origin, never the path and query of the page. |
| Content-Security-Policy | not sent | See below. |
| Strict-Transport-Security | `max-age=31536000` | Over HTTPS only. Browsers then use HTTPS for the host for max-age seconds (a year by default). |

Content-Security-Policy has no default. A useful policy depends on the
scripts, styles and frames each site uses, and a wrong one breaks pages.
Start with something like `default-src 'self'; frame-ancestors 'self'`.

For Strict-Transport-Security, add `; includeSubDomains` only if every
subdomain serves HTTPS. Set it to `no` while you are testing a
certificate, because browsers remember it for the whole max-age.

A page can still change a header with `response_set_field()` or remove it
with `response_del_field()`, because the headers are set before the page
runs.

## Upgrading

These headers are new in Klin 4.0.0. A site that relied on being framed by
another origin needs `x_frame_options no`, and a `frame-ancestors` policy
in its Content-Security-Policy.

See also: [TLS](TLS.md) before you turn on a long
Strict-Transport-Security max-age, [HARDENING](HARDENING.md).
