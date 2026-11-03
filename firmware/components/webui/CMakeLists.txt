# FW-8 step 2: dashboard JSON + 10-min history ring (pure C, host-tested).
# The web task and HTTP server are added in step 3 (docs/12-DASHBOARD-DESIGN.md).
# web_json.h includes csv_row.h (public); ag_fsm.h is only used for the
# AG_FSM_FAULT_* bits inside web_json.c.
idf_component_register(
    SRCS "web_json.c"
    INCLUDE_DIRS "include"
    REQUIRES storage
    PRIV_REQUIRES fsm
)
