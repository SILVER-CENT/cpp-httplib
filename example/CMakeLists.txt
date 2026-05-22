set(_examples
    accept_header
    benchmark
    client
    hello
    one_time_request
    redirect
    server
    server_and_client
    simplecli
    simplesvr
    ssecli
    ssecli-stream
    ssesvr
    upload
    wsecho
)

foreach(_ex ${_examples})
    string(REPLACE "-" "_" _target "example_${_ex}")
    add_executable(${_target} ${_ex}.cc)
    target_link_libraries(${_target} PRIVATE httplib::httplib)
    set_target_properties(${_target} PROPERTIES OUTPUT_NAME ${_ex})
endforeach()
