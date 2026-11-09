/* Distribution version values.
 *
 * Follow the living Version Specifiers specification:
 * https://packaging.python.org/en/latest/specifications/version-specifiers/
 * Ordering rules:
 * https://packaging.python.org/en/latest/specifications/version-specifiers/#summary-of-permitted-suffixes-and-relative-ordering
 *
 * Local labels affect value equality and ordering.  Dependency specifier
 * matching has separate rules and is not implemented here.
 *
 * Storage
 * =======
 *
 *       VersionObject                         VersionFull
 *   +--------------------+                 +----------------------+
 *   | object header      |                 | parts[PART_COUNT]    |
 *   | cached hash        |                 |   epoch, release,    |
 *   | release_len, ready |                 |   pre, post, dev,     |
 *   | value              |--- full ------->|   local              |
 *   | replacement        |                 | cached tuple key     |
 *   +--------------------+                 +----------------------+
 *
 * After initialization, release_len selects the VersionValue union member:
 *   1..4: value.key holds two uint64_t words; release_len is the component count.
 *      0: value.full owns a VersionFull allocation.
 *
 * Compact values have epoch zero, no local label, one to four release
 * components, and unsigned 16-bit component numbers.  Larger values use full
 * storage.  from_parts also keeps integer subclasses in full storage to
 * preserve their state and behavior.
 *
 * The compact order key uses 118 bits: release(64), pre(20), post(17), dev(17).
 *
 *   high: 63                                                     0
 *         +-------------------------------------------------------+
 *         | release (64)                                          |
 *         +-------------------------------------------------------+
 *   low:  63          54 53             34 33        17 16        0
 *         +-------------+----------------+------------+-----------+
 *         | zero (10)   | pre (20)       | post (17)  | dev (17)  |
 *         +-------------+----------------+------------+-----------+
 *
 * The four 16-bit release fields are zero-padded, so 1 and 1.0 compare equal.
 * release_len preserves their different release tuples and strings.
 * pre has a 2-bit phase, 2-bit kind and 16-bit number:
 *   phase 0: development-only, without an explicit pre or post segment
 *   phase 1: explicit prerelease; kind 0=a, 1=b, 2=rc
 *   phase 2: no prerelease
 * post has a presence bit and 16-bit number.  dev holds a present number or
 * 0x1ffff for absence, which sorts after all present development numbers.
 * Compare high then low.  Hash the tuple key so equal compact and full values
 * have equal hashes.
 *
 * Construction and reads
 * ======================
 *
 *   str -> ParsedVersion -- compact -> SmallParts -> pack_small_key
 *                         \-- full --> Python parts -> full_from_parts
 *
 *   from_parts / __replace__ -> normalize_part -> make_parts
 *                                                 | compact: make_small_parts
 *                                                 | full:    make_full_parts
 *
 * Both routes prepare a VersionValue, then call publish_version.  ParsedVersion
 * borrows spans from the input string, which stays owned until parsing ends.
 * Compact parsing creates no Python component objects.  Getters, formatting
 * and mixed comparisons unpack components as needed without changing storage.
 * Full values cache their tuple key; compact values cache only their hash.
 * __replace__ chooses storage for a new value when components change.
 *
 * Initialization and ownership
 * ============================
 *
 * publish_version installs one complete value under the object lock, then
 * release-stores ready.  Readers acquire ready before reading the union.
 * Empty objects have ready == 0 and own no full allocation, even though their
 * release_len is zero.  __sizeof__ must check ready too: another thread may
 * still be initializing an object exposed through object.__new__.
 *
 * Parsing and callbacks run outside publication locks.  Full-key construction
 * can call integer-subclass methods, so only the completed key is installed
 * under the cache lock.
 *
 * Subtypes with __mutable_version__ = True can update through __setstate__:
 *
 *   subtype: original storage -- replacement --> immutable exact Version
 *
 * Updates swap the snapshot under the lock and release the old reference
 * outside it.  Readers take an owned snapshot under the lock.  Original
 * storage stays unchanged.  GC visits the type, original full storage and
 * replacement; compact objects need tracking because their heap type can
 * participate in a cycle.  Module state owns the type, exception and shared
 * stable suffix per interpreter.
 */
#ifndef Py_BUILD_CORE_BUILTIN
#  define Py_BUILD_CORE_MODULE 1
#endif

#include "Python.h"
#include "pycore_call.h"          // _PyObject_MakeTpCall()
#include "pycore_modsupport.h"    // _PyArg_NoKeywords()
#include "pycore_pystate.h"       // _PyThreadState_GET()

enum { PRE_A, PRE_B, PRE_RC, PRE_NONE = 0xff };
/* SmallParts.flags */
enum { HAS_POST = 1, HAS_DEV = 2 };
/* Compact key fields; see the layout above. */
enum { PHASE_DEV_ONLY, PHASE_PRE, PHASE_NONE };
#define DEV_ABSENT 0x1ffffu
enum {
    PART_EPOCH,
    PART_RELEASE,
    PART_PRE,
    PART_POST,
    PART_DEV,
    PART_LOCAL,
    PART_COUNT
};

static const char *const PRE_NAMES[] = {"a", "b", "rc"};
/* Longer spellings first so "pre" does not shadow "preview". */
static const struct {
    const char *word;
    uint8_t letter;
} PRE_WORDS[] = {
    {"alpha", PRE_A}, {"beta", PRE_B}, {"preview", PRE_RC}, {"pre", PRE_RC},
    {"rc", PRE_RC},   {"a", PRE_A},    {"b", PRE_B},        {"c", PRE_RC},
};
static const char *const PART_NAMES[] = {"epoch", "release", "pre",
                                         "post",  "dev",     "local"};

typedef struct {
    uint64_t high;
    uint64_t low;
} VersionKey;

typedef struct {
    PyObject *parts[PART_COUNT];
    PyObject *key;
} VersionFull;

typedef union {
    VersionKey key;
    VersionFull *full;
} VersionValue;

typedef struct {
    PyObject_HEAD
    Py_hash_t hash;
    /* Zero means full; compact releases contain 1-4 components. */
    uint8_t release_len;
    uint32_t ready;
    VersionValue value;
    PyObject *replacement;
} VersionObject;

typedef struct {
    PyObject *version_type;
    PyObject *invalid_version;
    PyObject *stable_suffix;
} VersionState;

typedef struct {
    uint16_t release[4];
    uint8_t release_count;
    uint8_t pre_letter;
    uint16_t pre_n;
    uint16_t post_n;
    uint16_t dev_n;
    uint8_t flags;
} SmallParts;

/* len == 0 means the number was absent. */
typedef struct {
    const char *s;
    Py_ssize_t len;
    Py_ssize_t val;
    int too_big;
} NumberField;

typedef struct {
    int has_epoch;
    NumberField epoch;
    Py_ssize_t rel_count;
    const char *rel_span;
    Py_ssize_t rel_span_len;
    uint16_t rel_small[4];
    int rel_all_small;
    int has_pre;
    int pre_letter;
    NumberField pre_n;
    int has_post;
    NumberField post_n;
    int has_dev;
    NumberField dev_n;
    int has_local;
    const char *local_span;
    Py_ssize_t local_span_len;
} ParsedVersion;

static struct PyModuleDef version_module;

/*[clinic input]
module _version
class _version.Version "PyObject *" "clinic_state()->version_type"
[clinic start generated code]*/
/*[clinic end generated code: output=da39a3ee5e6b4b0d input=abb758212f42d273]*/

#include "clinic/_versionmodule.c.h"

static inline VersionObject *
version_cast(PyObject *object)
{
    return (VersionObject *)object;
}

static inline int
is_small(const VersionObject *version)
{
    return version->release_len != 0;
}

/* Historical subclass pickles allocate without arguments, then restore once. */
static int
version_ready(PyObject *self)
{
    if (!_Py_atomic_load_uint32_acquire(&version_cast(self)->ready)) {
        PyErr_SetString(PyExc_ValueError, "Version is not initialized");
        return 0;
    }
    return 1;
}

/* Borrow self unless an update requires an owned snapshot. */
static PyObject *
version_current(PyObject *self)
{
    if (!version_ready(self)) {
        return NULL;
    }
    VersionObject *version = version_cast(self);
    if (_Py_atomic_load_ptr_acquire(&version->replacement) == NULL) {
        return self;
    }
    PyObject *current;
    Py_BEGIN_CRITICAL_SECTION(self);
    current = Py_NewRef(version->replacement);
    Py_END_CRITICAL_SECTION();
    return current;
}

/* Release what version_current returned. */
static inline void
release_current(PyObject *self, PyObject *current)
{
    if (current != self) {
        Py_DECREF(current);
    }
}

#define DELEGATE_VERSION(self, result_type, call, error) \
    PyObject *current = version_current(self); \
    if (current != (self)) { \
        if (current == NULL) { \
            return (error); \
        } \
        result_type result = (call); \
        Py_DECREF(current); \
        return result; \
    }

static VersionState *
state_from_type(PyTypeObject *type)
{
    PyObject *module = PyType_GetModuleByDef(type, &version_module);
    if (module == NULL) {
        return NULL;
    }
    return PyModule_GetState(module);
}

