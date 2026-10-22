"""Run a command inside a Windows pseudo-console (ConPTY) and capture what it prints (D27).

`claude --cloud` refuses to run unless stdout is a terminal (M0 item 10). ConPTY (Windows 10 1809+)
gives the child a real console while we read its output from a pipe. Standard library only (ctypes).
"""
import ctypes
import os
import re
import subprocess
import threading
import time
from ctypes import wintypes

ANSI = re.compile(r'\x1b\[[0-?]*[ -/]*[@-~]|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)|\x1b[@-Z\\-_]')

if os.name == 'nt':
    kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)

    class COORD(ctypes.Structure):
        _fields_ = [('X', wintypes.SHORT), ('Y', wintypes.SHORT)]

    class STARTUPINFOW(ctypes.Structure):
        _fields_ = [('cb', wintypes.DWORD), ('lpReserved', wintypes.LPWSTR), ('lpDesktop', wintypes.LPWSTR),
                    ('lpTitle', wintypes.LPWSTR), ('dwX', wintypes.DWORD), ('dwY', wintypes.DWORD),
                    ('dwXSize', wintypes.DWORD), ('dwYSize', wintypes.DWORD), ('dwXCountChars', wintypes.DWORD),
                    ('dwYCountChars', wintypes.DWORD), ('dwFillAttribute', wintypes.DWORD),
                    ('dwFlags', wintypes.DWORD), ('wShowWindow', wintypes.WORD), ('cbReserved2', wintypes.WORD),
                    ('lpReserved2', ctypes.c_void_p), ('hStdInput', wintypes.HANDLE),
                    ('hStdOutput', wintypes.HANDLE), ('hStdError', wintypes.HANDLE)]

    class STARTUPINFOEXW(ctypes.Structure):
        _fields_ = [('StartupInfo', STARTUPINFOW), ('lpAttributeList', ctypes.c_void_p)]

    class PROCESS_INFORMATION(ctypes.Structure):
        _fields_ = [('hProcess', wintypes.HANDLE), ('hThread', wintypes.HANDLE),
                    ('dwProcessId', wintypes.DWORD), ('dwThreadId', wintypes.DWORD)]

    kernel32.CreatePseudoConsole.argtypes = [COORD, wintypes.HANDLE, wintypes.HANDLE, wintypes.DWORD,
                                             ctypes.POINTER(ctypes.c_void_p)]
    kernel32.CreatePseudoConsole.restype = ctypes.c_long
    kernel32.ClosePseudoConsole.argtypes = [ctypes.c_void_p]
    kernel32.CreatePipe.argtypes = [ctypes.POINTER(wintypes.HANDLE), ctypes.POINTER(wintypes.HANDLE),
                                    ctypes.c_void_p, wintypes.DWORD]
    kernel32.InitializeProcThreadAttributeList.argtypes = [ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD,
                                                           ctypes.POINTER(ctypes.c_size_t)]
    kernel32.UpdateProcThreadAttribute.argtypes = [ctypes.c_void_p, wintypes.DWORD, ctypes.c_size_t,
                                                   ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                                                   ctypes.c_void_p]
    kernel32.DeleteProcThreadAttributeList.argtypes = [ctypes.c_void_p]
    kernel32.CreateProcessW.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR, ctypes.c_void_p, ctypes.c_void_p,
                                        wintypes.BOOL, wintypes.DWORD, ctypes.c_void_p, wintypes.LPCWSTR,
                                        ctypes.POINTER(STARTUPINFOEXW), ctypes.POINTER(PROCESS_INFORMATION)]
    kernel32.ReadFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                                  ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
    kernel32.WriteFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                                   ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
    kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    kernel32.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]

EXTENDED_STARTUPINFO_PRESENT = 0x00080000
CREATE_UNICODE_ENVIRONMENT = 0x00000400
PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE = 0x00020016
STARTF_USESTDHANDLES = 0x00000100
WAIT_OBJECT_0 = 0


def _check(ok, what):
    if not ok:
        raise OSError(ctypes.get_last_error(), f'{what} failed')


def _env_block(env):
    items = sorted(env.items(), key=lambda kv: kv[0].upper())
    return ''.join(f'{k}={v}\0' for k, v in items) + '\0'


