#include <pybind11/pybind11.h>
#include "transport.hpp"

namespace py = pybind11;

/* Adds a Python-source strong reference to ScopedBuffer's RAII semantics,
 * so the underlying memory (bytearray / numpy / torch tensor) survives until
 * the MR is dereg'd. */
struct RegisteredBuffer {
    ScopedBuffer sb;
    py::object source;
};

PYBIND11_MODULE(_core, m) {
    m.doc() = "Low-level pybind11 binding for the rai_rdma Transport layer";

    py::class_<RegisteredBuffer>(m, "RegisteredBuffer")
        .def_property_readonly("addr",
            [](const RegisteredBuffer &r) {
                return reinterpret_cast<uintptr_t>(r.sb.h.addr);
            })
        .def_property_readonly("size",
            [](const RegisteredBuffer &r) {return r.sb.h.size;});

    py::class_<Transport>(m, "Transport")
        .def("listen", &Transport::listen, py::arg("port"))
        .def("accept", &Transport::accept, py::call_guard<py::gil_scoped_release>())
        .def("connect", &Transport::connect,
            py::arg("host"), py::arg("port"),
            py::call_guard<py::gil_scoped_release>())
        .def("close", &Transport::close)
        .def("reg_buf",
            [](Transport &t, py::buffer b) {
                py::buffer_info info = b.request(true);   // writable
                auto rb = std::make_unique<RegisteredBuffer>();
                rb->source = py::reinterpret_borrow<py::object>(b);
                if (rb->sb.init(&t, info.ptr, info.size * info.itemsize) != 0) {
                    throw std::runtime_error("reg_buf failed");
                }
                return rb;
            },
            py::arg("buf"),
            py::keep_alive<0, 1>())    // keep Transport alive while RegisteredBuffer lives
        .def("send_async",
            [](Transport &t, RegisteredBuffer &mr, size_t len, uint64_t id, size_t offset) {
                if (t.send_async(&mr.sb.h, len, id, offset) != 0)
                    throw std::runtime_error("send_async failed");
            },
            py::arg("mr"), py::arg("len"), py::arg("id") = 0, py::arg("offset") = 0)
        .def("recv_async",
            [](Transport &t, RegisteredBuffer &mr, size_t len, uint64_t id, size_t offset) {
                if (t.recv_async(&mr.sb.h, len, id, offset) != 0)
                    throw std::runtime_error("recv_async failed");
            },
            py::arg("mr"), py::arg("len"), py::arg("id") = 0, py::arg("offset") = 0)
        .def("poll",
            [](Transport &t) {
                uint64_t id = 0;
                if (t.poll(&id) != 0)
                    throw std::runtime_error("poll failed");
                return id;
            },
            py::call_guard<py::gil_scoped_release>())
        .def("exchange_buf",
            [](Transport &t, RegisteredBuffer &mr) {
                uint64_t remote_addr = 0;
                uint32_t rkey = 0;
                if (t.exchange_buf(&mr.sb.h, &remote_addr, &rkey) != 0)
                    throw std::runtime_error("exchange_buf failed");
                return py::make_tuple(remote_addr, rkey);
            },
            py::arg("mr"),
            py::call_guard<py::gil_scoped_release>())
        .def("write_async",
            [](Transport &t, RegisteredBuffer &mr, uint64_t remote_addr, uint32_t rkey,
            size_t len, uint64_t id, size_t offset) {
                if (t.write_async(&mr.sb.h, remote_addr, rkey, len, id, offset) != 0)
                    throw std::runtime_error("write_async failed");
            },
            py::arg("mr"), py::arg("remote_addr"), py::arg("rkey"),
            py::arg("len"), py::arg("id") = 0, py::arg("offset") = 0)
        .def("read_async",
            [](Transport &t, RegisteredBuffer &mr, uint64_t remote_addr, uint32_t rkey,
            size_t len, uint64_t id, size_t offset) {
                if (t.read_async(&mr.sb.h, remote_addr, rkey, len, id, offset) != 0)
                    throw std::runtime_error("read_async failed");
            },
            py::arg("mr"), py::arg("remote_addr"), py::arg("rkey"),
            py::arg("len"), py::arg("id") = 0, py::arg("offset") = 0);

    m.def("create_rdma", [](){
        return std::unique_ptr<Transport>(create_rdma_transport());
    });
    m.def("create_tcp", [](){
        return std::unique_ptr<Transport>(create_tcp_transport());
    });
}