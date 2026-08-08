import rai_rdma._core as core

t = core.create_tcp()
t.listen(12345)
t.accept()

buf = bytearray(4096)
mr = t.reg_buf(buf)

t.recv_async(mr, 4096)
t.poll()
print("server got: ", bytes(buf[:16]))

# echo
buf[:16] = b"echo: " + bytes(buf[:10])
t.send_async(mr, 4096)
t.poll()
print("server done")