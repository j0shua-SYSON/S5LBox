/*
 * iOS3-VM — VMInstances. See the header for what "multiple instances" does and
 * does not mean here.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "VMInstances.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

const char *vm_instance_status_text(vm_instance_status_t status) {
    switch (status) {
        case VM_INSTANCE_OK:             return "ok";
        case VM_INSTANCE_ERR_NULL:       return "a required value was missing";
        case VM_INSTANCE_ERR_FULL:       return "no room for another machine";
        case VM_INSTANCE_ERR_RANGE:      return "no machine at that position";
        case VM_INSTANCE_ERR_NAME_EMPTY: return "the name is empty";
        case VM_INSTANCE_ERR_NAME_LONG:  return "the name is too long";
        case VM_INSTANCE_ERR_NAME_CONTROL:
            return "the name contains a control character";
        case VM_INSTANCE_ERR_ID_INVALID: return "the identifier is malformed";
        case VM_INSTANCE_ERR_ID_TAKEN:
            return "another machine already has that identifier";
        case VM_INSTANCE_ERR_PARSE:      return "the saved machine list is unreadable";
    }
    return "unknown";
}

void vm_instance_list_reset(vm_instance_list_t *list) {
    if (!list) return;
    memset(list, 0, sizeof *list);
}

/* ------------------------------------------------------------ validation --- */

vm_instance_status_t vm_instance_name_check(const char *name) {
    if (!name) return VM_INSTANCE_ERR_NULL;

    size_t len = strlen(name);
    if (len > VM_INSTANCE_NAME_MAX) return VM_INSTANCE_ERR_NAME_LONG;

    /*
     * Control bytes are refused rather than stripped, and newline is the one
     * that actually matters: the persisted form is line-oriented, so a name
     * containing '\n' would write a record that reads back as two. Refusing
     * here means the serialiser never has to escape anything, which is why it
     * has no escaping code to get wrong.
     */
    bool any_visible = false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20u || c == 0x7fu) return VM_INSTANCE_ERR_NAME_CONTROL;
        if (c != ' ') any_visible = true;
    }
    if (!any_visible) return VM_INSTANCE_ERR_NAME_EMPTY;
    return VM_INSTANCE_OK;
}

vm_instance_status_t vm_instance_id_check(const char *id) {
    if (!id) return VM_INSTANCE_ERR_NULL;
    if (strlen(id) != VM_INSTANCE_ID_LEN) return VM_INSTANCE_ERR_ID_INVALID;
    for (unsigned i = 0; i < VM_INSTANCE_ID_LEN; i++) {
        char c = id[i];
        bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex) return VM_INSTANCE_ERR_ID_INVALID;
    }
    return VM_INSTANCE_OK;
}

bool vm_instance_options_fit(unsigned option_count) {
    return option_count <= VM_INSTANCE_OPTION_MAX;
}

/* ------------------------------------------------------------- mutations --- */

int vm_instance_index_of_id(const vm_instance_list_t *list, const char *id) {
    if (!list || !id) return -1;
    if (vm_instance_id_check(id) != VM_INSTANCE_OK) return -1;
    for (unsigned i = 0; i < list->count; i++)
        if (strcmp(list->slot[i].id, id) == 0) return (int)i;
    return -1;
}

const vm_instance_t *vm_instance_at(const vm_instance_list_t *list,
                                    unsigned index) {
    if (!list || index >= list->count) return NULL;
    return &list->slot[index];
}

vm_instance_status_t vm_instance_add(vm_instance_list_t *list,
                                     const char *id, const char *name,
                                     const bool *options, unsigned option_count,
                                     uint64_t created_unix,
                                     unsigned *out_index) {
    if (!list) return VM_INSTANCE_ERR_NULL;

    vm_instance_status_t s = vm_instance_id_check(id);
    if (s != VM_INSTANCE_OK) return s;
    s = vm_instance_name_check(name);
    if (s != VM_INSTANCE_OK) return s;

    /* Fullness is checked AFTER validity so that a full list still tells a
     * caller their name was malformed, rather than hiding one fault behind
     * another. */
    if (list->count >= VM_INSTANCE_MAX) return VM_INSTANCE_ERR_FULL;
    if (vm_instance_index_of_id(list, id) >= 0) return VM_INSTANCE_ERR_ID_TAKEN;

    vm_instance_t *row = &list->slot[list->count];
    memset(row, 0, sizeof *row);
    memcpy(row->id, id, VM_INSTANCE_ID_LEN);
    row->id[VM_INSTANCE_ID_LEN] = '\0';
    memcpy(row->name, name, strlen(name));
    row->name[strlen(name)] = '\0';
    row->created_unix = created_unix;

    if (options) {
        unsigned n = option_count;
        if (n > VM_INSTANCE_OPTION_MAX) n = VM_INSTANCE_OPTION_MAX;
        for (unsigned i = 0; i < n; i++) row->options[i] = options[i];
    }

    if (out_index) *out_index = list->count;
    list->count++;
    return VM_INSTANCE_OK;
}

