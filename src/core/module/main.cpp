#include "pyinterp/detail/gsl/error_handler.hpp"
#include <pybind11/pybind11.h>

namespace py = pybind11;

extern void init_axis(py::module&);

PYBIND11_MODULE(core, m) {
  m.doc() = R"__doc__(
Core module
-----------
)__doc__";

  pyinterp::detail::gsl::set_error_handler();
  init_axis(m);
}