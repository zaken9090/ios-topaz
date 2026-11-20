#!/usr/bin/env python3
"""Read the lavender iOS console log over USB (WinUSB, Windows, no driver to install).

The kernel (xnu patch 1016, boot-args lv_usb=1 sweetusb=2) makes the phone a vendor USB device
(VID 1209 PID 0002) whose MS OS 2.0 descriptors bind Windows' built-in WinUSB driver, with the
device interface GUID below; bulk IN 0x81 carries the console log as it is written.

  python usblog.py [OUT.txt] [--grep WORD[,WORD...]] [--quiet]
  python usblog.py --cmd fastboot|reboot|poweroff     (send a command to the phone and exit)

Writes everything to OUT.txt (default dump\\usblog-<time>.txt next to this repo's project), and
prints the lines (or only those with one of the --grep words) to the terminal. Waits for the
phone and reconnects when it comes back.
"""
import ctypes, ctypes.wintypes as wt, os, sys, threading, time

GUID_STR = '{8F6B4C2E-5A1D-4E7B-9C3A-2D6E1F0A7B55}'
# WinUSB allows one open handle: a running reader sends the commands queued in this file
CMD_QUEUE = os.path.join(os.environ.get('TEMP', '.'), 'lavender-usbcmd.txt')


class GUID(ctypes.Structure):
    _fields_ = [('Data1', wt.DWORD), ('Data2', wt.WORD), ('Data3', wt.WORD), ('Data4', ctypes.c_ubyte * 8)]


def guid_from_str(s):
    s = s.strip('{}')
    p = s.split('-')
    g = GUID()
    g.Data1 = int(p[0], 16)
    g.Data2 = int(p[1], 16)
    g.Data3 = int(p[2], 16)
    rest = bytes.fromhex(p[3] + p[4])
    for i in range(8):
        g.Data4[i] = rest[i]
    return g


class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
    _fields_ = [('cbSize', wt.DWORD), ('InterfaceClassGuid', GUID), ('Flags', wt.DWORD),
                ('Reserved', ctypes.c_size_t)]


setupapi = ctypes.WinDLL('setupapi', use_last_error=True)
kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
winusb = ctypes.WinDLL('winusb', use_last_error=True)

DIGCF_PRESENT, DIGCF_DEVICEINTERFACE = 0x2, 0x10
INVALID_HANDLE = ctypes.c_void_p(-1).value
setupapi.SetupDiGetClassDevsW.restype = ctypes.c_void_p
setupapi.SetupDiGetClassDevsW.argtypes = [ctypes.POINTER(GUID), wt.LPCWSTR, wt.HWND, wt.DWORD]
setupapi.SetupDiEnumDeviceInterfaces.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(GUID), wt.DWORD,
                                                 ctypes.POINTER(SP_DEVICE_INTERFACE_DATA)]
setupapi.SetupDiGetDeviceInterfaceDetailW.argtypes = [ctypes.c_void_p, ctypes.POINTER(SP_DEVICE_INTERFACE_DATA),
                                                      ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD), ctypes.c_void_p]
setupapi.SetupDiDestroyDeviceInfoList.argtypes = [ctypes.c_void_p]
kernel32.CreateFileW.restype = ctypes.c_void_p
kernel32.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, ctypes.c_void_p, wt.DWORD, wt.DWORD, ctypes.c_void_p]
kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
winusb.WinUsb_Initialize.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
winusb.WinUsb_Free.argtypes = [ctypes.c_void_p]
winusb.WinUsb_ReadPipe.argtypes = [ctypes.c_void_p, ctypes.c_ubyte, ctypes.c_void_p, wt.ULONG,
                                   ctypes.POINTER(wt.ULONG), ctypes.c_void_p]
winusb.WinUsb_WritePipe.argtypes = [ctypes.c_void_p, ctypes.c_ubyte, ctypes.c_void_p, wt.ULONG,
                                    ctypes.POINTER(wt.ULONG), ctypes.c_void_p]
winusb.WinUsb_SetPipePolicy.argtypes = [ctypes.c_void_p, ctypes.c_ubyte, wt.ULONG, wt.ULONG, ctypes.c_void_p]


def device_path(guid):
    h = setupapi.SetupDiGetClassDevsW(ctypes.byref(guid), None, None, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)
    if h in (None, INVALID_HANDLE):
        return None
    try:
        ifd = SP_DEVICE_INTERFACE_DATA()
        ifd.cbSize = ctypes.sizeof(SP_DEVICE_INTERFACE_DATA)
        if not setupapi.SetupDiEnumDeviceInterfaces(h, None, ctypes.byref(guid), 0, ctypes.byref(ifd)):
            return None
        need = wt.DWORD(0)
        setupapi.SetupDiGetDeviceInterfaceDetailW(h, ctypes.byref(ifd), None, 0, ctypes.byref(need), None)
        buf = ctypes.create_string_buffer(need.value)
        ctypes.cast(buf, ctypes.POINTER(wt.DWORD))[0] = 8 if ctypes.sizeof(ctypes.c_void_p) == 8 else 6
        if not setupapi.SetupDiGetDeviceInterfaceDetailW(h, ctypes.byref(ifd), buf, need, None, None):
            return None
        return ctypes.wstring_at(ctypes.addressof(buf) + 4)
    finally:
        setupapi.SetupDiDestroyDeviceInfoList(h)


