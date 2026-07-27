/*
 * S5LBox -- a bounded, read-only scanner over Apple XML property lists.
 *
 * Two documents decide what an IPSW contains: Restore.plist names the
 * kernelcache, the board and the restore images, and the resource fork in a
 * UDIF trailer names the partitions inside the root filesystem. Both arrive
 * from a file the user downloaded, so both are untrusted input in the only
 * sense that matters here -- nobody checked them.
 *
 * The shape of the answer is a scanner, not a parser. Nothing is allocated and
 * no tree is built: a lookup walks the caller's bytes, descends into <dict> by
 * matching <key> and into <array> by counting, and copies out one scalar or
 * hands back one span. That costs a rescan per path component, which for a
 * 1.8 KB manifest and a few hundred KB of resource fork is nothing, and it buys
 * three properties a tree cannot have on a phone: no allocation to fail
 * halfway, no state to leak when a caller bails, and a bounded cost for a
 * document that nests a million deep.
 *
 * Every scan is bounded by pl->len and container nesting is bounded by
 * VMFW_PLIST_MAX_DEPTH, enforced with an explicit stack rather than recursion,
 * so a hostile document costs a named refusal and never a read past the end or
 * a blown C stack.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "VMFirmwareFormats.h"

#include <string.h>

/* ------------------------------------------------------------------------ */
/* Bytes                                                                     */
/* ------------------------------------------------------------------------ */

/* XML's S production, and exactly the set Apple's writer uses to wrap a base64
 * <data> body. Nothing outside it may ever be skipped silently: a decoder that
 * ignored an unexpected byte would turn a corrupted blob into a shorter,
 * plausible-looking one instead of an error. */
