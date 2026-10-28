#!/usr/bin/env bash 

# FAILS ON: close_and_reopen, step 1526
#
#   1522 insert   in txn
#   1523 insert   in txn
#   1524 delete   in txn   nvars 25 -> 24
#   1525 rollback_txn     nvars back to 25
#   1526 close_and_reopen -> FAILS
#
# A clean close, not a crash, and the delete was already explicitly rolled
# back one step earlier. Walking the restored variable's layers hits a page
# with a garbage type byte:
#
#   ns_visit_variables -> ns_consume_one_var -> ns_rope_validate
#     -> ns_rope_get_layers (ns_rope_layer_scan.c:39) -> pgr_get
#     -> "expected page type UNKNOWN, got UNKNOWN"
#
# Worth noting: cur_var here is one of the very long generated names (~250+
# chars), so its directory entry spills into PG_VAR_TAIL overflow pages.

# {
#   "action": "insert",
#   "completed": true,
#   "in_txn": true,
#   "step": 1521,
#   "seed": 3416210320,
#   "commit_hash": "2f2096547dca3fc359e657e21a92e31f04994f2a",
#   "sequence_id": 3416210320,
#   "dbname": "simtest.db",
#   "max_tsize": 4096,
#   "max_insert_len": 1000000,
#   "sample_space_prob": 0,
#   "op_duration_ms": 0.251000,
#   "total_working_ms": 4972.450000,
#   "db_size_gb": 0.133791744,
#   "elapsed_ms": 6194.938000,
#   "nvars": 25,
#   "tracked_gb": 0.010767176,
#   "cur_var": "VLyD9",
#   "cur_var_len": 9547,
#   "cur_var_tsize": 64,
#   "ofst": 0,
#   "nelems": 9547,
#   "bytes_moved": 611008,
#   "bytes_per_ms": 2434294.821,
#   "total_gb_moved": 0.083440270,
#   "avg_bytes_per_ms": 16780.515,
#   "placeholder": null
# }
# {
#   "action": "insert",
#   "completed": true,
#   "in_txn": true,
#   "step": 1522,
#   "seed": 3416210320,
#   "commit_hash": "2f2096547dca3fc359e657e21a92e31f04994f2a",
#   "sequence_id": 3416210320,
#   "dbname": "simtest.db",
#   "max_tsize": 4096,
#   "max_insert_len": 1000000,
#   "sample_space_prob": 0,
#   "op_duration_ms": 0.793000,
#   "total_working_ms": 4976.134000,
#   "db_size_gb": 0.133791744,
#   "elapsed_ms": 6200.000000,
#   "nvars": 25,
#   "tracked_gb": 0.011810120,
#   "cur_var": "VLyD9",
#   "cur_var_len": 25843,
#   "cur_var_tsize": 64,
#   "ofst": 3187,
#   "nelems": 16296,
#   "bytes_moved": 1042944,
#   "bytes_per_ms": 1315187.894,
#   "total_gb_moved": 0.084483214,
#   "avg_bytes_per_ms": 16977.681,
#   "placeholder": null
# }
# {
#   "action": "insert",
#   "completed": true,
#   "in_txn": true,
#   "step": 1523,
#   "seed": 3416210320,
#   "commit_hash": "2f2096547dca3fc359e657e21a92e31f04994f2a",
#   "sequence_id": 3416210320,
#   "dbname": "simtest.db",
#   "max_tsize": 4096,
#   "max_insert_len": 1000000,
#   "sample_space_prob": 0,
#   "op_duration_ms": 0.894000,
#   "total_working_ms": 4978.563000,
#   "db_size_gb": 0.133791744,
#   "elapsed_ms": 6203.244000,
#   "nvars": 25,
#   "tracked_gb": 0.012214664,
#   "cur_var": "VLyD9",
#   "cur_var_len": 32164,
#   "cur_var_tsize": 64,
#   "ofst": 14281,
#   "nelems": 6321,
#   "bytes_moved": 404544,
#   "bytes_per_ms": 452510.067,
#   "total_gb_moved": 0.084887758,
#   "avg_bytes_per_ms": 17050.655,
#   "placeholder": null
# }
# {
#   "action": "delete",
#   "completed": true,
#   "in_txn": true,
#   "step": 1524,
#   "seed": 3416210320,
#   "commit_hash": "2f2096547dca3fc359e657e21a92e31f04994f2a",
#   "sequence_id": 3416210320,
#   "dbname": "simtest.db",
#   "max_tsize": 4096,
#   "max_insert_len": 1000000,
#   "sample_space_prob": 0,
#   "op_duration_ms": 51.627000,
#   "total_working_ms": 5030.190000,
#   "db_size_gb": 0.133791744,
#   "elapsed_ms": 6254.889000,
#   "nvars": 24,
#   "tracked_gb": 0.010156168,
#   "cur_var": "DUX2dDTZ",
#   "cur_var_len": 0,
#   "cur_var_tsize": 8,
#   "next": "DUX2dDTZ",
#   "placeholder": null
# }
# {
#   "action": "rollback_txn",
#   "completed": true,
#   "in_txn": false,
#   "step": 1525,
#   "seed": 3416210320,
#   "commit_hash": "2f2096547dca3fc359e657e21a92e31f04994f2a",
#   "sequence_id": 3416210320,
#   "dbname": "simtest.db",
#   "max_tsize": 4096,
#   "max_insert_len": 1000000,
#   "sample_space_prob": 0,
#   "op_duration_ms": 18.929000,
#   "total_working_ms": 5049.119000,
#   "db_size_gb": 0.133791744,
#   "elapsed_ms": 6274.136000,
#   "nvars": 25,
#   "tracked_gb": 0.010156168,
#   "cur_var": "Ilbq54UoIy89RlH78kjPeA6xKkgVhpX72_IzVRvyaSWeheUKdlr_NRZgIW8FW8I5rDVtbNh4xE_pWrrCY0cU0fUqfUkF8fdNMLjioBx5ibWOMK9Ns2xm_GAss4BygJQ8wH3W5kDK5s0aTobslHYexHsoAbrcZXJZSmXrmJOxlI1EUXNFwko23wN1LQqZChsVXBG0XkmM83kTaSnwdfGrLvDZOZaLb7m9qym3GCev9yP6RO4oayvRMGieMV...",
#   "cur_var_len": 164391,
#   "cur_var_tsize": 2,
#   "placeholder": null
# }
# [ERROR   ]: expected page type UNKNOWN, got UNKNOWN
# [ERROR   ]: TRACE: ns_pgr_get.c:70 (pgr_get):
# [ERROR   ]: TRACE: ns_rope_layer_scan.c:39 (ns_rope_get_layers):
# [ERROR   ]: TRACE: ns_rope_validate.c:35 (ns_rope_validate):
# [ERROR   ]: TRACE: ns_visit_variables.c:62 (ns_consume_one_var):
# [ERROR   ]: TRACE: ns_visit_variables.c:102 (ns_consume_var_chain):
# [ERROR   ]: TRACE: ns_visit_variables.c:135 (ns_visit_variables):
# [ERROR   ]: TRACE: ns_numstore_simulation.c:124 (nss_close_and_reopen):
# {
#   "action": "close_and_reopen",
#   "completed": false,
#   "in_txn": false,
#   "step": 1526,
#   "seed": 3416210320,
#   "commit_hash": "2f2096547dca3fc359e657e21a92e31f04994f2a",
#   "sequence_id": 3416210320,
#   "dbname": "simtest.db",
#   "max_tsize": 4096,
#   "max_insert_len": 1000000,
#   "sample_space_prob": 0,
#   "op_duration_ms": 4.798000,
#   "total_working_ms": 5053.917000,
#   "db_size_gb": 0.133791744,
#   "elapsed_ms": 6281.990000,
#   "nvars": 25,
#   "tracked_gb": 0.010156168,
#   "cur_var": "Ilbq54UoIy89RlH78kjPeA6xKkgVhpX72_IzVRvyaSWeheUKdlr_NRZgIW8FW8I5rDVtbNh4xE_pWrrCY0cU0fUqfUkF8fdNMLjioBx5ibWOMK9Ns2xm_GAss4BygJQ8wH3W5kDK5s0aTobslHYexHsoAbrcZXJZSmXrmJOxlI1EUXNFwko23wN1LQqZChsVXBG0XkmM83kTaSnwdfGrLvDZOZaLb7m9qym3GCev9yP6RO4oayvRMGieMV...",
#   "cur_var_len": 164391,
#   "cur_var_tsize": 2,
#   "placeholder": null
# }
# [ERROR   ]: TRACE: ns_numstore_simulation.c:978 (ns_simul_step):
# [ERROR   ]: expected page type UNKNOWN, got UNKNOWN

build/debug/bin/ns_simtest \
      --dbname simtest.db \
      --duration 10 \
      --seed 3416210320 \
      --commit-hash 2f2096547dca3fc359e657e21a92e31f04994f2a \
      --seqid 3416210320
