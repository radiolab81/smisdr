#include <pybind11/pybind11.h>

namespace py = pybind11;

void bind_encoder(py::module&);
void bind_decoder(py::module&);

PYBIND11_MODULE(smisdr_python, m)
{
    py::module::import("gnuradio.gr");

    bind_encoder(m);
    bind_decoder(m);
}
