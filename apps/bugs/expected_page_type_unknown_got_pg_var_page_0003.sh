#!/usr/bin/env bash 

# FAILS ON: crash_and_reopen, step 702 -- the earliest in the corpus, so this
# is the best minimization candidate.
#
#   698 begin_txn
#   699 insert   in txn
#   700 delete   in txn   nvars 5 -> 4
#   701 delete   in txn   nvars 4 -> 3
#   702 crash_and_reopen  -> FAILS   (nvars back to 5)
#
# Two deletes inside one transaction, then a crash that must undo both. A
# rope layer scan then reads a page that is a variable directory page:
#
#   ns_visit_variables -> ns_consume_one_var -> ns_rope_validate
#     -> ns_rope_get_layers (ns_rope_layer_scan.c:89) -> pgr_get
#     -> "expected page type UNKNOWN, got PG_VAR_PAGE"
#
# Getting PG_VAR_PAGE specifically is the useful part: a rope pointer is
# aimed at a page that now belongs to the variable directory, which is what a
# freed-and-reallocated page looks like from the rope side.

# {
#   "action": "switch",
#   "completed": true,
#   "in_txn": false,
#   "step": 697,
#   "seed": 3519560868,
#   "commit_hash": "2f2096547dca3fc359e657e21a92e31f04994f2a",
#   "sequence_id": 3519560868,
#   "dbname": "simtest.db",
#   "max_tsize": 4096,
#   "max_insert_len": 1000000,
#   "sample_space_prob": 0,
#   "op_duration_ms": 0.000000,
#   "total_working_ms": 2845.218000,
#   "db_size_gb": 0.133791744,
#   "elapsed_ms": 3281.726000,
#   "nvars": 5,
#   "tracked_gb": 0.004513768,
#   "cur_var": "D",
#   "cur_var_len": 0,
#   "cur_var_tsize": 8,
#   "vname": "D",
#   "placeholder": null
# }
# {
#   "action": "begin_txn",
#   "completed": true,
#   "in_txn": true,
#   "step": 698,
#   "seed": 3519560868,
#   "commit_hash": "2f2096547dca3fc359e657e21a92e31f04994f2a",
#   "sequence_id": 3519560868,
#   "dbname": "simtest.db",
#   "max_tsize": 4096,
#   "max_insert_len": 1000000,
#   "sample_space_prob": 0,
#   "op_duration_ms": 0.025000,
#   "total_working_ms": 2845.243000,
#   "db_size_gb": 0.133791744,
#   "elapsed_ms": 3281.965000,
#   "nvars": 5,
#   "tracked_gb": 0.004513768,
#   "cur_var": "D",
#   "cur_var_len": 0,
#   "cur_var_tsize": 8,
#   "placeholder": null
# }
# {
#   "action": "insert",
#   "completed": true,
#   "in_txn": true,
#   "step": 699,
#   "seed": 3519560868,
#   "commit_hash": "2f2096547dca3fc359e657e21a92e31f04994f2a",
#   "sequence_id": 3519560868,
#   "dbname": "simtest.db",
#   "max_tsize": 4096,
#   "max_insert_len": 1000000,
#   "sample_space_prob": 0,
#   "op_duration_ms": 1.057000,
#   "total_working_ms": 2847.329000,
#   "db_size_gb": 0.133791744,
#   "elapsed_ms": 3284.750000,
#   "nvars": 5,
#   "tracked_gb": 0.004909032,
#   "cur_var": "D",
#   "cur_var_len": 49408,
#   "cur_var_tsize": 8,
#   "ofst": 0,
#   "nelems": 49408,
#   "bytes_moved": 395264,
#   "bytes_per_ms": 373948.912,
#   "total_gb_moved": 0.047115833,
#   "avg_bytes_per_ms": 16547.379,
#   "placeholder": null
# }
# {
#   "action": "delete",
#   "completed": true,
#   "in_txn": true,
#   "step": 700,
#   "seed": 3519560868,
#   "commit_hash": "2f2096547dca3fc359e657e21a92e31f04994f2a",
#   "sequence_id": 3519560868,
#   "dbname": "simtest.db",
#   "max_tsize": 4096,
#   "max_insert_len": 1000000,
#   "sample_space_prob": 0,
#   "op_duration_ms": 9.964000,
#   "total_working_ms": 2857.293000,
#   "db_size_gb": 0.133791744,
#   "elapsed_ms": 3294.730000,
#   "nvars": 4,
#   "tracked_gb": 0.004513768,
#   "cur_var": "Lq3Tezdj",
#   "cur_var_len": 28186,
#   "cur_var_tsize": 96,
#   "next": "Lq3Tezdj",
#   "placeholder": null
# }
# {
#   "action": "delete",
#   "completed": true,
#   "in_txn": true,
#   "step": 701,
#   "seed": 3519560868,
#   "commit_hash": "2f2096547dca3fc359e657e21a92e31f04994f2a",
#   "sequence_id": 3519560868,
#   "dbname": "simtest.db",
#   "max_tsize": 4096,
#   "max_insert_len": 1000000,
#   "sample_space_prob": 0,
#   "op_duration_ms": 67.608000,
#   "total_working_ms": 2924.901000,
#   "db_size_gb": 0.133791744,
#   "elapsed_ms": 3362.360000,
#   "nvars": 3,
#   "tracked_gb": 0.001807912,
#   "cur_var": "ijmbLDQxn",
#   "cur_var_len": 111970,
#   "cur_var_tsize": 8,
#   "next": "ijmbLDQxn",
#   "placeholder": null
# }
# [ERROR   ]: expected page type UNKNOWN, got PG_VAR_PAGE
# [ERROR   ]: TRACE: ns_pgr_get.c:70 (pgr_get):
# [ERROR   ]: TRACE: ns_rope_layer_scan.c:89 (ns_rope_get_layers):
# [ERROR   ]: TRACE: ns_rope_validate.c:35 (ns_rope_validate):
# [ERROR   ]: TRACE: ns_visit_variables.c:62 (ns_consume_one_var):
# [ERROR   ]: TRACE: ns_visit_variables.c:102 (ns_consume_var_chain):
# [ERROR   ]: TRACE: ns_visit_variables.c:135 (ns_visit_variables):
# [ERROR   ]: TRACE: ns_numstore_simulation.c:114 (nss_crash_and_reopen):
# {
#   "action": "crash_and_reopen",
#   "completed": false,
#   "in_txn": false,
#   "step": 702,
#   "seed": 3519560868,
#   "commit_hash": "2f2096547dca3fc359e657e21a92e31f04994f2a",
#   "sequence_id": 3519560868,
#   "dbname": "simtest.db",
#   "max_tsize": 4096,
#   "max_insert_len": 1000000,
#   "sample_space_prob": 0,
#   "op_duration_ms": 75.421000,
#   "total_working_ms": 3000.322000,
#   "db_size_gb": 0.133791744,
#   "elapsed_ms": 3437.912000,
#   "nvars": 5,
#   "tracked_gb": 0.004513768,
#   "cur_var": "D",
#   "cur_var_len": 0,
#   "cur_var_tsize": 8,
#   "placeholder": null
# }
# [ERROR   ]: TRACE: ns_numstore_simulation.c:978 (ns_simul_step):
# [ERROR   ]: expected page type UNKNOWN, got PG_VAR_PAGE

build/debug/bin/ns_simtest \
      --dbname simtest.db \
      --duration 10 \
      --seed 3519560868 \
      --commit-hash 2f2096547dca3fc359e657e21a92e31f04994f2a \
      --seqid 3519560868
