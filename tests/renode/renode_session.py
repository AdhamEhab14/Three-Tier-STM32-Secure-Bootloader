"""Drive Renode in lock step with the test, so runs are deterministic.

Renode is started with a monitor port. Virtual time only advances when the test
calls run_for(), and UART input is injected while the machine is paused. That
removes every wall-clock race between the host tool and the emulated firmware
(the firmware's receive timeouts are measured in virtual time).

RenodeSerial wraps a session with the read/write/close interface that
host/bl_host.py expects, so the real host tool talks to the real firmware.
"""
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PROMPT_RE = re.compile(rb"\([^)\r\n]*\) $")


def find_renode():
    exe = os.environ.get("RENODE_PATH") or shutil.which("renode")
    if not exe and sys.platform == "win32" and os.path.exists("D:/Tools/renode/renode.exe"):
        exe = "D:/Tools/renode/renode.exe"
    return exe


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def resolved_platform():
    """Renode resolves script paths oddly, so write a copy with absolute ones."""
    src = os.path.join(ROOT, "tests", "renode", "platform", "f103_bl.repl")
    models = ROOT.replace("\\", "/") + "/tests/renode/models/"
    text = open(src).read().replace("tests/renode/models/", models)
    fd, path = tempfile.mkstemp(suffix=".repl")
    with os.fdopen(fd, "w") as f:
        f.write(text)
    return path.replace("\\", "/")


def connect(port, wait):
    end = time.time() + wait
    while True:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=5)
        except OSError:
            if time.time() > end:
                raise
            time.sleep(0.2)


