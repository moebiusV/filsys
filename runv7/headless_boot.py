#!/usr/bin/env python3
"""Headless boot of V7 over SET CONSOLE TELNET, full two-stage bootstrap.

simh reads headless.ini (ends in `boot rp0`), waits for the telnet connection,
then serves the console over the socket.  The two-stage bootstrap needs:
    boot            (load second-stage /boot)
    hp(0,0)unix     (at the ':' prompt)
The kernel then prints `mem = ...` and lands on the '#' root shell.
"""
import subprocess, socket, time, sys, os, threading, select

SIMH = '/usr/bin/pdp11'
WD = os.path.dirname(os.path.abspath(__file__))
PORT = 10023

IAC = 0xFF
WILL, WONT, DO, DONT = 0xFB, 0xFC, 0xFD, 0xFE
SB, SE = 0xFA, 0xF0


def negotiate(sock):
    try:
        data = sock.recv(65536)
    except socket.timeout:
        return b''
    except OSError:
        return b''
    out = b''; i = 0; n = len(data); rep = b''
    while i < n:
        b = data[i]
        if b == IAC:
            if i + 1 >= n:
                break
            c = data[i + 1]
            if c in (WILL, WONT, DO, DONT):
                if i + 2 >= n:
                    break
                o = data[i + 2]
                if c == WILL: rep += bytes([IAC, DONT, o])
                elif c == DO: rep += bytes([IAC, WONT, o])
                elif c == WONT: rep += bytes([IAC, DONT, o])
                elif c == DONT: rep += bytes([IAC, WONT, o])
                i += 3
            elif c == IAC:
                out += bytes([IAC]); i += 2
            elif c == SB:
                j = data.find(bytes([IAC, SE]), i)
                i = n if j < 0 else j + 2
            else:
                i += 2
        else:
            out += bytes([b]); i += 1
    if rep:
        try:
            sock.sendall(rep)
        except OSError:
            pass
    return out


def main():
    proc = subprocess.Popen(
        [SIMH, 'headless.ini'],
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, cwd=WD,
    )
    def drain():
        while True:
            r, _, _ = select.select([proc.stdout], [], [], 0.1)
            if not r:
                continue
            d = os.read(proc.stdout.fileno(), 65536)
            if not d:
                return
            sys.stdout.write('[simh] ' + d.decode(errors='replace'))
            sys.stdout.flush()
    threading.Thread(target=drain, daemon=True).start()

    sock = None
    for _ in range(80):
        try:
            sock = socket.create_connection(('127.0.0.1', PORT), timeout=2)
            break
        except OSError:
            time.sleep(0.25)
    if sock is None:
        print('[driver] connect failed'); proc.kill(); return 1
    sock.settimeout(0.2)
    print('[driver] connected to telnet console')

    console = bytearray()

    def poll(dur):
        end = time.time() + dur
        while time.time() < end:
            console.extend(negotiate(sock))
            time.sleep(0.05)

    def send(s):
        try:
            sock.sendall(s.encode())
        except OSError:
            pass

    def has(sub):
        return sub.encode() in bytes(console)

    def wait_for(sub, timeout):
        end = time.time() + timeout
        while time.time() < end and not has(sub):
            poll(0.5)
        return has(sub)

    # settle negotiation
    poll(2.0)

    # two-stage bootstrap
    send('boot\r')
    print('[driver] sent "boot" (2nd-stage)')
    wait_for(':', 8.0)
    print('[driver] ":" prompt seen: %s' % has(':'))
    send('hp(0,0)unix\r')
    print('[driver] sent kernel name')

    # kernel boot -> '#' shell
    wait_for('mem =', 60.0)
    print('[driver] kernel banner ("mem =") seen: %s' % has('mem ='))
    wait_for('# ', 30.0)
    print('[driver] "# " shell seen: %s' % has('# '))

    # run a command and capture its output
    send('echo hello-from-v7\r')
    wait_for('hello-from-v7', 6.0)
    send('ls -l /tmp\r')
    poll(4.0)

    time.sleep(1)
    sock.close()
    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()

    txt = bytes(console).decode(errors='replace')
    print('\n=== console ===')
    print(txt)
    # V7 console is KSR uppercase mode, so compare case-insensitively.
    up = txt.upper()
    markers = ['Boot', ':', 'mem =', '# ', 'HELLO-FROM-V7', 'TOTAL']
    print('=== markers ===')
    for m in markers:
        print('  %-14s: %s' % (m, m.upper() in up))
    ok = ('MEM =' in up) and ('# ' in up) and ('HELLO-FROM-V7' in up)
    print('=== [headless_boot] fully booted and ran a command: %s ===' % ok)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
