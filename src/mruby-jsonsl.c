#include "mruby.h"
#include "mruby/data.h"
#include "mruby/class.h"
#include "mruby/array.h"
#include "mruby/hash.h"
#include "mruby/value.h"
#include "mruby/string.h"

#include "jsonsl.h"
#include "mruby-jsonsl.h"

const static struct mrb_data_type mrb_jsonsl_type = {
  "JSONSL",
  mrb_mruby_jsonsl_free,
};

static int MAX_DESCENT_LEVEL = 20;
static int DEFAULT_MAX_JSON_SIZE = 0x100;

#define MRB_JSONSL_PENDING_KEY mrb_sym2str(mrb, mrb_intern_lit(mrb, "pending_key"))

static inline struct RClass *
get_jsonsl_error(mrb_state *mrb)
{
  return mrb_class_get_under(mrb, mrb_class_get(mrb, "JSONSL"), "Error");
}

static inline void
set_pending_key(mrb_state *mrb, mrb_value hash, mrb_value value)
{
  mrb_hash_set(mrb, hash, MRB_JSONSL_PENDING_KEY, value);
}

static inline void
remove_pending_key(mrb_state *mrb, mrb_value hash)
{
  mrb_hash_delete_key(mrb, hash, MRB_JSONSL_PENDING_KEY);
}

static inline mrb_value
get_pending_key(mrb_state *mrb, mrb_value hash)
{
  return mrb_hash_get(mrb, hash, MRB_JSONSL_PENDING_KEY);
}

static void
add_to_hash(mrb_state *mrb, mrb_value parent, mrb_value value)
{
  mrb_value pending_key = get_pending_key(mrb, parent);
  mrb_assert(mrb_test(pending_key));
  mrb_hash_set(mrb, parent, pending_key, value);
  remove_pending_key(mrb, parent);
}

static void
add_to_list(mrb_state *mrb, mrb_value parent, mrb_value value)
{
  mrb_ary_push(mrb, parent, value);
}

static void
create_new_element(jsonsl_t jsn,
                   jsonsl_action_t action,
                   struct jsonsl_state_st *state,
                   const char *buf)
{
  mrb_jsonsl_data *data = (mrb_jsonsl_data *)jsn->data;
  mrb_state *mrb = (mrb_state *)data->mrb;

  if (state->level == 1 &&
      ((state->type != JSONSL_T_LIST) && (state->type != JSONSL_T_OBJECT))) {
    mrb_raise(mrb, get_jsonsl_error(mrb), "Toplevel element should be Hash or List");
  }

  switch(state->type) {
  case JSONSL_T_SPECIAL:
  case JSONSL_T_STRING:
    break;
  case JSONSL_T_HKEY:
    break;
  case JSONSL_T_LIST:
    state->data = (mrb_value *)mrb_malloc(mrb, sizeof(mrb_value));
    *(mrb_value *)(state->data) = mrb_ary_new(mrb);
    break;
  case JSONSL_T_OBJECT:
    state->data = (mrb_value *)mrb_malloc(mrb, sizeof(mrb_value));
    *(mrb_value *)(state->data) = mrb_hash_new(mrb);
    break;
  default:
    mrb_raisef(mrb, get_jsonsl_error(mrb), "Unhandled type %c\n", state->type);
    break;
  }
}

