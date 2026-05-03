import torch
import rai_rdma._core as core
from rai_rdma import register

t = core.create_tcp()
t.listen(12345)
t.accept()

buf = torch.zeros(1024, dtype=torch.float32)
size = buf.numel() * buf.element_size()
mr = register(t, buf)

t.recv_async(mr, size)
t.poll()
print("server got: ", buf[:8].tolist())

buf.mul_(2)
t.send_async(mr, size)
t.poll()
print("server done")