/* Adapters may retain their public exception identity without wrapping hot calls. */
static PyObject *
format_invalid_version(PyTypeObject *type, const char *format, ...)
{
    VersionState *state = state_from_type(type);
    if (state == NULL) {
        return NULL;
    }
    PyObject *exception = NULL;
    if (PyObject_GetOptionalAttrString((PyObject *)type, "__invalid_version__",
                                      &exception) < 0) {
        return NULL;
    }
    if (exception == NULL) {
        exception = Py_NewRef(state->invalid_version);
    }
    else {
        int valid = PyObject_IsSubclass(exception, state->invalid_version);
        if (valid <= 0) {
            Py_DECREF(exception);
            if (valid == 0) {
                PyErr_SetString(PyExc_TypeError,
                                "__invalid_version__ must derive from InvalidVersion");
            }
            return NULL;
        }
    }
    va_list args;
    va_start(args, format);
    PyErr_FormatV(exception, format, args);
    va_end(args);
    Py_DECREF(exception);
    return NULL;
}

static void
clear_parts(PyObject **parts)
{
    for (int i = 0; i < PART_COUNT; i++) {
        Py_XDECREF(parts[i]);
    }
}

/* Four stack entries cover the compact release width.
 * Longer tuples use heap storage. */
typedef struct {
    PyObject **items;
    Py_ssize_t length;
    PyObject *stack[4];
} TupleBuilder;

static int
tuple_builder_init(TupleBuilder *builder, Py_ssize_t length)
{
    memset(builder, 0, sizeof(*builder));
    builder->length = length;
    if (length <=
        (Py_ssize_t)(sizeof(builder->stack) / sizeof(builder->stack[0]))) {
        builder->items = builder->stack;
    }
    else {
        builder->items = PyMem_Calloc((size_t)length, sizeof(PyObject *));
        if (builder->items == NULL) {
            PyErr_NoMemory();
            return -1;
        }
    }
    return 0;
}

static void
tuple_builder_clear(TupleBuilder *builder)
{
    for (Py_ssize_t i = 0; i < builder->length; i++) {
        Py_XDECREF(builder->items[i]);
    }
    if (builder->items != builder->stack) {
        PyMem_Free(builder->items);
    }
}

static PyObject *
tuple_builder_finish(TupleBuilder *builder)
{
    PyObject *tuple = PyTuple_FromArray(builder->items, builder->length);
    tuple_builder_clear(builder);
    return tuple;
}

static VersionKey
pack_small_key(const SmallParts *parts)
{
    uint64_t release = 0;
    for (int i = 0; i < parts->release_count; i++) {
        release |= (uint64_t)parts->release[i] << (48 - 16 * i);
    }
    int has_post = parts->flags & HAS_POST;
    int has_dev = parts->flags & HAS_DEV;
    uint32_t pre;
    if (parts->pre_letter != PRE_NONE) {
        pre = ((uint32_t)PHASE_PRE << 18) |
              ((uint32_t)parts->pre_letter << 16) | parts->pre_n;
    }
    else if (has_dev && !has_post) {
        pre = (uint32_t)PHASE_DEV_ONLY << 18;
    }
    else {
        pre = (uint32_t)PHASE_NONE << 18;
    }
    uint32_t post = has_post ? (1u << 16) | parts->post_n : 0;
    uint32_t dev = has_dev ? parts->dev_n : DEV_ABSENT;
    /* Two uint64_t words avoid a compiler-specific 128-bit integer type. */
    return (VersionKey){
        .high = release,
        .low = ((uint64_t)pre << 34) |
               ((uint64_t)post << 17) | dev,
    };
}

static SmallParts
unpack_small(const VersionObject *version)
{
    uint64_t release = version->value.key.high;
    uint64_t low = version->value.key.low;
    uint32_t pre = (uint32_t)((low >> 34) & 0xfffff);
    uint32_t post = (uint32_t)((low >> 17) & 0x1ffff);
    uint32_t dev = (uint32_t)(low & 0x1ffff);
    int has_pre = (pre >> 18) == PHASE_PRE;
    int has_post = post >> 16;
    int has_dev = dev != DEV_ABSENT;
    SmallParts parts = {
        .release_count = version->release_len,
        .pre_letter = has_pre ? (pre >> 16) & 3 : PRE_NONE,
        .pre_n = has_pre ? pre & UINT16_MAX : 0,
        .post_n = has_post ? post & UINT16_MAX : 0,
        .dev_n = has_dev ? dev : 0,
        .flags = (has_post ? HAS_POST : 0) | (has_dev ? HAS_DEV : 0),
    };
    for (int i = 0; i < parts.release_count; i++) {
        parts.release[i] = (uint16_t)(release >> (48 - 16 * i));
    }
    return parts;
}

/* Ownership transfers only after all fields are ready for readers. */
static int
publish_version(VersionObject *target, uint8_t release_len, VersionValue value)
{
    int published = 0;
    Py_BEGIN_CRITICAL_SECTION((PyObject *)target);
    if (!_Py_atomic_load_uint32_relaxed(&target->ready)) {
        target->release_len = release_len;
        target->value = value;
        _Py_atomic_store_ssize_relaxed(&target->hash, -1);
        _Py_atomic_store_uint32_release(&target->ready, 1);
        published = 1;
    }
    Py_END_CRITICAL_SECTION();
    if (!published) {
        PyErr_SetString(PyExc_TypeError, "Version is already initialized");
        return -1;
    }
    return 0;
}

static void
free_full(VersionFull *full)
{
    clear_parts(full->parts);
    Py_XDECREF(full->key);
    PyMem_Free(full);
}

static void
clear_value(uint8_t release_len, VersionValue value)
{
    if (release_len == 0 && value.full != NULL) {
        free_full(value.full);
    }
}

