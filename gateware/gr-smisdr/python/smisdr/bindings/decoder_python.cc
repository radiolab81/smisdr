/*
 * Handgeschriebene pybind11-Bindings (siehe Hinweis in encoder_python.cc).
 */
#include <pybind11/complex.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;

#include <gnuradio/smisdr/decoder.h>

void bind_decoder(py::module& m)
{
    using decoder = gr::smisdr::decoder;

    py::class_<decoder, gr::block, gr::basic_block, std::shared_ptr<decoder>>(
        m, "decoder", "smiSDR In-Band-Signaling Decoder (short -> complex + cmd-Messages)")

        .def(py::init(&decoder::make),
             py::arg("master_clock") = 50e6,
             py::arg("scale") = 8191,
             py::arg("cmd_timeout_words") = 0,
             "Erzeugt einen smisdr.decoder Block");
}