vm_instance_status_t vm_instance_remove(vm_instance_list_t *list,
                                        unsigned index) {
    if (!list) return VM_INSTANCE_ERR_NULL;
    if (index >= list->count) return VM_INSTANCE_ERR_RANGE;

    /* Order is preserved because the UI shows it and a list that reshuffles
     * when you delete something is a list nobody trusts. */
    for (unsigned i = index; i + 1u < list->count; i++)
        list->slot[i] = list->slot[i + 1u];
    memset(&list->slot[list->count - 1u], 0, sizeof list->slot[0]);
    list->count--;
    return VM_INSTANCE_OK;
}

vm_instance_status_t vm_instance_rename(vm_instance_list_t *list,
                                        unsigned index, const char *name) {
    if (!list) return VM_INSTANCE_ERR_NULL;
    if (index >= list->count) return VM_INSTANCE_ERR_RANGE;
    vm_instance_status_t s = vm_instance_name_check(name);
    if (s != VM_INSTANCE_OK) return s;

    vm_instance_t *row = &list->slot[index];
    memset(row->name, 0, sizeof row->name);
    memcpy(row->name, name, strlen(name));
    return VM_INSTANCE_OK;
}

vm_instance_status_t vm_instance_duplicate(vm_instance_list_t *list,
                                           unsigned index,
                                           const char *new_id,
                                           const char *new_name,
                                           uint64_t created_unix,
                                           unsigned *out_index) {
    if (!list) return VM_INSTANCE_ERR_NULL;
    if (index >= list->count) return VM_INSTANCE_ERR_RANGE;

    /* Copy the source out first: vm_instance_add() writes into the array, and
     * `index` would be a dangling read if the array were ever reallocated.
     * It is not today -- it is a fixed array -- and this is still copied,
     * because "today it happens to be safe" is how that becomes a bug. */
    vm_instance_t source = list->slot[index];

    unsigned added = 0;
    vm_instance_status_t s = vm_instance_add(list, new_id, new_name,
                                             source.options,
                                             VM_INSTANCE_OPTION_MAX,
                                             created_unix, &added);
    if (s != VM_INSTANCE_OK) return s;

    /* retired_total and last_opened_unix deliberately stay zero: see header. */
    if (out_index) *out_index = added;
    return VM_INSTANCE_OK;
}

/* ----------------------------------------------------------- persistence --- */

/*
 * One header line, then one line per instance:
 *
 *   ios3vm-instances 1
 *   <id> <created> <last_opened> <retired> <options-bits> <name...>
 *
 * The name is LAST and unquoted, so it may contain spaces and needs no
 * escaping; every field before it is a fixed token with no spaces in it.
 * Options are a fixed-width lower-case hex word, least significant bit = row 0,
 * which keeps the line stable in width as toggles are added.
 */
#define VM_INSTANCE_HEADER "ios3vm-instances"

static uint32_t options_to_bits(const bool *options) {
    uint32_t bits = 0;
    for (unsigned i = 0; i < VM_INSTANCE_OPTION_MAX && i < 32u; i++)
        if (options[i]) bits |= (uint32_t)1u << i;
    return bits;
}

static void bits_to_options(uint32_t bits, bool *options) {
    for (unsigned i = 0; i < VM_INSTANCE_OPTION_MAX && i < 32u; i++)
        options[i] = (bits & ((uint32_t)1u << i)) != 0u;
}

/*
 * Append to a snprintf-style cursor, tracking the length the WHOLE text would
 * have had even once the buffer is full — that is what lets a caller size the
 * buffer with one dry run at cap 0, the same contract vm_option_command_line()
 * already uses.
 */
