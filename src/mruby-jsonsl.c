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
#undef  JSONSL_STATE_GENERIC
#define JSONSL_STATE_USER_FIELDS mrb_value mrb_data;

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
  mrb_value parent;
  mrb_value v;

  mrb_jsonsl_data *data = (mrb_jsonsl_data *)jsn->data;
  mrb_state *mrb = (mrb_state *)data->mrb;

  struct jsonsl_state_st *last_state = jsonsl_last_state(jsn, state);
  parent = last_state->mrb_data;
  printf("--create_new_element\n");
  printf("      state:%p jsn:%p stack:%p\n", state, jsn, jsn->stack);
  printf(" last_state:%p\n", last_state);
  printf("l->mrb_data:%x\n", last_state->mrb_data);
  printf("l->mrb_d.tt:%x\n", (last_state->mrb_data).tt);
  printf("     parent:%p\n", parent);
  printf("         tt:%x\n", parent.tt);
  printf("        buf:%s\n", buf);

  switch(state->type) {
  case JSONSL_T_SPECIAL:
  case JSONSL_T_STRING:
    state->mrb_data = mrb_cptr_value(mrb, (void *)buf);
    break;
  case JSONSL_T_HKEY:
    //child = mrb_cptr_value(mrb, (void *)buf);
    //printf("(parent).tt=%d\n",(parent)->tt);
    //mrb_assert(mrb_hash_p(parent));
    //set_pending_key(mrb, parent, child);
    state->mrb_data = mrb_cptr_value(mrb, (void *)buf);
    break;
  case JSONSL_T_LIST:
    state->mrb_data = mrb_ary_new(mrb);
    break;
  case JSONSL_T_OBJECT:
    state->mrb_data = mrb_hash_new(mrb);
    break;
  default:
    mrb_raisef(mrb, E_TYPE_ERROR, "Unhandled type %c\n", state->type);
    break;
  }

  //mrb_assert(child);
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
    printf("buf:'%s'(%d)\n",buf,len);
    mrb_raise(mrb, E_ARGUMENT_ERROR, "Ivalid special value");
  }
}

static void
cleanup_closing_element(jsonsl_t jsn,
                        jsonsl_action_t action,
                        struct jsonsl_state_st *state,
                        const char *at)
{
  mrb_value parent;
  mrb_jsonsl_data *data = (mrb_jsonsl_data *)jsn->data;
  mrb_state *mrb = (mrb_state *)data->mrb;

  mrb_value elem;
  char *buf;
  struct jsonsl_state_st *last_state = jsonsl_last_state(jsn, state);

  mrb_assert(state);

  switch(state->type) {
  case JSONSL_T_SPECIAL:
  case JSONSL_T_STRING:
    /* String or Speical value */
    buf = (char *)mrb_cptr(state->mrb_data);
    if (*at == '"') {
      elem = mrb_str_new(mrb, buf+1, at - buf - 2);
    } else {
      elem = create_special_value(mrb, buf, at - buf);
    }
    break;
  case JSONSL_T_HKEY:
    buf = (char *)mrb_cptr(state->mrb_data);
    elem = mrb_str_new(mrb, buf+1, at - buf - 2);
    break;
  case JSONSL_T_LIST:
  case JSONSL_T_OBJECT:
    elem = state->mrb_data;
    break;
  default:
      mrb_raise(mrb, E_ARGUMENT_ERROR, "unknown value");
  }

  if (!last_state) {
    data->result = state->mrb_data;
  } else if (last_state->type == JSONSL_T_LIST || last_state->type == JSONSL_T_OBJECT) {
    printf("l_state->type:%x\n", last_state->type);
    parent = last_state->mrb_data;
    printf("l_s->mrb_data:%x\n", parent);
    if (mrb_array_p(parent)) {
      add_to_list(mrb, parent, elem);
    } else if (mrb_hash_p(parent)) {
      /* ignore keys; do add only values */
      if (state->type == JSONSL_T_HKEY) {
        set_pending_key(mrb, parent, elem);
      } else {
        add_to_hash(mrb, parent, elem);
      }
    } else {
      printf("parent-tt:%i\n", mrb_type(parent));
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
  mrb_jsonsl_data *data = (mrb_jsonsl_data *)jsn->data;
  mrb_state *mrb = (mrb_state *)data->mrb;
  mrb_value value;

  mrb_assert(action == JSONSL_ACTION_PUSH);

  if (state->type == JSONSL_T_LIST) {
    value = mrb_ary_new(mrb);
    state->mrb_data = value;
  } else if (state->type == JSONSL_T_OBJECT) {
    value = mrb_hash_new(mrb);
    state->mrb_data = value;
    printf("mrb_p:");
    mrb_p(mrb,value);
    printf("mrb_p:");
    mrb_p(mrb,state->mrb_data);
    printf("   value.tt:%x\n", value.tt);
    printf("   value.tt:%x\n", state->mrb_data.tt);
  } else {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "Type is neither Hash nor List");
  }

  jsn->action_callback = NULL;
  jsn->action_callback_PUSH = create_new_element;
  jsn->action_callback_POP = cleanup_closing_element;
  printf("\n init_state:%p jsn:%p stack:%p\n", state, jsn, jsn->stack);
  printf("sizeof(state):%ld\n", sizeof(struct jsonsl_state_st));
  printf("state->type:%d\n",state->type);
  printf("     &value:%p\n", &value);
  printf("         tt:%x\n", value.tt);
  printf("s->mrb_data:%x\n", state->mrb_data);
  printf("s->data.tt :%x\n", state->mrb_data.tt);
  printf("sizeof(m_d):%ld\n", sizeof(state->mrb_data));
  printf("sizeof(val):%ld\n", sizeof(value));
  mrb_p(mrb, value);
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
  jsn->action_callback = nest_callback_initial;
  jsn->action_callback_PUSH = NULL;
  jsn->action_callback_POP = NULL;
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

static void
mrb_mruby_jsonsl_free(mrb_state *mrb, void *ptr)
{
  jsonsl_t jsn = (jsonsl_t)ptr;
  mrb_jsonsl_data *data = (mrb_jsonsl_data *)jsn->data;
  mrb_free(mrb, data);
  jsonsl_destroy(jsn);
}

void
mrb_mruby_jsonsl_gem_init(mrb_state* mrb)
{
  struct RClass *jsonsl = mrb_define_class(mrb, "JSONSL", mrb->object_class);
  MRB_SET_INSTANCE_TT(jsonsl, MRB_TT_DATA);
  mrb_define_method(mrb, jsonsl, "initialize", mrb_jsonsl_init, MRB_ARGS_NONE());
  mrb_define_method(mrb, jsonsl, "parse", mrb_jsonsl_parse, MRB_ARGS_REQ(1));
}

void
mrb_mruby_jsonsl_gem_final(mrb_state* mrb)
{
  /* finalizer */
}
