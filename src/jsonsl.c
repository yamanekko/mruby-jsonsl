/* Copyright (C) 2012-2015 Mark Nunberg.
 *
 * See included LICENSE file for license details.
 */

#include "jsonsl.h"
#include <assert.h>
#include <limits.h>
#include <ctype.h>

#ifdef JSONSL_USE_METRICS
#define XMETRICS \
    X(STRINGY_INSIGNIFICANT) \
    X(STRINGY_SLOWPATH) \
    X(ALLOWED_WHITESPACE) \
    X(QUOTE_FASTPATH) \
    X(SPECIAL_FASTPATH) \
    X(SPECIAL_WSPOP) \
    X(SPECIAL_SLOWPATH) \
    X(GENERIC) \
    X(STRUCTURAL_TOKEN) \
    X(SPECIAL_SWITCHFIRST) \
    X(STRINGY_CATCH) \
    X(ESCAPES) \
    X(TOTAL) \

struct jsonsl_metrics_st {
#define X(m) \
    unsigned long metric_##m;
    XMETRICS
#undef X
};

static struct jsonsl_metrics_st GlobalMetrics = { 0 };
static unsigned long GenericCounter[0x100] = { 0 };
static unsigned long StringyCatchCounter[0x100] = { 0 };

#define INCR_METRIC(m) \
    GlobalMetrics.metric_##m++;

#define INCR_GENERIC(c) \
        INCR_METRIC(GENERIC); \
        GenericCounter[c]++; \

#define INCR_STRINGY_CATCH(c) \
    INCR_METRIC(STRINGY_CATCH); \
    StringyCatchCounter[c]++;

