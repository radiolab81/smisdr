/*
 * Handgeschriebene pybind11-Bindings (kompatibel zum Layout, das
 * `gr_modtool bind` für GNU Radio 3.10 OOT-Module erzeugen würde).
 * Hinweis: Wer die Docstrings automatisch aus den Header-Kommentaren
 * ziehen möchte, kann `gr_modtool bind encoder` laufen lassen -
 * das ersetzt diese Datei durch eine Version mit D(...)-Makros.
 */
#include <pybind11/complex.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;

#include <gnuradio/smisdr/encoder.h>

void bind_encoder(py::module& m)
{
    using encoder = gr::smisdr::encoder;

    py::class_<encoder, gr::block, gr::basic_block, std::shared_ptr<encoder>>(
        m, "encoder", "smiSDR In-Band-Signaling Encoder (complex -> short)")

        .def(py::init(&encoder::make),
             py::arg("sample_rate"),
             py::arg("shift_hz"),
             py::arg("master_clock") = 50e6,
             py::arg("scale") = 8191,
             py::arg("inject_at_start") = true,
             "Erzeugt einen smisdr.encoder Block")

        .def("set_shift",
             &encoder::set_shift,
             py::arg("shift_hz"),
             "Injiziert zur Laufzeit ein neues 'S'-Kommando in den Stream")

        .def("set_sample_rate",
             &encoder::set_sample_rate,
             py::arg("sample_rate"),
             "Injiziert zur Laufzeit ein neues 'R'-Kommando in den Stream");
}