static bool is_ws(uint8_t c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* Deliberately generous: any byte >= 0x80 is accepted so a UTF-8 element name
 * is scanned as one token rather than splitting the tag in two. We only ever
 * compare names against ASCII literals, so accepting more here cannot make a
 * wrong name match -- it only keeps the tokeniser from losing its place. */
static bool is_name_char(uint8_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9')
        || c == '_' || c == '-' || c == ':' || c == '.' || c >= 0x80;
}

static bool find_seq(const uint8_t *p, size_t len, size_t from,
                     const char *needle, size_t nlen, size_t *out) {
    if (nlen == 0 || nlen > len) return false;
    for (size_t i = from; i + nlen <= len; i++) {
        if (p[i] == (uint8_t)needle[0] && memcmp(p + i, needle, nlen) == 0) {
            *out = i;
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------------ */
/* Tokeniser                                                                 */
/* ------------------------------------------------------------------------ */

typedef enum { TOK_NONE = 0, TOK_OPEN, TOK_CLOSE } tok_kind_t;

typedef struct {
    size_t start;         /* offset of '<'                                   */
    size_t end;           /* first byte after '>'                            */
    size_t name_off;
    size_t name_len;
    bool   self_closing;  /* <foo/>                                          */
} plist_tag_t;

/*
 * Produce the next element tag at or after `pos`, skipping character data, XML
 * comments, the <?xml?> declaration and the <!DOCTYPE> declaration.
 *
 * Skipping comments here rather than searching the raw bytes is what keeps a
 * document like "<!-- <plist> -->" from being mistaken for a property list, and
 * what keeps a "</dict>" written inside a comment from unbalancing a walk.
 */
static vmfw_plist_status_t next_tag(const vmfw_plist_t *pl, size_t pos,
                                    plist_tag_t *t, tok_kind_t *kind) {
    const uint8_t *x = pl->xml;
    const size_t   n = pl->len;

    memset(t, 0, sizeof *t);
    *kind = TOK_NONE;

    for (;;) {
        /* Character data is not tokenised. In well-formed XML a literal '<' in
         * content must be written "&lt;", so a '<' can only begin markup. */
        while (pos < n && x[pos] != '<') pos++;
        if (pos >= n) return VMFW_PLIST_OK;      /* end of input, no tag */

        const size_t i = pos;
        /* A '<' as the last byte of the document is a tag that was cut off. */
        if (i + 1 >= n) return VMFW_PLIST_ERR_MALFORMED;

        if (x[i + 1] == '!') {
            if (i + 4 <= n && memcmp(x + i, "<!--", 4) == 0) {
                size_t e;
                if (!find_seq(x, n, i + 4, "-->", 3, &e))
                    return VMFW_PLIST_ERR_MALFORMED;
                pos = e + 3;
                continue;
            }
            /*
             * CDATA is refused rather than skipped. Skipping it would drop the
             * section's text while the <string> reader, which does not
             * tokenise, would copy the "<![CDATA[" markup out as if it were the
             * value -- two readers disagreeing about the same bytes. No Apple
             * plist writer emits CDATA, so refusing costs nothing real and
             * keeps us from shipping a half-implemented feature on the
             * untrusted-input path.
             */
            if (i + 9 <= n && memcmp(x + i, "<![CDATA[", 9) == 0)
                return VMFW_PLIST_ERR_MALFORMED;

            /*
             * <!DOCTYPE ...>. The terminating '>' is found with quoting
             * respected, because the public identifier is a quoted string and a
             * naive search would stop inside it on any DTD that contained '>'.
             * An internal subset "[ ... ]" is stepped over for the same reason.
             */
            size_t j = i + 2;
            unsigned subset = 0;
            uint8_t quote = 0;
            bool done = false;
            while (j < n) {
                const uint8_t c = x[j];
                if (quote) {
                    if (c == quote) quote = 0;
                } else if (c == '"' || c == '\'') {
                    quote = c;
                } else if (c == '[') {
                    subset++;
                } else if (c == ']') {
                    if (subset) subset--;
                } else if (c == '>' && subset == 0) {
                    pos = j + 1;
                    done = true;
                    break;
                }
                j++;
            }
            if (!done) return VMFW_PLIST_ERR_MALFORMED;
            continue;
        }

        if (x[i + 1] == '?') {                   /* <?xml ... ?> */
            size_t e;
            if (!find_seq(x, n, i + 2, "?>", 2, &e))
                return VMFW_PLIST_ERR_MALFORMED;
            pos = e + 2;
            continue;
        }

        const bool closing = (x[i + 1] == '/');
        const size_t name_off = i + (closing ? 2u : 1u);
        size_t j = name_off;
        while (j < n && is_name_char(x[j])) j++;
        /* "<>", "< foo>" and a "</" at the very end all land here. */
        if (j == name_off) return VMFW_PLIST_ERR_MALFORMED;

        /* Attribute values are quoted and may legally contain '>'. */
        size_t k = j;
        uint8_t quote = 0;
        bool closed = false;
        while (k < n) {
            const uint8_t c = x[k];
            if (quote) {
                if (c == quote) quote = 0;
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '>') {
                closed = true;
                break;
            }
            k++;
        }
        /* An unterminated tag is an error, never "the rest of the file". */
        if (!closed) return VMFW_PLIST_ERR_MALFORMED;

        const bool self_closing = (k > j && x[k - 1] == '/');
        if (closing) {
            /* An end tag is a name and nothing else. Because the '/' of a
             * "</foo/>" is one of the bytes this looks at, the same rule also
             * makes it impossible for a tag to be an end tag and a
             * self-closing tag at once -- which would otherwise let one tag
             * both open and close a level. */
            for (size_t q = j; q < k; q++)
                if (!is_ws(x[q])) return VMFW_PLIST_ERR_MALFORMED;
        }

        t->start        = i;
        t->end          = k + 1;
        t->name_off     = name_off;
        t->name_len     = j - name_off;
        t->self_closing = self_closing;
        *kind = closing ? TOK_CLOSE : TOK_OPEN;
        return VMFW_PLIST_OK;
    }
}

/* ------------------------------------------------------------------------ */
/* Elements                                                                  */
/* ------------------------------------------------------------------------ */

typedef struct {
    size_t start;        /* offset of '<' of the start tag                   */
    size_t name_off;
    size_t name_len;
    size_t content_off;  /* first byte after '>' of the start tag            */
    size_t content_end;  /* offset of '<' of the matching end tag            */
    size_t end;          /* first byte after '>' of the end tag              */
    bool   self_closing;
} plist_elem_t;

/*
 * Read one complete element at or after `pos`, at document level `depth`.
 *
 * `*kind` reports what was found: TOK_OPEN and `*e` filled, TOK_CLOSE (the
 * enclosing container ended) or TOK_NONE (input exhausted). The caller decides
 * whether either of those is an error, because "no more children" and "no such
 * value" are the same token in different places.
 */
static vmfw_plist_status_t read_element(const vmfw_plist_t *pl, size_t pos,
                                        unsigned depth, plist_elem_t *e,
                                        tok_kind_t *kind) {
    plist_tag_t t;
    memset(e, 0, sizeof *e);

    vmfw_plist_status_t st = next_tag(pl, pos, &t, kind);
    if (st != VMFW_PLIST_OK || *kind != TOK_OPEN) return st;

    e->start        = t.start;
    e->name_off     = t.name_off;
    e->name_len     = t.name_len;
    e->content_off  = t.end;
    e->self_closing = t.self_closing;

    /* A self-closing container has no content and does not open a level. This
     * is why "<dict/>" and "<array/>" cannot be descended into and cannot
     * unbalance the walk that steps over them. */
    if (t.self_closing) {
        e->content_end = t.end;
        e->end         = t.end;
        return VMFW_PLIST_OK;
    }

    /*
     * Matching is done with an explicit stack of names, iteratively.
     *
     * Iteratively, because recursion here would let a document choose how much
     * C stack to consume. With names, because a plain depth counter would
     * happily accept "<dict><array></dict></array>" -- and a walker that
     * believes a mis-nested document is balanced will hand a caller a value
     * from the wrong container.
     *
     * The stack is exactly VMFW_PLIST_MAX_DEPTH entries: the element itself
     * sits at level `depth` >= 1, so no legal document can need more.
     */
    struct { size_t off, len; } stk[VMFW_PLIST_MAX_DEPTH];
    size_t used = 0;
    stk[used].off = t.name_off;
    stk[used].len = t.name_len;
    used++;

    size_t cur = t.end;
    for (;;) {
        plist_tag_t u;
        tok_kind_t  uk;
        st = next_tag(pl, cur, &u, &uk);
        if (st != VMFW_PLIST_OK) return st;

        /* Input ran out with containers still open. Reporting this rather than
         * returning what we have is the difference between a truncated IPSW
         * being refused and being restored. */
        if (uk == TOK_NONE) return VMFW_PLIST_ERR_MALFORMED;

        if (uk == TOK_OPEN) {
            if (u.self_closing) { cur = u.end; continue; }
            /*
             * Two bounds. `used` is what keeps the write below inside stk[];
             * the absolute term is what makes the cap mean "document level"
             * rather than "levels below wherever this call started". Every
             * caller today reaches the document through the <plist> element at
             * depth 1, where the two fire on exactly the same input, so no test
             * can tell them apart and breaking either one alone is still safe.
             * Both are kept anyway: dropping the first would let stk[] be
             * overrun the moment the second is wrong, and dropping the second
             * would silently turn the cap into a relative one the first time
             * read_element is entered somewhere other than the root.
             */
            if (used >= VMFW_PLIST_MAX_DEPTH ||
                (size_t)depth + used > (size_t)VMFW_PLIST_MAX_DEPTH)
                return VMFW_PLIST_ERR_TOO_DEEP;
            stk[used].off = u.name_off;
            stk[used].len = u.name_len;
            used++;
            cur = u.end;
            continue;
        }

        /* TOK_CLOSE */
        if (used == 0) return VMFW_PLIST_ERR_MALFORMED;
        if (u.name_len != stk[used - 1].len ||
            memcmp(pl->xml + u.name_off, pl->xml + stk[used - 1].off,
                   u.name_len) != 0)
            return VMFW_PLIST_ERR_MALFORMED;
        used--;
        if (used == 0) {
            e->content_end = u.start;
            e->end         = u.end;
            return VMFW_PLIST_OK;
        }
        cur = u.end;
    }
}

/*
 * A view over one container's content. Scanning a child through this view
 * cannot run past the container's own end tag, because the view simply does not
 * contain it -- which is cheaper and harder to get wrong than threading a limit
 * through every scan.
 */
static vmfw_plist_t inner_view(const vmfw_plist_t *pl, const plist_elem_t *e) {
    vmfw_plist_t v;
    v.xml = pl->xml;
    v.len = e->content_end;
    return v;
}

static bool name_is(const vmfw_plist_t *pl, const plist_elem_t *e,
                    const char *s) {
    const size_t n = strlen(s);
    return e->name_len == n && memcmp(pl->xml + e->name_off, s, n) == 0;
}

/* ------------------------------------------------------------------------ */
/* Character data                                                            */
/* ------------------------------------------------------------------------ */

static const struct { const char *tail; size_t len; char ch; } k_entities[] = {
    { "amp;",  4, '&'  },
    { "lt;",   3, '<'  },
    { "gt;",   3, '>'  },
    { "quot;", 5, '"'  },
    { "apos;", 5, '\'' },
};

typedef struct {
    const uint8_t *xml;
    size_t pos;
    size_t end;
} text_iter_t;

/* Yield the next decoded byte, or -1 once the span is exhausted. */
static vmfw_plist_status_t text_next(text_iter_t *it, int *out_byte) {
    *out_byte = -1;
    if (it->pos >= it->end) return VMFW_PLIST_OK;

    const uint8_t c = it->xml[it->pos];

    /* Markup where character data should be means the element has children -- a
     * <string> containing an element is not a string, and copying the markup
     * out as text would hand a caller a filename Apple never wrote. */
    if (c == '<') return VMFW_PLIST_ERR_MALFORMED;

    if (c != '&') {
        it->pos++;
        *out_byte = c;
        return VMFW_PLIST_OK;
    }

    /*
     * The five predefined entities and nothing else.
     *
     * A numeric reference ("&#0;") or a DTD-defined one is refused rather than
     * passed through raw. Passing it through would put a literal '&' and a
     * name into a string a caller is about to use as an archive member name;
     * resolving "&#0;" would put a NUL in the middle of it. Refusing is the
     * only answer that is both honest and safe, and no plist this project reads
     * contains anything but these five.
     */
    const size_t rest = it->end - it->pos - 1u;
    for (size_t k = 0; k < sizeof k_entities / sizeof k_entities[0]; k++) {
        if (k_entities[k].len <= rest &&
            memcmp(it->xml + it->pos + 1, k_entities[k].tail,
                   k_entities[k].len) == 0) {
            it->pos += 1u + k_entities[k].len;
            *out_byte = (uint8_t)k_entities[k].ch;
            return VMFW_PLIST_OK;
        }
    }
    return VMFW_PLIST_ERR_MALFORMED;
}

/*
 * Copy an element's text into `out`, NUL-terminated.
 *
 * XML line-ending normalisation (CRLF -> LF) is not performed. Neither plist
 * this project reads contains a CR, and a normaliser would be untested code
 * whose only effect on real input is none.
 */
static vmfw_plist_status_t text_copy(const vmfw_plist_t *pl,
                                     const plist_elem_t *e,
                                     char *out, size_t cap) {
    text_iter_t it = { pl->xml, e->content_off, e->content_end };
    size_t n = 0;

    for (;;) {
        int b;
        const vmfw_plist_status_t st = text_next(&it, &b);
        if (st != VMFW_PLIST_OK) return st;
        if (b < 0) break;
        /* Refused, never truncated. A caller handed half a member name would
         * go looking for a member that does not exist and blame the archive. */
        if (n + 1u >= cap) return VMFW_PLIST_ERR_TOO_LONG;
        out[n++] = (char)b;
    }
    if (n >= cap) return VMFW_PLIST_ERR_TOO_LONG;   /* cap == 0 */
    out[n] = '\0';
    return VMFW_PLIST_OK;
}

/*
 * Compare an element's text with `s` without a buffer, so that an arbitrarily
 * long <key> costs nothing to reject.
 *
 * Decoding continues past the first difference on purpose: whether a document
 * is refused for a bad entity must not depend on which key the caller happened
 * to ask for.
 */
static vmfw_plist_status_t text_equals(const vmfw_plist_t *pl,
                                       const plist_elem_t *e,
                                       const char *s, size_t slen, bool *eq) {
    text_iter_t it = { pl->xml, e->content_off, e->content_end };
    size_t n = 0;
    bool differs = false;

    *eq = false;
    for (;;) {
        int b;
        const vmfw_plist_status_t st = text_next(&it, &b);
        if (st != VMFW_PLIST_OK) return st;
        if (b < 0) break;
        if (!differs) {
            if (n >= slen || (uint8_t)s[n] != (uint8_t)b) differs = true;
            else n++;
        }
    }
    *eq = (!differs && n == slen);
    return VMFW_PLIST_OK;
}

/* ------------------------------------------------------------------------ */
/* Path resolution                                                           */
/* ------------------------------------------------------------------------ */

/*
 * A component that is all decimal digits indexes an <array>; anything else is a
 * <dict> key. That is a rule about the path, not about the document: no key in
 * either plist this project reads is a decimal number, and a syntax for saying
 * "the key literally named 0" would be untested code for a case that does not
 * occur.
 *
 * An index too large for size_t saturates instead of wrapping, so it resolves
 * to VMFW_PLIST_ERR_NOT_FOUND rather than to element 7.
 */
static bool comp_is_index(const char *p, size_t len, size_t *out_idx) {
    if (len == 0) return false;
    size_t v = 0;
    bool saturated = false;
    for (size_t i = 0; i < len; i++) {
        if (p[i] < '0' || p[i] > '9') return false;
        const size_t d = (size_t)(p[i] - '0');
        if (v > (SIZE_MAX - d) / 10u) saturated = true;
        else v = v * 10u + d;
    }
    *out_idx = saturated ? SIZE_MAX : v;
    return true;
}

static vmfw_plist_status_t resolve(const vmfw_plist_t *pl, const char *path,
                                   plist_elem_t *out, unsigned *out_depth) {
    if (!pl || !pl->xml || !path) return VMFW_PLIST_ERR_INVALID_ARGUMENT;

    vmfw_plist_status_t st;
    tok_kind_t kind;

    /*
     * Read the whole <plist> element before anything else. That is what turns a
     * stray "</dict>" written after the root object into a refusal instead of a
     * silently ignored suffix: the stray end tag is inside <plist>, so the
     * balance check in read_element sees it.
     */
    plist_elem_t wrap;
    size_t scan = 0;
    for (;;) {
        st = read_element(pl, scan, 1u, &wrap, &kind);
        if (st != VMFW_PLIST_OK) return st;
        if (kind == TOK_NONE)  return VMFW_PLIST_ERR_NOT_XML;
        if (kind == TOK_CLOSE) return VMFW_PLIST_ERR_MALFORMED;
        if (name_is(pl, &wrap, "plist")) break;
        scan = wrap.end;
    }

    /* The root object is the first element inside <plist>. "<plist/>" has none,
     * so every path names nothing. */
    const vmfw_plist_t body = inner_view(pl, &wrap);
    plist_elem_t cur;
    st = read_element(&body, wrap.content_off, 2u, &cur, &kind);
    if (st != VMFW_PLIST_OK) return st;
    if (kind != TOK_OPEN) return VMFW_PLIST_ERR_NOT_FOUND;
    unsigned depth = 2u;

    const char *p = path;
    while (*p) {
        const char *slash = strchr(p, '/');
        const size_t clen = slash ? (size_t)(slash - p) : strlen(p);
        /* "", "a//b" and "a/" are all a caller bug, not a missing key. */
        if (clen == 0 || (slash && slash[1] == '\0'))
            return VMFW_PLIST_ERR_INVALID_ARGUMENT;

        const vmfw_plist_t sub = inner_view(pl, &cur);
        size_t idx = 0;
        plist_elem_t found;
        memset(&found, 0, sizeof found);
        bool got = false;

        if (comp_is_index(p, clen, &idx)) {
            /* Indexing anything but an array names nothing. */
            if (!name_is(pl, &cur, "array") || cur.self_closing)
                return VMFW_PLIST_ERR_NOT_FOUND;
            size_t at = 0;
            size_t cpos = cur.content_off;
            for (;;) {
                plist_elem_t child;
                st = read_element(&sub, cpos, depth + 1u, &child, &kind);
                if (st != VMFW_PLIST_OK) return st;
                if (kind == TOK_NONE)  break;
                if (kind == TOK_CLOSE) return VMFW_PLIST_ERR_MALFORMED;
                if (at == idx) { found = child; got = true; break; }
                at++;
                cpos = child.end;
            }
        } else {
            if (!name_is(pl, &cur, "dict") || cur.self_closing)
                return VMFW_PLIST_ERR_NOT_FOUND;
            size_t cpos = cur.content_off;
            for (;;) {
                plist_elem_t key_el, val_el;
                st = read_element(&sub, cpos, depth + 1u, &key_el, &kind);
                if (st != VMFW_PLIST_OK) return st;
                if (kind == TOK_NONE)  break;
                if (kind == TOK_CLOSE) return VMFW_PLIST_ERR_MALFORMED;

                /* A dict is strictly <key> then value, repeating. A bare value
                 * with no key, or two keys in a row, is a dictionary whose
                 * entries do not pair up; guessing which value belongs to which
                 * key is exactly the kind of repair that turns a corrupt
                 * manifest into a confidently wrong one. */
                if (!name_is(pl, &key_el, "key"))
                    return VMFW_PLIST_ERR_MALFORMED;

                st = read_element(&sub, key_el.end, depth + 1u, &val_el, &kind);
                if (st != VMFW_PLIST_OK) return st;
                if (kind != TOK_OPEN) return VMFW_PLIST_ERR_MALFORMED;
                if (name_is(pl, &val_el, "key"))
                    return VMFW_PLIST_ERR_MALFORMED;

                bool eq = false;
                st = text_equals(pl, &key_el, p, clen, &eq);
                if (st != VMFW_PLIST_OK) return st;
                if (eq) { found = val_el; got = true; break; }
                cpos = val_el.end;
            }
        }

        if (!got) return VMFW_PLIST_ERR_NOT_FOUND;
        cur = found;
        depth++;

        /* A component only ever matches at the current level; the search never
         * descends on its own, which is why "Platform" does not resolve through
         * DeviceMap's nested dictionaries. */
        if (slash) p = slash + 1;
        else       p += clen;
    }

    *out = cur;
    if (out_depth) *out_depth = depth;
    return VMFW_PLIST_OK;
}

/* ------------------------------------------------------------------------ */
/* Public interface                                                          */
/* ------------------------------------------------------------------------ */

vmfw_plist_status_t vmfw_plist_init(vmfw_plist_t *pl,
                                    const uint8_t *xml, size_t len) {
    if (!pl) return VMFW_PLIST_ERR_INVALID_ARGUMENT;
    pl->xml = NULL;
    pl->len = 0;
    if (!xml) return VMFW_PLIST_ERR_INVALID_ARGUMENT;

    /*
     * Cheap on purpose: tokenise past the declaration, the DOCTYPE and any
     * comments, and require an opening <plist> tag. Structure is validated on
     * the first lookup instead, so a caller that only wanted to know "is this a
     * plist at all?" does not pay for a walk of a 200 KB resource fork.
     */
    const vmfw_plist_t probe = { xml, len };
    size_t pos = 0;
    for (;;) {
        plist_tag_t t;
        tok_kind_t kind;
        const vmfw_plist_status_t st = next_tag(&probe, pos, &t, &kind);
        if (st != VMFW_PLIST_OK) return st;
        if (kind == TOK_NONE) return VMFW_PLIST_ERR_NOT_XML;
        if (kind == TOK_OPEN && t.name_len == 5 &&
            memcmp(xml + t.name_off, "plist", 5) == 0)
            break;
        pos = t.end;
    }

    pl->xml = xml;
    pl->len = len;
    return VMFW_PLIST_OK;
}

vmfw_plist_status_t vmfw_plist_get_string(const vmfw_plist_t *pl,
                                          const char *path,
                                          char *out, size_t cap) {
    if (!out || cap == 0) return VMFW_PLIST_ERR_INVALID_ARGUMENT;
    out[0] = '\0';

    plist_elem_t e;
    vmfw_plist_status_t st = resolve(pl, path, &e, NULL);
    if (st != VMFW_PLIST_OK) return st;

    /* <integer>, <true/> and <data> are all findable and none of them is a
     * string; saying so is more useful than an empty result. */
    if (!name_is(pl, &e, "string")) return VMFW_PLIST_ERR_WRONG_TYPE;

    st = text_copy(pl, &e, out, cap);
    /* Leave nothing half-written behind a refusal. */
    if (st != VMFW_PLIST_OK) out[0] = '\0';
    return st;
}

vmfw_plist_status_t vmfw_plist_array_count(const vmfw_plist_t *pl,
                                           const char *path, size_t *out) {
    if (!out) return VMFW_PLIST_ERR_INVALID_ARGUMENT;
    *out = 0;

    plist_elem_t e;
    unsigned depth = 0;
    const vmfw_plist_status_t st = resolve(pl, path, &e, &depth);
    if (st != VMFW_PLIST_OK) return st;
    if (!name_is(pl, &e, "array")) return VMFW_PLIST_ERR_WRONG_TYPE;
    if (e.self_closing) return VMFW_PLIST_OK;    /* <array/> holds nothing */

    const vmfw_plist_t sub = inner_view(pl, &e);
    size_t n = 0;
    size_t pos = e.content_off;
    for (;;) {
        plist_elem_t child;
        tok_kind_t kind;
        const vmfw_plist_status_t cst =
            read_element(&sub, pos, depth + 1u, &child, &kind);
        if (cst != VMFW_PLIST_OK) return cst;
        if (kind == TOK_NONE)  break;
        if (kind == TOK_CLOSE) return VMFW_PLIST_ERR_MALFORMED;
        n++;
        pos = child.end;
    }

    *out = n;
    return VMFW_PLIST_OK;
}

vmfw_plist_status_t vmfw_plist_get_data_span(const vmfw_plist_t *pl,
                                             const char *path,
                                             const uint8_t **out_b64,
                                             size_t *out_b64_len) {
    if (!out_b64 || !out_b64_len) return VMFW_PLIST_ERR_INVALID_ARGUMENT;
    *out_b64 = NULL;
    *out_b64_len = 0;

    plist_elem_t e;
    const vmfw_plist_status_t st = resolve(pl, path, &e, NULL);
    if (st != VMFW_PLIST_OK) return st;
    if (!name_is(pl, &e, "data")) return VMFW_PLIST_ERR_WRONG_TYPE;

    /* Apple's writer puts the base64 on its own indented lines, so the raw
     * content begins and ends with whitespace. Trimming it makes the reported
     * length the length of the encoded text, which is what a caller sizing a
     * buffer or logging a span actually wants; the interior line breaks stay,
     * and vmfw_base64_decode skips them. */
    size_t s = e.content_off;
    size_t t = e.content_end;
    while (s < t && is_ws(pl->xml[s])) s++;
    while (t > s && is_ws(pl->xml[t - 1])) t--;

    *out_b64 = pl->xml + s;
    *out_b64_len = t - s;
    return VMFW_PLIST_OK;
}

/* ------------------------------------------------------------------------ */
/* base64                                                                    */
/* ------------------------------------------------------------------------ */

static int b64_val(uint8_t c) {
    if (c >= 'A' && c <= 'Z') return (int)(c - 'A');
    if (c >= 'a' && c <= 'z') return (int)(c - 'a') + 26;
    if (c >= '0' && c <= '9') return (int)(c - '0') + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static vmfw_plist_status_t b64_emit(const uint8_t g[4], unsigned pad,
                                    uint8_t *out, size_t cap, size_t *n) {
    uint32_t v = 0;
    for (unsigned i = 0; i < 4u; i++) {
        const int d = (g[i] == '=') ? 0 : b64_val(g[i]);
        v = (v << 6) | (uint32_t)d;
    }

    /*
     * The bits the encoder had nothing to put in must be zero.
     *
     * With one '=' the last character carries 2 spare bits; with two it carries
     * 4. Every correct encoder writes them as zero, and all 76 <data> blocks in
     * the 7E18 BuildManifesto.plist re-encode byte-for-byte, so the writer that
     * produced them does too. A non-zero tail therefore has no source we have
     * seen except corruption or a hand edit -- and there two decoders would
     * disagree about the bytes, because "Zg==" and "Zh==" decode to the same
     * byte under a lenient one. Refusing keeps a blob's encoding one-to-one
     * with its decoding.
     */
    if (pad == 1u && (v & 0xFFu) != 0u)    return VMFW_PLIST_ERR_BAD_BASE64;
    if (pad == 2u && (v & 0xFFFFu) != 0u)  return VMFW_PLIST_ERR_BAD_BASE64;

    const uint8_t b[3] = { (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    const size_t want = 3u - pad;
    if (*n + want > cap) return VMFW_PLIST_ERR_TOO_LONG;
    memcpy(out + *n, b, want);
    *n += want;
    return VMFW_PLIST_OK;
}

vmfw_plist_status_t vmfw_base64_decode(const uint8_t *in, size_t in_len,
                                       uint8_t *out, size_t cap,
                                       size_t *out_len) {
    if (out_len) *out_len = 0;
    if (!in && in_len) return VMFW_PLIST_ERR_INVALID_ARGUMENT;
    if (!out && cap)   return VMFW_PLIST_ERR_INVALID_ARGUMENT;

    uint8_t  grp[4];
    unsigned g = 0;
    unsigned pad = 0;
    size_t   n = 0;

    for (size_t i = 0; i < in_len; i++) {
        const uint8_t c = in[i];
        /* The only characters skipped are the ones Apple's writer inserts to
         * wrap the line. Anything else is refused rather than ignored. */
        if (is_ws(c)) continue;

        if (c == '=') {
            /* At most two, and only as the tail of the final quantum. */
            if (++pad > 2u) return VMFW_PLIST_ERR_BAD_BASE64;
        } else {
            /* Data after padding means the encoder and the decoder disagree
             * about where the payload ends. */
            if (pad) return VMFW_PLIST_ERR_BAD_BASE64;
            if (b64_val(c) < 0) return VMFW_PLIST_ERR_BAD_BASE64;
        }

        grp[g++] = c;
        if (g == 4u) {
            const vmfw_plist_status_t st = b64_emit(grp, pad, out, cap, &n);
            if (st != VMFW_PLIST_OK) return st;
            g = 0;
        }
    }

    /* The alphabet-character count must be a whole number of quanta. A trailing
     * partial group is a truncated blob, not a shorter one. */
    if (g != 0u) return VMFW_PLIST_ERR_BAD_BASE64;

    if (out_len) *out_len = n;
    return VMFW_PLIST_OK;
}

size_t vmfw_base64_decoded_size(const uint8_t *in, size_t in_len) {
    if (!in) return 0;

    /* Only alphabet characters count: whitespace carries nothing, '=' stands
     * for a byte that was never there, and anything else will be refused by the
     * decoder anyway. Counting either of the latter would over-report and hand
     * the caller a length the decode can never reach. */
    size_t n = 0;
    for (size_t i = 0; i < in_len; i++)
        if (b64_val(in[i]) >= 0) n++;

    /* n * 3 / 4, written so that a hostile length cannot wrap size_t. */
    return (n / 4u) * 3u + (n % 4u) * 3u / 4u;
}

const char *vmfw_plist_strerror(vmfw_plist_status_t st) {
    switch (st) {
        case VMFW_PLIST_OK:                   return "ok";
        case VMFW_PLIST_ERR_INVALID_ARGUMENT: return "invalid plist argument";
        case VMFW_PLIST_ERR_NOT_XML:          return "not an XML property list";
        case VMFW_PLIST_ERR_MALFORMED:        return "malformed property list";
        case VMFW_PLIST_ERR_TOO_DEEP:         return "property list nests too deeply";
        case VMFW_PLIST_ERR_NOT_FOUND:        return "no such property list path";
        case VMFW_PLIST_ERR_WRONG_TYPE:       return "property list value is not that type";
        case VMFW_PLIST_ERR_TOO_LONG:         return "property list value does not fit";
        case VMFW_PLIST_ERR_BAD_BASE64:       return "invalid base64 in a <data> element";
        default:                              return "unknown error";
    }
}