static void
cleanup_closing_element(jsonsl_t jsn,
                        jsonsl_action_t action,
                        struct jsonsl_state_st *state,
                        const char *at)
{
  mrb_value *parent;
  mrb_jsonsl_data *data = (mrb_jsonsl_data *)jsn->data;
  mrb_state *mrb = (mrb_state *)data->mrb;

  mrb_value elem;
  mrb_value temp_str;
  char *buf;
  struct jsonsl_state_st *last_state = jsonsl_last_state(jsn, state);

  mrb_assert(state);

  switch(state->type) {
  case JSONSL_T_SPECIAL:
    /* Integer, Float or true/false/null */
    if (state->special_flags & JSONSL_SPECIALf_NUMNOINT) {
      buf = (char *)jsn->base + state->pos_begin;
      temp_str = mrb_str_new(mrb, buf, at - buf);
      elem = mrb_float_value(mrb, mrb_str_to_dbl(mrb, temp_str, TRUE));
    } else if (state->special_flags & JSONSL_SPECIALf_NUMERIC) {
      buf = (char *)jsn->base + state->pos_begin;
      temp_str = mrb_str_new(mrb, buf, at - buf);
      elem = mrb_str_to_inum(mrb, temp_str, 10, TRUE);
    } else if (state->special_flags & JSONSL_SPECIALf_TRUE) {
      elem = mrb_true_value();
    } else if (state->special_flags & JSONSL_SPECIALf_FALSE) {
      elem = mrb_false_value();
    } else if (state->special_flags & JSONSL_SPECIALf_NULL) {
      elem = mrb_nil_value();
    } else {
      mrb_raise(mrb, get_jsonsl_error(mrb), "Invalid special value");
    }
    break;
  case JSONSL_T_STRING:
    /* String */
    buf = (char *)jsn->base + state->pos_begin;
    elem = mrb_str_unescaped_utf8(mrb, buf+1, at - buf - 1, state->pos_begin+1);
    break;
  case JSONSL_T_HKEY:
    /* String as key of Hash */
    buf = (char *)jsn->base + state->pos_begin;
    if (((mrb_jsonsl_data *)jsn->data)->symbol_key) {
      elem = mrb_symbol_value(mrb_intern(mrb, buf+1, at - buf - 1));
    } else {
      elem = mrb_str_unescaped_utf8(mrb, buf+1, at - buf - 1, state->pos_begin+1);
    }
    break;
  case JSONSL_T_LIST:
  case JSONSL_T_OBJECT:
    elem = *(mrb_value *)state->data;
    break;
  default:
    mrb_raise(mrb, get_jsonsl_error(mrb), "Unknown value");
  }

  if (state->data) {
    mrb_free(mrb, state->data);
  }

  if (!last_state) {
    data->result = elem;
  } else if (last_state->type == JSONSL_T_LIST) {
    parent = (mrb_value *)last_state->data;
    mrb_assert(mrb_array_p(*parent));
    add_to_list(mrb, *parent, elem);
  } else if (last_state->type == JSONSL_T_OBJECT) {
    parent = (mrb_value *)last_state->data;
    mrb_assert((mrb_hash_p(*parent)));
    /* ignore keys; do add only values */
    if (state->type == JSONSL_T_HKEY) {
      set_pending_key(mrb, *parent, elem);
    } else {
      add_to_hash(mrb, *parent, elem);
    }
  } else {
    mrb_raise(mrb, get_jsonsl_error(mrb), "Requested to add to non-container parent type!");
  }
}


static uint32_t
char2codepoint(char *p, mrb_int *err_pos)
{
  uint32_t num = 0;
  char *ch = p;
  for (; ch < p + 4; ch++) {
    num *= 16;
    if ('0' <= ch[0] && ch[0] <= '9') {
      num += (int)(ch[0] - '0');
    } else if ('a' <= ch[0] && ch[0] <= 'f') {
      num += (int)(ch[0] - 'a' + 10);
    } else if ('A' <= ch[0] && ch[0] <= 'F') {
      num += (int)(ch[0] - 'A' + 10);
    } else {
      *err_pos = (mrb_int)(ch-p);
      return 0;
    }
  }
  *err_pos = 0;
  return num;
}

