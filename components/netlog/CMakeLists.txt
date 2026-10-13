idf_component_register(
    SRCS "logbuf.c" "netlog.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES lwip log)