def open_device(guid):
    path = device_path(guid)
    if not path:
        return None, None
    hf = kernel32.CreateFileW(path, 0xC0000000, 3, None, 3, 0x40000080, None)
    if hf in (None, INVALID_HANDLE):
        return None, None
    hu = ctypes.c_void_p()
    if not winusb.WinUsb_Initialize(hf, ctypes.byref(hu)):
        kernel32.CloseHandle(hf)
        return None, None
    return hf, hu


def send_command(word):
    guid = guid_from_str(GUID_STR)
    hf, hu = open_device(guid)
    if hu is None:
        if device_path(guid):
            with open(CMD_QUEUE, 'a', encoding='utf-8') as q:   # a reader holds the device: it sends it
                q.write(word + '\n')
            print('usblog: queued for the running reader:', word)
            return 0
        print('usblog: phone not connected over USB')
        return 1
    msg = ('lv:' + word).encode()
    n = wt.ULONG(0)
    ok = winusb.WinUsb_WritePipe(hu, 0x01, msg, len(msg), ctypes.byref(n), None)
    print('usblog: sent', msg.decode(), 'ok' if ok else 'FAILED (error %d)' % ctypes.get_last_error())
    winusb.WinUsb_Free(hu)
    kernel32.CloseHandle(hf)
    return 0 if ok else 1


def main():
    args = sys.argv[1:]
    if len(args) == 2 and args[0] == '--cmd':
        sys.exit(send_command(args[1]))
    grep, quiet, out = None, False, None
    i = 0
    while i < len(args):
        if args[i] == '--grep':
            grep = [w for w in args[i + 1].split(',') if w]
            i += 2
            continue
        if args[i] == '--quiet':
            quiet = True
        else:
            out = args[i]
        i += 1
    if out is None:
        here = os.path.dirname(os.path.abspath(__file__))
        dump = os.path.normpath(os.path.join(here, '..', '..', '..', 'dump'))
        os.makedirs(dump, exist_ok=True)
        out = os.path.join(dump, time.strftime('usblog-%Y%m%d-%H%M%S.txt'))
    print('usblog: writing to', out, flush=True)
    guid = guid_from_str(GUID_STR)
    f = open(out, 'ab', buffering=0)
    told = False
    pending = b''
    while True:
        path = device_path(guid)
        if not path:
            if not told:
                print('usblog: waiting for the phone (WinUSB device', GUID_STR + ')...', flush=True)
                told = True
            time.sleep(1)
            continue
        told = False
        hf = kernel32.CreateFileW(path, 0xC0000000, 3, None, 3, 0x40000080, None)
        if hf in (None, INVALID_HANDLE):
            print('usblog: cannot open the device (error %d)' % ctypes.get_last_error(), flush=True)
            time.sleep(2)
            continue
        hu = ctypes.c_void_p()
        if not winusb.WinUsb_Initialize(hf, ctypes.byref(hu)):
            print('usblog: WinUsb_Initialize failed (error %d)' % ctypes.get_last_error(), flush=True)
            kernel32.CloseHandle(hf)
            time.sleep(2)
            continue
        # No read timeout: WinUSB cancels a timed-out read and resets the pipe, which used to stall the
        # phone's endpoint. Reads block; commands go from a thread of their own (one handle, two pipes).
        print('usblog: connected', flush=True)
        buf = ctypes.create_string_buffer(65536)
        n = wt.ULONG(0)
        alive = [True]

        def sender(hu=hu):
            while alive[0]:
                cmds = []
                if os.path.exists(CMD_QUEUE):
                    try:
                        with open(CMD_QUEUE, encoding='utf-8') as q:
                            cmds = [c.strip() for c in q if c.strip()]
                        os.remove(CMD_QUEUE)
                    except OSError:
                        cmds = []
                for c in cmds:
                    msg = ('lv:' + c).encode()
                    w = wt.ULONG(0)
                    okw = winusb.WinUsb_WritePipe(hu, 0x01, msg, len(msg), ctypes.byref(w), None)
                    f.write(('\n[usblog] sent %s %s\n' % (msg.decode(), 'ok' if okw else 'FAILED error %d' % ctypes.get_last_error())).encode())
                time.sleep(0.1)

        threading.Thread(target=sender, daemon=True).start()
        while True:
            ok = winusb.WinUsb_ReadPipe(hu, 0x81, buf, len(buf), ctypes.byref(n), None)
            if not ok:
                err = ctypes.get_last_error()
                print('usblog: read error %d, reconnecting' % err, flush=True)
                break
            data = buf.raw[:n.value].replace(b'\x00', b'')
            f.write(data)
            if quiet:
                continue
            pending += data
            *lines, pending = pending.split(b'\n')
            for ln in lines:
                s = ln.decode('utf-8', 'replace')
                if grep is None or any(w in s for w in grep):
                    print(s, flush=True)
        alive[0] = False
        time.sleep(0.3)
        winusb.WinUsb_Free(hu)
        kernel32.CloseHandle(hf)
        time.sleep(1)


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        pass