static VersionFull *
full_from_parts(PyObject *const *parts)
{
    VersionFull *full = PyMem_Calloc(1, sizeof(*full));
    if (full == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    for (int i = 0; i < PART_COUNT; i++) {
        full->parts[i] = Py_NewRef(parts[i]);
    }
    return full;
}

/* Takes ownership of value, including on allocation or publication failure. */
static PyObject *
version_from_value(PyTypeObject *type, uint8_t release_len, VersionValue value)
{
    VersionObject *target = (VersionObject *)type->tp_alloc(type, 0);
    if (target == NULL || publish_version(target, release_len, value) < 0) {
        clear_value(release_len, value);
        Py_XDECREF(target);
        return NULL;
    }
    return (PyObject *)target;
}

static PyObject *
make_small_parts(PyTypeObject *type, const SmallParts *parts)
{
    VersionValue value = {.key = pack_small_key(parts)};
    return version_from_value(type, parts->release_count, value);
}

static PyObject *
make_full_parts(PyTypeObject *type, PyObject *const *parts)
{
    VersionValue value = {.full = full_from_parts(parts)};
    if (value.full == NULL) {
        return NULL;
    }
    return version_from_value(type, 0, value);
}

/* PyLong_FromString needs a terminated string; digit runs end mid-buffer. */
static PyObject *
long_from(const char *s, Py_ssize_t len)
{
    char stackbuf[32];
    char *buf = (len < (Py_ssize_t)sizeof(stackbuf)) ? stackbuf
                                                     : PyMem_Malloc(len + 1);
    if (buf == NULL) {
        return PyErr_NoMemory();
    }
    memcpy(buf, s, len);
    buf[len] = '\0';
    PyObject *r = PyLong_FromString(buf, NULL, 10);
    if (buf != stackbuf) {
        PyMem_Free(buf);
    }
    return r;
}

static int
is_sep(char c)
{
    return c == '.' || c == '-' || c == '_';
}

static const char *
skip_sep(const char *p, const char *end)
{
    return p < end && is_sep(*p) ? p + 1 : p;
}

/* case-insensitive match of kw at p; return length matched or 0 */
static Py_ssize_t
match_kw(const char *p, const char *end, const char *kw)
{
    Py_ssize_t i = 0;
    while (kw[i]) {
        if (p + i >= end || Py_TOLOWER(p[i]) != kw[i]) {
            return 0;
        }
        i++;
    }
    return i;
}

/* read [0-9]+ into f; returns number of digits consumed (0 if none) */
static Py_ssize_t
read_digits(const char *p, const char *end, NumberField *f)
{
    const char *start = p;
    while (p < end && Py_ISDIGIT(*p)) {
        p++;
    }
    Py_ssize_t n = p - start;
    if (n == 0) {
        memset(f, 0, sizeof(*f));
        return 0;
    }
    f->s = start;
    f->len = n;
    f->too_big = 0;
    Py_ssize_t v = 0;
    for (const char *q = start; q < p; q++) {
        v = v * 10 + (*q - '0');
        if (v > 0xffff) {
            f->too_big = 1;
            break;
        }
    }
    f->val = v;
    return n;
}

static int
match_pre_letter(const char *p, const char *end, int *letter, Py_ssize_t *mlen)
{
    for (size_t i = 0; i < Py_ARRAY_LENGTH(PRE_WORDS); i++) {
        Py_ssize_t m = match_kw(p, end, PRE_WORDS[i].word);
        if (m) {
            *letter = PRE_WORDS[i].letter;
            *mlen = m;
            return 1;
        }
    }
    return 0;
}

/* Normalized pre-release letters are exactly "a", "b" or "rc". */
static int
pre_letter_index(PyObject *letter)
{
    return PyUnicode_CompareWithASCIIString(letter, "a") == 0   ? PRE_A
           : PyUnicode_CompareWithASCIIString(letter, "b") == 0 ? PRE_B
                                                                : PRE_RC;
}

/* Local labels: [a-z0-9]+ ([.-_] [a-z0-9]+)*.  Returns the matched length. */
static Py_ssize_t
scan_local(const char *p, const char *end)
{
    const char *start = p;
    int seg_chars = 0;
    for (;;) {
        if (p < end && Py_ISALNUM(*p)) {
            seg_chars = 1;
            p++;
        }
        else if (p < end && is_sep(*p) && seg_chars) {
            seg_chars = 0;
            p++;
        }
        else {
            break;
        }
    }
    return seg_chars ? p - start : -1;
}

/* returns 0 on success, -1 on invalid version */
static int
parse_version(const char *s, Py_ssize_t n, ParsedVersion *out)
{
    memset(out, 0, sizeof(*out));
    const char *p = s;
    const char *end = s + n;

    if (p < end && Py_TOLOWER(*p) == 'v') {
        p++;
    }

    /* epoch vs first release segment: digits then '!' => epoch */
    {
        const char *d = p;
        while (d < end && Py_ISDIGIT(*d)) {
            d++;
        }
        if (d > p && d < end && *d == '!') {
            NumberField f;
            read_digits(p, end, &f);
            out->has_epoch = 1;
            out->epoch = f;
            p = d + 1;
        }
    }

    /* release: [0-9]+ (. [0-9]+)* */
    if (!(p < end && Py_ISDIGIT(*p))) {
        return -1;
    }
    out->rel_span = p;
    out->rel_all_small = 1;
    out->rel_count = 0;
    for (;;) {
        NumberField f;
        Py_ssize_t got = read_digits(p, end, &f);
        if (got == 0) {
            return -1;
        }
        if (out->rel_count < 4) {
            out->rel_small[out->rel_count] = (uint16_t)f.val;
        }
        if (f.too_big) {
            out->rel_all_small = 0;
        }
        out->rel_count++;
        p += got;
        if (p < end && *p == '.') {
            /* lookahead: only consume '.' if a digit follows (else it may be
               a pre/post/dev separator) */
            if (p + 1 < end && Py_ISDIGIT(p[1])) {
                p++;
                continue;
            }
        }
        break;
    }
    out->rel_span_len = p - out->rel_span;

    /* pre: [sep]? letter [sep]? [N]? */
    {
        const char *q = skip_sep(p, end);
        int letter;
        Py_ssize_t m;
        if (q < end && match_pre_letter(q, end, &letter, &m)) {
            q = skip_sep(q + m, end);
            NumberField f;
            p = q + read_digits(q, end, &f);
            out->has_pre = 1;
            out->pre_letter = letter;
            out->pre_n = f;
        }
    }

    /* post: '-' N  |  [sep]? (post|rev|r) [sep]? [N]? */
    if (p + 1 < end && *p == '-' && Py_ISDIGIT(p[1])) {
        p++;
        NumberField f;
        p += read_digits(p, end, &f);
        out->has_post = 1;
        out->post_n = f;
    }
    else {
        const char *q = skip_sep(p, end);
        Py_ssize_t m = match_kw(q, end, "post");
        if (!m) {
            m = match_kw(q, end, "rev");
        }
        if (!m) {
            m = match_kw(q, end, "r");
        }
        if (m) {
            q = skip_sep(q + m, end);
            NumberField f;
            p = q + read_digits(q, end, &f);
            out->has_post = 1;
            out->post_n = f;
        }
    }

    /* dev: [sep]? dev [sep]? [N]? */
    {
        const char *q = skip_sep(p, end);
        Py_ssize_t m = match_kw(q, end, "dev");
        if (m) {
            q = skip_sep(q + m, end);
            NumberField f;
            p = q + read_digits(q, end, &f);
            out->has_dev = 1;
            out->dev_n = f;
        }
    }

    if (p < end && *p == '+') {
        p++;
        Py_ssize_t length = scan_local(p, end);
        if (length < 0) {
            return -1;
        }
        out->has_local = 1;
        out->local_span = p;
        out->local_span_len = length;
        p += length;
    }

    if (p != end) {
        return -1;
    }
    return 0;
}

static PyObject *
local_from_ascii(const char *text, Py_ssize_t length)
{
    Py_ssize_t count = 1;
    for (Py_ssize_t i = 0; i < length; i++) {
        count += is_sep(text[i]);
    }
    TupleBuilder builder;
    if (tuple_builder_init(&builder, count) < 0) {
        return NULL;
    }
    Py_ssize_t start = 0;
    for (Py_ssize_t i = 0; i < count; i++) {
        Py_ssize_t end = start;
        int numeric = 1;
        while (end < length && !is_sep(text[end])) {
            if (!Py_ISDIGIT(text[end])) {
                numeric = 0;
            }
            end++;
        }
        PyObject *part;
        if (numeric) {
            part = long_from(text + start, end - start);
        }
        else {
            part = PyUnicode_New(end - start, 127);
            if (part != NULL) {
                Py_UCS1 *data = PyUnicode_1BYTE_DATA(part);
                for (Py_ssize_t j = start; j < end; j++) {
                    data[j - start] = (Py_UCS1)Py_TOLOWER(text[j]);
                }
            }
        }
        if (part == NULL) {
            tuple_builder_clear(&builder);
            return NULL;
        }
        builder.items[i] = part;
        start = end + 1;
    }
    return tuple_builder_finish(&builder);
}

static PyObject *
normalize_local(PyObject *value, PyTypeObject *type)
{
    if (value == Py_None) {
        return Py_NewRef(Py_None);
    }
    if (PyUnicode_Check(value) && PyUnicode_IS_ASCII(value)) {
        Py_ssize_t length;
        const char *text = PyUnicode_AsUTF8AndSize(value, &length);
        if (text == NULL) {
            return NULL;
        }
        if (scan_local(text, text + length) == length) {
            return local_from_ascii(text, length);
        }
    }
    format_invalid_version(type,
                 "local must be a valid version string, got %R", value);
    return NULL;
}

static int
parts_from_parsed(const ParsedVersion *parsed, PyObject **parts)
{
    for (int i = 0; i < PART_COUNT; i++) {
        parts[i] = Py_NewRef(Py_None);
    }
    Py_SETREF(parts[PART_EPOCH],
              parsed->has_epoch ? long_from(parsed->epoch.s, parsed->epoch.len)
                                : PyLong_FromLong(0));
    if (parts[PART_EPOCH] == NULL) {
        return -1;
    }
    TupleBuilder builder;
    if (tuple_builder_init(&builder, parsed->rel_count) < 0) {
        return -1;
    }
    const char *cursor = parsed->rel_span;
    const char *end = cursor + parsed->rel_span_len;
    for (Py_ssize_t i = 0; i < parsed->rel_count; i++) {
        const char *dot = memchr(cursor, '.', end - cursor);
        const char *stop = dot ? dot : end;
        PyObject *part = long_from(cursor, stop - cursor);
        if (part == NULL) {
            tuple_builder_clear(&builder);
            return -1;
        }
        builder.items[i] = part;
        cursor = stop + 1;
    }
    Py_SETREF(parts[PART_RELEASE], tuple_builder_finish(&builder));
    if (parts[PART_RELEASE] == NULL) {
        return -1;
    }
    if (parsed->has_pre) {
        PyObject *number = parsed->pre_n.len
                               ? long_from(parsed->pre_n.s, parsed->pre_n.len)
                               : PyLong_FromLong(0);
        if (number == NULL) {
            return -1;
        }
        Py_SETREF(
            parts[PART_PRE],
            Py_BuildValue("(sN)", PRE_NAMES[parsed->pre_letter], number));
        if (parts[PART_PRE] == NULL) {
            return -1;
        }
    }
    if (parsed->has_post) {
        Py_SETREF(parts[PART_POST],
                  parsed->post_n.len
                      ? long_from(parsed->post_n.s, parsed->post_n.len)
                      : PyLong_FromLong(0));
        if (parts[PART_POST] == NULL) {
            return -1;
        }
    }
    if (parsed->has_dev) {
        Py_SETREF(parts[PART_DEV],
                  parsed->dev_n.len
                      ? long_from(parsed->dev_n.s, parsed->dev_n.len)
                      : PyLong_FromLong(0));
        if (parts[PART_DEV] == NULL) {
            return -1;
        }
    }
    if (parsed->has_local) {
        Py_SETREF(parts[PART_LOCAL], local_from_ascii(parsed->local_span,
                                                      parsed->local_span_len));
        if (parts[PART_LOCAL] == NULL) {
            return -1;
        }
    }
    return 0;
}

static int
parse_into_version(VersionObject *target, PyObject *original)
{
    PyTypeObject *type = Py_TYPE(target);
    PyObject *text = NULL;
    if (!PyUnicode_Check(original)) {
        goto invalid;
    }
    Py_ssize_t start = 0;
    Py_ssize_t end = PyUnicode_GET_LENGTH(original);
    while (start < end &&
           Py_UNICODE_ISSPACE(PyUnicode_READ_CHAR(original, start))) {
        start++;
    }
    while (end > start &&
           Py_UNICODE_ISSPACE(PyUnicode_READ_CHAR(original, end - 1))) {
        end--;
    }
    text = PyUnicode_Substring(original, start, end);
    if (text == NULL) {
        return -1;
    }
    if (!PyUnicode_IS_ASCII(text)) {
        goto invalid;
    }
    Py_ssize_t length;
    const char *data = PyUnicode_AsUTF8AndSize(text, &length);
    if (data == NULL) {
        Py_DECREF(text);
        return -1;
    }
    ParsedVersion parsed;
    if (parse_version(data, length, &parsed) < 0) {
        goto invalid;
    }
    int small = (!parsed.has_epoch ||
                 (!parsed.epoch.too_big && parsed.epoch.val == 0)) &&
                !parsed.has_local && parsed.rel_count <= 4 &&
                parsed.rel_all_small &&
                (!parsed.has_pre || !parsed.pre_n.too_big) &&
                (!parsed.has_post || !parsed.post_n.too_big) &&
                (!parsed.has_dev || !parsed.dev_n.too_big);
    VersionValue value = {0};
    uint8_t release_len = 0;
    if (small) {
        SmallParts parts = {
            .release_count = (uint8_t)parsed.rel_count,
            .pre_letter = parsed.has_pre ? parsed.pre_letter : PRE_NONE,
            .pre_n = parsed.has_pre ? (uint16_t)parsed.pre_n.val : 0,
            .post_n = parsed.has_post ? (uint16_t)parsed.post_n.val : 0,
            .dev_n = parsed.has_dev ? (uint16_t)parsed.dev_n.val : 0,
            .flags = (parsed.has_post ? HAS_POST : 0) |
                     (parsed.has_dev ? HAS_DEV : 0),
        };
        memcpy(parts.release, parsed.rel_small, sizeof(parts.release));
        value.key = pack_small_key(&parts);
        release_len = parts.release_count;
    }
    else {
        PyObject *parts[PART_COUNT] = {0};
        if (parts_from_parsed(&parsed, parts) < 0) {
            clear_parts(parts);
            Py_DECREF(text);
            return -1;
        }
        value.full = full_from_parts(parts);
        clear_parts(parts);
        if (value.full == NULL) {
            Py_DECREF(text);
            return -1;
        }
    }
    Py_DECREF(text);
    if (publish_version(target, release_len, value) < 0) {
        clear_value(release_len, value);
        return -1;
    }
    return 0;

invalid:
    Py_XDECREF(text);
    format_invalid_version(type, "Invalid version: %R", original);
    return -1;
}

/* Match pickle's default dictionary and slot restoration for subclasses. */
static PyObject *
restore_subclass_state(PyObject *self, PyObject *state)
{
    PyObject *dictionary = state;
    PyObject *slots = NULL;
    if (PyTuple_Check(state) && PyTuple_GET_SIZE(state) == 2) {
        dictionary = PyTuple_GET_ITEM(state, 0);
        slots = PyTuple_GET_ITEM(state, 1);
    }
    if ((dictionary != Py_None && !PyDict_Check(dictionary)) ||
        (slots != NULL && !PyDict_Check(slots))) {
        PyErr_SetString(PyExc_TypeError, "invalid subclass state");
        return NULL;
    }
    if (dictionary != Py_None) {
        PyObject *target = PyObject_GetAttrString(self, "__dict__");
        if (target == NULL) {
            return NULL;
        }
        int result = PyDict_Update(target, dictionary);
        Py_DECREF(target);
        if (result < 0) {
            return NULL;
        }
    }
    if (slots != NULL) {
        /* Copy before invoking setters, which may mutate the supplied dict. */
        PyObject *items = PyDict_Items(slots);
        if (items == NULL) {
            return NULL;
        }
        for (Py_ssize_t i = 0; i < PyList_GET_SIZE(items); i++) {
            PyObject *item = PyList_GET_ITEM(items, i);
            if (PyObject_SetAttr(self, PyTuple_GET_ITEM(item, 0),
                                PyTuple_GET_ITEM(item, 1)) < 0) {
                Py_DECREF(items);
                return NULL;
            }
        }
        Py_DECREF(items);
    }
    Py_RETURN_NONE;
}

static int parts_from_version(PyObject *self, PyObject **parts);
static PyObject *make_parts(PyTypeObject *type, PyObject *const *parts);

static PyObject *
restore_value_once(PyObject *self, PyObject *created)
{
    VersionObject *source = version_cast(created);
    uint8_t release_len = source->release_len;
    VersionValue value = source->value;
    if (release_len == 0) {
        value.full = full_from_parts(source->value.full->parts);
        if (value.full == NULL) {
            return NULL;
        }
    }
    if (publish_version(version_cast(self), release_len, value) < 0) {
        clear_value(release_len, value);
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
version_setstate(PyObject *self, PyObject *state)
{
    VersionObject *target = version_cast(self);
    VersionState *module_state = state_from_type(Py_TYPE(self));
    if (module_state == NULL) {
        return NULL;
    }
    if (PyObject_TypeCheck(state, (PyTypeObject *)module_state->version_type)) {
        int initialized = _Py_atomic_load_uint32_acquire(&target->ready);
        PyObject *allowed = NULL;
        if (initialized &&
            PyObject_GetOptionalAttrString((PyObject *)Py_TYPE(self),
                                          "__mutable_version__", &allowed) < 0) {
            return NULL;
        }
        int mutable = allowed == Py_True;
        Py_XDECREF(allowed);
        if (initialized && !mutable) {
            if (!PyErr_Occurred()) {
                PyErr_SetString(PyExc_TypeError, "Version is immutable");
            }
            return NULL;
        }
        PyObject *current = version_current(state);
        if (current == NULL) {
            return NULL;
        }
        PyObject *parts[PART_COUNT] = {0};
        int result = parts_from_version(current, parts);
        release_current(state, current);
        PyObject *snapshot = result < 0 ? NULL
            : make_parts((PyTypeObject *)module_state->version_type, parts);
        clear_parts(parts);
        if (snapshot == NULL) {
            return NULL;
        }
        if (!initialized) {
            PyObject *restored = restore_value_once(self, snapshot);
            Py_DECREF(snapshot);
            return restored;
        }
        PyObject *previous;
        Py_BEGIN_CRITICAL_SECTION(self);
        previous = target->replacement;
        _Py_atomic_store_ptr_release(&target->replacement, snapshot);
        Py_END_CRITICAL_SECTION();
        Py_XDECREF(previous);
        Py_RETURN_NONE;
    }
    if (_Py_atomic_load_uint32_acquire(&target->ready)) {
        if (!PyUnicode_Check(state)) {
            return restore_subclass_state(self, state);
        }
        PyErr_SetString(PyExc_TypeError, "Version is already initialized");
        return NULL;
    }
    if (parse_into_version(target, state) < 0) {
        return NULL;
    }
    Py_RETURN_NONE;
}


/* Inherit object allocation so old object-based pickles remain loadable. */
static int
version_init(PyObject *self, PyObject *args, PyObject *keywords)
{
    if (_Py_atomic_load_uint32_acquire(&version_cast(self)->ready)) {
        PyErr_SetString(PyExc_TypeError, "Version is already initialized");
        return -1;
    }
    PyObject *text;
    if (!_PyArg_NoKeywords("Version", keywords) ||
        !PyArg_ParseTuple(args, "O:Version", &text)) {
        return -1;
    }
    return parse_into_version(version_cast(self), text);
}

/* Version(text) skips tuple packing and format parsing.  Other call shapes
 * take tp_call, so version_init reports their errors.  Subclasses do not
 * inherit this slot. */
static PyObject *
version_vectorcall(PyObject *type, PyObject *const *args, size_t nargsf,
                   PyObject *kwnames)
{
    Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
    if (nargs != 1 || (kwnames != NULL && PyTuple_GET_SIZE(kwnames) != 0)) {
        return _PyObject_MakeTpCall(_PyThreadState_GET(), type, args, nargs,
                                    kwnames);
    }
    PyTypeObject *version_type = (PyTypeObject *)type;
    PyObject *target = version_type->tp_alloc(version_type, 0);
    if (target == NULL) {
        return NULL;
    }
    if (parse_into_version(version_cast(target), args[0]) < 0) {
        Py_DECREF(target);
        return NULL;
    }
    return target;
}

static int
nonnegative_integer(PyObject *value)
{
    if (!PyLong_Check(value)) {
        return 0;
    }
    if (PyLong_CheckExact(value)) {
        return !PyLong_IsNegative(value);
    }
    return PyObject_RichCompareBool(
        value, Py_GetConstantBorrowed(Py_CONSTANT_ZERO), Py_GE);
}

static PyObject *
normalize_part(int index, PyObject *value, PyTypeObject *type)
{
    if (index == PART_EPOCH) {
        int truth = PyObject_IsTrue(value);
        if (truth < 0) {
            return NULL;
        }
        if (!truth) {
            return PyLong_FromLong(0);
        }
        int valid = nonnegative_integer(value);
        if (valid < 0) {
            return NULL;
        }
        if (valid) {
            return Py_NewRef(value);
        }
        format_invalid_version(type,
                     "epoch must be non-negative integer, got %S", value);
    }
    else if (index == PART_RELEASE) {
        if (value == Py_None) {
            return Py_BuildValue("(i)", 0);
        }
        if (PyTuple_Check(value) && PyTuple_GET_SIZE(value) > 0) {
            Py_ssize_t i;
            for (i = 0; i < PyTuple_GET_SIZE(value); i++) {
                int valid = nonnegative_integer(PyTuple_GET_ITEM(value, i));
                if (valid < 0) {
                    return NULL;
                }
                if (!valid) {
                    break;
                }
            }
            if (i == PyTuple_GET_SIZE(value)) {
                return PyTuple_GetSlice(value, 0, PyTuple_GET_SIZE(value));
            }
        }
        format_invalid_version(type,
                     "release must be a non-empty tuple of non-negative "
                     "integers, got %S",
                     value);
    }
    else if (index == PART_PRE) {
        if (value == Py_None) {
            return Py_NewRef(Py_None);
        }
        if (PyTuple_Check(value) && PyTuple_GET_SIZE(value) == 2) {
            PyObject *letter = PyTuple_GET_ITEM(value, 0);
            PyObject *number = PyTuple_GET_ITEM(value, 1);
            if (PyUnicode_Check(letter) && PyUnicode_IS_ASCII(letter)) {
                Py_ssize_t length, matched;
                const char *text = PyUnicode_AsUTF8AndSize(letter, &length);
                if (!text) {
                    return NULL;
                }
                int kind;
                if (match_pre_letter(text, text + length, &kind, &matched) &&
                    matched == length) {
                    int valid = nonnegative_integer(number);
                    if (valid < 0) {
                        return NULL;
                    }
                    if (valid) {
                        return Py_BuildValue("(sO)", PRE_NAMES[kind], number);
                    }
                }
            }
        }
        format_invalid_version(
            type,
            "pre must be a tuple of ('a'|'b'|'rc', non-negative int), got %S",
            value);
    }
    else if (index == PART_POST || index == PART_DEV) {
        if (value == Py_None) {
            return Py_NewRef(Py_None);
        }
        int valid = nonnegative_integer(value);
        if (valid < 0) {
            return NULL;
        }
        if (valid) {
            return Py_NewRef(value);
        }
        format_invalid_version(type,
                     "%s must be non-negative integer, got %S",
                     PART_NAMES[index], value);
    }
    else {
        return normalize_local(value, type);
    }
    return NULL;
}

static int
small_number(PyObject *value, uint16_t *result)
{
    /* Preserve int subclasses in FULL, including their public representation.
     */
    if (!PyLong_CheckExact(value)) {
        return 0;
    }
    int overflow;
    long number = PyLong_AsLongAndOverflow(value, &overflow);
    if (overflow || number > UINT16_MAX) {
        return 0;
    }
    assert(number >= 0);
    *result = (uint16_t)number;
    return 1;
}

/* Apply the given normalized parts to small; NULL parts keep their value.
 * Return 0 if a part needs full storage. */
static int
apply_small_parts(SmallParts *small, PyObject *const *parts)
{
    if (parts[PART_EPOCH] && (!PyLong_CheckExact(parts[PART_EPOCH]) ||
                              !PyLong_IsZero(parts[PART_EPOCH]))) {
        return 0;
    }
    if (parts[PART_LOCAL] && parts[PART_LOCAL] != Py_None) {
        return 0;
    }
    if (parts[PART_RELEASE]) {
        PyObject *release = parts[PART_RELEASE];
        Py_ssize_t count = PyTuple_GET_SIZE(release);
        if (count > 4) {
            return 0;
        }
        small->release_count = (uint8_t)count;
        for (Py_ssize_t i = 0; i < count; i++) {
            if (!small_number(PyTuple_GET_ITEM(release, i),
                              &small->release[i])) {
                return 0;
            }
        }
    }
    if (parts[PART_PRE]) {
        small->pre_letter = PRE_NONE;
        small->pre_n = 0;
        if (parts[PART_PRE] != Py_None) {
            PyObject *pre = parts[PART_PRE];
            if (!small_number(PyTuple_GET_ITEM(pre, 1), &small->pre_n)) {
                return 0;
            }
            small->pre_letter = pre_letter_index(PyTuple_GET_ITEM(pre, 0));
        }
    }
    for (int index = PART_POST; index <= PART_DEV; index++) {
        if (!parts[index]) {
            continue;
        }
        int flag = index == PART_POST ? HAS_POST : HAS_DEV;
        uint16_t *number = index == PART_POST ? &small->post_n : &small->dev_n;
        *number = 0;
        small->flags &= ~flag;
        if (parts[index] != Py_None) {
            if (!small_number(parts[index], number)) {
                return 0;
            }
            small->flags |= flag;
        }
    }
    return 1;
}

static PyObject *
make_parts(PyTypeObject *type, PyObject *const *parts)
{
    SmallParts small = {0};
    if (apply_small_parts(&small, parts)) {
        return make_small_parts(type, &small);
    }
    return make_full_parts(type, parts);
}

/*[clinic input]
@classmethod
_version.Version.from_parts

    *
    release: object
    epoch: object(c_default="Py_None") = 0
    pre: object = None
    post: object = None
    dev: object = None
    local: object = None

Build a version directly from validated components.
[clinic start generated code]*/

static PyObject *
_version_Version_from_parts_impl(PyTypeObject *type, PyObject *release,
                                 PyObject *epoch, PyObject *pre,
                                 PyObject *post, PyObject *dev,
                                 PyObject *local)
/*[clinic end generated code: output=219df961e7e8b0ce input=895d328e910ba492]*/
{
    PyObject *arguments[PART_COUNT] = {epoch, release, pre, post, dev, local};
    PyObject *parts[PART_COUNT] = {0};
    for (int i = 0; i < PART_COUNT; i++) {
        parts[i] = normalize_part(i, arguments[i], type);
        if (!parts[i]) {
            clear_parts(parts);
            return NULL;
        }
    }
    PyObject *result = make_parts(type, parts);
    clear_parts(parts);
    return result;
}

static PyObject *
get_epoch_impl(PyObject *self, void *Py_UNUSED(context))
{
    VersionObject *version = version_cast(self);
    if (is_small(version)) {
        return PyLong_FromLong(0);
    }
    return Py_NewRef(version->value.full->parts[PART_EPOCH]);
}

static PyObject *
get_release_impl(PyObject *self, void *Py_UNUSED(context))
{
    VersionObject *version = version_cast(self);
    if (!is_small(version)) {
        return Py_NewRef(version->value.full->parts[PART_RELEASE]);
    }
    SmallParts parts = unpack_small(version);
    TupleBuilder builder;
    if (tuple_builder_init(&builder, parts.release_count) < 0) {
        return NULL;
    }
    for (int i = 0; i < parts.release_count; i++) {
        builder.items[i] = PyLong_FromLong(parts.release[i]);
        if (builder.items[i] == NULL) {
            tuple_builder_clear(&builder);
            return NULL;
        }
    }
    return tuple_builder_finish(&builder);
}

static PyObject *
get_pre_impl(PyObject *self, void *Py_UNUSED(context))
{
    VersionObject *version = version_cast(self);
    if (!is_small(version)) {
        return Py_NewRef(version->value.full->parts[PART_PRE]);
    }
    SmallParts parts = unpack_small(version);
    if (parts.pre_letter == PRE_NONE) {
        Py_RETURN_NONE;
    }
    return Py_BuildValue("(sI)", PRE_NAMES[parts.pre_letter],
                         (unsigned int)parts.pre_n);
}

static PyObject *
get_post_impl(PyObject *self, void *Py_UNUSED(context))
{
    VersionObject *version = version_cast(self);
    if (!is_small(version)) {
        return Py_NewRef(version->value.full->parts[PART_POST]);
    }
    SmallParts parts = unpack_small(version);
    if (!(parts.flags & HAS_POST)) {
        Py_RETURN_NONE;
    }
    return PyLong_FromLong(parts.post_n);
}

static PyObject *
get_dev_impl(PyObject *self, void *Py_UNUSED(context))
{
    VersionObject *version = version_cast(self);
    if (!is_small(version)) {
        return Py_NewRef(version->value.full->parts[PART_DEV]);
    }
    SmallParts parts = unpack_small(version);
    if (!(parts.flags & HAS_DEV)) {
        Py_RETURN_NONE;
    }
    return PyLong_FromLong(parts.dev_n);
}

static int
write_local(PyUnicodeWriter *writer, PyObject *local)
{
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(local); i++) {
        if (i != 0 && PyUnicodeWriter_WriteChar(writer, '.') < 0) {
            return -1;
        }
        if (PyUnicodeWriter_WriteStr(writer, PyTuple_GET_ITEM(local, i)) < 0) {
            return -1;
        }
    }
    return 0;
}

static PyObject *
get_local_impl(PyObject *self, void *Py_UNUSED(context))
{
    VersionObject *version = version_cast(self);
    if (is_small(version)) {
        Py_RETURN_NONE;
    }
    PyObject *local = version->value.full->parts[PART_LOCAL];
    if (local == Py_None) {
        Py_RETURN_NONE;
    }
    PyUnicodeWriter *writer = PyUnicodeWriter_Create(0);
    if (writer == NULL) {
        return NULL;
    }
    if (write_local(writer, local) < 0) {
        PyUnicodeWriter_Discard(writer);
        return NULL;
    }
    return PyUnicodeWriter_Finish(writer);
}

static PyObject *
get_is_prerelease_impl(PyObject *self, void *Py_UNUSED(context))
{
    VersionObject *version = version_cast(self);
    if (is_small(version)) {
        SmallParts parts = unpack_small(version);
        return PyBool_FromLong(parts.pre_letter != PRE_NONE ||
                               (parts.flags & HAS_DEV));
    }
    PyObject **parts = version->value.full->parts;
    return PyBool_FromLong(parts[PART_PRE] != Py_None ||
                           parts[PART_DEV] != Py_None);
}

static PyObject *
get_is_postrelease_impl(PyObject *self, void *Py_UNUSED(context))
{
    VersionObject *version = version_cast(self);
    return PyBool_FromLong(
        is_small(version) ? unpack_small(version).flags & HAS_POST
                          : version->value.full->parts[PART_POST] != Py_None);
}

static PyObject *
get_is_devrelease_impl(PyObject *self, void *Py_UNUSED(context))
{
    VersionObject *version = version_cast(self);
    return PyBool_FromLong(
        is_small(version) ? unpack_small(version).flags & HAS_DEV
                          : version->value.full->parts[PART_DEV] != Py_None);
}

static PyObject *
release_item(PyObject *self, Py_ssize_t index)
{
    VersionObject *version = version_cast(self);
    if (is_small(version)) {
        SmallParts parts = unpack_small(version);
        return PyLong_FromLong(
            index < parts.release_count ? parts.release[index] : 0);
    }
    PyObject *release = version->value.full->parts[PART_RELEASE];
    if (index >= PyTuple_GET_SIZE(release)) {
        return PyLong_FromLong(0);
    }
    return Py_NewRef(PyTuple_GET_ITEM(release, index));
}

static PyObject *
get_major_impl(PyObject *self, void *Py_UNUSED(context))
{
    return release_item(self, 0);
}

static PyObject *
get_minor_impl(PyObject *self, void *Py_UNUSED(context))
{
    return release_item(self, 1);
}

static PyObject *
get_micro_impl(PyObject *self, void *Py_UNUSED(context))
{
    return release_item(self, 2);
}

enum { FORMAT_ALL, FORMAT_PUBLIC, FORMAT_BASE };

static char *
write_small_number(char *output, uint16_t number)
{
    if (number < 10) {
        *output++ = (char)('0' + number);
        return output;
    }

    char digits[5];  /* UINT16_MAX has five decimal digits. */
    char *end = digits + sizeof(digits);
    char *start = end;
    do {
        *--start = (char)('0' + number % 10);
        number /= 10;
    } while (number != 0);

    size_t length = (size_t)(end - start);
    memcpy(output, start, length);
    return output + length;
}

static PyObject *
format_small_version(const VersionObject *version, int mode)
{
    SmallParts parts = unpack_small(version);
    /* Seven five-digit numbers, three dots, "rc", ".post" and ".dev". */
    char buffer[7 * 5 + 3 + 2 + 5 + 4];
    char *output = buffer;
    for (int i = 0; i < parts.release_count; i++) {
        if (i != 0) {
            *output++ = '.';
        }
        output = write_small_number(output, parts.release[i]);
    }
    if (mode != FORMAT_BASE) {
        if (parts.pre_letter != PRE_NONE) {
            *output++ = PRE_NAMES[parts.pre_letter][0];
            if (parts.pre_letter == PRE_RC) {
                *output++ = 'c';
            }
            output = write_small_number(output, parts.pre_n);
        }
        if (parts.flags & HAS_POST) {
            memcpy(output, ".post", 5);
            output = write_small_number(output + 5, parts.post_n);
        }
        if (parts.flags & HAS_DEV) {
            memcpy(output, ".dev", 4);
            output = write_small_number(output + 4, parts.dev_n);
        }
    }
    return PyUnicode_FromStringAndSize(buffer, output - buffer);
}

static PyObject *
format_version(PyObject *self, int mode)
{
    VersionObject *version = version_cast(self);
    if (is_small(version)) {
        return format_small_version(version, mode);
    }

    PyObject **parts = version->value.full->parts;
    PyUnicodeWriter *writer = PyUnicodeWriter_Create(0);
    if (writer == NULL) {
        return NULL;
    }
    int epoch = PyObject_IsTrue(parts[PART_EPOCH]);
    if (epoch < 0 ||
        (epoch && (PyUnicodeWriter_WriteStr(writer, parts[PART_EPOCH]) < 0 ||
                   PyUnicodeWriter_WriteChar(writer, '!') < 0))) {
        goto error;
    }
    PyObject *release = parts[PART_RELEASE];
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(release); i++) {
        if ((i != 0 && PyUnicodeWriter_WriteChar(writer, '.') < 0) ||
            PyUnicodeWriter_WriteStr(writer, PyTuple_GET_ITEM(release, i)) < 0) {
            goto error;
        }
    }
    if (mode == FORMAT_BASE) {
        return PyUnicodeWriter_Finish(writer);
    }
    PyObject *pre = parts[PART_PRE];
    if (pre != Py_None &&
        (PyUnicodeWriter_WriteStr(writer, PyTuple_GET_ITEM(pre, 0)) < 0 ||
         PyUnicodeWriter_WriteStr(writer, PyTuple_GET_ITEM(pre, 1)) < 0)) {
        goto error;
    }
    if (parts[PART_POST] != Py_None &&
        (PyUnicodeWriter_WriteASCII(writer, ".post", 5) < 0 ||
         PyUnicodeWriter_WriteStr(writer, parts[PART_POST]) < 0)) {
        goto error;
    }
    if (parts[PART_DEV] != Py_None &&
        (PyUnicodeWriter_WriteASCII(writer, ".dev", 4) < 0 ||
         PyUnicodeWriter_WriteStr(writer, parts[PART_DEV]) < 0)) {
        goto error;
    }
    if (mode == FORMAT_ALL && parts[PART_LOCAL] != Py_None) {
        if (PyUnicodeWriter_WriteChar(writer, '+') < 0 ||
            write_local(writer, parts[PART_LOCAL]) < 0) {
            goto error;
        }
    }
    return PyUnicodeWriter_Finish(writer);

error:
    PyUnicodeWriter_Discard(writer);
    return NULL;
}

static PyObject *
version_str(PyObject *self)
{
    DELEGATE_VERSION(self, PyObject *, version_str(current), NULL);
    return format_version(self, FORMAT_ALL);
}

static PyObject *
get_base_version_impl(PyObject *self, void *Py_UNUSED(context))
{
    return format_version(self, FORMAT_BASE);
}

static PyObject *
get_public_impl(PyObject *self, void *Py_UNUSED(context))
{
    return format_version(self, FORMAT_PUBLIC);
}

static PyObject *
version_repr(PyObject *self)
{
    PyObject *text = version_str(self);
    if (text == NULL) {
        return NULL;
    }
    PyObject *name = PyType_GetName(Py_TYPE(self));
    if (name == NULL) {
        Py_DECREF(text);
        return NULL;
    }
    PyObject *result = PyUnicode_FromFormat("<%U(%R)>", name, text);
    Py_DECREF(name);
    Py_DECREF(text);
    return result;
}

static PyObject *
version_sizeof(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    size_t size = (size_t)Py_TYPE(self)->tp_basicsize;
    VersionObject *version = version_cast(self);
    if (_Py_atomic_load_uint32_acquire(&version->ready) &&
        !is_small(version) && version->value.full != NULL) {
        size += sizeof(VersionFull);
    }
    return PyLong_FromSize_t(size);
}

static PyObject *
version_parts_dict(PyObject *self)
{
    PyObject *current = version_current(self);
    if (current == NULL) {
        return NULL;
    }
    PyObject *parts[PART_COUNT] = {0};
    int status = parts_from_version(current, parts);
    if (status == 0) {
        /* Factory keywords use text; full storage retains local segments. */
        PyObject *local = get_local_impl(current, NULL);
        if (local == NULL) {
            status = -1;
        }
        else {
            Py_SETREF(parts[PART_LOCAL], local);
        }
    }
    release_current(self, current);
    if (status < 0) {
        clear_parts(parts);
        return NULL;
    }
    PyObject *result = PyDict_New();
    if (result != NULL) {
        for (int i = 0; i < PART_COUNT; i++) {
            if (PyDict_SetItemString(result, PART_NAMES[i], parts[i]) < 0) {
                Py_CLEAR(result);
                break;
            }
        }
    }
    clear_parts(parts);
    return result;
}

static PyObject *
version_reduce(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PyObject *parts = version_parts_dict(self);
    if (parts == NULL) {
        return NULL;
    }
    PyObject *factory = NULL, *partial = NULL;
    PyObject *arguments = NULL, *constructor = NULL, *state = NULL;
    PyObject *empty = NULL, *result = NULL;
    factory = PyObject_GetAttrString((PyObject *)Py_TYPE(self), "from_parts");
    if (factory == NULL) {
        goto done;
    }
    partial = PyImport_ImportModuleAttrString("functools", "partial");
    if (partial == NULL) {
        goto done;
    }
    arguments = PyTuple_Pack(1, factory);
    if (arguments == NULL) {
        goto done;
    }
    /* Components retain int subclasses; their display text need not parse. */
    constructor = PyObject_Call(partial, arguments, parts);
    if (constructor == NULL) {
        goto done;
    }
    state = PyObject_CallMethod(self, "__getstate__", NULL);
    if (state == NULL) {
        goto done;
    }
    empty = PyTuple_New(0);
    if (empty != NULL) {
        result = PyTuple_Pack(3, constructor, empty, state);
    }
done:
    Py_DECREF(parts);
    Py_XDECREF(factory);
    Py_XDECREF(partial);
    Py_XDECREF(arguments);
    Py_XDECREF(constructor);
    Py_XDECREF(state);
    Py_XDECREF(empty);
    return result;
}

static int
parts_from_version(PyObject *self, PyObject **parts)
{
    VersionObject *version = version_cast(self);
    if (!is_small(version)) {
        for (int i = 0; i < PART_COUNT; i++) {
            parts[i] = Py_NewRef(version->value.full->parts[i]);
        }
        return 0;
    }
    static const getter getters[PART_COUNT] = {
        get_epoch_impl, get_release_impl, get_pre_impl,
        get_post_impl, get_dev_impl, get_local_impl};
    for (int i = 0; i < PART_COUNT; i++) {
        parts[i] = getters[i](self, NULL);
        if (parts[i] == NULL) {
            return -1;
        }
    }
    return 0;
}

/* arguments holds the new components; NULL keeps the current one. */
static PyObject *
replace_parts(PyObject *self, PyObject *const *arguments,
              PyTypeObject *result_type)
{
    PyObject *parts[PART_COUNT] = {0};
    for (int i = 0; i < PART_COUNT; i++) {
        if (arguments[i]) {
            parts[i] = normalize_part(i, arguments[i], result_type);
            if (!parts[i]) {
                clear_parts(parts);
                return NULL;
            }
        }
    }

    VersionObject *version = version_cast(self);
    if (is_small(version)) {
        SmallParts small = unpack_small(version);
        if (apply_small_parts(&small, parts)) {
            VersionKey key = pack_small_key(&small);
            int unchanged = small.release_count == version->release_len &&
                            key.high == version->value.key.high &&
                            key.low == version->value.key.low;
            PyObject *result = unchanged
                                   ? Py_NewRef(self)
                                   : make_small_parts(result_type, &small);
            clear_parts(parts);
            return result;
        }
    }

    PyObject *old[PART_COUNT] = {0};
    if (parts_from_version(self, old) < 0) {
        clear_parts(old);
        clear_parts(parts);
        return NULL;
    }

    int unchanged = 1;
    for (int i = 0; i < PART_COUNT; i++) {
        if (!parts[i]) {
            parts[i] = Py_NewRef(old[i]);
        }
        else if (unchanged) {
            int equal = PyObject_RichCompareBool(parts[i], old[i], Py_EQ);
            if (equal < 0) {
                clear_parts(old);
                clear_parts(parts);
                return NULL;
            }
            unchanged = equal;
        }
    }
    PyObject *result =
        unchanged ? Py_NewRef(self) : make_parts(result_type, parts);
    clear_parts(old);
    clear_parts(parts);
    return result;
}

/*[clinic input]
_version.Version.__replace__

    *
    epoch: object(c_default="NULL", py_default="...") = None
    release: object(c_default="NULL", py_default="...") = None
    pre: object(c_default="NULL", py_default="...") = None
    post: object(c_default="NULL", py_default="...") = None
    dev: object(c_default="NULL", py_default="...") = None
    local: object(c_default="NULL", py_default="...") = None

Replace components; omitted components retain their values.
[clinic start generated code]*/

static PyObject *
_version_Version___replace___impl(PyObject *self, PyObject *epoch,
                                  PyObject *release, PyObject *pre,
                                  PyObject *post, PyObject *dev,
                                  PyObject *local)
/*[clinic end generated code: output=f97988a570b681ba input=6fedfeba7daeb96e]*/
{
    PyObject *arguments[PART_COUNT] = {epoch, release, pre, post, dev, local};
    PyObject *current = version_current(self);
    if (current == NULL) {
        return NULL;
    }
    /* Keep the original subtype when replacing an immutable snapshot. */
    PyObject *result = replace_parts(current, arguments, Py_TYPE(self));
    if (result == current && current != self) {
        Py_SETREF(result, Py_NewRef(self));
    }
    release_current(self, current);
    return result;
}

/* Keep the general key flat and allocate it only when ordering needs it. */
static PyObject *
build_cmpkey(PyTypeObject *type, PyObject *const *parts)
{
    PyObject *release = parts[PART_RELEASE];
    Py_ssize_t length = PyTuple_GET_SIZE(release);
    while (length != 0) {
        PyObject *last = PyTuple_GET_ITEM(release, length - 1);
        int zero =
            PyLong_CheckExact(last)
                ? PyLong_IsZero(last)
                : PyObject_RichCompareBool(
                      last, Py_GetConstantBorrowed(Py_CONSTANT_ZERO), Py_EQ);
        if (zero < 0) {
            return NULL;
        }
        if (!zero) {
            break;
        }
        length--;
    }
    PyObject *trimmed = length == PyTuple_GET_SIZE(release)
                            ? Py_NewRef(release)
                            : PyTuple_GetSlice(release, 0, length);
    if (trimmed == NULL) {
        return NULL;
    }
    PyObject *pre = parts[PART_PRE];
    PyObject *post = parts[PART_POST];
    PyObject *dev = parts[PART_DEV];
    PyObject *suffix = NULL;
    PyObject *local_key = NULL;
    PyObject *result = NULL;
    if (pre == Py_None && post == Py_None && dev == Py_None) {
        VersionState *state = state_from_type(type);
        if (state == NULL) {
            goto done;
        }
        suffix = Py_NewRef(state->stable_suffix);
    }
    else {
        PyObject *zero = Py_GetConstantBorrowed(Py_CONSTANT_ZERO);
        int rank;
        PyObject *pre_number;
        if (pre == Py_None) {
            rank = post == Py_None && dev != Py_None ? -1 : 3;
            pre_number = zero;
        }
        else {
            rank = pre_letter_index(PyTuple_GET_ITEM(pre, 0));
            pre_number = PyTuple_GET_ITEM(pre, 1);
        }
        suffix = Py_BuildValue("(iOiOiO)", rank, pre_number, post != Py_None,
                               post == Py_None ? zero : post, dev == Py_None,
                               dev == Py_None ? zero : dev);
    }
    if (suffix == NULL) {
        goto done;
    }
    PyObject *local = parts[PART_LOCAL];
    if (local == Py_None) {
        result = PyTuple_Pack(3, parts[PART_EPOCH], trimmed, suffix);
        goto done;
    }
    TupleBuilder builder;
    if (tuple_builder_init(&builder, PyTuple_GET_SIZE(local)) < 0) {
        goto done;
    }
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(local); i++) {
        PyObject *part = PyTuple_GET_ITEM(local, i);
        PyObject *key = PyLong_Check(part) ? Py_BuildValue("(Os)", part, "")
                                           : Py_BuildValue("(iO)", -1, part);
        if (key == NULL) {
            tuple_builder_clear(&builder);
            goto done;
        }
        builder.items[i] = key;
    }
    local_key = tuple_builder_finish(&builder);
    if (local_key == NULL) {
        goto done;
    }
    result = PyTuple_Pack(4, parts[PART_EPOCH], trimmed, suffix, local_key);

done:
    Py_DECREF(trimmed);
    Py_XDECREF(suffix);
    Py_XDECREF(local_key);
    return result;
}

