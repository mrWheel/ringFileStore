idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS ""
    PRIV_REQUIRES ring_file_store littlefs fatfs nvs_flash
)
