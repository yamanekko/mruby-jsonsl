#include "mruby.h"
#include "mruby/data.h"
#include "mruby/class.h"
#include "mruby/array.h"
#include "mruby/hash.h"
#include "mruby/value.h"
#include "mruby/string.h"

#define JSONSL_CALLOC(ptr) mrb_calloc((mrb), (ptr))
#define JSONSL_MALLOC(ptr) mrb_malloc((mrb), (ptr))
#define JSONSL_FREE(ptr)   mrb_free((mrb), (ptr))

#include "jsonsl.h"
#include "mruby-jsonsl.h"

const static struct mrb_data_type mrb_jsonsl_type = {
  "JSONSL",
  mrb_mruby_jsonsl_free,
};

static int MAX_DESCENT_LEVEL = 20;
static int MAX_JSON_SIZE = 0x100;

#define MRB_JSONSL_PENDING_KEY mrb_sym2str(mrb, mrb_intern_lit(mrb, "pending_key"))

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
    mrb_raise(mrb, E_ARGUMENT_ERROR, "Toplevel element should be Hash or List");
  }

  switch(state->type) {
  case JSONSL_T_SPECIAL:
  case JSONSL_T_STRING:
    state->data = (mrb_value *)mrb_malloc(mrb, sizeof(mrb_value));
    *(mrb_value *)(state->data) = mrb_cptr_value(mrb, (void *)buf);
    break;
  case JSONSL_T_HKEY:
    state->data = (mrb_value *)mrb_malloc(mrb, sizeof(mrb_value));
    *(mrb_value *)(state->data) = mrb_cptr_value(mrb, (void *)buf);
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
    mrb_raisef(mrb, E_TYPE_ERROR, "Unhandled type %c\n", state->type);
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
    if (state->special_flags & JSONSL_SPECIALf_NUMNOINT) {
      buf = (char *)mrb_cptr(*(mrb_value *)(state->data));
      temp_str = mrb_str_new(mrb, buf, at - buf);
      elem = mrb_float_value(mrb, mrb_str_to_dbl(mrb, temp_str, FALSE));
    } else if (state->special_flags & JSONSL_SPECIALf_NUMERIC) {
      buf = (char *)mrb_cptr(*(mrb_value *)(state->data));
      temp_str = mrb_str_new(mrb, buf, at - buf);
      elem = mrb_str_to_inum(mrb, temp_str, 10, FALSE);
    } else if (state->special_flags & JSONSL_SPECIALf_TRUE) {
      elem = mrb_true_value();
    } else if (state->special_flags & JSONSL_SPECIALf_FALSE) {
      elem = mrb_false_value();
    } else if (state->special_flags & JSONSL_SPECIALf_NULL) {
      elem = mrb_nil_value();
    } else {
      mrb_raise(mrb, E_ARGUMENT_ERROR, "Ivalid special value");
    }
    break;
  case JSONSL_T_STRING:
    /* String or Speical value */
    buf = (char *)mrb_cptr(*(mrb_value *)(state->data));
    elem = mrb_str_new(mrb, buf+1, at - buf - 1);
    break;
  case JSONSL_T_HKEY:
    buf = (char *)mrb_cptr(*(mrb_value *)(state->data));
    elem = mrb_str_new(mrb, buf+1, at - buf - 1);
    break;
  case JSONSL_T_LIST:
  case JSONSL_T_OBJECT:
    elem = *(mrb_value *)state->data;
    break;
  default:
    mrb_raise(mrb, E_ARGUMENT_ERROR, "unknown value");
  }

  if (!last_state) {
    data->result = *(mrb_value *)(state->data);
  } else if (last_state->type == JSONSL_T_LIST || last_state->type == JSONSL_T_OBJECT) {
    parent = (mrb_value *)last_state->data;
    if (mrb_array_p(*parent)) {
      add_to_list(mrb, *parent, elem);
    } else if (mrb_hash_p(*parent)) {
      /* ignore keys; do add only values */
      if (state->type == JSONSL_T_HKEY) {
        set_pending_key(mrb, *parent, elem);
      } else {
        add_to_hash(mrb, *parent, elem);
      }
    } else {
      mrb_raise(mrb, E_ARGUMENT_ERROR, "Requested to add to non-container parent type!");
    }
  }
}

int error_callback(jsonsl_t jsn,
                    jsonsl_error_t err,
                    struct jsonsl_state_st *state,
                    char *at)
{
  mrb_jsonsl_data *data = (mrb_jsonsl_data *)jsn->data;
  mrb_state *mrb = (mrb_state *)data->mrb;

  mrb_raisef(mrb, E_ARGUMENT_ERROR,
             "Got error at pos %lu: %s\n", jsn->pos, jsonsl_strerror(err));

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

  mrb_get_args(mrb, "s", &str, &len);

  /* get jsonsl and reset it */
  jsn = DATA_PTR(self);
  jsonsl_reset(jsn);

  /* initialize jsn->data */
  data = (mrb_jsonsl_data *)jsn->data;
  data->result = mrb_undef_value();

  /* initalize callbacks */
  jsonsl_enable_all_callbacks(jsn);
  jsn->action_callback = NULL;
  jsn->action_callback_PUSH = create_new_element;
  jsn->action_callback_POP = cleanup_closing_element;
  jsn->error_callback = error_callback;
  jsn->max_callback_level = MAX_DESCENT_LEVEL;

  /* do parse */
  jsonsl_feed(jsn, str, len);

  /* return result of parsing */
  return data->result;
}

static mrb_value
mrb_jsonsl_init(mrb_state *mrb, mrb_value self)
{
  jsonsl_t jsn;

  mrb_jsonsl_data *data = (mrb_jsonsl_data *)mrb_malloc(mrb, sizeof(mrb_jsonsl_data));
  data->mrb = mrb;
  data->result = mrb_undef_value(); /* result = undef */

  jsn = jsonsl_new(MAX_JSON_SIZE); /* jsonsl_new() uses calloc() */
  DATA_TYPE(self) = &mrb_jsonsl_type;
  DATA_PTR(self) = jsn;
  jsn->data = data;

  return self;
}

static mrb_value
mrb_jsonsl_init_copy(mrb_state *mrb, mrb_value copy)
{
  jsonsl_t jsn;
  mrb_value src;
  mrb_jsonsl_data *data;

  mrb_get_args(mrb, "o", &src);
  if (mrb_obj_equal(mrb, copy, src)) return copy;
  if (!mrb_obj_is_instance_of(mrb, src, mrb_obj_class(mrb, copy))) {
    mrb_raise(mrb, E_TYPE_ERROR, "wrong argument class");
  }
  if (!DATA_PTR(copy)) {
    data = (mrb_jsonsl_data *)mrb_malloc(mrb, sizeof(mrb_jsonsl_data));
    data->mrb = mrb;
    data->result = mrb_undef_value(); /* result = undef */
    jsn = jsonsl_new(MAX_JSON_SIZE); /* jsonsl_new() uses calloc() */
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
  mrb_define_method(mrb, jsonsl, "initialize", mrb_jsonsl_init, MRB_ARGS_NONE());
  mrb_define_method(mrb, jsonsl, "parse", mrb_jsonsl_parse, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, jsonsl, "initialize_copy", mrb_jsonsl_init_copy, MRB_ARGS_REQ(1));
}

void
mrb_mruby_jsonsl_gem_final(mrb_state* mrb)
{
  /* finalizer */
}
