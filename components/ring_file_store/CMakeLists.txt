idf_component_register(
    SRCS "src/ring_file_store.c" "src/rfs_time.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp_common log
)
