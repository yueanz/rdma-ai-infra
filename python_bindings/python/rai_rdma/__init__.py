from . import _core

def register(t: _core.Transport, obj) -> _core.RegisteredBuffer:
    """Register memory with the transport. Handles torch.Tensor and buffer-protocol objects."""
    try:
        import torch
        if isinstance(obj, torch.Tensor):
            if not obj.is_contiguous():
                raise ValueError("tensor must be contiguous")
            return t.reg_tensor(
                obj.data_ptr(),
                obj.numel() * obj.element_size(),
                obj,
            )
    except ImportError:
        pass
    return t.reg_buf(obj)