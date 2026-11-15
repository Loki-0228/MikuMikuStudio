"""Sparse-file helpers for the standalone format inspector regressions."""
import os


def make_sparse(stream):
    if os.name != "nt":
        return
    import ctypes
    from ctypes import wintypes
    import msvcrt

    control = ctypes.WinDLL("kernel32", use_last_error=True).DeviceIoControl
    control.argtypes = (wintypes.HANDLE, wintypes.DWORD, wintypes.LPVOID,
                        wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD,
                        ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID)
    control.restype = wintypes.BOOL
    returned = wintypes.DWORD()
    # FSCTL_SET_SPARSE must precede the seek beyond 4 GiB on Windows.
    if not control(msvcrt.get_osfhandle(stream.fileno()), 0x900C4,
                   None, 0, None, 0, ctypes.byref(returned), None):
        raise ctypes.WinError(ctypes.get_last_error())


def allocated_bytes(path):
    if os.name != "nt":
        return path.stat().st_blocks * 512
    import ctypes
    from ctypes import wintypes

    get_size = ctypes.WinDLL("kernel32", use_last_error=True).GetCompressedFileSizeW
    get_size.argtypes = (wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD))
    get_size.restype = wintypes.DWORD
    high = wintypes.DWORD()
    ctypes.set_last_error(0)
    low = get_size(str(path), ctypes.byref(high))
    if low == 0xFFFFFFFF and ctypes.get_last_error():
        raise ctypes.WinError(ctypes.get_last_error())
    return (high.value << 32) | low
