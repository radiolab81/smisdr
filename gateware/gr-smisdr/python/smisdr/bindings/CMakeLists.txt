find_package(pybind11 CONFIG REQUIRED)

list(APPEND smisdr_python_files
    encoder_python.cc
    decoder_python.cc
    python_bindings.cc
)

pybind11_add_module(smisdr_python MODULE ${smisdr_python_files})

target_link_libraries(smisdr_python
    PRIVATE
        gnuradio-smisdr
        gnuradio::gnuradio-runtime
        gnuradio::gnuradio-pmt
)
target_include_directories(smisdr_python PRIVATE ${CMAKE_SOURCE_DIR}/include)

install(TARGETS smisdr_python DESTINATION ${GR_PYTHON_DIR}/gnuradio/smisdr COMPONENT pythonapi)