static PyObject *
get_cmpkey(PyObject *self)
{
    VersionObject *version = version_cast(self);
    if (is_small(version)) {
        PyObject *parts[PART_COUNT] = {0};
        if (parts_from_version(self, parts) < 0) {
            clear_parts(parts);
            return NULL;
        }
        PyObject *key = build_cmpkey(Py_TYPE(self), parts);
        clear_parts(parts);
        return key;
    }

    PyObject *key;
    Py_BEGIN_CRITICAL_SECTION(self);
    key = Py_XNewRef(version->value.full->key);
    Py_END_CRITICAL_SECTION();
    if (key != NULL) {
        return key;
    }
    if (Py_EnterRecursiveCall(" while building a version comparison key")) {
        return NULL;
    }
    PyObject *created =
        build_cmpkey(Py_TYPE(self), version->value.full->parts);
    Py_LeaveRecursiveCall();
    if (created == NULL) {
        return NULL;
    }
    /* Building may invoke an int subclass; publish only after it completes. */
    Py_BEGIN_CRITICAL_SECTION(self);
    if (version->value.full->key == NULL) {
        version->value.full->key = Py_NewRef(created);
    }
    key = Py_NewRef(version->value.full->key);
    Py_END_CRITICAL_SECTION();
    Py_DECREF(created);
    return key;
}

