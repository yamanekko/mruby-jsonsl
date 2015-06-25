/*
 * mruby-jsonsl.h - JSONSL for mruby
 *
 */
#ifndef MRUBY_JSONSL_H_
#define MRUBY_JSONSL_H_

typedef struct mrb_jsonsl_data {
  mrb_state *mrb;
  mrb_value result;
  mrb_bool symbol_key;
} mrb_jsonsl_data;

static void
add_to_hash(mrb_state *mrb, mrb_value parent, mrb_value value);

static void
add_to_list(mrb_state *mrb, mrb_value parent, mrb_value value);

static void
create_new_element(jsonsl_t jsn,
                   jsonsl_action_t action,
                   struct jsonsl_state_st *state,
                   const char *buf);

static void
cleanup_closing_element(jsonsl_t jsn,
                        jsonsl_action_t action,
                        struct jsonsl_state_st *state,
                        const char *at);

int
error_callback(jsonsl_t jsn,
               jsonsl_error_t err,
               struct jsonsl_state_st *state,
               char *at);

static mrb_value
mrb_jsonsl_parse(mrb_state *mrb, mrb_value self);

static mrb_value
mrb_jsonsl_init(mrb_state *mrb, mrb_value self);

static void
mrb_mruby_jsonsl_free(mrb_state *mrb, void *ptr);

void
mrb_mruby_jsonsl_gem_init(mrb_state* mrb);

void
mrb_mruby_jsonsl_gem_final(mrb_state* mrb);


#endif /* MRUBY_JSONSL_H_ */