JSONSL_API
void jsonsl_dump_global_metrics(void)
{
    int ii;
    printf("JSONSL Metrics:\n");
#define X(m) \
    printf("\t%-30s %20lu (%0.2f%%)\n", #m, GlobalMetrics.metric_##m, \
           (float)((float)(GlobalMetrics.metric_##m/(float)GlobalMetrics.metric_TOTAL)) * 100);
    XMETRICS
#undef X
    printf("Generic Characters:\n");
    for (ii = 0; ii < 0xff; ii++) {
        if (GenericCounter[ii]) {
            printf("\t[ %c ] %lu\n", ii, GenericCounter[ii]);
        }
    }
    printf("Weird string loop\n");
    for (ii = 0; ii < 0xff; ii++) {
        if (StringyCatchCounter[ii]) {
            printf("\t[ %c ] %lu\n", ii, StringyCatchCounter[ii]);
        }
    }
}

#else
#define INCR_METRIC(m)
#define INCR_GENERIC(c)
#define INCR_STRINGY_CATCH(c)
JSONSL_API
void jsonsl_dump_global_metrics(void) { }
#endif /* JSONSL_USE_METRICS */

#define CASE_DIGITS \
case '1': \
case '2': \
case '3': \
case '4': \
case '5': \
case '6': \
case '7': \
case '8': \
case '9': \
case '0':

static unsigned extract_special(unsigned);
static int is_special_end(unsigned);
static int is_allowed_whitespace(unsigned);
static int is_allowed_escape(unsigned);
static char get_escape_equiv(unsigned);

JSONSL_API
jsonsl_t jsonsl_new(int nlevels)
{
    struct jsonsl_st *jsn = (struct jsonsl_st *)
            calloc(1, sizeof (*jsn) +
                    ( (nlevels-1) * sizeof (struct jsonsl_state_st) )
            );

    jsn->levels_max = nlevels;
    jsn->max_callback_level = -1;
    jsonsl_reset(jsn);
    return jsn;
}

JSONSL_API
void jsonsl_reset(jsonsl_t jsn)
{
    unsigned int ii;
    jsn->tok_last = 0;
    jsn->can_insert = 1;
    jsn->pos = 0;
    jsn->level = 0;
    jsn->stopfl = 0;
    jsn->in_escape = 0;
    jsn->expecting = 0;

    memset(jsn->stack, 0, (jsn->levels_max * sizeof (struct jsonsl_state_st)));

    for (ii = 0; ii < jsn->levels_max; ii++) {
        jsn->stack[ii].level = ii;
    }
}

JSONSL_API
void jsonsl_destroy(jsonsl_t jsn)
{
    if (jsn) {
        free(jsn);
    }
}

JSONSL_API
void
jsonsl_feed(jsonsl_t jsn, const jsonsl_char_t *bytes, size_t nbytes)
{

#define INVOKE_ERROR(eb) \
    if (jsn->error_callback(jsn, JSONSL_ERROR_##eb, state, (char*)c)) { \
        goto GT_AGAIN; \
    } \
    return;

#define STACK_PUSH \
    if (jsn->level >= (levels_max-1)) { \
        jsn->error_callback(jsn, JSONSL_ERROR_LEVELS_EXCEEDED, state, (char*)c); \
        return; \
    } \
    state = jsn->stack + (++jsn->level); \
    state->ignore_callback = jsn->stack[jsn->level-1].ignore_callback; \
    state->pos_begin = jsn->pos;

#define STACK_POP_NOPOS \
    state->pos_cur = jsn->pos; \
    state = jsn->stack + (--jsn->level);


#define STACK_POP \
    STACK_POP_NOPOS; \
    state->pos_cur = jsn->pos;

#define CALLBACK_AND_POP_NOPOS(T) \
        state->pos_cur = jsn->pos; \
        DO_CALLBACK(T, POP); \
        state->nescapes = 0; \
        state = jsn->stack + (--jsn->level);

#define CALLBACK_AND_POP(T) \
        CALLBACK_AND_POP_NOPOS(T); \
        state->pos_cur = jsn->pos;

#define SPECIAL_POP \
    CALLBACK_AND_POP(SPECIAL); \
    jsn->expecting = 0; \
    jsn->tok_last = 0; \

#define CUR_CHAR (*(jsonsl_uchar_t*)c)

#define DO_CALLBACK(T, action) \
    if (jsn->call_##T && \
            jsn->max_callback_level > state->level && \
            state->ignore_callback == 0) { \
        \
        if (jsn->action_callback_##action) { \
            jsn->action_callback_##action(jsn, JSONSL_ACTION_##action, state, (jsonsl_char_t*)c); \
        } else if (jsn->action_callback) { \
            jsn->action_callback(jsn, JSONSL_ACTION_##action, state, (jsonsl_char_t*)c); \
        } \
        if (jsn->stopfl) { return; } \
    }

    /**
     * Verifies that we are able to insert the (non-string) item into a hash.
     */
#define ENSURE_HVAL \
    if (state->nelem % 2 == 0 && state->type == JSONSL_T_OBJECT) { \
        INVOKE_ERROR(HKEY_EXPECTED); \
    }

#define VERIFY_SPECIAL(lit) \
        if (CUR_CHAR != (lit)[jsn->pos - state->pos_begin]) { \
            INVOKE_ERROR(SPECIAL_EXPECTED); \
        }

#define STATE_SPECIAL_LENGTH \
    (state)->nescapes

#define IS_NORMAL_NUMBER \
    ((state)->special_flags == JSONSL_SPECIALf_UNSIGNED || \
        (state)->special_flags == JSONSL_SPECIALf_SIGNED)

#define STATE_NUM_LAST jsn->tok_last

    const jsonsl_uchar_t *c = (jsonsl_uchar_t*)bytes;
    size_t levels_max = jsn->levels_max;
    struct jsonsl_state_st *state = jsn->stack + jsn->level;
    static int chrt_string_nopass[0x100] = { JSONSL_CHARTABLE_string_nopass };
    jsn->base = bytes;

    for (; nbytes; nbytes--, jsn->pos++, c++) {
        unsigned state_type;
        INCR_METRIC(TOTAL);
        /* Special escape handling for some stuff */
        if (jsn->in_escape) {
            jsn->in_escape = 0;
            if (!is_allowed_escape(CUR_CHAR)) {
                INVOKE_ERROR(ESCAPE_INVALID);
            } else if (CUR_CHAR == 'u') {
                DO_CALLBACK(UESCAPE, UESCAPE);
                if (jsn->return_UESCAPE) {
                    return;
                }
            }
            goto GT_NEXT;
        }
        GT_AGAIN:
        /**
         * Several fast-tracks for common cases:
         */
        state_type = state->type;
        if (state_type & JSONSL_Tf_STRINGY) {
            /* check if our character cannot ever change our current string state
             * or throw an error
             */
            if (
#ifdef JSONSL_USE_WCHAR
                    CUR_CHAR >= 0x100 ||
#endif /* JSONSL_USE_WCHAR */
                    (!chrt_string_nopass[CUR_CHAR & 0xff])) {
                INCR_METRIC(STRINGY_INSIGNIFICANT);
                goto GT_NEXT;
            } else if (CUR_CHAR == '"') {
                goto GT_QUOTE;
            } else if (CUR_CHAR == '\\') {
                goto GT_ESCAPE;
            } else {
                INVOKE_ERROR(WEIRD_WHITESPACE);
            }
            INCR_METRIC(STRINGY_SLOWPATH);

        } else if (state_type == JSONSL_T_SPECIAL) {
            /* Fast track for signed/unsigned */
            if (IS_NORMAL_NUMBER) {
                if (isdigit(CUR_CHAR)) {
                    state->nelem = (state->nelem * 10) + (CUR_CHAR-0x30);
                    goto GT_NEXT;
                } else {
                    goto GT_SPECIAL_NUMERIC;
                }

            } else if (state->special_flags == JSONSL_SPECIALf_DASH) {
                if (!isdigit(CUR_CHAR)) {
                    INVOKE_ERROR(INVALID_NUMBER);
                }

                if (CUR_CHAR == '0') {
                    state->special_flags = JSONSL_SPECIALf_ZERO|JSONSL_SPECIALf_SIGNED;
                } else if (isdigit(CUR_CHAR)) {
                    state->special_flags = JSONSL_SPECIALf_SIGNED;
                    state->nelem = CUR_CHAR - 0x30;
                } else {
                    INVOKE_ERROR(INVALID_NUMBER);
                }

                goto GT_NEXT;

            } else if (state->special_flags == JSONSL_SPECIALf_ZERO) {
                if (isdigit(CUR_CHAR)) {
                    /* Following a zero! */
                    INVOKE_ERROR(INVALID_NUMBER);
                }
                /* Unset the 'zero' flag: */
                if (state->special_flags & JSONSL_SPECIALf_SIGNED) {
                    state->special_flags = JSONSL_SPECIALf_SIGNED;
                } else {
                    state->special_flags = JSONSL_SPECIALf_UNSIGNED;
                }
                goto GT_SPECIAL_NUMERIC;
            }

            if (state->special_flags & JSONSL_SPECIALf_NUMERIC) {
                GT_SPECIAL_NUMERIC:
                switch (CUR_CHAR) {
                CASE_DIGITS
                    STATE_NUM_LAST = '1';
                    goto GT_NEXT;

                case '.':
                    if (state->special_flags & JSONSL_SPECIALf_FLOAT) {
                        INVOKE_ERROR(INVALID_NUMBER);
                    }
                    state->special_flags |= JSONSL_SPECIALf_FLOAT;
                    STATE_NUM_LAST = '.';
                    goto GT_NEXT;

                case 'e':
                case 'E':
                    if (state->special_flags & JSONSL_SPECIALf_EXPONENT) {
                        INVOKE_ERROR(INVALID_NUMBER);
                    }
                    state->special_flags |= JSONSL_SPECIALf_EXPONENT;
                    STATE_NUM_LAST = 'e';
                    goto GT_NEXT;

                case '-':
                case '+':
                    if (STATE_NUM_LAST != 'e') {
                        INVOKE_ERROR(INVALID_NUMBER);
                    }
                    STATE_NUM_LAST = '-';
                    goto GT_NEXT;

                default:
                    if (is_special_end(CUR_CHAR)) {
                        goto GT_SPECIAL_POP;
                    }
                    INVOKE_ERROR(INVALID_NUMBER);
                    break;
                }
            }
            /* else if (!NUMERIC) */
            if (!is_special_end(CUR_CHAR)) {
                STATE_SPECIAL_LENGTH++;

                /* Verify TRUE, FALSE, NULL */
                if (state->special_flags == JSONSL_SPECIALf_TRUE) {
                    VERIFY_SPECIAL("true");
                } else if (state->special_flags == JSONSL_SPECIALf_FALSE) {
                    VERIFY_SPECIAL("false");
                } else if (state->special_flags == JSONSL_SPECIALf_NULL) {
                    VERIFY_SPECIAL("null");
                }
                INCR_METRIC(SPECIAL_FASTPATH);
                goto GT_NEXT;
            }

            GT_SPECIAL_POP:
            if (IS_NORMAL_NUMBER) {
                /* Nothing */
            } else if (state->special_flags == JSONSL_SPECIALf_ZERO ||
                    state->special_flags == (JSONSL_SPECIALf_ZERO|JSONSL_SPECIALf_SIGNED)) {
                /* 0 is unsigned! */
                state->special_flags = JSONSL_SPECIALf_UNSIGNED;
            } else if (state->special_flags == JSONSL_SPECIALf_DASH) {
                /* Still in dash! */
                INVOKE_ERROR(INVALID_NUMBER);
            } else if (state->special_flags & JSONSL_SPECIALf_NUMERIC) {
                /* Check that we're not at the end of a token */
                if (STATE_NUM_LAST != '1') {
                    INVOKE_ERROR(INVALID_NUMBER);
                }
            } else if (state->special_flags == JSONSL_SPECIALf_TRUE) {
                if (STATE_SPECIAL_LENGTH != 4) {
                    INVOKE_ERROR(SPECIAL_INCOMPLETE);
                }
                state->nelem = 1;
            } else if (state->special_flags == JSONSL_SPECIALf_FALSE) {
                if (STATE_SPECIAL_LENGTH != 5) {
                    INVOKE_ERROR(SPECIAL_INCOMPLETE);
                }
            } else if (state->special_flags == JSONSL_SPECIALf_NULL) {
                if (STATE_SPECIAL_LENGTH != 4) {
                    INVOKE_ERROR(SPECIAL_INCOMPLETE);
                }
            }
            SPECIAL_POP;
            jsn->expecting = ',';
            if (is_allowed_whitespace(CUR_CHAR)) {
                goto GT_NEXT;
            }
            /**
             * This works because we have a non-whitespace token
             * which is not a special token. If this is a structural
             * character then it will be gracefully handled by the
             * switch statement. Otherwise it will default to the 'special'
             * state again,
             */
            goto GT_STRUCTURAL_TOKEN;
        } else if (is_allowed_whitespace(CUR_CHAR)) {
            INCR_METRIC(ALLOWED_WHITESPACE);
            /* So we're not special. Harmless insignificant whitespace
             * passthrough
             */
            goto GT_NEXT;
        } else if (extract_special(CUR_CHAR)) {
            /* not a string, whitespace, or structural token. must be special */
            goto GT_SPECIAL_BEGIN;
        }

        INCR_GENERIC(CUR_CHAR);

        if (CUR_CHAR == '"') {
            GT_QUOTE:
            jsn->can_insert = 0;
            switch (state_type) {

            /* the end of a string or hash key */
            case JSONSL_T_STRING:
                CALLBACK_AND_POP(STRING);
                goto GT_NEXT;
            case JSONSL_T_HKEY:
                CALLBACK_AND_POP(HKEY);
                goto GT_NEXT;

            case JSONSL_T_OBJECT:
                state->nelem++;
                if ( (state->nelem-1) % 2 ) {
                    /* Odd, this must be a hash value */
                    if (jsn->tok_last != ':') {
                        INVOKE_ERROR(MISSING_TOKEN);
                    }
                    jsn->expecting = ','; /* Can't figure out what to expect next */
                    jsn->tok_last = 0;

                    STACK_PUSH;
                    state->type = JSONSL_T_STRING;
                    DO_CALLBACK(STRING, PUSH);

                } else {
                    /* hash key */
                    if (jsn->expecting != '"') {
                        INVOKE_ERROR(STRAY_TOKEN);
                    }
                    jsn->tok_last = 0;
                    jsn->expecting = ':';

                    STACK_PUSH;
                    state->type = JSONSL_T_HKEY;
                    DO_CALLBACK(HKEY, PUSH);
                }
                goto GT_NEXT;

            case JSONSL_T_LIST:
                state->nelem++;
                STACK_PUSH;
                state->type = JSONSL_T_STRING;
                jsn->expecting = ',';
                jsn->tok_last = 0;
                DO_CALLBACK(STRING, PUSH);
                goto GT_NEXT;

            case JSONSL_T_SPECIAL:
                INVOKE_ERROR(STRAY_TOKEN);
                break;

            default:
                INVOKE_ERROR(STRING_OUTSIDE_CONTAINER);
                break;
            } /* switch(state->type) */
        } else if (CUR_CHAR == '\\') {
            GT_ESCAPE:
            INCR_METRIC(ESCAPES);
        /* Escape */
            if ( (state->type & JSONSL_Tf_STRINGY) == 0 ) {
                INVOKE_ERROR(ESCAPE_OUTSIDE_STRING);
            }
            state->nescapes++;
            jsn->in_escape = 1;
            goto GT_NEXT;
        } /* " or \ */

        GT_STRUCTURAL_TOKEN:
        switch (CUR_CHAR) {
        case ':':
            INCR_METRIC(STRUCTURAL_TOKEN);
            if (jsn->expecting != CUR_CHAR) {
                INVOKE_ERROR(STRAY_TOKEN);
            }
            jsn->tok_last = ':';
            jsn->can_insert = 1;
            jsn->expecting = '"';
            goto GT_NEXT;

        case ',':
            INCR_METRIC(STRUCTURAL_TOKEN);
            /**
             * The comma is one of the more generic tokens.
             * In the context of an OBJECT, the can_insert flag
             * should never be set, and no other action is
             * necessary.
             */
            if (jsn->expecting != CUR_CHAR) {
                /* make this branch execute only when we haven't manually
                 * just placed the ',' in the expecting register.
                 */
                INVOKE_ERROR(STRAY_TOKEN);
            }

            if (state->type == JSONSL_T_OBJECT) {
                /* end of hash value, expect a string as a hash key */
                jsn->expecting = '"';
            } else {
                jsn->can_insert = 1;
            }

            jsn->tok_last = ',';
            jsn->expecting = '"';
            goto GT_NEXT;

            /* new list or object */
            /* hashes are more common */
        case '{':
        case '[':
            INCR_METRIC(STRUCTURAL_TOKEN);
            if (!jsn->can_insert) {
                INVOKE_ERROR(CANT_INSERT);
            }

            ENSURE_HVAL;
            state->nelem++;

            STACK_PUSH;
            /* because the constants match the opening delimiters, we can do this: */
            state->type = CUR_CHAR;
            state->nelem = 0;
            jsn->can_insert = 1;
            if (CUR_CHAR == '{') {
                /* If we're a hash, we expect a key first, which is quouted */
                jsn->expecting = '"';
            }
            if (CUR_CHAR == JSONSL_T_OBJECT) {
                DO_CALLBACK(OBJECT, PUSH);
            } else {
                DO_CALLBACK(LIST, PUSH);
            }
            jsn->tok_last = 0;
            goto GT_NEXT;

            /* closing of list or object */
        case '}':
        case ']':
            INCR_METRIC(STRUCTURAL_TOKEN);
            if (jsn->tok_last == ',' && jsn->options.allow_trailing_comma == 0) {
                INVOKE_ERROR(TRAILING_COMMA);
            }

            jsn->can_insert = 0;
            jsn->level--;
            jsn->expecting = ',';
            jsn->tok_last = 0;
            if (CUR_CHAR == ']') {
                if (state->type != '[') {
                    INVOKE_ERROR(BRACKET_MISMATCH);
                }
                DO_CALLBACK(LIST, POP);
            } else {
                if (state->type != '{') {
                    INVOKE_ERROR(BRACKET_MISMATCH);
                } else if (state->nelem && state->nelem % 2 != 0) {
                    INVOKE_ERROR(VALUE_EXPECTED);
                }
                DO_CALLBACK(OBJECT, POP);
            }
            state = jsn->stack + jsn->level;
            state->pos_cur = jsn->pos;
            goto GT_NEXT;

        default:
            GT_SPECIAL_BEGIN:
            /**
             * Not a string, not a structural token, and not benign whitespace.
             * Technically we should iterate over the character always, but since
             * we are not doing full numerical/value decoding anyway (but only hinting),
             * we only check upon entry.
             */
            if (state->type != JSONSL_T_SPECIAL) {
                int special_flags = extract_special(CUR_CHAR);
                if (!special_flags) {
                    /**
                     * Try to do some heuristics here anyway to figure out what kind of
                     * error this is. The 'special' case is a fallback scenario anyway.
                     */
                    if (CUR_CHAR == '\0') {
                        INVOKE_ERROR(FOUND_NULL_BYTE);
                    } else if (CUR_CHAR < 0x20) {
                        INVOKE_ERROR(WEIRD_WHITESPACE);
                    } else {
                        INVOKE_ERROR(SPECIAL_EXPECTED);
                    }
                }
                ENSURE_HVAL;
                state->nelem++;
                if (!jsn->can_insert) {
                    INVOKE_ERROR(CANT_INSERT);
                }
                STACK_PUSH;
                state->type = JSONSL_T_SPECIAL;
                state->special_flags = special_flags;
                STATE_SPECIAL_LENGTH = 1;

                if (special_flags == JSONSL_SPECIALf_UNSIGNED) {
                    state->nelem = CUR_CHAR - 0x30;
                    STATE_NUM_LAST = '1';
                } else {
                    STATE_NUM_LAST = '-';
                    state->nelem = 0;
                }
                DO_CALLBACK(SPECIAL, PUSH);
            }
            goto GT_NEXT;
        }

        GT_NEXT:
        continue;
    }
}

JSONSL_API
const char* jsonsl_strerror(jsonsl_error_t err)
{
    if (err == JSONSL_ERROR_SUCCESS) {
        return "SUCCESS";
    }
#define X(t) \
    if (err == JSONSL_ERROR_##t) \
        return #t;
    JSONSL_XERR;
#undef X
    return "<UNKNOWN_ERROR>";
}

JSONSL_API
const char *jsonsl_strtype(jsonsl_type_t type)
{
#define X(o,c) \
    if (type == JSONSL_T_##o) \
        return #o;
    JSONSL_XTYPE
#undef X
    return "UNKNOWN TYPE";

}

/*
 *
 * JPR/JSONPointer functions
 *
 *
 */
#ifndef JSONSL_NO_JPR
static
jsonsl_jpr_type_t
populate_component(char *in,
                   struct jsonsl_jpr_component_st *component,
                   char **next,
                   jsonsl_error_t *errp)
{
    unsigned long pctval;
    char *c = NULL, *outp = NULL, *end = NULL;
    size_t input_len;
    jsonsl_jpr_type_t ret = JSONSL_PATH_NONE;

    if (*next == NULL || *(*next) == '\0') {
        return JSONSL_PATH_NONE;
    }

    /* Replace the next / with a NULL */
    *next = strstr(in, "/");
    if (*next != NULL) {
        *(*next) = '\0'; /* drop the forward slash */
        input_len = *next - in;
        end = *next;
        *next += 1; /* next character after the '/' */
    } else {
        input_len = strlen(in);
        end = in + input_len + 1;
    }

    component->pstr = in;

    /* Check for special components of interest */
    if (*in == JSONSL_PATH_WILDCARD_CHAR && input_len == 1) {
        /* Lone wildcard */
        ret = JSONSL_PATH_WILDCARD;
        goto GT_RET;
    } else if (isdigit(*in)) {
        /* ASCII Numeric */
        char *endptr;
        component->idx = strtoul(in, &endptr, 10);
        if (endptr && *endptr == '\0') {
            ret = JSONSL_PATH_NUMERIC;
            goto GT_RET;
        }
    }

    /* Default, it's a string */
    ret = JSONSL_PATH_STRING;
    for (c = outp = in; c < end; c++, outp++) {
        char origc;
        if (*c != '%') {
            goto GT_ASSIGN;
        }
        /*
         * c = { [+0] = '%', [+1] = 'b', [+2] = 'e', [+3] = '\0' }
         */

        /* Need %XX */
        if (c+2 >= end) {
            *errp = JSONSL_ERROR_PERCENT_BADHEX;
            return JSONSL_PATH_INVALID;
        }
        if (! (isxdigit(*(c+1)) && isxdigit(*(c+2))) ) {
            *errp = JSONSL_ERROR_PERCENT_BADHEX;
            return JSONSL_PATH_INVALID;
        }

        /* Temporarily null-terminate the characters */
        origc = *(c+3);
        *(c+3) = '\0';
        pctval = strtoul(c+1, NULL, 16);
        *(c+3) = origc;

        *outp = (char) pctval;
        c += 2;
        continue;

        GT_ASSIGN:
        *outp = *c;
    }
    /* Null-terminate the string */
    for (; outp < c; outp++) {
        *outp = '\0';
    }

    GT_RET:
    component->ptype = ret;
    if (ret != JSONSL_PATH_WILDCARD) {
        component->len = strlen(component->pstr);
    }
    return ret;
}

JSONSL_API
jsonsl_jpr_t
jsonsl_jpr_new(const char *path, jsonsl_error_t *errp)
{
    char *my_copy = NULL;
    int count, curidx;
    struct jsonsl_jpr_st *ret = NULL;
    struct jsonsl_jpr_component_st *components = NULL;
    size_t origlen;
    jsonsl_error_t errstacked;

#define JPR_BAIL(err) *errp = err; goto GT_ERROR;

    if (errp == NULL) {
        errp = &errstacked;
    }

    if (path == NULL || *path != '/') {
        JPR_BAIL(JSONSL_ERROR_JPR_NOROOT);
        return NULL;
    }

    count = 1;
    path++;
    {
        const char *c = path;
        for (; *c; c++) {
            if (*c == '/') {
                count++;
                if (*(c+1) == '/') {
                    JPR_BAIL(JSONSL_ERROR_JPR_DUPSLASH);
                }
            }
        }
    }
    if(*path) {
        count++;
    }

    components = (struct jsonsl_jpr_component_st *)
            malloc(sizeof(*components) * count);
    if (!components) {
        JPR_BAIL(JSONSL_ERROR_ENOMEM);
    }

    my_copy = (char *)malloc(strlen(path) + 1);
    if (!my_copy) {
        JPR_BAIL(JSONSL_ERROR_ENOMEM);
    }

    strcpy(my_copy, path);

    components[0].ptype = JSONSL_PATH_ROOT;

    if (*my_copy) {
        char *cur = my_copy;
        int pathret = JSONSL_PATH_STRING;
        curidx = 1;
        while (pathret > 0 && curidx < count) {
            pathret = populate_component(cur, components + curidx, &cur, errp);
            if (pathret > 0) {
                curidx++;
            } else {
                break;
            }
        }

        if (pathret == JSONSL_PATH_INVALID) {
            JPR_BAIL(JSONSL_ERROR_JPR_BADPATH);
        }
    } else {
        curidx = 1;
    }

    path--; /*revert path to leading '/' */
    origlen = strlen(path) + 1;
    ret = (struct jsonsl_jpr_st *)malloc(sizeof(*ret));
    if (!ret) {
        JPR_BAIL(JSONSL_ERROR_ENOMEM);
    }
    ret->orig = (char *)malloc(origlen);
    if (!ret->orig) {
        JPR_BAIL(JSONSL_ERROR_ENOMEM);
    }
    ret->components = components;
    ret->ncomponents = curidx;
    ret->basestr = my_copy;
    ret->norig = origlen-1;
    strcpy(ret->orig, path);

    return ret;

    GT_ERROR:
    free(my_copy);
    free(components);
    if (ret) {
        free(ret->orig);
    }
    free(ret);
    return NULL;
#undef JPR_BAIL
}

void jsonsl_jpr_destroy(jsonsl_jpr_t jpr)
{
    free(jpr->components);
    free(jpr->basestr);
    free(jpr->orig);
    free(jpr);
}

JSONSL_API
jsonsl_jpr_match_t
jsonsl_jpr_match(jsonsl_jpr_t jpr,
                   unsigned int parent_type,
                   unsigned int parent_level,
                   const char *key,
                   size_t nkey)
{
    /* find our current component. This is the child level */
    int cmpret;
    struct jsonsl_jpr_component_st *p_component;
    p_component = jpr->components + parent_level;

    if (parent_level >= jpr->ncomponents) {
        return JSONSL_MATCH_NOMATCH;
    }

    /* Lone query for 'root' element. Always matches */
    if (parent_level == 0) {
        if (jpr->ncomponents == 1) {
            return JSONSL_MATCH_COMPLETE;
        } else {
            return JSONSL_MATCH_POSSIBLE;
        }
    }

    /* Wildcard, always matches */
    if (p_component->ptype == JSONSL_PATH_WILDCARD) {
        if (parent_level == jpr->ncomponents-1) {
            return JSONSL_MATCH_COMPLETE;
        } else {
            return JSONSL_MATCH_POSSIBLE;
        }
    }

    /* Check numeric array index. This gets its special block so we can avoid
     * string comparisons */
    if (p_component->ptype == JSONSL_PATH_NUMERIC) {
        if (parent_type == JSONSL_T_LIST) {
            if (p_component->idx != nkey) {
                /* Wrong index */
                return JSONSL_MATCH_NOMATCH;
            } else {
                if (parent_level == jpr->ncomponents-1) {
                    /* This is the last element of the path */
                    return JSONSL_MATCH_COMPLETE;
                } else {
                    /* Intermediate element */
                    return JSONSL_MATCH_POSSIBLE;
                }
            }
        } else if (p_component->is_arridx) {
            /* Numeric and an array index (set explicitly by user). But not
             * a list for a parent */
            return JSONSL_MATCH_TYPE_MISMATCH;
        }
    } else if (parent_type == JSONSL_T_LIST) {
        return JSONSL_MATCH_TYPE_MISMATCH;
    }

    /* Check lengths */
    if (p_component->len != nkey) {
        return JSONSL_MATCH_NOMATCH;
    }

    /* Check string comparison */
    cmpret = strncmp(p_component->pstr, key, nkey);
    if (cmpret == 0) {
        if (parent_level == jpr->ncomponents-1) {
            return JSONSL_MATCH_COMPLETE;
        } else {
            return JSONSL_MATCH_POSSIBLE;
        }
    }

    return JSONSL_MATCH_NOMATCH;
}

JSONSL_API
void jsonsl_jpr_match_state_init(jsonsl_t jsn,
                                 jsonsl_jpr_t *jprs,
                                 size_t njprs)
{
    size_t ii, *firstjmp;
    if (njprs == 0) {
        return;
    }
    jsn->jprs = (jsonsl_jpr_t *)malloc(sizeof(jsonsl_jpr_t) * njprs);
    jsn->jpr_count = njprs;
    jsn->jpr_root = (size_t*)calloc(1, sizeof(size_t) * njprs * jsn->levels_max);
    memcpy(jsn->jprs, jprs, sizeof(jsonsl_jpr_t) * njprs);
    /* Set the initial jump table values */

    firstjmp = jsn->jpr_root;
    for (ii = 0; ii < njprs; ii++) {
        firstjmp[ii] = ii+1;
    }
}

JSONSL_API
void jsonsl_jpr_match_state_cleanup(jsonsl_t jsn)
{
    if (jsn->jpr_count == 0) {
        return;
    }

    free(jsn->jpr_root);
    free(jsn->jprs);
    jsn->jprs = NULL;
    jsn->jpr_root = NULL;
    jsn->jpr_count = 0;
}

/**
 * This function should be called exactly once on each element...
 * This should also be called in recursive order, since we rely
 * on the parent having been initalized for a match.
 *
 * Since the parent is checked for a match as well, we maintain a 'serial' counter.
 * Whenever we traverse an element, we expect the serial to be the same as a global
 * integer. If they do not match, we re-initialize the context, and set the serial.
 *
 * This ensures a type of consistency without having a proactive reset by the
 * main lexer itself.
 *
 */
JSONSL_API
jsonsl_jpr_t jsonsl_jpr_match_state(jsonsl_t jsn,
                                    struct jsonsl_state_st *state,
                                    const char *key,
                                    size_t nkey,
                                    jsonsl_jpr_match_t *out)
{
    struct jsonsl_state_st *parent_state;
    jsonsl_jpr_t ret = NULL;

    /* Jump and JPR tables for our own state and the parent state */
    size_t *jmptable, *pjmptable;
    size_t jmp_cur, ii, ourjmpidx;

    if (!jsn->jpr_root) {
        *out = JSONSL_MATCH_NOMATCH;
        return NULL;
    }

    pjmptable = jsn->jpr_root + (jsn->jpr_count * (state->level-1));
    jmptable = pjmptable + jsn->jpr_count;

    /* If the parent cannot match, then invalidate it */
    if (*pjmptable == 0) {
        *jmptable = 0;
        *out = JSONSL_MATCH_NOMATCH;
        return NULL;
    }

    parent_state = jsn->stack + state->level - 1;

    if (parent_state->type == JSONSL_T_LIST) {
        nkey = (size_t) parent_state->nelem;
    }

    *jmptable = 0;
    ourjmpidx = 0;
    memset(jmptable, 0, sizeof(int) * jsn->jpr_count);

    for (ii = 0; ii <  jsn->jpr_count; ii++) {
        jmp_cur = pjmptable[ii];
        if (jmp_cur) {
            jsonsl_jpr_t jpr = jsn->jprs[jmp_cur-1];
            *out = jsonsl_jpr_match(jpr,
                                    parent_state->type,
                                    parent_state->level,
                                    key, nkey);
            if (*out == JSONSL_MATCH_COMPLETE) {
                ret = jpr;
                *jmptable = 0;
                return ret;
            } else if (*out == JSONSL_MATCH_POSSIBLE) {
                jmptable[ourjmpidx] = ii+1;
                ourjmpidx++;
            }
        } else {
            break;
        }
    }
    if (!*jmptable) {
        *out = JSONSL_MATCH_NOMATCH;
    }
    return NULL;
}

JSONSL_API
const char *jsonsl_strmatchtype(jsonsl_jpr_match_t match)
{
#define X(T,v) \
    if ( match == JSONSL_MATCH_##T ) \
        return #T;
    JSONSL_XMATCH
#undef X
    return "<UNKNOWN>";
}

#endif /* JSONSL_WITH_JPR */

/**
 * Utility function to convert escape sequences
 */
JSONSL_API
size_t jsonsl_util_unescape_ex(const char *in,
                               char *out,
                               size_t len,
                               const int toEscape[128],
                               unsigned *oflags,
                               jsonsl_error_t *err,
                               const char **errat)
{
    const unsigned char *c = (const unsigned char*)in;
    int in_escape = 0;
    size_t origlen = len;
    /* difference between the length of the input buffer and the output buffer */
    size_t ndiff = 0;
    if (oflags) {
        *oflags = 0;
    }
#define UNESCAPE_BAIL(e,offset) \
    *err = JSONSL_ERROR_##e; \
    if (errat) { \
        *errat = (const char*)(c+ (ptrdiff_t)(offset)); \
    } \
    return 0;

    for (; len; len--, c++, out++) {
        unsigned int uesc_val[2];
        if (in_escape) {
            /* inside a previously ignored escape. Ignore */
            in_escape = 0;
            goto GT_ASSIGN;
        }

        if (*c != '\\') {
            /* Not an escape, so we don't care about this */
            goto GT_ASSIGN;
        }

        if (len < 2) {
            UNESCAPE_BAIL(ESCAPE_INVALID, 0);
        }
        if (!is_allowed_escape(c[1])) {
            UNESCAPE_BAIL(ESCAPE_INVALID, 1)
        }
        if ((toEscape[(unsigned char)c[1] & 0x7f] == 0 &&
                c[1] != '\\' && c[1] != '"')) {
            /* if we don't want to unescape this string, just continue with
             * the escape flag set
             */
            in_escape = 1;
            goto GT_ASSIGN;
        }

        if (c[1] != 'u') {
            /* simple skip-and-replace using pre-defined maps.
             * TODO: should the maps actually reflect the desired
             * replacement character in toEscape?
             */
            char esctmp = get_escape_equiv(c[1]);
            if (esctmp) {
                /* Check if there is a corresponding replacement */
                *out = esctmp;
            } else {
                /* Just gobble up the 'reverse-solidus' */
                *out = c[1];
            }
            len--;
            ndiff++;
            c++;
            /* do not assign, just continue */
            continue;
        }

        /* next == 'u' */
        if (len < 6) {
            /* Need at least six characters:
             * { [0] = '\\', [1] = 'u', [2] = 'f', [3] = 'f', [4] = 'f', [5] = 'f' }
             */
            UNESCAPE_BAIL(UESCAPE_TOOSHORT, -1);
        }

        if (sscanf((const char*)(c+2), "%02x%02x", uesc_val, uesc_val+1) != 2) {
            /* We treat the sequence as two octets */
            UNESCAPE_BAIL(UESCAPE_TOOSHORT, -1);
        }

        /* By now, we gobble up all the six bytes (current implied + 5 next
         * characters), and have at least four missing bytes from the output
         * buffer.
         */
        len -= 5;
        c += 5;

        ndiff += 4;
        if (uesc_val[0] == 0) {
            /* only one byte is extracted from the two
             * possible octets. Increment the diff counter by one.
             */
            *out = uesc_val[1];
            if (oflags && *(unsigned char*)out > 0x7f) {
                *oflags |= JSONSL_SPECIALf_NONASCII;
            }
            ndiff++;
        } else {
            *(out++) = uesc_val[0];
            *out = uesc_val[1];
            if (oflags && (uesc_val[0] > 0x7f || uesc_val[1] > 0x7f)) {
                *oflags |= JSONSL_SPECIALf_NONASCII;
            }
        }
        continue;

        /* Only reached by previous branches */
        GT_ASSIGN:
        *out = *c;
    }
    *err = JSONSL_ERROR_SUCCESS;
    return origlen - ndiff;
}

/**
 * Character Table definitions.
 * These were all generated via srcutil/genchartables.pl
 */

/**
 * This table contains the beginnings of non-string
 * allowable (bareword) values.
 */
static unsigned short Special_Table[0x100] = {
        /* 0x00 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x1f */
        /* 0x20 */ 0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x2c */
        /* 0x2d */ JSONSL_SPECIALf_DASH /* <-> */, /* 0x2d */
        /* 0x2e */ 0,0, /* 0x2f */
        /* 0x30 */ JSONSL_SPECIALf_ZERO /* <0> */, /* 0x30 */
        /* 0x31 */ JSONSL_SPECIALf_UNSIGNED /* <1> */, /* 0x31 */
        /* 0x32 */ JSONSL_SPECIALf_UNSIGNED /* <2> */, /* 0x32 */
        /* 0x33 */ JSONSL_SPECIALf_UNSIGNED /* <3> */, /* 0x33 */
        /* 0x34 */ JSONSL_SPECIALf_UNSIGNED /* <4> */, /* 0x34 */
        /* 0x35 */ JSONSL_SPECIALf_UNSIGNED /* <5> */, /* 0x35 */
        /* 0x36 */ JSONSL_SPECIALf_UNSIGNED /* <6> */, /* 0x36 */
        /* 0x37 */ JSONSL_SPECIALf_UNSIGNED /* <7> */, /* 0x37 */
        /* 0x38 */ JSONSL_SPECIALf_UNSIGNED /* <8> */, /* 0x38 */
        /* 0x39 */ JSONSL_SPECIALf_UNSIGNED /* <9> */, /* 0x39 */
        /* 0x3a */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x59 */
        /* 0x5a */ 0,0,0,0,0,0,0,0,0,0,0,0, /* 0x65 */
        /* 0x66 */ JSONSL_SPECIALf_FALSE /* <f> */, /* 0x66 */
        /* 0x67 */ 0,0,0,0,0,0,0, /* 0x6d */
        /* 0x6e */ JSONSL_SPECIALf_NULL /* <n> */, /* 0x6e */
        /* 0x6f */ 0,0,0,0,0, /* 0x73 */
        /* 0x74 */ JSONSL_SPECIALf_TRUE /* <t> */, /* 0x74 */
        /* 0x75 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x94 */
        /* 0x95 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0xb4 */
        /* 0xb5 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0xd4 */
        /* 0xd5 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0xf4 */
        /* 0xf5 */ 0,0,0,0,0,0,0,0,0,0, /* 0xfe */
};

/**
 * Contains characters which signal the termination of any of the 'special' bareword
 * values.
 */
static int Special_Endings[0x100] = {
        /* 0x00 */ 0,0,0,0,0,0,0,0,0, /* 0x08 */
        /* 0x09 */ 1 /* <TAB> */, /* 0x09 */
        /* 0x0a */ 1 /* <LF> */, /* 0x0a */
        /* 0x0b */ 0,0, /* 0x0c */
        /* 0x0d */ 1 /* <CR> */, /* 0x0d */
        /* 0x0e */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x1f */
        /* 0x20 */ 1 /* <SP> */, /* 0x20 */
        /* 0x21 */ 0, /* 0x21 */
        /* 0x22 */ 1 /* " */, /* 0x22 */
        /* 0x23 */ 0,0,0,0,0,0,0,0,0, /* 0x2b */
        /* 0x2c */ 1 /* , */, /* 0x2c */
        /* 0x2d */ 0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x39 */
        /* 0x3a */ 1 /* : */, /* 0x3a */
        /* 0x3b */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x5a */
        /* 0x5b */ 1 /* [ */, /* 0x5b */
        /* 0x5c */ 1 /* \ */, /* 0x5c */
        /* 0x5d */ 1 /* ] */, /* 0x5d */
        /* 0x5e */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x7a */
        /* 0x7b */ 1 /* { */, /* 0x7b */
        /* 0x7c */ 0, /* 0x7c */
        /* 0x7d */ 1 /* } */, /* 0x7d */
        /* 0x7e */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x9d */
        /* 0x9e */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0xbd */
        /* 0xbe */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0xdd */
        /* 0xde */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0xfd */
        /* 0xfe */ 0 /* 0xfe */
};

/**
 * This table contains entries for the allowed whitespace as per RFC 4627
 */
static int Allowed_Whitespace[0x100] = {
        /* 0x00 */ 0,0,0,0,0,0,0,0,0, /* 0x08 */
        /* 0x09 */ 1 /* <TAB> */, /* 0x09 */
        /* 0x0a */ 1 /* <LF> */, /* 0x0a */
        /* 0x0b */ 0,0, /* 0x0c */
        /* 0x0d */ 1 /* <CR> */, /* 0x0d */
        /* 0x0e */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x1f */
        /* 0x20 */ 1 /* <SP> */, /* 0x20 */
        /* 0x21 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x40 */
        /* 0x41 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x60 */
        /* 0x61 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x80 */
        /* 0x81 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0xa0 */
        /* 0xa1 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0xc0 */
        /* 0xc1 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0xe0 */
        /* 0xe1 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 /* 0xfe */
};

/**
 * Allowable two-character 'common' escapes:
 */
static int Allowed_Escapes[0x100] = {
        /* 0x00 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x1f */
        /* 0x20 */ 0,0, /* 0x21 */
        /* 0x22 */ 1 /* <"> */, /* 0x22 */
        /* 0x23 */ 0,0,0,0,0,0,0,0,0,0,0,0, /* 0x2e */
        /* 0x2f */ 1 /* </> */, /* 0x2f */
        /* 0x30 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x4f */
        /* 0x50 */ 0,0,0,0,0,0,0,0,0,0,0,0, /* 0x5b */
        /* 0x5c */ 1 /* <\> */, /* 0x5c */
        /* 0x5d */ 0,0,0,0,0, /* 0x61 */
        /* 0x62 */ 1 /* <b> */, /* 0x62 */
        /* 0x63 */ 0,0,0, /* 0x65 */
        /* 0x66 */ 1 /* <f> */, /* 0x66 */
        /* 0x67 */ 0,0,0,0,0,0,0, /* 0x6d */
        /* 0x6e */ 1 /* <n> */, /* 0x6e */
        /* 0x6f */ 0,0,0, /* 0x71 */
        /* 0x72 */ 1 /* <r> */, /* 0x72 */
        /* 0x73 */ 0, /* 0x73 */
        /* 0x74 */ 1 /* <t> */, /* 0x74 */
        /* 0x75 */ 1 /* <u> */, /* 0x75 */
        /* 0x76 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x95 */
        /* 0x96 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0xb5 */
        /* 0xb6 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0xd5 */
        /* 0xd6 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0xf5 */
        /* 0xf6 */ 0,0,0,0,0,0,0,0,0, /* 0xfe */
};

/**
 * This table contains the _values_ for a given (single) escaped character.
 */
static unsigned char Escape_Equivs[0x100] = {
        /* 0x00 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x1f */
        /* 0x20 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x3f */
        /* 0x40 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x5f */
        /* 0x60 */ 0,0, /* 0x61 */
        /* 0x62 */ 8 /* <b> */, /* 0x62 */
        /* 0x63 */ 0,0,0, /* 0x65 */
        /* 0x66 */ 12 /* <f> */, /* 0x66 */
        /* 0x67 */ 0,0,0,0,0,0,0, /* 0x6d */
        /* 0x6e */ 10 /* <n> */, /* 0x6e */
        /* 0x6f */ 0,0,0, /* 0x71 */
        /* 0x72 */ 13 /* <r> */, /* 0x72 */
        /* 0x73 */ 0, /* 0x73 */
        /* 0x74 */ 9 /* <t> */, /* 0x74 */
        /* 0x75 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x94 */
        /* 0x95 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0xb4 */
        /* 0xb5 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0xd4 */
        /* 0xd5 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0xf4 */
        /* 0xf5 */ 0,0,0,0,0,0,0,0,0,0 /* 0xfe */
};

/* Definitions of above-declared static functions */
static char get_escape_equiv(unsigned c) {
    return Escape_Equivs[c & 0xff];
}
static unsigned extract_special(unsigned c) {
    return Special_Table[c & 0xff];
}
static int is_special_end(unsigned c) {
    return Special_Endings[c & 0xff];
}
static int is_allowed_whitespace(unsigned c) {
    return c == ' ' || Allowed_Whitespace[c & 0xff];
}
static int is_allowed_escape(unsigned c) {
    return Allowed_Escapes[c & 0xff];
}

/**
 * Utility function to implement original unescape function
 */
JSONSL_API
char jsonsl_get_escape_equiv(unsigned char c)
{
  return get_escape_equiv(c);
}

JSONSL_API
int jsonsl_is_allowed_escape(unsigned char c)
{
  return is_allowed_escape(c);
}


/* Clean up all our macros! */
#undef INCR_METRIC
#undef INCR_GENERIC
#undef INCR_STRINGY_CATCH
#undef CASE_DIGITS
#undef INVOKE_ERROR
#undef STACK_PUSH
#undef STACK_POP_NOPOS
#undef STACK_POP
#undef CALLBACK_AND_POP_NOPOS
#undef CALLBACK_AND_POP
#undef SPECIAL_POP
#undef CUR_CHAR
#undef DO_CALLBACK
#undef ENSURE_HVAL
#undef VERIFY_SPECIAL
#undef STATE_SPECIAL_LENGTH
#undef IS_NORMAL_NUMBER
#undef STATE_NUM_LAST