def run(args, *, cwd=None, env=None, timeout=120, stop_when=None, columns=200, rows=50):
    """Run args under a pseudo-console. Returns (exit_code or None if stopped, text without terminal codes).

    stop_when(text) -> bool ends the run early (the child is terminated) once it returns True;
    the cloud launcher uses it to stop after the session id has been printed.
    """
    if os.name != 'nt':
        raise OSError('ConPTY is Windows-only')
    in_read, in_write, out_read, out_write = (wintypes.HANDLE() for _ in range(4))
    _check(kernel32.CreatePipe(ctypes.byref(in_read), ctypes.byref(in_write), None, 0), 'CreatePipe')
    _check(kernel32.CreatePipe(ctypes.byref(out_read), ctypes.byref(out_write), None, 0), 'CreatePipe')
    hpc = ctypes.c_void_p()
    hr = kernel32.CreatePseudoConsole(COORD(columns, rows), in_read, out_write, 0, ctypes.byref(hpc))
    if hr != 0:
        raise OSError(hr, 'CreatePseudoConsole failed')
    kernel32.CloseHandle(in_read)
    kernel32.CloseHandle(out_write)
    size = ctypes.c_size_t()
    kernel32.InitializeProcThreadAttributeList(None, 1, 0, ctypes.byref(size))
    attrs = ctypes.create_string_buffer(size.value)
    _check(kernel32.InitializeProcThreadAttributeList(attrs, 1, 0, ctypes.byref(size)), 'InitializeProcThreadAttributeList')
    _check(kernel32.UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hpc,
                                              ctypes.sizeof(ctypes.c_void_p), None, None), 'UpdateProcThreadAttribute')
    si = STARTUPINFOEXW()
    si.StartupInfo.cb = ctypes.sizeof(STARTUPINFOEXW)
    # Without this the child inherits our (possibly redirected) std handles instead of the pseudo-console.
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES
    si.lpAttributeList = ctypes.cast(attrs, ctypes.c_void_p)
    pi = PROCESS_INFORMATION()
    block = ctypes.create_unicode_buffer(_env_block(env if env is not None else dict(os.environ)))
    cmdline = ctypes.create_unicode_buffer(subprocess.list2cmdline([str(a) for a in args]))
    _check(kernel32.CreateProcessW(None, cmdline, None, None, False,
                                   EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, block,
                                   str(cwd) if cwd else None, ctypes.byref(si), ctypes.byref(pi)), 'CreateProcessW')
    chunks, lock = [], threading.Lock()

    def reader():
        buf = ctypes.create_string_buffer(65536)
        n = wintypes.DWORD()
        while kernel32.ReadFile(out_read, buf, len(buf), ctypes.byref(n), None) and n.value:
            data = buf.raw[:n.value]
            with lock:
                chunks.append(data)
            if b'\x1b[6n' in data:  # cursor-position query: answer it, or some programs wait forever
                reply = b'\x1b[1;1R'
                kernel32.WriteFile(in_write, reply, len(reply), ctypes.byref(wintypes.DWORD()), None)

    thread = threading.Thread(target=reader, daemon=True)
    thread.start()

    def text():
        with lock:
            raw = b''.join(chunks)
        return ANSI.sub('', raw.decode('utf-8', 'replace')).replace('\r', '')

    deadline = time.monotonic() + timeout
    code = None
    try:
        while True:
            if kernel32.WaitForSingleObject(pi.hProcess, 200) == WAIT_OBJECT_0:
                exit_code = wintypes.DWORD()
                kernel32.GetExitCodeProcess(pi.hProcess, ctypes.byref(exit_code))
                code = exit_code.value
                break
            if stop_when and stop_when(text()):
                time.sleep(1.0)  # let the last lines arrive
                kernel32.TerminateProcess(pi.hProcess, 0)
                break
            if time.monotonic() > deadline:
                kernel32.TerminateProcess(pi.hProcess, 1)
                break
    finally:
        kernel32.ClosePseudoConsole(hpc)  # also ends the reader's ReadFile
        thread.join(timeout=5)
        for h in (in_write, out_read, pi.hProcess, pi.hThread):
            kernel32.CloseHandle(h)
        kernel32.DeleteProcThreadAttributeList(attrs)
    return code, text()