static PyObject *
compare_current_versions(PyObject *self, PyObject *other, int op)
{
    VersionObject *left = version_cast(self);
    VersionObject *right = version_cast(other);
    if (is_small(left) && is_small(right)) {
        VersionKey a = left->value.key;
        VersionKey b = right->value.key;
        int comparison = a.high < b.high   ? -1
                         : a.high > b.high ? 1
                         : a.low < b.low   ? -1
                         : a.low > b.low   ? 1
                                           : 0;
        Py_RETURN_RICHCOMPARE(comparison, 0, op);
    }
    PyObject *a = get_cmpkey(self);
    if (a == NULL) {
        return NULL;
    }
    PyObject *b = get_cmpkey(other);
    if (b == NULL) {
        Py_DECREF(a);
        return NULL;
    }
    PyObject *result = PyObject_RichCompare(a, b, op);
    Py_DECREF(a);
    Py_DECREF(b);
    return result;
}

static PyObject *
version_richcompare(PyObject *self, PyObject *other, int op)
{
    if (Py_TYPE(self) != Py_TYPE(other)) {
        VersionState *state = state_from_type(Py_TYPE(self));
        if (state == NULL) {
            return NULL;
        }
        if (!PyObject_TypeCheck(other, (PyTypeObject *)state->version_type)) {
            Py_RETURN_NOTIMPLEMENTED;
        }
    }
    PyObject *left = version_current(self);
    if (left == NULL) {
        return NULL;
    }
    PyObject *right = self == other ? left : version_current(other);
    PyObject *result = right == NULL ? NULL : compare_current_versions(left, right, op);
    release_current(self, left);
    if (other != self && right != NULL) {
        release_current(other, right);
    }
    return result;
}

