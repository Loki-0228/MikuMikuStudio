"""Run an existing Stage test on a private desktop with exact import notices.

MAINTENANCE: integration-manual. This handles only WarnModelImport's Chinese
informational notice for an explicitly pinned test EXE and copied Stage input.
No desktop switching, input injection, product hooks, or generic dialog dismissal.
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
from datetime import datetime
import hashlib
import itertools
import json
import msvcrt
import os
from pathlib import Path
import shutil
import subprocess
import time


HEADING = '模型已导入。以下内容无法完整保存到部分原版 MMD 格式：\n'
ITEMS = (
    '－名称的字符或长度（VMD 的骨骼／表情名最多为 15 个 CP932 字节）\n',
    '－模型或贴图引用路径\n',
    '－材质、物理或显示项目\n',
)
ENDING = '\n已保留读取到的数据。保存时会按所选格式检查，无法完整保存时会提示。'
# dataFit is true when namesFit is false; reject impossible combinations too.
NOTICES = {
    HEADING + ''.join(item for item, selected in zip(ITEMS, flags) if selected) + ENDING
    for flags in itertools.product((False, True), repeat=3)
    if any(flags) and not (flags[0] and flags[2])
}


def recognizes(dialog, pid):
    buttons = dialog['buttons']
    return (
        dialog['pid'] == pid and dialog['owner_pid'] == pid
        and dialog['tid'] == dialog['owner_tid']
        and dialog['class'] == '#32770' and dialog['title'] == 'MMD'
        and not dialog['child_window']
        and dialog['body'].replace('\r\n', '\n') in NOTICES
        and len(buttons) == 1 and buttons[0]['id'] == 1
        and buttons[0]['text'] in ('确定', 'OK')
        and buttons[0]['pid'] == pid and buttons[0]['enabled']
    )


def selftest():
    notice = HEADING + ITEMS[0] + ENDING
    base = dict(pid=17, owner_pid=17, tid=29, owner_tid=29, **{'class': '#32770'},
                title='MMD', child_window=False, body=notice,
                buttons=[dict(id=1, text='确定', pid=17, enabled=True)])
    assert all(recognizes(dict(base, body=body), 17) for body in NOTICES)
    assert recognizes(dict(base, body=notice.replace('\n', '\r\n')), 17)
    for changed in (
        dict(pid=18), dict(owner_pid=18), dict(owner_tid=30), dict(title='Warning'),
        dict(child_window=True), dict(body=notice + '\n是否覆盖？'),
        dict(body=notice[:-1]), dict(body='已取消导入：名称或引用路径超出 CP932 字符或字段长度限制。'),
        dict(buttons=[dict(id=2, text='取消', pid=17, enabled=True)]),
        dict(buttons=base['buttons'] * 2),
        dict(buttons=[dict(id=1, text='保存', pid=17, enabled=True)]),
        dict(buttons=[dict(id=1, text='确定', pid=18, enabled=True)]),
    ):
        assert not recognizes(dict(base, **changed), 17), changed
    print('PASS exact notice matching, unknown/refusal/dialog-owner/button rejection')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def now():
    return datetime.now().astimezone().isoformat()


class PrivateProcess:
    """CreateProcessW is required: subprocess.STARTUPINFO ignores lpDesktop."""
    def __init__(self, command, directory, desktop_name, stdout, stderr):
        kernel = c.WinDLL('kernel32', use_last_error=True)
        def api(name, result, *params):
            fn = getattr(kernel, name); fn.restype = result; fn.argtypes = params; return fn
        class Startup(c.Structure):
            _fields_ = [('cb',w.DWORD),('reserved',w.LPWSTR),('desktop',w.LPWSTR),('title',w.LPWSTR),
                        ('x',w.DWORD),('y',w.DWORD),('width',w.DWORD),('height',w.DWORD),
                        ('chars_x',w.DWORD),('chars_y',w.DWORD),('fill',w.DWORD),('flags',w.DWORD),
                        ('show',w.WORD),('reserved_size',w.WORD),('reserved_data',c.c_void_p),
                        ('stdin',w.HANDLE),('stdout',w.HANDLE),('stderr',w.HANDLE)]
        class Information(c.Structure):
            _fields_ = [('process',w.HANDLE),('thread',w.HANDLE),('pid',w.DWORD),('tid',w.DWORD)]
        create = api('CreateProcessW',w.BOOL,w.LPCWSTR,w.LPWSTR,c.c_void_p,c.c_void_p,w.BOOL,
                     w.DWORD,c.c_void_p,w.LPCWSTR,c.POINTER(Startup),c.POINTER(Information))
        self._close=api('CloseHandle',w.BOOL,w.HANDLE)
        self._wait=api('WaitForSingleObject',w.DWORD,w.HANDLE,w.DWORD)
        self._exit=api('GetExitCodeProcess',w.BOOL,w.HANDLE,c.POINTER(w.DWORD))
        self._terminate=api('TerminateProcess',w.BOOL,w.HANDLE,w.UINT)
        start=Startup();start.cb=c.sizeof(start);start.desktop=desktop_name;start.flags=0x101
        info=Information(); self.command=command; self.handle=None; self.returncode=None
        with open(os.devnull,'rb') as stdin:
            handles=[msvcrt.get_osfhandle(f.fileno()) for f in (stdin,stdout,stderr)]
            old=[os.get_handle_inheritable(h) for h in handles]
            try:
                for h in handles: os.set_handle_inheritable(h,True)
                start.stdin,start.stdout,start.stderr=handles
                line=c.create_unicode_buffer(subprocess.list2cmdline(command))
                if not create(command[0],line,None,None,True,subprocess.CREATE_NO_WINDOW,
                              None,str(directory),c.byref(start),c.byref(info)):
                    raise c.WinError(c.get_last_error())
            finally:
                for h,state in zip(handles,old): os.set_handle_inheritable(h,state)
        self.handle=info.process;self.pid=info.pid;self.tid=info.tid;self._close(info.thread)
        self.actual_desktop=None
        user=c.WinDLL('user32',use_last_error=True)
        open_desktop=user.OpenDesktopW;open_desktop.restype=w.HANDLE
        open_desktop.argtypes=[w.LPCWSTR,w.DWORD,w.BOOL,w.DWORD]
        close_desktop=user.CloseDesktop;close_desktop.restype=w.BOOL;close_desktop.argtypes=[w.HANDLE]
        callback=c.WINFUNCTYPE(w.BOOL,w.HWND,w.LPARAM)
        enumerate_desktop=user.EnumDesktopWindows;enumerate_desktop.restype=w.BOOL
        enumerate_desktop.argtypes=[w.HANDLE,callback,w.LPARAM]
        window_pid=user.GetWindowThreadProcessId;window_pid.restype=w.DWORD
        window_pid.argtypes=[w.HWND,c.POINTER(w.DWORD)]
        query=user.GetUserObjectInformationW;query.restype=w.BOOL
        query.argtypes=[w.HANDLE,c.c_int,c.c_void_p,w.DWORD,c.POINTER(w.DWORD)]
        name=c.create_unicode_buffer(256);needed=w.DWORD()
        observed_desktop=open_desktop(desktop_name,0,False,0x41)
        ready=time.monotonic()+5
        @callback
        def observe_window(hwnd,_):
            pid=w.DWORD();tid=window_pid(hwnd,c.byref(pid))
            if pid.value==self.pid and tid==self.tid: self.actual_desktop=name.value
            return True
        try:
            if observed_desktop and query(observed_desktop,2,name,c.sizeof(name),c.byref(needed)):
                while time.monotonic()<ready and self.poll() is None:
                    enumerate_desktop(observed_desktop,observe_window,0)
                    if self.actual_desktop: break
                    time.sleep(.01)
        finally:
            if observed_desktop: close_desktop(observed_desktop)
        if self.actual_desktop != desktop_name:
            failure={'desktop':self.actual_desktop,'pid':self.pid,'exit':self.poll(),'win32_error':c.get_last_error()}
            self.kill();self.wait(5);self.close()
            raise RuntimeError('Child desktop was not verified: '+repr(failure))

    def poll(self):
        if self.returncode is None and self._wait(self.handle,0)==0:
            code=w.DWORD()
            if not self._exit(self.handle,c.byref(code)): raise c.WinError(c.get_last_error())
            self.returncode=code.value
        return self.returncode

    def wait(self, timeout):
        state=self._wait(self.handle,int(timeout*1000))
        if state==258: raise subprocess.TimeoutExpired(self.command,timeout)
        if state!=0: raise c.WinError(c.get_last_error())
        return self.poll()

    def kill(self):
        if self.poll() is None and not self._terminate(self.handle,1): raise c.WinError(c.get_last_error())

    def close(self):
        if self.handle: self._close(self.handle);self.handle=None


def run(args):
    exe, scene, fixture, output = (x.resolve() for x in (args.exe, args.scene, args.fixture_root, args.output))
    if exe.name != 'stage_inheritance_load_tests.exe' or digest(exe) != args.expected_exe_sha256:
        raise ValueError('Only the explicitly pinned existing Stage test EXE is permitted')
    if not scene.is_relative_to(fixture) or digest(scene) != args.expected_scene_sha256:
        raise ValueError('Expected copied fixture identity did not match')
    if not 1 <= args.timeout <= 120:
        raise ValueError('Timeout must be 1..120 seconds')
    output.mkdir(parents=True, exist_ok=False)
    install = output / 'installation'; install.mkdir()
    child_exe = install / exe.name; shutil.copy2(exe, child_exe)
    settings = exe.parent / 'data/settings.json'
    config = json.loads(settings.read_text(encoding='utf-8'))
    if config['sections']['window']['uiLanguage'] != 2 or config['sections']['autosave']['enabled']:
        raise ValueError('Expected existing Chinese test configuration with autosave disabled')
    (install / 'data').mkdir(); shutil.copy2(settings, install / 'data/settings.json')
    original_files = {str(p): digest(p) for p in fixture.rglob('*') if p.is_file()}

    u = c.WinDLL('user32', use_last_error=True)
    def api(name, result, *params):
        fn = getattr(u, name); fn.restype = result; fn.argtypes = params; return fn
    ptr = c.c_void_p; callback = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    create = api('CreateDesktopW', w.HANDLE, w.LPCWSTR, ptr, ptr, w.DWORD, w.DWORD, ptr)
    close = api('CloseDesktop', w.BOOL, w.HANDLE)
    set_thread_desktop = api('SetThreadDesktop', w.BOOL, w.HANDLE)
    get_thread_desktop = api('GetThreadDesktop', w.HANDLE, w.DWORD)
    enum_windows = api('EnumDesktopWindows', w.BOOL, w.HANDLE, callback, w.LPARAM)
    enum_children = api('EnumChildWindows', w.BOOL, w.HWND, callback, w.LPARAM)
    get_class = api('GetClassNameW', c.c_int, w.HWND, w.LPWSTR, c.c_int)
    get_text = api('GetWindowTextW', c.c_int, w.HWND, w.LPWSTR, c.c_int)
    get_pid = api('GetWindowThreadProcessId', w.DWORD, w.HWND, c.POINTER(w.DWORD))
    get_owner = api('GetWindow', w.HWND, w.HWND, c.c_uint)
    style = api('GetWindowLongPtrW', c.c_ssize_t, w.HWND, c.c_int)
    control_id = api('GetDlgCtrlID', c.c_int, w.HWND)
    enabled = api('IsWindowEnabled', w.BOOL, w.HWND)
    post = api('PostMessageW', w.BOOL, w.HWND, c.c_uint, w.WPARAM, w.LPARAM)

    def identity(hwnd):
        pid = w.DWORD(); tid = get_pid(hwnd, c.byref(pid)); return pid.value, tid
    def text(hwnd):
        buf = c.create_unicode_buffer(8192); get_text(hwnd, buf, len(buf)); return buf.value
    def cls(hwnd):
        buf = c.create_unicode_buffer(128); get_class(hwnd, buf, len(buf)); return buf.value
    def inspect(hwnd):
        pid, tid = identity(hwnd); owner_pid, owner_tid = identity(get_owner(hwnd, 4))
        children = []
        @callback
        def collect(child, _):
            children.append(dict(hwnd=child, **{'class': cls(child)}, text=text(child),
                                 id=control_id(child), pid=identity(child)[0],
                                 enabled=bool(enabled(child)), style=style(child, -16)))
            return True
        enum_children(hwnd, collect, 0)
        body = '\n'.join(x['text'] for x in children if x['class'] == 'Static'
                         and (x['style'] & 0x1f) != 3 and x['text'])
        return dict(hwnd=hwnd, pid=pid, tid=tid, owner_pid=owner_pid, owner_tid=owner_tid,
                    **{'class': cls(hwnd)}, title=text(hwnd), body=body,
                    child_window=bool(style(hwnd, -16) & 0x40000000),
                    buttons=[x for x in children if x['class'] == 'Button'])

    desktop_name = 'MMSStageNotice_' + str(time.time_ns())
    desktop = create(desktop_name, None, None, 0, 0x1ff, None)
    if not desktop: raise c.WinError(c.get_last_error())
    kernel=c.WinDLL('kernel32'); kernel.GetCurrentThreadId.restype=w.DWORD
    observer_desktop=get_thread_desktop(kernel.GetCurrentThreadId())
    if not set_thread_desktop(desktop):
        close(desktop)
        raise c.WinError(c.get_last_error())
    result = dict(started_at=now(), exe=str(exe), exe_sha256=digest(exe), scene=str(scene),
                  scene_sha256=digest(scene), settings_sha256=digest(settings),
                  private_desktop=desktop_name, cycles=1, timeout_seconds=args.timeout, notices=[])
    proc = None
    def save():
        (output / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    try:
        with (output/'stdout.log').open('wb') as stdout, (output/'stderr.log').open('wb') as stderr:
            proc = PrivateProcess([str(child_exe), str(scene), '1'], install, desktop_name, stdout, stderr)
            result['actual_desktop'] = proc.actual_desktop
            result['pid'] = proc.pid; deadline = time.monotonic() + args.timeout
            save()
            pending = None
            while proc.poll() is None:
                if time.monotonic() >= deadline:
                    result['blocked'] = 'deadline'; break
                dialogs = []
                @callback
                def collect(hwnd, _):
                    if identity(hwnd)[0] == proc.pid and cls(hwnd) == '#32770' and not style(hwnd,-16) & 0x40000000:
                        dialogs.append(hwnd)
                    return True
                c.set_last_error(0)
                enumerated=enum_windows(desktop, collect, 0)
                if not enumerated and c.get_last_error(): raise c.WinError(c.get_last_error())
                if len(dialogs) > 1:
                    result['blocked'] = 'multiple dialogs'; result['unknown_dialogs'] = [inspect(h) for h in dialogs]; break
                if not dialogs:
                    pending = None
                else:
                    observed = inspect(dialogs[0])
                    if not recognizes(observed, proc.pid):
                        # Creation may be observed before text/buttons are initialized.
                        time.sleep(.05); observed = inspect(dialogs[0])
                        if not recognizes(observed, proc.pid):
                            result['blocked'] = 'unknown dialog'; result['unknown_dialog'] = observed; break
                    signature = (observed['hwnd'], observed['body'])
                    if pending != signature:
                        if len(result['notices']) >= 80:
                            result['blocked'] = 'notice count exceeded'; break
                        event = dict(at=now(), observed=observed, action='WM_COMMAND IDOK to this test dialog')
                        result['notices'].append(event); save()
                        # Recheck identity and the complete snapshot immediately before acknowledging.
                        current = inspect(observed['hwnd'])
                        if current != observed or not recognizes(current, proc.pid):
                            result['blocked'] = 'dialog changed before acknowledgement'; break
                        if not post(observed['hwnd'], 0x111, 1, observed['buttons'][0]['hwnd']):
                            raise c.WinError(c.get_last_error())
                        pending = signature
                time.sleep(.025)
            if proc.poll() is None:
                proc.kill(); proc.wait(timeout=5); result['terminated_by_runner'] = True
            result['exit'] = proc.returncode
    except Exception as error:
        result['blocked'] = 'driver or isolated startup error'
        result['error'] = repr(error)
    finally:
        if proc and proc.poll() is None: proc.kill(); proc.wait(timeout=5)
        if proc: proc.close()
        set_thread_desktop(observer_desktop)
        close(desktop)
        result['finished_at'] = now()
        result['exe_unchanged'] = digest(exe) == args.expected_exe_sha256 and digest(child_exe) == args.expected_exe_sha256
        result['fixture_changes'] = [p for p, expected in original_files.items() if not Path(p).is_file() or digest(Path(p)) != expected]
        save()
    log = (output/'stdout.log').read_text(errors='replace')
    result['passed'] = (result.get('exit') == 0 and not result.get('blocked')
                        and 'models=61 accessories=10 invalid_inheritance=1' in log
                        and 'cycle=0 PASS 12 frames' in log
                        and 'PASS Stage inheritance load, repeat evaluation and shutdown' in log
                        and result['exe_unchanged'] and not result['fixture_changes'])
    save()
    print(json.dumps({k:v for k,v in result.items() if k != 'notices'}, ensure_ascii=False, indent=2))
    print('Acknowledged exact informational notices:', len(result['notices']))
    print(log)
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--exe', type=Path)
    parser.add_argument('--scene', type=Path)
    parser.add_argument('--fixture-root', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--expected-exe-sha256')
    parser.add_argument('--expected-scene-sha256')
    parser.add_argument('--timeout', type=int, default=90)
    args = parser.parse_args()
    if args.self_test:
        selftest()
    else:
        if not all((args.exe,args.scene,args.fixture_root,args.output,args.expected_exe_sha256,args.expected_scene_sha256)):
            parser.error('Explicit EXE, copied fixture, new output directory, and both expected hashes are required')
        raise SystemExit(run(args))
