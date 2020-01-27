project(xxHash C)
set(DIR external/xxHash-r42)

add_library(xxHash OBJECT ${DIR}/xxhash.c ${DIR}/xxhsum.c)
target_include_directories(xxHash SYSTEM PUBLIC ${DIR})

add_library(xxHash::xxHash ALIAS xxHash)