static Py_hash_t
version_hash(PyObject *self)
{
    DELEGATE_VERSION(self, Py_hash_t, version_hash(current), -1);
    VersionObject *version = version_cast(self);
    Py_hash_t hash = _Py_atomic_load_ssize_relaxed(&version->hash);
    if (hash != -1) {
        return hash;
    }
    PyObject *key = get_cmpkey(self);
    if (key == NULL) {
        return -1;
    }
    hash = PyObject_Hash(key);
    Py_DECREF(key);
    if (hash == -1) {
        return -1;
    }
    Py_hash_t expected = -1;
    _Py_atomic_compare_exchange_ssize(&version->hash, &expected, hash);
    return expected == -1 ? hash : expected;
}

/* Each public getter reads the active snapshot, then calls its _impl. */
#define DELEGATING_GETTER(name) \
    static PyObject * \
    get_##name(PyObject *self, void *context) \
    { \
        DELEGATE_VERSION(self, PyObject *, get_##name##_impl(current, context), \
                         NULL); \
        return get_##name##_impl(self, context); \
    }

DELEGATING_GETTER(epoch)
DELEGATING_GETTER(release)
DELEGATING_GETTER(pre)
DELEGATING_GETTER(post)
DELEGATING_GETTER(dev)
DELEGATING_GETTER(local)
DELEGATING_GETTER(is_prerelease)
DELEGATING_GETTER(is_postrelease)
DELEGATING_GETTER(is_devrelease)
DELEGATING_GETTER(major)
DELEGATING_GETTER(minor)
DELEGATING_GETTER(micro)
DELEGATING_GETTER(base_version)
DELEGATING_GETTER(public)

