idf_component_register(
    SRCS "src/button.c"
    INCLUDE_DIRS "include"
    REQUIRES driver
    PRIV_REQUIRES board_profile log
)
