#!/usr/bin/env bash 

# FAILS ON: close_and_reopen, step 1352
#
#   1348 begin_txn
#   1349 delete   in txn   nvars 28 -> 27
#   1350 delete   in txn   nvars 27 -> 26
#   1351 commit_txn       (the deletes are kept)
#   1352 close_and_reopen -> FAILS
#
#   "Bottom layer has: 1784266 bytes while root has: 399170 bytes"
#
# No corrupt page at all: every page walks fine, but the rope's size
# bookkeeping is wrong by 4.5x. The bottom layer holds 1784266 bytes while
# the root claims 399170.
#
# Unlike the others the transaction COMMITTED, so this is not an undo bug --
# a committed multi-delete left a surviving variable's root tracking the
# wrong total. Compare against 0004: same commit-then-reopen shape, and the
# damage is to bookkeeping rather than to structure.

# {
#   "action": "remove",
#   "completed": true,
#   "in_txn": true,
#   "step": 1344,
#   "seed": 984153725,
#   "commit_hash": "2f2096547dca3fc359e657e21a92e31f04994f2a",
#   "sequence_id": 984153725,
#   "dbname": "simtest.db",
#   "max_tsize": 4096,
#   "max_insert_len": 1000000,
#   "sample_space_prob": 0,
#   "op_duration_ms": 16.960000,
#   "total_working_ms": 6464.564000,
#   "db_size_gb": 0.133791744,
#   "elapsed_ms": 8889.436000,
#   "nvars": 29,
#   "tracked_gb": 0.013293184,
#   "cur_var": "FtWfPoD",
#   "cur_var_len": 776506,
#   "cur_var_tsize": 2,
#   "start": 83277,
#   "stride": 162721,
#   "nelems": 5,
#   "bytes_moved": 10,
#   "bytes_per_ms": 0.590,
#   "total_gb_moved": 0.095507590,
#   "avg_bytes_per_ms": 14774.019,
#   "placeholder": null
# }
# {
#   "action": "write",
#   "completed": true,
#   "in_txn": true,
#   "step": 1345,
#   "seed": 984153725,
#   "commit_hash": "2f2096547dca3fc359e657e21a92e31f04994f2a",
#   "sequence_id": 984153725,
#   "dbname": "simtest.db",
#   "max_tsize": 4096,
#   "max_insert_len": 1000000,
#   "sample_space_prob": 0,
#   "op_duration_ms": 15.957000,
#   "total_working_ms": 6481.062000,
#   "db_size_gb": 0.133791744,
#   "elapsed_ms": 8909.650000,
#   "nvars": 29,
#   "tracked_gb": 0.013293184,
#   "cur_var": "FtWfPoD",
#   "cur_var_len": 776506,
#   "cur_var_tsize": 2,
#   "start": 518513,
#   "stride": 249864,
#   "nelems": 2,
#   "bytes_moved": 4,
#   "bytes_per_ms": 0.251,
#   "total_gb_moved": 0.095507594,
#   "avg_bytes_per_ms": 14736.411,
#   "placeholder": null
# }
# {
#   "action": "read",
#   "completed": true,
#   "in_txn": true,
#   "step": 1346,
#   "seed": 984153725,
#   "commit_hash": "2f2096547dca3fc359e657e21a92e31f04994f2a",
#   "sequence_id": 984153725,
#   "dbname": "simtest.db",
#   "max_tsize": 4096,
#   "max_insert_len": 1000000,
#   "sample_space_prob": 0,
#   "op_duration_ms": 0.029000,
#   "total_working_ms": 6481.091000,
#   "db_size_gb": 0.133791744,
#   "elapsed_ms": 8909.708000,
#   "nvars": 29,
#   "tracked_gb": 0.013293184,
#   "cur_var": "FtWfPoD",
#   "cur_var_len": 776506,
#   "cur_var_tsize": 2,
#   "start": 320901,
#   "stride": 412193,
#   "nelems": 2,
#   "bytes_moved": 4,
#   "bytes_per_ms": 137.931,
#   "total_gb_moved": 0.095507598,
#   "avg_bytes_per_ms": 14736.346,
#   "placeholder": null
# }
# {
#   "action": "rollback_txn",
#   "completed": true,
#   "in_txn": false,
#   "step": 1347,
#   "seed": 984153725,
#   "commit_hash": "2f2096547dca3fc359e657e21a92e31f04994f2a",
#   "sequence_id": 984153725,
#   "dbname": "simtest.db",
#   "max_tsize": 4096,
#   "max_insert_len": 1000000,
#   "sample_space_prob": 0,
#   "op_duration_ms": 15.063000,
#   "total_working_ms": 6496.154000,
#   "db_size_gb": 0.133791744,
#   "elapsed_ms": 8925.192000,
#   "nvars": 28,
#   "tracked_gb": 0.012139342,
#   "cur_var": "R2x",
#   "cur_var_len": 13406,
#   "cur_var_tsize": 145,
#   "placeholder": null
# }
# {
#   "action": "begin_txn",
#   "completed": true,
#   "in_txn": true,
#   "step": 1348,
#   "seed": 984153725,
#   "commit_hash": "2f2096547dca3fc359e657e21a92e31f04994f2a",
#   "sequence_id": 984153725,
#   "dbname": "simtest.db",
#   "max_tsize": 4096,
#   "max_insert_len": 1000000,
#   "sample_space_prob": 0,
#   "op_duration_ms": 0.004000,
#   "total_working_ms": 6496.158000,
#   "db_size_gb": 0.133791744,
#   "elapsed_ms": 8925.641000,
#   "nvars": 28,
#   "tracked_gb": 0.012139342,
#   "cur_var": "R2x",
#   "cur_var_len": 13406,
#   "cur_var_tsize": 145,
#   "placeholder": null
# }
# {
#   "action": "delete",
#   "completed": true,
#   "in_txn": true,
#   "step": 1349,
#   "seed": 984153725,
#   "commit_hash": "2f2096547dca3fc359e657e21a92e31f04994f2a",
#   "sequence_id": 984153725,
#   "dbname": "simtest.db",
#   "max_tsize": 4096,
#   "max_insert_len": 1000000,
#   "sample_space_prob": 0,
#   "op_duration_ms": 64.946000,
#   "total_working_ms": 6561.104000,
#   "db_size_gb": 0.133791744,
#   "elapsed_ms": 8990.616000,
#   "nvars": 27,
#   "tracked_gb": 0.010195472,
#   "cur_var": "hW11g",
#   "cur_var_len": 482215,
#   "cur_var_tsize": 2,
#   "next": "hW11g",
#   "placeholder": null
# }
# {
#   "action": "delete",
#   "completed": true,
#   "in_txn": true,
#   "step": 1350,
#   "seed": 984153725,
#   "commit_hash": "2f2096547dca3fc359e657e21a92e31f04994f2a",
#   "sequence_id": 984153725,
#   "dbname": "simtest.db",
#   "max_tsize": 4096,
#   "max_insert_len": 1000000,
#   "sample_space_prob": 0,
#   "op_duration_ms": 25.743000,
#   "total_working_ms": 6586.847000,
#   "db_size_gb": 0.133791744,
#   "elapsed_ms": 9016.380000,
#   "nvars": 26,
#   "tracked_gb": 0.009231042,
#   "cur_var": "Xz2fCmn",
#   "cur_var_len": 0,
#   "cur_var_tsize": 32,
#   "next": "Xz2fCmn",
#   "placeholder": null
# }
# {
#   "action": "commit_txn",
#   "completed": true,
#   "in_txn": false,
#   "step": 1351,
#   "seed": 984153725,
#   "commit_hash": "2f2096547dca3fc359e657e21a92e31f04994f2a",
#   "sequence_id": 984153725,
#   "dbname": "simtest.db",
#   "max_tsize": 4096,
#   "max_insert_len": 1000000,
#   "sample_space_prob": 0,
#   "op_duration_ms": 0.315000,
#   "total_working_ms": 6587.162000,
#   "db_size_gb": 0.133791744,
#   "elapsed_ms": 9017.174000,
#   "nvars": 26,
#   "tracked_gb": 0.009231042,
#   "cur_var": "Xz2fCmn",
#   "cur_var_len": 0,
#   "cur_var_tsize": 32,
#   "placeholder": null
# }
# [ERROR   ]: Bottom layer has: 1784266 bytes while root has: 399170 bytes
# [ERROR   ]: TRACE: ns_visit_variables.c:62 (ns_consume_one_var):
# [ERROR   ]: TRACE: ns_visit_variables.c:102 (ns_consume_var_chain):
# [ERROR   ]: TRACE: ns_visit_variables.c:135 (ns_visit_variables):
# [ERROR   ]: TRACE: ns_numstore_simulation.c:124 (nss_close_and_reopen):
# {
#   "action": "close_and_reopen",
#   "completed": false,
#   "in_txn": false,
#   "step": 1352,
#   "seed": 984153725,
#   "commit_hash": "2f2096547dca3fc359e657e21a92e31f04994f2a",
#   "sequence_id": 984153725,
#   "dbname": "simtest.db",
#   "max_tsize": 4096,
#   "max_insert_len": 1000000,
#   "sample_space_prob": 0,
#   "op_duration_ms": 9.459000,
#   "total_working_ms": 6596.621000,
#   "db_size_gb": 0.133791744,
#   "elapsed_ms": 9028.090000,
#   "nvars": 26,
#   "tracked_gb": 0.009231042,
#   "cur_var": "Xz2fCmn",
#   "cur_var_len": 0,
#   "cur_var_tsize": 32,
#   "placeholder": null
# }
# [ERROR   ]: TRACE: ns_numstore_simulation.c:978 (ns_simul_step):
# [ERROR   ]: Bottom layer has: 1784266 bytes while root has: 399170 bytes

build/debug/bin/ns_simtest \
      --dbname simtest.db \
      --duration 10 \
      --seed 984153725 \
      --commit-hash 2f2096547dca3fc359e657e21a92e31f04994f2a \
      --seqid 984153725
