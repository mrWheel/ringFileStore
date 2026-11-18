idf_component_register(
    SRCS "src/ring_file_store.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp_common log
)
