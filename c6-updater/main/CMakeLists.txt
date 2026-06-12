idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES driver esp_timer console
                       EMBED_FILES "slave_fw.bin")
