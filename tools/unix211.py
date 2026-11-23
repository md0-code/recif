"""Builds the 2.11BSD pack shipped with Recif from the original distribution tape.

    python tools/unix211.py [--tape 211bsd.tap] [--out work/disks/unix211-rp06.dsk]

Downloads the 2.11BSD tape (pdp-11.org.ru) unless --tape is given, then
installs it on an RP06 pack on the CORAL 4021 reference through the console:
disk label, root file system, /usr, /usr/src and the disk boot block, as in
"Installing and Operating 2.11BSD".
"""

import argparse
import io
import os
import socket
import subprocess
import sys
import time
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TAPE_URL = 'https://pdp-11.org.ru/files/unix/211bsd.zip'
PORT = 2457


class Console:
    def __init__(self, port):
        for _ in range(300):
            try:
                self.sock = socket.create_connection(('127.0.0.1', port))
                break
            except OSError:
                time.sleep(0.2)
        else:
            raise SystemExit('Recif did not open its console')
        self.sock.settimeout(0.5)
        self.buffer = ''

    def expect(self, text, timeout=7200):
        end = time.time() + timeout
        while text not in self.buffer:
            if time.time() > end:
                raise SystemExit('Timed out waiting for %r' % text)
            try:
                data = self.sock.recv(4096)
            except socket.timeout:
                continue
            if not data:
                raise SystemExit('Console closed while waiting for %r' % text)
            # The kernel adds even parity to console output.
            chunk = ''.join(chr(b & 0x7f) for b in data if b & 0x7f)
            sys.stdout.write(chunk)
            sys.stdout.flush()
            self.buffer += chunk
        self.buffer = self.buffer[self.buffer.index(text) + len(text):]

    def send(self, line):
        time.sleep(0.3)
        for ch in line + '\r':
            self.sock.send(ch.encode('ascii'))
            time.sleep(0.03)

    def step(self, prompt, line, timeout=7200):
        self.expect(prompt, timeout)
        self.send(line)

    def settle(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            try:
                data = self.sock.recv(4096)
            except socket.timeout:
                continue
            chunk = ''.join(chr(b & 0x7f) for b in data if b & 0x7f)
            sys.stdout.write(chunk)
            self.buffer += chunk

    def field(self, menu, letter, prompt, value):
        """A disklabel field. The standalone reader sometimes takes the end
        of the command line as an empty answer; the value then goes on the
        command line itself."""
        self.step(menu, letter)
        self.expect(prompt)
        self.settle(1.5)
        if menu in self.buffer:
            self.step(menu, letter + value)
            self.expect(prompt)
        else:
            self.send(value)


def fetch_tape(path):
    if os.path.exists(path):
        return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    print('Downloading', TAPE_URL)
    data = urllib.request.urlopen(TAPE_URL).read()
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        with open(path, 'wb') as f:
            f.write(z.read('211bsd.tap'))


def label(c):
    """Root 72 cylinders, swap 40, whole disk, /usr on the rest."""
    menu = 'q(uit)? '
    c.step('Disk? ', 'xp(0,0)')
    c.step('[y/n] ', 'y')
    c.step(menu, 'D')
    c.step(menu, 'm')
    c.step(menu, 'p')
    c.step(menu, 'n')
    c.step('[1]? ', '8')
    for part, offset, size, fstype, frag in (
            ('a', '0', '30096', None, None), ('b', '30096', '16720', 'swap', None),
            ('c', '0', '340670', None, None), ('g', '46816', '293854', '2.11BSD', '1024')):
        c.step(menu, 's')
        c.step(menu, part)
        c.field(menu, 'o', 'offset [', offset)
        c.field(menu, 's', 'size [', size)
        if fstype:
            c.field(menu, 't', 'fstype [', fstype)
        if frag:
            c.field(menu, 'f', 'fs-block [', '1')
            c.field(menu, 'F', 'frag size [', frag)
        c.step(menu, 'q')
    c.step(menu, 'q')
    c.step(menu, 'q')
    c.step(menu, 'w')
    c.step(menu, 'q')


def install(recif, tape, pack):
    if os.path.exists(pack):
        raise SystemExit('%s exists' % pack)
    run = subprocess.Popen([recif, 'run', '--model', '4021', '--disks', 'rp06', '--disk', pack,
                            '--tape', tape, '--boot-tape', '--full-speed', '--serial', 'tcp-listen:%d' % PORT],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        c = Console(PORT)
        boot = ': '
        c.step(boot, 'tm(0,1)')                     # standalone disklabel
        label(c)
        c.step(boot, 'tm(0,2)')                     # standalone mkfs
        c.step('file system: ', 'xp(0,0)')
        for prompt in ('size [', 'inode [', 'default): ', 'default): '):
            c.step(prompt, '')
        c.step(boot, 'tm(0,3)')                     # standalone restor
        c.step('Tape? ', 'tm(0,5)')
        c.step('Disk? ', 'xp(0,0)')
        c.step('disk.', '')
        c.step(boot, 'xp(0,0)unix')
        shell = '# '
        for command in ('rm -f /dev/rmt0 /dev/nrmt0', 'mknod /dev/rmt0 c 7 0', 'mknod /dev/nrmt0 c 7 4',
                        'dd if=/mdec/hpuboot of=/dev/rxp0a count=1', 'newfs /dev/rxp0g',
                        'mount /dev/xp0g /usr', 'cd /usr', 'mt -f /dev/nrmt0 rew',
                        'mt -f /dev/nrmt0 fsf 6', 'tar xpbf 20 /dev/nrmt0', 'mkdir /usr/src',
                        'cd /usr/src', 'mt -f /dev/nrmt0 fsf 1', 'tar xpbf 20 /dev/nrmt0',
                        'mt -f /dev/nrmt0 fsf 1', 'tar xpbf 20 /dev/nrmt0', 'cd /',
                        # The distribution's fstab names an MSCP disk.
                        "echo '/dev/xp0a / ufs rw 1 1' > /etc/fstab",
                        "echo '/dev/xp0b none swap sw 0 0' >> /etc/fstab",
                        "echo '/dev/xp0g /usr ufs rw 1 2' >> /etc/fstab",
                        'umount /dev/xp0g', 'halt'):
            c.step(shell, command)
        c.expect('halting')
        time.sleep(3)
    finally:
        run.kill()
        run.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--tape', default=os.path.join(ROOT, 'work', 'media', '211bsd.tap'))
    parser.add_argument('--out', default=os.path.join(ROOT, 'work', 'disks', 'unix211-rp06.dsk'))
    parser.add_argument('--recif', default=os.path.join(ROOT, 'build', 'x64', 'recif.exe'))
    args = parser.parse_args()
    fetch_tape(args.tape)
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    install(args.recif, args.tape, args.out)
    print('\nWrote', args.out)


if __name__ == '__main__':
    main()
