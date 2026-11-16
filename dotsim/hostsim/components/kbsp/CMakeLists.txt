# kbsp for the sim: the SDK's board facts, without its project_include.cmake
# (that moves `flash` to the app mailbox, which a host build has no use for).
set(B "${CMAKE_CURRENT_LIST_DIR}/../../sdk/components/kbsp")
idf_component_register(
    SRCS "${B}/owners/src/kbsp_owners.c" "${B}/exp/src/kbsp_exp.c" "${B}/exp/src/kbsp_station.c"
    INCLUDE_DIRS "${B}/board/include" "${B}/exp/include" "${B}/owners/include"
)
