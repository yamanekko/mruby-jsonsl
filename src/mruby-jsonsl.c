#include "mruby.h"
#include "mruby/data.h"

#define JSONSL_CALLOC mrb_calloc
#define JSONSL_MALLOC mrb_malloc
#define JSONSL_STATE_USER_FIELDS mrb_value mrb_data;

#include "jsonsl.h"


const static struct mrb_data_type mrb_jsonsl_type = {
  "JSONSL",
  mrb_mruby_jsonsl_free,
};

typedef struct mrb_jsonsl_data {
  mrb_state *mrb;
  mrb_value result;
} mrb_jsonsl_data;

static int MAX_DESCENT_LEVEL = 20;
static int MAX_JSON_SIZE = 0x1000;

#define MRB_JSONSL_PENDING_KEY mrb_intern_lit(mrb, "pending_key")

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
  mrb_hash_get(mrb, hash, MRB_JSONSL_PENDING_KEY);
}

static inline void
add_to_hash(mrb_value parent, mrb_value value)
{
  mrb_value pending_key = get_pending_key(mrb, parent);
  mrb_assert(mrb_test(pending_key));
  mrb_hash_set(mrb, parent, pending_key, value);
  remove_pending_key(mrb, parent);
}

static inline void
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
  mrb_value child;
  mrb_value parent;

  mrb_jsonsl_data *data = (mrb_jsonsl_data *)jsn->data;
  mrb_state *mrb = (mrb_state *)data->mrb;

  struct jsonsl_state_st *last_state = jsonsl_last_state(jsn, state);
  parent = last_state->mrb_data;

  switch(state->type) {
  case JSONSL_T_SPECIAL:
  case JSONSL_T_STRING: {
    child = mrb_cptr_value(buf);
    break;
  }
  case JSONSL_T_HKEY: {
    child = mrb_cptr_value(buf);
    set_pending_key(mrb, parent, child);
    break;
  }
  case JSONSL_T_LIST: {
    child = mrb_ary_new(mrb);
    break;
  }
  case JSONSL_T_OBJECT: {
    child = mrb_hash_new(mrb);
    break;
  }
  default:
    mrb_raisef(mrb, E_TYPE_ERROR, "Unhandled type %c\n", state->type);
    break;
  }

  mrb_assert(child);
  state->mrb_data = child;
}

static mrb_value
create_special_value(mrb_state *mrb, const char *buf, mrb_int len)
{
  if ((len == 4) && (memcmp(buf, "true", len) == 0)) {
    return mrb_true_value();
  } else if ((len == 5) && (memcmp(buf, "false", len) == 0)) {
    return mrb_false_value();
  } else if ((len == 4) && (memcmp(buf, "null", len) == 0)) {
    return mrb_nil_value();
  } else {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "Ivalid special value");
  }
}

static void
cleanup_closing_element(jsonsl_t jsn,
                        jsonsl_action_t action,
                        struct jsonsl_state_st *state,
                        const char *at)
{
  mrb_value parent = mrb_undef_value();
  mrb_state *mrb = (mrb_state *)jsn->data;
  struct jsonsl_state_st *last_state = jsonsl_last_state(jsn, state);
  if (last_state) {
    parent = last_state->mrb_data;
  }

  mrb_assert(state);
  mrb_value elem = state->mrb_data;

  if (mrb_cptr_p(elem)) {
    /* String or Speical value */
    const char *buf = (const char *)mrb_cptr(elem);
    if (*at != '"') {
      elem = create_special_value(mrb, buf, at - buf);
    } else {
      elem = mrb_str_new(mrb, buf+1, at - buf - 2);
    }
  }

  if (!mrb_undef_p(parent)) {
    if (parent->type == TYPE_LIST) {
      add_to_list(mrb, parent, elem);
    } else if (parent->type == TYPE_HASH) {
      /* ignore keys; do add only values */
      if (state->type != JSONSL_T_HKEY) {
        add_to_hash(mrb, parent, elem);
      }
    } else {
      mrb_raise(mrb, E_ARGUMENT_ERROR, "Requested to add to non-container parent type!");
    }
  }
}

void
nest_callback_initial(jsonsl_t jsn,
                      jsonsl_action_t action,
                      struct jsonsl_state_st *state,
                      const char *at)
{
  mrb_state *mrb = (mrb_state *)jsn->data;

  mrb_assert(action == JSONSL_ACTION_PUSH);

  if (state->type == JSONSL_T_LIST) {
    state->mrb_data = mrb_ary_new(mrb);
  } else if (state->type == JSONSL_T_OBJECT) {
    state->mrb_data = mrb_hash_new(mrb);
  } else {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "Type is neither Hash nor List");
  }

  jsn->action_callback = NULL;
  jsn->action_callback_PUSH = create_new_element;
  jsn->action_callback_POP = cleanup_closing_element;
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
  mrb_value result;

  mrb_get_args(mrb, "s", &str, &len);

  /* get jsonsl and reset it */
  jsn = DATA_PTR(self);
  jsonsl_reset(jsn);

  /* initialize jsn->data */
  mrb_jsonsl_data *data = (mrb_jsonsl_data *)jsn->data;
  data->result = mrb_undef_value();

  /* initalize callbacks */
  jsonsl_enable_all_callbacks(jsn);
  jsn->action_callback = nest_callback_initial;
  jsn->action_callback_PUSH = NULL;
  jsn->action_callback_POP = NULL;
  jsn->error_callback = error_callback;
  jsn->max_callback_level = MAX_DESCENT_LEVEL;

  /* do parse */
  jsonsl_feed(jsn, str, len);

  /* get result of parsing */
  result = data->result;

  return result;
}

static mrb_value
mrb_jsonsl_init(mrb_state *mrb, mrb_value self)
{
  jsonsl_t jsn;

  mrb_jsonsl_data *data = (mrb_jsonsl_data *)mrb_malloc(mrb, sizeof(mrb_jsonsl_data));
  data->mrb = mrb;
  data->result = mrb_undef_value(); /* result = undef */

  jsn = jsonsl_new(MAX_JSON_SIZE); /* jsonsl_new() uses calloc() */
  DATA_TYPE(self) = &mrb_jsonsl_typey;
  DATA_PTR(self) = jsn;

  return self;
}

static void
mrb_mruby_jsonsl_free(mrb_state *mrb, void *ptr)
{
  jsonsl_t jsn = (jsonsl_t)ptr;
  mrb_jsonsl_data *data = (mrb_jsonsl_data *)jsn->data;
  mrb_free(data);
  jsonsl_destroy(jsn);
}

void
mrb_mruby_jsonsl_gem_init(mrb_state* mrb) {
  struct RClass *jsonsl = mrb_define_module(mrb, "JSONSL");
  MRB_SET_INSTANCE_TT(jsonsl, MRB_TT_DATA);
  mrb_define_class_method(mrb, jsonsl, "initialize", mrb_jsonsl_init, MRB_ARGS_NONE());
  mrb_define_method(mrb, jsonsl, "parse", mrb_jsonsl_parse, MRB_ARGS_REQ(1));
}

void
mrb_mruby_jsonsl_gem_final(mrb_state* mrb) {
  /* finalizer */
}
