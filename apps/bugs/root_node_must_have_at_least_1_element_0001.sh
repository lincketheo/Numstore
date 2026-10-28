#!/usr/bin/env bash 

# FAILS ON: crash_and_reopen, step 1204
#
#   1200 write    in txn
#   1201 remove   in txn
#   1202 write    in txn
#   1203 delete   in txn   nvars 1 -> 0, the last variable goes away
#   1204 crash_and_reopen  -> FAILS
#
# The crash rolls the txn back, so the deleted variables come back (nvars 4).
# Validating one of them cannot even find its layers:
#
#   ns_visit_variables -> ns_consume_one_var -> ns_rope_validate
#     -> ns_rope_get_layers (ns_rope_layer_scan.c:89) -> pgr_get
#     -> "Root node must have at least 1 element"
#
# So the rope root restored by undo is not a usable root. Note the delete at
# 1203 emptied the database, which makes this the most extreme version of the
# family: undo has to restore a variable that was the only one left.

# {
#   "action": "write",
#   "completed": true,
#   "in_txn": true,
#   "step": 1199,
#   "seed": 2172185989,
#   "commit_hash": "2f2096547dca3fc359e657e21a92e31f04994f2a",
#   "sequence_id": 2172185989,
#   "dbname": "simtest.db",
#   "max_tsize": 4096,
#   "max_insert_len": 1000000,
#   "sample_space_prob": 0,
#   "op_duration_ms": 1.336000,
#   "total_working_ms": 5444.064000,
#   "db_size_gb": 0.133791744,
#   "elapsed_ms": 6600.032000,
#   "nvars": 1,
#   "tracked_gb": 0.000788912,
#   "cur_var": "bb3FfKo",
#   "cur_var_len": 49307,
#   "cur_var_tsize": 16,
#   "start": 44050,
#   "stride": 1177,
#   "nelems": 4,
#   "bytes_moved": 64,
#   "bytes_per_ms": 47.904,
#   "total_gb_moved": 0.077881133,
#   "avg_bytes_per_ms": 14305.698,
#   "placeholder": null
# }
# {
#   "action": "write",
#   "completed": true,
#   "in_txn": true,
#   "step": 1200,
#   "seed": 2172185989,
#   "commit_hash": "2f2096547dca3fc359e657e21a92e31f04994f2a",
#   "sequence_id": 2172185989,
#   "dbname": "simtest.db",
#   "max_tsize": 4096,
#   "max_insert_len": 1000000,
#   "sample_space_prob": 0,
#   "op_duration_ms": 1.062000,
#   "total_working_ms": 5445.197000,
#   "db_size_gb": 0.133791744,
#   "elapsed_ms": 6601.567000,
#   "nvars": 1,
#   "tracked_gb": 0.000788912,
#   "cur_var": "bb3FfKo",
#   "cur_var_len": 49307,
#   "cur_var_tsize": 16,
#   "start": 30432,
#   "stride": 3364,
#   "nelems": 2,
#   "bytes_moved": 32,
#   "bytes_per_ms": 30.132,
#   "total_gb_moved": 0.077881165,
#   "avg_bytes_per_ms": 14302.727,
#   "placeholder": null
# }
# {
#   "action": "remove",
#   "completed": true,
#   "in_txn": true,
#   "step": 1201,
#   "seed": 2172185989,
#   "commit_hash": "2f2096547dca3fc359e657e21a92e31f04994f2a",
#   "sequence_id": 2172185989,
#   "dbname": "simtest.db",
#   "max_tsize": 4096,
#   "max_insert_len": 1000000,
#   "sample_space_prob": 0,
#   "op_duration_ms": 1.304000,
#   "total_working_ms": 5447.164000,
#   "db_size_gb": 0.133791744,
#   "elapsed_ms": 6604.240000,
#   "nvars": 1,
#   "tracked_gb": 0.000788880,
#   "cur_var": "bb3FfKo",
#   "cur_var_len": 49305,
#   "cur_var_tsize": 16,
#   "start": 33194,
#   "stride": 11149,
#   "nelems": 2,
#   "bytes_moved": 32,
#   "bytes_per_ms": 24.540,
#   "total_gb_moved": 0.077881197,
#   "avg_bytes_per_ms": 14297.568,
#   "placeholder": null
# }
# {
#   "action": "write",
#   "completed": true,
#   "in_txn": true,
#   "step": 1202,
#   "seed": 2172185989,
#   "commit_hash": "2f2096547dca3fc359e657e21a92e31f04994f2a",
#   "sequence_id": 2172185989,
#   "dbname": "simtest.db",
#   "max_tsize": 4096,
#   "max_insert_len": 1000000,
#   "sample_space_prob": 0,
#   "op_duration_ms": 1.071000,
#   "total_working_ms": 5448.255000,
#   "db_size_gb": 0.133791744,
#   "elapsed_ms": 6605.740000,
#   "nvars": 1,
#   "tracked_gb": 0.000788880,
#   "cur_var": "bb3FfKo",
#   "cur_var_len": 49305,
#   "cur_var_tsize": 16,
#   "start": 41280,
#   "stride": 1803,
#   "nelems": 1,
#   "bytes_moved": 16,
#   "bytes_per_ms": 14.939,
#   "total_gb_moved": 0.077881213,
#   "avg_bytes_per_ms": 14294.708,
#   "placeholder": null
# }
# {
#   "action": "delete",
#   "completed": true,
#   "in_txn": true,
#   "step": 1203,
#   "seed": 2172185989,
#   "commit_hash": "2f2096547dca3fc359e657e21a92e31f04994f2a",
#   "sequence_id": 2172185989,
#   "dbname": "simtest.db",
#   "max_tsize": 4096,
#   "max_insert_len": 1000000,
#   "sample_space_prob": 0,
#   "op_duration_ms": 20.001000,
#   "total_working_ms": 5468.256000,
#   "db_size_gb": 0.133791744,
#   "elapsed_ms": 6625.762000,
#   "nvars": 0,
#   "tracked_gb": 0.000000000,
#   "next": null,
#   "placeholder": null
# }
# [ERROR   ]: Root node must have at least 1 element
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
#   "step": 1204,
#   "seed": 2172185989,
#   "commit_hash": "2f2096547dca3fc359e657e21a92e31f04994f2a",
#   "sequence_id": 2172185989,
#   "dbname": "simtest.db",
#   "max_tsize": 4096,
#   "max_insert_len": 1000000,
#   "sample_space_prob": 0,
#   "op_duration_ms": 116.170000,
#   "total_working_ms": 5584.426000,
#   "db_size_gb": 0.133791744,
#   "elapsed_ms": 6743.099000,
#   "nvars": 4,
#   "tracked_gb": 0.006602399,
#   "cur_var": "zT",
#   "cur_var_len": 738996,
#   "cur_var_tsize": 5,
#   "placeholder": null
# }
# [ERROR   ]: TRACE: ns_numstore_simulation.c:978 (ns_simul_step):
# [ERROR   ]: Root node must have at least 1 element

build/debug/bin/ns_simtest \
      --dbname simtest.db \
      --duration 10 \
      --seed 2172185989 \
      --commit-hash 2f2096547dca3fc359e657e21a92e31f04994f2a \
      --seqid 2172185989
