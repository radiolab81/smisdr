include(GrPython)

add_subdirectory(bindings)

install(FILES
    __init__.py
    DESTINATION ${GR_PYTHON_DIR}/gnuradio/smisdr
)
