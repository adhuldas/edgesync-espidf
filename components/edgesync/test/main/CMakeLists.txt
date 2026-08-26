idf_component_register(
    SRCS "test_edgesync.c"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "../../src"
    REQUIRES edgesync unity esp_partition
)