#if defined(__GNUC__)
__attribute__((format(printf, 4, 5)))
#endif
static void emit(char **out, size_t *left, size_t *total,
                 const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf((*left > 0u) ? *out : NULL, *left, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    *total += (size_t)n;
    if ((size_t)n < *left) {
        *out += n;
        *left -= (size_t)n;
    } else if (*left > 0u) {
        /* Full. Park the cursor on the terminator and keep counting. */
        *out += *left - 1u;
        *left = 1u;
    }
}

size_t vm_instance_serialize(const vm_instance_list_t *list,
                             char *out, size_t cap) {
    size_t total = 0, left = cap;
    char *cursor = out;
    if (cap && out) out[0] = '\0';
    if (!list) return 0;

    emit(&cursor, &left, &total, "%s %u\n",
         VM_INSTANCE_HEADER, VM_INSTANCE_FORMAT_VERSION);

    for (unsigned i = 0; i < list->count; i++) {
        const vm_instance_t *row = &list->slot[i];
        emit(&cursor, &left, &total, "%s %llu %llu %llu %08x %s\n",
             row->id,
             (unsigned long long)row->created_unix,
             (unsigned long long)row->last_opened_unix,
             (unsigned long long)row->retired_total,
             options_to_bits(row->options),
             row->name);
    }
    return total;
}

/* Read one line into `dst`; returns the start of the next line, or NULL at the
 * end of the text. A line longer than the buffer is a parse error, signalled by
 * setting *overflow. */
static const char *next_line(const char *p, char *dst, size_t cap,
                             bool *overflow) {
    if (!p || !*p) return NULL;
    size_t n = 0;
    while (*p && *p != '\n') {
        if (n + 1u >= cap) { *overflow = true; return NULL; }
        dst[n++] = *p++;
    }
    dst[n] = '\0';
    /* Tolerate CRLF: a file round-tripped through a Windows editor is still
     * the user's machine list. */
    if (n && dst[n - 1u] == '\r') dst[n - 1u] = '\0';
    return (*p == '\n') ? p + 1 : p + strlen(p);
}

vm_instance_status_t vm_instance_deserialize(vm_instance_list_t *list,
                                             const char *text) {
    if (!list) return VM_INSTANCE_ERR_NULL;
    vm_instance_list_reset(list);
    if (!text) return VM_INSTANCE_ERR_NULL;

    /* Long enough for the longest legal record, and one byte of slack so an
     * over-long line is detected rather than truncated into a legal-looking
     * one. */
    char line[VM_INSTANCE_NAME_MAX + 128u];
    bool overflow = false;

    const char *p = next_line(text, line, sizeof line, &overflow);
    if (overflow || !p) return VM_INSTANCE_ERR_PARSE;

    unsigned version = 0;
    char tag[32];
    if (sscanf(line, "%31s %u", tag, &version) != 2) return VM_INSTANCE_ERR_PARSE;
    if (strcmp(tag, VM_INSTANCE_HEADER) != 0) return VM_INSTANCE_ERR_PARSE;
    /* An unknown version loads NOTHING. A newer app's file read by an older
     * one must not become a shorter list that looks like deletions. */
    if (version != VM_INSTANCE_FORMAT_VERSION) return VM_INSTANCE_ERR_PARSE;

    while ((p = next_line(p, line, sizeof line, &overflow)) != NULL) {
        if (overflow) { vm_instance_list_reset(list); return VM_INSTANCE_ERR_PARSE; }
        if (line[0] == '\0') continue;          /* a trailing blank line */

        char id[VM_INSTANCE_ID_LEN + 8u];
        unsigned long long created = 0, opened = 0, retired = 0;
        unsigned bits = 0;
        int consumed = 0;
        int got = sscanf(line, "%23s %llu %llu %llu %8x %n",
                         id, &created, &opened, &retired, &bits, &consumed);
        if (got != 5 || consumed <= 0) {
            vm_instance_list_reset(list);
            return VM_INSTANCE_ERR_PARSE;
        }
        const char *name = line + consumed;

        bool options[VM_INSTANCE_OPTION_MAX];
        bits_to_options((uint32_t)bits, options);

        unsigned added = 0;
        vm_instance_status_t s = vm_instance_add(list, id, name, options,
                                                 VM_INSTANCE_OPTION_MAX,
                                                 created, &added);
        if (s != VM_INSTANCE_OK) {
            /* A malformed or duplicate record invalidates the whole file. Half
             * a machine list is worse than an empty one, because the user can
             * tell an empty one is wrong. */
            vm_instance_list_reset(list);
            return (s == VM_INSTANCE_ERR_FULL) ? VM_INSTANCE_ERR_FULL
                                               : VM_INSTANCE_ERR_PARSE;
        }
        list->slot[added].last_opened_unix = opened;
        list->slot[added].retired_total = retired;
    }
    return VM_INSTANCE_OK;
}
