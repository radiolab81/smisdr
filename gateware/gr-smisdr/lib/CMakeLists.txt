include(GrPlatform) #define LIB_SUFFIX

list(APPEND smisdr_sources
    encoder_impl.cc
    decoder_impl.cc
)

add_library(gnuradio-smisdr SHARED ${smisdr_sources})
target_link_libraries(gnuradio-smisdr PUBLIC gnuradio::gnuradio-runtime gnuradio::gnuradio-pmt)
target_include_directories(gnuradio-smisdr
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_SOURCE_DIR}/lib
)
set_target_properties(gnuradio-smisdr PROPERTIES DEFINE_SYMBOL "gnuradio_smisdr_EXPORTS")

set_target_properties(gnuradio-smisdr PROPERTIES
    VERSION ${SMISDR_VERSION_MAJOR}.${SMISDR_VERSION_MINOR}.${SMISDR_VERSION_PATCH}
    SOVERSION ${SMISDR_VERSION_MAJOR}
)

install(TARGETS gnuradio-smisdr
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# ---------------------------------------------------------------------------
# Optionale QA-Tests (CTest). Einfache Selbsttests ohne GNU Radio Runtime-
# Abhängigkeit, prüfen nur die Protokoll-Bitlogik.
# ---------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/qa_smisdr.cc)
    add_executable(test-smisdr qa_smisdr.cc)
    target_link_libraries(test-smisdr gnuradio-smisdr)
    add_test(NAME qa_smisdr COMMAND test-smisdr)
endif()
