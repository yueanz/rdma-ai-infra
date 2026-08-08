import torch
import rai_rdma._core as core

t = core.create_tcp()
x = torch.zeros(1024, dtype=torch.float32)  # 4KB CPU tensor

mr = t.reg_tensor(x.data_ptr(), x.numel() * x.element_size(), x)

print("tensor data_ptr:", hex(x.data_ptr()))
print("MR addr:        ", hex(mr.addr))
print("MR size:        ", mr.size)
print("expected size:  ", x.numel() * x.element_size())