#include "GLZA.h"

/* TurboBench and other embedders pass params=NULL.  Use -D5000-equivalent
   max_rules rather than the CLI default 0xA00000: the latter builds very
   large grammars on multi-MB inputs that currently fail decode roundtrip. */
static struct param_data glza_embedded_defaults = {
  5000, /* max_rules */
  0, 0, 0,  /* cap_encoded, cap_lock_disabled, delta_disabled */
  1, 1, 0,  /* create_words, fast_mode, user_set_RAM_size */
  0, 0, 2, 1, /* user_set_profit_ratio_power, print_dictionary, use_mtf, two_threads */
  0.0, 0.0, 0.0 /* RAM_usage, order, profit_ratio_power */
};

struct param_data *GLZA_params_or_default(struct param_data *p) {
  return(p != 0 ? p : &glza_embedded_defaults);
}
