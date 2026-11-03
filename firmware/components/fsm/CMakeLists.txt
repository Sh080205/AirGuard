# FW-6: device state machine (pure C, host-tested). Called by the io task
# (main/tasks_io.c) since FW-6 step 3.
idf_component_register(
    SRCS "ag_fsm.c"
    INCLUDE_DIRS "include"
)
