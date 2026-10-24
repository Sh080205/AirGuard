# FW-6: device state machine (pure C, host-tested). Not called by main yet:
# FW-6 step 3 wires it into the io task after FW-5b and FW-7 are merged.
idf_component_register(
    SRCS "ag_fsm.c"
    INCLUDE_DIRS "include"
)
