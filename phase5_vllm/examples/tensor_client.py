import torch
import rai_rdma._core as core
from rai_rdma import register

t = core.create_tcp()
t.connect("127.0.0.1", 12345)

buf = torch.arange(1024, dtype=torch.float32)
size = buf.numel() * buf.element_size()
mr = register(t, buf)

t.send_async(mr, size)
t.poll()

buf.zero_()
t.recv_async(mr, size)
t.poll()

print("client got: ", buf[:8].tolist())