set(io_src_files
    network/addrinfo.cpp
    network/endpoint.cpp
    network/socket.cpp
    network/utils.cpp)

add_library(mg-io STATIC ${io_src_files})
target_link_libraries(mg-io stdc++fs Threads::Threads fmt mg-utils)
