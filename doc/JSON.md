# JSON

`<klin/json.h>` reads and writes JSON (RFC 8259). It lives in libutils,
so pages, webapp code and tools can all use it. Read this page when you
write an API page that takes or returns JSON.

## Reading

`json_parse()` builds a tree that you query with accessors and free with
`json_free()`:

```c
json_t *doc;
char err[128];

if(json_parse(text, len, &doc, err, sizeof err))
    ...                     /* err: what is wrong, and at what offset */
name = json_string(json_get(doc, "name"));      /* NULL if missing */
if(json_int64(json_get(doc, "id"), &id))
    ...                     /* missing, not an integer, out of range */
for(i = 0; i < json_count(list); ++i)
    item = json_at(list, i);
json_free(doc);
```

Every accessor accepts NULL and returns NULL, 0 or false, so you can chain
lookups without checking each step. `json_get()` returns NULL only for a
missing member; use `json_type()` to tell a JSON `null` from a member that
is not there.

Numbers come in two forms. `json_number()` gives any number as a
`double`. `json_int64()` succeeds only for an integer written without a
fraction or exponent that fits `int64_t` exactly, which is what you want
for ids and counts. Strings are NUL-terminated UTF-8, and
`json_string_len()` gives the true length when a string contains `\u0000`.

### The reader is strict

I made the reader strict on purpose, because its input comes from
clients. It refuses comments, trailing commas, single quotes, unquoted
keys, leading zeros, `NaN` and `Infinity`. Strings must be valid UTF-8,
with no raw control characters and no unpaired surrogates. Nesting is
limited to `JSON_MAX_DEPTH` (64).

A key may appear only once in an object. That one is a security rule
rather than pedantry: two parsers that handle a duplicate differently
(first wins in one, last wins in the other) are a known way around
validation, so Klin refuses the document instead of picking one.

### Reading a request body

In a page, `request_get_json(request, max, &doc)` reads and parses the
request body in one call. It returns 0 with the document, or the status
you should answer with:

| Status | When |
| --- | --- |
| 415 | the Content-Type is not `application/json` or a `+json` type (parameters such as `charset` are allowed) |
| 413 | the body is larger than `max` or than `post_maxsize` |
| 400 | the body is not valid JSON, or could not be read |

`request_read_body()` reads any body raw into a NUL-terminated buffer,
under the same two size limits. The body can be read only once: either
function fails if something has already read it.

## Writing

The writer streams straight to an `io_t`, normally the page's `out`:

```c
json_writer_t *w;

response_set_content_type(response, "application/json");
json_writer_create(out, &w);
json_begin_object(w);
json_key(w, "id");      json_int(w, 42);
json_key(w, "name");    json_str(w, name);
json_key(w, "tags");    json_begin_array(w);
json_str(w, "a");       json_str(w, "b");
json_end_array(w);
json_end_object(w);
if(json_writer_free(w))
    ...                     /* the document was incomplete or misused */
```

The writer checks the structure as you go: a key before each object
member, one value at the top level, and everything closed at the end.
`json_writer_free()` reports any mistake, so check its return value.

Strings are escaped so the output is also safe inside an HTML `<script>`
element: `<`, `>`, `&` and U+2028/U+2029 are written as `\u` escapes.
Invalid UTF-8 is written as U+FFFD. NaN and infinities have no JSON form,
so `json_double()` treats them as an error. `json_value()` writes a parsed
document back out, which is handy for echoing part of a request.