/* from mruby/mrbgems/mruby-string-utf8/src/string.c */
static size_t
codepoint2utf8(uint32_t cp, char *utf8)
{
  size_t len;

  if (cp < 0x80) {
    utf8[0] = (char)cp;
    len = 1;
  }
  else if (cp < 0x800) {
    utf8[0] = (char)(0xC0 | (cp >> 6));
    utf8[1] = (char)(0x80 | (cp & 0x3F));
    len = 2;
  }
  else if (cp < 0x10000) {
    utf8[0] = (char)(0xE0 |  (cp >> 12));
    utf8[1] = (char)(0x80 | ((cp >>  6) & 0x3F));
    utf8[2] = (char)(0x80 | ( cp        & 0x3F));
    len = 3;
  }
  else {
    utf8[0] = (char)(0xF0 |  (cp >> 18));
    utf8[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    utf8[2] = (char)(0x80 | ((cp >>  6) & 0x3F));
    utf8[3] = (char)(0x80 | ( cp        & 0x3F));
    len = 4;
  }
  return len;
}


static mrb_value
mrb_str_unescaped_utf8(mrb_state *mrb,
                       const char *in,
                       size_t len,
                       mrb_int pos_begin)
{
  char *ch = (char *)in;
  char *out;
  char *origout;
  char *end = (char *)in + len;
  size_t origlen = len;
  size_t ndiff = 0;
  mrb_int err_pos;
  char utf8[4];
  uint32_t codepoint;
  size_t utf8len;

#define UNESCAPE_ERROR(e,offset)                \
  mrb_raisef(mrb, get_jsonsl_error(mrb), "escape error at %S: %S", \
             mrb_fixnum_value(pos_begin+(int)(ch - in + (ptrdiff_t)offset)), \
             mrb_str_new_cstr(mrb, jsonsl_strerror(JSONSL_ERROR_##e)));

  out = (char *)mrb_malloc(mrb, len+1);
  origout = out;

  for (; ch < end; ch++, len--) {
    if (*ch != '\\') {
      *out = *ch;
      out++;
    } else {
      /* ch[0] == '\\' */
      if (len < 2) { /* 2 == strlen('\\b') */
        UNESCAPE_ERROR(ESCAPE_INVALID, 0);
      }
      if (!jsonsl_is_allowed_escape(ch[1])) {
        UNESCAPE_ERROR(ESCAPE_INVALID, 1);
      }
      if (ch[1] != 'u') {
        char esctmp = jsonsl_get_escape_equiv(ch[1]);
        if (!esctmp) {
          UNESCAPE_ERROR(ESCAPE_INVALID, 1);
        }
        *out = esctmp;
        out++;
        ndiff++;
        continue;
      }

      /* next == 'u' */
      if (len < 6) {
        /* Need at least six characters:
         * { [0]='\\', [1]='u', [2]='f', [3]='f', [4]='f', [5]='f' }
         */
        UNESCAPE_ERROR(UESCAPE_TOOSHORT, -1);
      }
      codepoint = char2codepoint(ch+2, &err_pos);
      if (err_pos) {
        UNESCAPE_ERROR(UESCAPE_TOOSHORT, -1);
      }
      if (0x10FFFF < codepoint) {
        UNESCAPE_ERROR(ESCAPE_INVALID, -1);
      }
      utf8len = codepoint2utf8(codepoint, utf8);
      len -= 5;
      ch += 5;
      memcpy(out, utf8, utf8len);
      out += utf8len;
      ndiff += (6 - utf8len);
    }
  }

  return mrb_str_new_static(mrb, origout, origlen - ndiff);
}

int error_callback(jsonsl_t jsn,
                    jsonsl_error_t err,
                    struct jsonsl_state_st *state,
                    char *at)
{
  mrb_jsonsl_data *data = (mrb_jsonsl_data *)jsn->data;
  mrb_state *mrb = (mrb_state *)data->mrb;

  mrb_raisef(mrb, get_jsonsl_error(mrb),
             "Got error at %S: %S\n", mrb_fixnum_value(jsn->pos), mrb_str_new_cstr(mrb, jsonsl_strerror(err)));

  /* do not reach */
  return 0;
}


static mrb_value
mrb_jsonsl_parse(mrb_state *mrb, mrb_value self)
{
  char *str;
  int len;
  jsonsl_t jsn;
  mrb_jsonsl_data *data;
  mrb_value obj;
  mrb_bool opt;
  mrb_value key ;

  mrb_get_args(mrb, "s|o?", &str, &len, &obj, &opt);

  /* get jsonsl and reset it */
  jsn = DATA_PTR(self);
  jsonsl_reset(jsn);

  /* initialize jsn->data */
  data = (mrb_jsonsl_data *)jsn->data;
  data->result = mrb_undef_value();
  key = mrb_symbol_value(mrb_intern_lit(mrb, "symbol_key"));
  if (!opt) {
    data->symbol_key = FALSE;
  } else {
    if (mrb_type(obj) != MRB_TT_HASH) {
      mrb_raise(mrb, get_jsonsl_error(mrb), "Option should be Hash");
    } else {
      if (mrb_bool(mrb_hash_get(mrb, obj, key))) {
        data->symbol_key = TRUE;
      } else {
        data->symbol_key = FALSE;
      }
    }
  }

  /* initalize callbacks */
  jsonsl_enable_all_callbacks(jsn);
  jsn->action_callback = NULL;
  jsn->action_callback_PUSH = create_new_element;
  jsn->action_callback_POP = cleanup_closing_element;
  jsn->error_callback = error_callback;
  jsn->max_callback_level = MAX_DESCENT_LEVEL;

  /* do parse */
  jsonsl_feed(jsn, str, len);
  if (jsn->level != 0) {
    mrb_raise(mrb, get_jsonsl_error(mrb), "JSON data is terminated");
  }

  /* return result of parsing */
  return data->result;
}

static mrb_value
mrb_jsonsl_init(mrb_state *mrb, mrb_value self)
{
  jsonsl_t jsn;
  mrb_int jsonsl_size;
  int n;

  mrb_jsonsl_data *data = (mrb_jsonsl_data *)mrb_malloc(mrb, sizeof(mrb_jsonsl_data));
  data->mrb = mrb;
  data->result = mrb_undef_value(); /* result = undef */

  n = mrb_get_args(mrb, "|i", &jsonsl_size);
  if (n == 0) {
    jsn = jsonsl_new(DEFAULT_MAX_JSON_SIZE); /* jsonsl_new() uses calloc() */
  } else {
    jsn = jsonsl_new(jsonsl_size);
  }

  DATA_TYPE(self) = &mrb_jsonsl_type;
  DATA_PTR(self) = jsn;
  jsn->data = data;

  return self;
}

static mrb_value
mrb_jsonsl_init_copy(mrb_state *mrb, mrb_value copy)
{
  jsonsl_t jsn, jsn_orig;
  mrb_value src;
  mrb_jsonsl_data *data;
  int jsonsl_size;

  mrb_get_args(mrb, "o", &src);
  if (mrb_obj_equal(mrb, copy, src)) return copy;
  if (!mrb_obj_is_instance_of(mrb, src, mrb_obj_class(mrb, copy))) {
    mrb_raise(mrb, get_jsonsl_error(mrb), "wrong argument class");
  }
  if (!DATA_PTR(copy)) {
    jsn_orig = DATA_PTR(src);
    jsonsl_size = jsn_orig->levels_max;
    data = (mrb_jsonsl_data *)mrb_malloc(mrb, sizeof(mrb_jsonsl_data));
    data->mrb = mrb;
    data->result = mrb_undef_value(); /* result = undef */
    jsn = jsonsl_new(jsonsl_size); /* jsonsl_new() uses calloc() */
    DATA_TYPE(copy) = &mrb_jsonsl_type;
    DATA_PTR(copy) = jsn;
    jsn->data = data;
  }

  return copy;
}

static void
mrb_mruby_jsonsl_free(mrb_state *mrb, void *ptr)
{
  jsonsl_t jsn = (jsonsl_t)ptr;
  mrb_jsonsl_data *data = (mrb_jsonsl_data *)jsn->data;
  if (data) {
    mrb_free(mrb, data);
  }
  if (jsn) {
    jsonsl_destroy(jsn);
  }
}

void
mrb_mruby_jsonsl_gem_init(mrb_state* mrb)
{
  struct RClass *jsonsl = mrb_define_class(mrb, "JSONSL", mrb->object_class);
  MRB_SET_INSTANCE_TT(jsonsl, MRB_TT_DATA);

  /* define "JSON::Error" class for runtime exception */
  mrb_define_class_under(mrb, jsonsl, "Error", E_RUNTIME_ERROR);

  mrb_define_method(mrb, jsonsl, "initialize", mrb_jsonsl_init, MRB_ARGS_OPT(1));
  mrb_define_method(mrb, jsonsl, "parse", mrb_jsonsl_parse, MRB_ARGS_ARG(1,1));
  mrb_define_method(mrb, jsonsl, "initialize_copy", mrb_jsonsl_init_copy, MRB_ARGS_REQ(1));
}

void
mrb_mruby_jsonsl_gem_final(mrb_state* mrb)
{
  /* finalizer */
}