PyDoc_STRVAR(
    sizeof_doc,
    "__sizeof__($self, /)\n"
    "--\n\n"
    "Include the separate full-representation allocation in the object size.");

PyDoc_STRVAR(reduce_doc,
             "__reduce__($self, /)\n"
             "--\n\n"
             "Return constructor arguments and subclass state for pickling.");

static PyMethodDef version_methods[] = {
    _VERSION_VERSION_FROM_PARTS_METHODDEF
    _VERSION_VERSION___REPLACE___METHODDEF
    {"__sizeof__", version_sizeof, METH_NOARGS, sizeof_doc},
    {"__reduce__", version_reduce, METH_NOARGS, reduce_doc},
    {"__setstate__", version_setstate, METH_O,
     "__setstate__($self, state, /)\n"
     "--\n\n"
     "Restore an empty value or subclass attributes."},
    {NULL, NULL, 0, NULL},
};

static PyGetSetDef version_getset[] = {
    {"epoch", get_epoch, NULL,
     "The epoch as an integer; zero when omitted.", NULL},
    {"release", get_release, NULL,
     "The release as a tuple of integers, including trailing zeros.", NULL},
    {"pre", get_pre, NULL,
     "A pair of 'a', 'b' or 'rc' and an integer, or None.", NULL},
    {"post", get_post, NULL, "The post-release number, or None.", NULL},
    {"dev", get_dev, NULL, "The development-release number, or None.", NULL},
    {"local", get_local, NULL,
     "The normalized local label as a string, or None.", NULL},
    {"is_prerelease", get_is_prerelease, NULL,
     "Whether the version has a pre-release or development-release component.",
     NULL},
    {"is_postrelease", get_is_postrelease, NULL,
     "Whether the version has a post-release component.", NULL},
    {"is_devrelease", get_is_devrelease, NULL,
     "Whether the version has a development-release component.", NULL},
    {"major", get_major, NULL,
     "The first release integer.", NULL},
    {"minor", get_minor, NULL,
     "The second release integer; zero when omitted.", NULL},
    {"micro", get_micro, NULL,
     "The third release integer; zero when omitted.", NULL},
    {"base_version", get_base_version, NULL,
     "The epoch and release as a string.", NULL},
    {"public", get_public, NULL,
     "The normalized version string without its local label.", NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static int
version_traverse(PyObject *self, visitproc visit, void *arg)
{
    VersionObject *version = version_cast(self);
    Py_VISIT(Py_TYPE(self));
    Py_VISIT(version->replacement);
    if (!is_small(version) && version->value.full != NULL) {
        for (int i = 0; i < PART_COUNT; i++) {
            Py_VISIT(version->value.full->parts[i]);
        }
        Py_VISIT(version->value.full->key);
    }
    return 0;
}

static int
version_clear(PyObject *self)
{
    VersionObject *version = version_cast(self);
    Py_CLEAR(version->replacement);
    if (!is_small(version) && version->value.full != NULL) {
        VersionFull *full = version->value.full;
        version->value.full = NULL;
        free_full(full);
    }
    return 0;
}

static void
version_dealloc(PyObject *self)
{
    PyTypeObject *type = Py_TYPE(self);
    PyObject_GC_UnTrack(self);
    version_clear(self);
    type->tp_free(self);
    Py_DECREF(type);
}

PyDoc_STRVAR(version_doc, "Version(version, /)\n"
                          "--\n\n"
                          "An immutable distribution version value.");

static PyType_Slot version_slots[] = {
    {Py_tp_doc, (void *)version_doc},
    {Py_tp_init, version_init},
    {Py_tp_vectorcall, version_vectorcall},
    {Py_tp_dealloc, version_dealloc},
    {Py_tp_traverse, version_traverse},
    {Py_tp_richcompare, version_richcompare},
    {Py_tp_hash, version_hash},
    {Py_tp_str, version_str},
    {Py_tp_repr, version_repr},
    {Py_tp_getset, version_getset},
    {Py_tp_methods, version_methods},
    {0, NULL},
};

static PyType_Spec version_spec = {
    .name = "importlib.version.Version",
    .basicsize = sizeof(VersionObject),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_GC |
             Py_TPFLAGS_IMMUTABLETYPE,
    .slots = version_slots,
};

static int
version_exec(PyObject *module)
{
    VersionState *state = PyModule_GetState(module);
    state->invalid_version =
        PyErr_NewException("importlib.version.InvalidVersion", PyExc_ValueError, NULL);
    if (state->invalid_version == NULL) {
        return -1;
    }
    if (PyModule_AddObjectRef(module, "InvalidVersion",
                              state->invalid_version) < 0) {
        return -1;
    }
    state->stable_suffix = Py_BuildValue("(iiiiii)", 3, 0, 0, 0, 1, 0);
    if (state->stable_suffix == NULL) {
        return -1;
    }
    state->version_type = PyType_FromModuleAndSpec(module, &version_spec, NULL);
    if (state->version_type == NULL) {
        return -1;
    }
    if (PyModule_AddType(module, (PyTypeObject *)state->version_type) < 0) {
        return -1;
    }
    return PyModule_Add(module, "__all__",
                        Py_BuildValue("[ss]", "InvalidVersion", "Version"));
}

static PyModuleDef_Slot module_slots[] = {
    {Py_mod_exec, version_exec},
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
    {0, NULL},
};

static int
module_traverse(PyObject *module, visitproc visit, void *arg)
{
    VersionState *state = PyModule_GetState(module);
    Py_VISIT(state->version_type);
    Py_VISIT(state->invalid_version);
    Py_VISIT(state->stable_suffix);
    return 0;
}

static int
module_clear(PyObject *module)
{
    VersionState *state = PyModule_GetState(module);
    Py_CLEAR(state->version_type);
    Py_CLEAR(state->invalid_version);
    Py_CLEAR(state->stable_suffix);
    return 0;
}

static void
module_free(void *module)
{
    module_clear((PyObject *)module);
}

static struct PyModuleDef version_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "importlib.version",
    .m_doc = "Distribution version values following the Python packaging specification.",
    .m_size = sizeof(VersionState),
    .m_slots = module_slots,
    .m_traverse = module_traverse,
    .m_clear = module_clear,
    .m_free = module_free,
};

PyMODINIT_FUNC
PyInit_version(void)
{
    return PyModuleDef_Init(&version_module);
}