class RenodeSession:
    def __init__(self, exe, build_dir="build", mips=72):
        self.exe = exe
        self.mon_port = free_port()
        self.uart_port = free_port()
        self.virtual_time = 0.0
        self.rx = bytearray()
        self.repl = resolved_platform()
        self.proc = subprocess.Popen(
            [exe, "--disable-xwt", "--plain", "--port", str(self.mon_port)],
            cwd=ROOT, stdin=subprocess.PIPE, stdout=self._log(), stderr=subprocess.STDOUT)
        self.mon = connect(self.mon_port, 90)
        self._buf = b""
        self._drain_banner()
        self.cmd("logLevel 3")
        self.cmd('emulation CreateServerSocketTerminal %d "term" false' % self.uart_port)
        self.cmd('mach create "f103"')
        self.cmd("machine LoadPlatformDescription @%s" % self.repl)
        self.cmd("sysbus LoadELF @%s/boot_manager.elf" % build_dir)
        self.cmd("sysbus LoadELF @%s/fbl.elf" % build_dir)
        self.cmd("cpu VectorTableOffset 0x08000000")
        self.cmd("cpu PerformanceInMips %d" % mips)
        self.cmd("connector Connect sysbus.usart2 term")
        self.uart = connect(self.uart_port, 10)

    @staticmethod
    def _log():
        path = os.environ.get("RENODE_LOG")
        return open(path, "wb") if path else subprocess.DEVNULL

    def _drain_banner(self):
        self.mon.settimeout(2)
        try:
            while True:
                if not self.mon.recv(4096):
                    break
        except OSError:
            pass

    def cmd(self, text, timeout=300):
        """Run one monitor command; raise if Renode reports an error."""
        self.mon.sendall(text.encode() + b"\n")
        self.mon.settimeout(timeout)
        buf = self._buf
        while True:
            i = buf.find(text.encode())
            if i >= 0 and PROMPT_RE.search(buf, i + len(text)):
                break
            chunk = self.mon.recv(4096)
            if not chunk:
                raise RuntimeError("Renode monitor closed")
            buf += chunk
        self._buf = b""
        out = buf.decode(errors="replace")
        low = out.lower()
        if "error" in low or "no such command" in low:
            raise RuntimeError("Renode rejected %r:\n%s" % (text, out))
        return out

    def run_for(self, seconds):
        self.cmd('emulation RunFor "%.7f"' % seconds)
        self.virtual_time += seconds
        self._pull()

    def _pull(self, wait=0.03):
        self.uart.settimeout(wait if wait else 0.001)
        try:
            while True:
                chunk = self.uart.recv(4096)
                if not chunk:
                    break
                self.rx += chunk
        except OSError:
            pass

    def cmd_batch(self, lines, timeout=300):
        """Send many monitor commands in one write; wait for all their prompts."""
        self.mon.sendall(("\n".join(lines) + "\n").encode())
        self.mon.settimeout(timeout)
        buf = self._buf
        want = len(lines)
        while len(re.findall(rb"\([^)\r\n]*\) ", buf)) < want:
            chunk = self.mon.recv(65536)
            if not chunk:
                raise RuntimeError("Renode monitor closed")
            buf += chunk
        self._buf = b""
        out = buf.decode(errors="replace")
        if "error" in out.lower():
            raise RuntimeError("Renode rejected a batch:\n%s" % out[-400:])

    def idle(self, seconds, mips=4):
        """Let virtual time pass while the firmware sits in its polling loop. Slowing the
        emulated CPU makes that cheap in wall-clock time; timers (SysTick) follow virtual
        time, so the firmware's own clock still advances by `seconds`."""
        self.cmd("cpu PerformanceInMips %d" % mips)
        try:
            self.run_for(seconds)
        finally:
            self.cmd("cpu PerformanceInMips 72")

    def uart_write(self, data, baud=115200):
        """Inject bytes one at a time at wire speed. The F103 USART has a
        single-byte receive buffer, so bursts would overrun it, as on hardware."""
        per_byte = "%.7f" % max(10.0 / baud, 0.0001)   # never finer than one 100 us quantum
        lines = []
        for b in data:
            lines.append("usart2 WriteChar %d" % b)
            lines.append('emulation RunFor "%s"' % per_byte)
        self.cmd_batch(lines)
        self.virtual_time += float(per_byte) * len(data)
        self._pull(0.0)

    def load_binary(self, rel_path, addr):
        """Put a file straight into memory (bypasses the UART). Path is relative to the repo root."""
        self.cmd("sysbus LoadBinary @%s 0x%X" % (rel_path, addr))

    def reboot(self, hold_b1=True, settle=1.5):
        """Reset the CPU and peripherals; flash contents are kept. B1 held = stay in the bootloader."""
        self.cmd("machine Reset")
        self.cmd("cpu VectorTableOffset 0x08000000")
        self.cmd("gpioPortC OnGPIO 13 %s" % ("false" if hold_b1 else "true"))
        self.rx.clear()
        self.run_for(settle)

    FLASH_CTL = 0x40022000

    def flash_ops(self):
        """Number of flash operations the model has seen since reset of the counter."""
        return self.read_word(self.FLASH_CTL + 0x100)

    def set_cut(self, opcount):
        """Pause the machine when the flash model is about to start operation number `opcount`."""
        self.cmd("sysbus WriteDoubleWord 0x%X 0x%X" % (self.FLASH_CTL + 0x104, opcount))

    def set_cut_on_erase(self, page_addr):
        """Pause when the firmware is about to erase the flash page at page_addr."""
        self.cmd("sysbus WriteDoubleWord 0x%X 0x%X" % (self.FLASH_CTL + 0x114, page_addr))

    def cut_fired(self):
        return self.read_word(self.FLASH_CTL + 0x108) == 1

    def cut_info(self):
        """(kind, page address): kind 1 = erase cut before it ran, 2 = program cut, 0 = none."""
        return self.read_word(self.FLASH_CTL + 0x10C), self.read_word(self.FLASH_CTL + 0x110)

    FLASH_BASE = 0x08000000
    FLASH_SIZE = 0x20000

    def dump_flash(self):
        """The whole 128 KB flash as bytes (slow: about 10 s)."""
        out = self.cmd("sysbus ReadBytes 0x%X 0x%X" % (self.FLASH_BASE, self.FLASH_SIZE), timeout=600)
        out = out[out.find("["):]
        data = bytes(int(t, 16) for t in re.findall(r"0x[0-9A-Fa-f]{2}", out))
        if len(data) != self.FLASH_SIZE:
            raise RuntimeError("flash dump returned %d bytes" % len(data))
        return data

    def restore_flash(self, data, scratch_rel):
        """Replace the whole flash with `data` (scratch_rel: a repo-relative temp file path)."""
        open(os.path.join(ROOT, scratch_rel), "wb").write(data)
        self.load_binary(scratch_rel, self.FLASH_BASE)

    def power_cycle(self, hold_b1=True, settle=1.5):
        """A real power loss: RAM is lost, flash is kept, everything restarts."""
        self.cmd("sram ZeroAll")
        self.cmd("cpu IsHalted false")
        self.cmd("sysbus WriteDoubleWord 0x%X 0xFFFFFFFF" % (self.FLASH_CTL + 0x104))
        self.cmd("sysbus WriteDoubleWord 0x%X 0xFFFFFFFF" % (self.FLASH_CTL + 0x114))
        self.cmd("sysbus WriteDoubleWord 0x%X 0" % (self.FLASH_CTL + 0x100))
        self.reboot(hold_b1=hold_b1, settle=settle)

    def pc(self):
        for tok in self.cmd("cpu PC").split():
            if tok.startswith("0x"):
                return int(tok, 16)
        return None

    def read_word(self, addr):
        text = "sysbus ReadDoubleWord 0x%X" % addr
        out = self.cmd(text)
        for tok in out[out.find(text) + len(text):].split():
            if tok.startswith("0x"):
                return int(tok, 16)
        return None

    def close(self):
        try:
            self.cmd("quit", timeout=5)
        except Exception:
            pass
        for s in (getattr(self, "mon", None), getattr(self, "uart", None)):
            try:
                s.close()
            except Exception:
                pass
        try:
            os.unlink(self.repl)
        except OSError:
            pass
        try:
            self.proc.stdin.close()
            self.proc.wait(timeout=10)
        except Exception:
            self.proc.kill()


class RenodeSerial:
    """pyserial-like view of the emulated USART2 for bl_host.py."""

    def __init__(self, session, timeout=20, step=0.01):
        self.s = session
        self.timeout = timeout
        self.step = step

    def write(self, data):
        self.s.uart_write(bytes(data))

    def read(self, n):
        waited = 0.0
        while len(self.s.rx) < n and waited < self.timeout:
            self.s.run_for(self.step)
            waited += self.step
        out = bytes(self.s.rx[:n])
        del self.s.rx[:n]
        return out

    def sleep(self, seconds):
        """bl_host waits through this, so a wait advances emulated time rather than real time."""
        self.s.idle(seconds)

    def close(self):
        pass
