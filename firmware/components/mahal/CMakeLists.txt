# FW-7: Mahalanobis score from the frozen ML-3 header (pure C).
# model/ holds the generated header; only ag_mahal.c includes it.
idf_component_register(
    SRCS "ag_mahal.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "model"
    REQUIRES storage
)
