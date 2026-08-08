import rai_rdma._core as core

t = core.create_tcp()
t.connect("127.0.0.1", 12345)

buf = bytearray(b"hello!".ljust(4096, b" "))
mr = t.reg_buf(buf)

t.send_async(mr, 4096)
t.poll()

buf[:] = b"\0" * 4096
t.recv_async(mr, 4096)
t.poll()
print("client got: ", bytes(buf[:24]))