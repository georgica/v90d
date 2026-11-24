#!/usr/bin/env python3
"""End-to-end test of v90arch: the real program on high ports, a fake
Protoweb proxy in this script, real sockets, nothing leaves 127.0.0.0/8.

usage: e2e.py [runner ...] path/to/v90arch      (runner: e.g. qemu-arm-static)

The address pool is put inside 127.0.0.0/8 (-n 127.64.0.0/16). On Linux
every such address is local, so a connection made to the address the DNS
responder handed out arrives at v90arch with that address as its local
address, which is what the program falls back to when there is no kernel
redirect. That way the whole name -> address -> name path is exercised
without any nftables rule.
"""
import os, signal, socket, struct, subprocess, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
checks = fails = 0

def check(cond, what):
    global checks, fails
    checks += 1
    if not cond:
        fails += 1
        print("  FAIL: %s" % what)

def check_eq(got, want, what):
    check(got == want, "%s: got %r, want %r" % (what, got if len(repr(got)) < 300 else repr(got)[:300], want if len(repr(want)) < 300 else repr(want)[:300]))

def fixture(name):
    with open(os.path.join(HERE, name), "rb") as f:
        return f.read()

def noise(n, seed):
    out = bytearray(n)
    x = seed
    for i in range(n):
        x = (x * 1103515245 + 12345) & 0x7fffffff
        out[i] = (x >> 16) & 0xff
    return bytes(out)

ROOT = fixture("protoweb_ftp_listing.html")
PUB = fixture("protoweb_ftp_pub.html")
WELCOME = (b"Welcome to the iNode File Archive.\r\n" * 40)[:1131]
BIG_FTP = noise(300000, 1)
BIG_HTTP = noise(200000, 2)
HOME = b"<html><body>iNode home page, restored.</body></html>\n"

# ---------------------------------------------------------------- fake proxy

class FakeProxy(threading.Thread):
    """Answers absolute-URI requests the way the Protoweb proxy does: HTTP/1.0,
    Connection: close, its own pages with bare LF line ends."""
    def __init__(self):
        super().__init__(daemon=True)
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(32)
        self.port = self.sock.getsockname()[1]
        self.seen = []          # request heads, as text
        self.lock = threading.Lock()

    def run(self):
        while True:
            try:
                c, _ = self.sock.accept()
            except OSError:
                return
            threading.Thread(target=self.serve, args=(c,), daemon=True).start()

    def stop(self):
        # shutdown wakes the accept(), close alone would leave it listening
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.sock.close()

    def serve(self, c):
        try:
            c.settimeout(10)
            data = b""
            while b"\r\n\r\n" not in data:
                d = c.recv(65536)
                if not d:
                    return
                data += d
            head, body = data.split(b"\r\n\r\n", 1)
            lines = head.decode("latin-1").split("\r\n")
            clen = 0
            for l in lines[1:]:
                if l.lower().startswith("content-length:"):
                    clen = int(l.split(":", 1)[1])
            while len(body) < clen:
                d = c.recv(65536)
                if not d:
                    break
                body += d
            with self.lock:
                self.seen.append(head.decode("latin-1"))
            c.sendall(self.answer(lines[0], body))
        except OSError:
            pass
        finally:
            c.close()

    @staticmethod
    def crlf(status, body, extra=b""):
        return b"HTTP/1.0 " + status + b"\r\nContent-Type: text/html\r\n" + extra + b"Connection: close\r\n\r\n" + body

    @staticmethod
    def lf(status, body):
        return b"HTTP/1.0 " + status + b"\nDate: Sun Nov 15 09:16:08 AM CST 2026\nConnection: close\nContent-Type: text/html\n\n" + body

    def answer(self, reqline, body):
        parts = reqline.split(" ")
        if len(parts) != 3 or "://" not in parts[1]:
            return self.lf(b"400 Bad Request", b"<html>generic proxy page: not an absolute URI</html>\n")
        method, url = parts[0], parts[1]
        if "//silent.example/" in url:          # connected, but the answer never comes
            time.sleep(6)
            return b""
        if url == "http://www.inode.com/" and method == "GET":
            return self.crlf(b"200 OK", HOME)
        if url == "http://www.inode.com/big" and method == "GET":
            return self.crlf(b"200 OK", BIG_HTTP)
        if url == "http://www.inode.com/cgi-bin/form" and method == "POST":
            return self.crlf(b"200 OK", b"posted:" + body)
        if url == "http://www.inode.com:8080/port":
            return self.crlf(b"200 OK", b"port 8080 page")
        if url == "ftp://ftp.inode.com/":
            return self.lf(b"200 OK", ROOT)
        if url == "ftp://ftp.inode.com/pub/":
            return self.lf(b"200 OK", PUB)
        if url == "ftp://ftp.inode.com/pub":
            return b"HTTP/1.0 302 Found\nLocation: ftp://ftp.inode.com/pub/\n\n"
        files = {"ftp://ftp.inode.com/welcome.msg": WELCOME,
                 "ftp://ftp.inode.com/pub/big.bin": BIG_FTP,
                 "ftp://ftp.inode.com/pub/read%20me.txt": b"read me\r\n",
                 "ftp://ftp.inode.com/pub/empty": b""}
        if url in files:
            head = b"HTTP/1.0 200 OK\r\nContent-type: text/plain\r\nContent-Length: %d\r\n\r\n" % len(files[url])
            return head + (files[url] if method == "GET" else b"")
        if url.startswith("ftp://"):
            return self.lf(b"404 Not Found", b"<html><head>\n<title>404 Not Found</title>\n</head><body>\n<h1>Not Found</h1>\n</body></html>\n")
        return self.lf(b"503 Service Unavailable", b"<html>503: this site is not in the archive</html>\n")

# ------------------------------------------------------------------ helpers

def free_tcp_port():
    s = socket.socket()
    s.bind(("0.0.0.0", 0))
    p = s.getsockname()[1]
    s.close()
    return p

def free_range(n):
    for base in range(42000, 60000, 37):
        socks = []
        try:
            for p in range(base, base + n):
                s = socket.socket()
                s.bind(("0.0.0.0", p))
                socks.append(s)
            return base
        except OSError:
            pass
        finally:
            for s in socks:
                s.close()
    raise RuntimeError("no free port range")

def dns_query(port, name, qtype=1, qid=0x4242):
    q = struct.pack(">HHHHHH", qid, 0x0100, 1, 0, 0, 0)
    for label in name.split("."):
        q += bytes([len(label)]) + label.encode()
    q += b"\0" + struct.pack(">HH", qtype, 1)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(5)
    s.sendto(q, ("127.0.0.1", port))
    r, _ = s.recvfrom(600)
    s.close()
    rid, flags, qd, an, ns, ar = struct.unpack(">HHHHHH", r[:12])
    addr = None
    if an == 1:
        addr = socket.inet_ntoa(r[-4:])
    return rid, flags, an, addr, r

def http_fetch(addr, port, request, pieces=None, shut=False):
    s = socket.create_connection((addr, port), timeout=10)
    if pieces:
        for p in pieces:
            s.sendall(p)
            time.sleep(0.05)
    else:
        s.sendall(request)
    if shut:
        s.shutdown(socket.SHUT_WR)
    out = b""
    while True:
        d = s.recv(65536)
        if not d:
            break
        out += d
    s.close()
    return out

class Ftp:
    def __init__(self, addr, port):
        self.addr = addr
        self.s = socket.create_connection((addr, port), timeout=10)
        self.buf = b""

    def line(self):
        while b"\r\n" not in self.buf:
            d = self.s.recv(4096)
            if not d:
                raise EOFError("control connection closed")
            self.buf += d
        l, self.buf = self.buf.split(b"\r\n", 1)
        return l.decode("latin-1")

    def cmd(self, text):
        self.s.sendall(text.encode() + b"\r\n")
        return self.line()

    def pasv(self):
        r = self.cmd("PASV")
        check(r.startswith("227 "), "PASV reply: " + r)
        nums = [int(x) for x in r[r.index("(") + 1:r.index(")")].split(",")]
        return "%d.%d.%d.%d" % tuple(nums[:4]), nums[4] * 256 + nums[5]

    def port(self):
        ls = socket.socket()
        ls.bind((self.s.getsockname()[0], 0))
        ls.listen(1)
        ls.settimeout(10)
        ip, p = ls.getsockname()
        r = self.cmd("PORT %s,%d,%d" % (ip.replace(".", ","), p >> 8, p & 255))
        check(r.startswith("200 "), "PORT reply: " + r)
        return ls

    @staticmethod
    def drain(d):
        d.settimeout(10)
        out = b""
        while True:
            x = d.recv(65536)
            if not x:
                break
            out += x
        d.close()
        return out

    def get_pasv(self, command, connect_late=False):
        """PASV transfer; returns (first reply, data, final reply)"""
        ip, p = self.pasv()
        check_eq(ip, self.addr, "227 names the address the client connected to")
        if connect_late:
            self.s.sendall(command.encode() + b"\r\n")
            time.sleep(0.2)
            d = socket.create_connection((ip, p), timeout=10)
            first = self.line()
        else:
            d = socket.create_connection((ip, p), timeout=10)
            first = self.cmd(command)
        if not first.startswith("150"):
            d.close()
            return first, b"", ""
        data = self.drain(d)
        return first, data, self.line()

    def get_port(self, command):
        ls = self.port()
        first = self.cmd(command)
        if not first.startswith("150"):
            ls.close()
            return first, b"", ""
        d, _ = ls.accept()
        ls.close()
        data = self.drain(d)
        return first, data, self.line()

# --------------------------------------------------------------------- main

def main():
    cmd = sys.argv[1:]
    if not cmd:
        print(__doc__)
        return 2
    proxy = FakeProxy()
    proxy.start()
    dns_port, http_port, ftp_port = free_tcp_port(), free_tcp_port(), free_tcp_port()
    pasv = free_range(4)
    log_path = os.path.join(os.path.dirname(os.path.abspath(cmd[-1])), "e2e.log")
    log = open(log_path, "wb")
    args = cmd + ["-x", "127.0.0.1:%d" % proxy.port, "-d", str(dns_port), "-w", str(http_port),
                  "-f", str(ftp_port), "-p", "%d-%d" % (pasv, pasv + 3), "-n", "127.64.0.0/16", "-v"]
    proc = subprocess.Popen(args, stderr=log)
    try:
        run(proc, proxy, dns_port, http_port, ftp_port, pasv)
        # a second run with the idle time cut to one second, for the timeouts
        proxy = FakeProxy()
        proxy.start()
        args[args.index("-x") + 1] = "127.0.0.1:%d" % proxy.port
        proc = subprocess.Popen(args + ["-t", "1"], stderr=log)
        timeouts(proc, proxy, dns_port, http_port, ftp_port)
    except Exception as e:      # a hang or a reset shows up here
        global fails
        fails += 1
        print("  FAIL: exception %s: %s" % (type(e).__name__, e))
        import traceback
        traceback.print_exc()
    finally:
        if proc.poll() is None:
            proc.kill()
        log.close()
    text = open(log_path, "r", errors="replace").read()
    check("\nhttp 127." in text and "www.inode.com GET" in text, "verbose log has an http session line")
    check("\nftp 127." in text and "ftp.inode.com cmds=" in text, "verbose log has an ftp session line")
    check("stopped" in text, "verbose log has the stop line")
    print("e2e: %d checks, %d failures (log: %s)" % (checks, fails, log_path))
    return 1 if fails else 0

def run(proc, proxy, dns_port, http_port, ftp_port, pasv):
    # wait for the listeners
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", http_port), timeout=1).close()
            break
        except OSError:
            time.sleep(0.05)
    check(proc.poll() is None, "v90arch is running")

    # ---- DNS
    rid, flags, an, www, raw = dns_query(dns_port, "www.inode.com")
    check_eq(rid, 0x4242, "dns id")
    check(flags & 0x8000, "dns response flag")
    check_eq(flags & 15, 0, "dns rcode")
    check(www is not None and www.startswith("127.64."), "A answer from the pool: %r" % www)
    check_eq(struct.unpack(">I", raw[-10:-6])[0], 60, "dns ttl")
    check_eq(dns_query(dns_port, "WWW.inode.COM")[3], www, "same name, same address")
    ftpa = dns_query(dns_port, "ftp.inode.com")[3]
    check(ftpa is not None and ftpa != www and ftpa.startswith("127.64."), "second name, another address")
    nosite = dns_query(dns_port, "not.in.the.archive.example")[3]
    rid, flags, an, addr, raw = dns_query(dns_port, "www.inode.com", qtype=28)
    check_eq((an, flags & 15), (0, 0), "AAAA: empty NOERROR")
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.sendto(b"\x00\x01garbage", ("127.0.0.1", dns_port))       # ignored, and no harm done
    s.close()
    check_eq(dns_query(dns_port, "www.inode.com")[3], www, "still answering after garbage")

    # ---- HTTP
    # a chunked body (its end unknown to us) is refused, not passed on half
    r = http_fetch(www, http_port, b"POST /cgi HTTP/1.1\r\nHost: www.inode.com\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nab\r\n0\r\n\r\n")
    check(r.startswith(b"HTTP/1.0 411 "), "chunked request body: 411: %r" % r[:30])
    # one caller cannot take every session: the ninth from one address is refused
    held = [socket.create_connection((www, http_port), timeout=10) for _ in range(8)]
    time.sleep(0.2)
    try:
        r = http_fetch(www, http_port, b"GET / HTTP/1.0\r\n\r\n")
    except ConnectionResetError:
        r = b"(reset)"
    check(r.startswith(b"HTTP/1.0 503 ") or r == b"(reset)", "ninth session of one caller refused: %r" % r[:30])
    for h in held: h.close()
    time.sleep(0.2)
    # no Host header: the name comes from the address the client connected to
    r = http_fetch(www, http_port, b"GET / HTTP/1.0\r\nUser-Agent: Mozilla/1.0N (Windows)\r\n\r\n")
    check_eq(r, FakeProxy.crlf(b"200 OK", HOME), "HTTP/1.0 fetch without Host")
    seen = proxy.seen[-1]
    check(seen.startswith("GET http://www.inode.com/ HTTP/1.0\r\n"), "proxy saw the absolute URI: %r" % seen[:60])
    check("Connection: close" in seen and "User-Agent: Mozilla/1.0N (Windows)" in seen, "headers at the proxy")
    # Host header wins over the address; keep-alive headers do not reach the proxy
    r = http_fetch("127.0.0.1", http_port, b"GET /big HTTP/1.1\r\nHost: www.inode.com\r\nConnection: Keep-Alive\r\nKeep-Alive: 300\r\n\r\n")
    check_eq(r, FakeProxy.crlf(b"200 OK", BIG_HTTP), "200 kB body through the relay")
    seen = proxy.seen[-1]
    check(seen.startswith("GET http://www.inode.com/big HTTP/1.0\r\n"), "request line with Host")
    check("Keep-Alive" not in seen, "hop headers dropped")
    r = http_fetch(www, http_port, b"GET /port HTTP/1.0\r\nHost: www.inode.com:8080\r\n\r\n")
    check(r.endswith(b"port 8080 page"), "Host with a port")
    # the head in three pieces
    r = http_fetch(www, http_port, None, pieces=[b"GET / HT", b"TP/1.0\r\nAccept: */*\r", b"\n\r\n"])
    check_eq(r, FakeProxy.crlf(b"200 OK", HOME), "head split across reads")
    # HTTP/0.9: no response head for the client
    r = http_fetch(www, http_port, b"GET /\r\n")
    check_eq(r, HOME, "HTTP/0.9 gets the bare body")
    # POST: body partly with the head, partly later, plus the stray CR LF of old browsers
    body = b"name=Old+PC&comment=" + b"x" * 3000
    head = b"POST /cgi-bin/form HTTP/1.0\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: %d\r\n\r\n" % len(body)
    r = http_fetch(www, http_port, None, pieces=[head + body[:10], body[10:1500], body[1500:] + b"\r\n"])
    check(r.endswith(b"posted:" + body), "POST body relayed whole (%d bytes back)" % len(r))
    # already absolute (a browser with a proxy configured anyway)
    r = http_fetch("127.0.0.1", http_port, b"GET http://www.inode.com/ HTTP/1.0\r\n\r\n")
    check_eq(r, FakeProxy.crlf(b"200 OK", HOME), "absolute request")
    # client that half-closes after the request
    r = http_fetch(www, http_port, b"GET / HTTP/1.0\r\n\r\n", shut=True)
    check_eq(r, FakeProxy.crlf(b"200 OK", HOME), "half-closing client")
    # a site the archive does not have: the proxy's 503 page is passed through
    r = http_fetch(nosite, http_port, b"GET / HTTP/1.0\r\n\r\n")
    check(r.startswith(b"HTTP/1.0 503 ") and b"not in the archive" in r, "503 passed through")
    check(proxy.seen[-1].startswith("GET http://not.in.the.archive.example/ "), "unknown site by name")
    # an address nobody asked the DNS for: dotted form
    r = http_fetch("127.64.200.200", http_port, b"GET / HTTP/1.0\r\n\r\n")
    check(proxy.seen[-1].startswith("GET http://127.64.200.200/ "), "address without a name")
    # things that cannot be relayed
    r = http_fetch(www, http_port, b"CONNECT www.inode.com:443 HTTP/1.0\r\n\r\n")
    check(r.startswith(b"HTTP/1.0 400 "), "CONNECT refused")
    r = http_fetch(www, http_port, b"GET / HTTP/1.0\r\nX: " + b"y" * 9000 + b"\r\n\r\n")
    check(r.startswith(b"HTTP/1.0 400 "), "oversized head refused")

    # ---- FTP, passive
    f = Ftp(ftpa, ftp_port)
    banner = f.line()
    check(banner.startswith("220 ") and "ftp.inode.com" in banner and "Protoweb" in banner, "banner: " + banner)
    check(f.cmd("USER anonymous").startswith("331 "), "USER")
    check(f.cmd("PASS guest@").startswith("230 "), "PASS")
    check_eq(f.cmd("SYST"), "215 UNIX Type: L8", "SYST")
    check(f.cmd("PWD").startswith('257 "/"'), "PWD")
    check(f.cmd("TYPE A").startswith("200 "), "TYPE A")
    first, data, last = f.get_pasv("LIST")
    check(first.startswith("150 ") and last.startswith("226 "), "LIST replies: %s / %s" % (first, last))
    check_eq(data, b"drwxr-xr-x   1 ftp      ftp               0 Dec 16  2023 misc\r\n"
                   b"drwxr-xr-x   1 ftp      ftp               0 Jul 26  2025 pub\r\n"
                   b"-rw-r--r--   1 ftp      ftp            1229 Jul 18  2024 welcome.msg\r\n", "LIST of the root")
    check(proxy.seen[-1].startswith("GET ftp://ftp.inode.com/ HTTP/1.0\r\n"), "listing URL at the proxy")
    check(f.cmd("CWD pub").startswith("250 "), "CWD pub")
    check(f.cmd("PWD").startswith('257 "/pub"'), "PWD after CWD")
    check(f.cmd("CWD nonexistent").startswith("550 "), "CWD to nowhere")
    check(f.cmd("CWD /welcome.msg").startswith("550 "), "CWD to a file")
    first, data, last = f.get_pasv("LIST -la", connect_late=True)
    check(first.startswith("150 ") and last.startswith("226 "), "LIST with a late data connection")
    check_eq(data.count(b"\r\n"), 14, "LIST of /pub: entries")
    check(data.startswith(b"drwxr-xr-x   1 ftp      ftp               0 Sep 11  2022 aol\r\n"), "LIST of /pub: first line")
    check(f.cmd("TYPE I").startswith("200 "), "TYPE I")
    check_eq(f.cmd("SIZE /welcome.msg"), "213 1131", "SIZE")
    check(f.cmd("SIZE nothing.txt").startswith("550 "), "SIZE of nothing")
    first, data, last = f.get_pasv("RETR big.bin")
    check(first.startswith("150 ") and last.startswith("226 "), "RETR replies: %s / %s" % (first, last))
    check(data == BIG_FTP, "RETR of 300 kB over PASV: %d bytes" % len(data))
    first, data, last = f.get_pasv("RETR read me.txt")
    check_eq(data, b"read me\r\n", "RETR of a name with a space")
    first, data, last = f.get_pasv("RETR empty")
    check_eq((first[:3], data, last[:3]), ("150", b"", "226"), "RETR of an empty file")
    first, data, last = f.get_pasv("RETR nofile.bin")
    check(first.startswith("550 "), "RETR of a missing file: " + first)
    first, data, last = f.get_pasv("RETR /pub")
    check(first.startswith("550 "), "RETR of a directory: " + first)
    check(f.cmd("LIST").startswith("425 "), "LIST without PASV or PORT")
    check(f.cmd("STOR x").startswith("550 "), "STOR refused")
    check(f.cmd("SITE HELP").startswith("502 "), "unknown command")
    check(f.cmd("CDUP").startswith("250 "), "CDUP")

    # ---- FTP, active
    first, data, last = f.get_port("NLST")
    check_eq(data, b"misc\r\npub\r\nwelcome.msg\r\n", "NLST over PORT")
    check(first.startswith("150 ") and last.startswith("226 "), "NLST replies")
    first, data, last = f.get_port("RETR welcome.msg")
    check_eq(data, WELCOME, "RETR over PORT")
    check(last.startswith("226 "), "RETR over PORT final reply: " + last)
    # a RETR that fails before its 150 leaves the PORT for the next one
    ls = f.port()
    check(f.cmd("RETR nofile.bin").startswith("550 "), "RETR of a missing file over PORT")
    first = f.cmd("RETR welcome.msg")
    check(first.startswith("150 "), "PORT still there after a 550: " + first)
    if first.startswith("150 "):
        d, _ = ls.accept()
        check(f.drain(d).startswith(b"Welcome"), "RETR over the kept PORT")
        check(f.line().startswith("226 "), "RETR over the kept PORT final reply")
    ls.close()
    first, data, last = f.get_port("RETR pub/big.bin")
    check(data == BIG_FTP and last.startswith("226 "), "RETR of 300 kB over PORT: %d bytes" % len(data))
    check(f.cmd("PORT 10,1,2,3,4,5").startswith("5"), "PORT to another address refused")
    # two commands in one segment
    f.s.sendall(b"NOOP\r\nPWD\r\n")
    check(f.line().startswith("200 "), "pipelined NOOP")
    check(f.line().startswith('257 "/"'), "pipelined PWD")
    # the client gives up in the middle of a transfer: 426, and the session lives on
    ip, p = f.pasv()
    d = socket.create_connection((ip, p), timeout=10)
    check(f.cmd("RETR pub/big.bin").startswith("150 "), "RETR to be dropped")
    d.recv(1000)
    d.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    d.close()
    check(f.line().startswith("426 "), "dropped data connection gives 426")
    # ABOR in the middle
    ip, p = f.pasv()
    d = socket.create_connection((ip, p), timeout=10)
    check(f.cmd("RETR pub/big.bin").startswith("150 "), "RETR to be aborted")
    d.recv(1000)
    check(f.cmd("\xff\xf4\xff\xf2ABOR".encode("latin-1").decode("latin-1")).startswith("426 "), "ABOR: 426")
    check(f.line().startswith("226 "), "ABOR: 226")
    d.close()
    check(f.cmd("NOOP").startswith("200 "), "alive after ABOR")
    # an ABOR behind another command waiting for the transfer is taken at once
    ip, p = f.pasv()
    d = socket.create_connection((ip, p), timeout=10)
    check(f.cmd("RETR pub/big.bin").startswith("150 "), "RETR to be aborted from behind")
    d.recv(1000)
    f.s.sendall(b"PWD\r\nABOR\r\n")
    check(f.line().startswith("426 "), "queued ABOR: 426 before the PWD")
    check(f.line().startswith("226 "), "queued ABOR: 226")
    check(f.line().startswith('257 '), "then the PWD")
    d.close()
    check(f.cmd("QUIT").startswith("221 "), "QUIT")
    check_eq(f.s.recv(10), b"", "closed after QUIT")
    f.s.close()

    # FTP to an address without a name: the banner says so with the address
    f = Ftp("127.64.200.201", ftp_port)
    check("127.64.200.201" in f.line(), "banner for an unnamed address")
    f.s.close()

    # ---- the session limit: 12 at a time, the 13th is told to come back
    idle = [socket.create_connection((www, http_port), timeout=10) for _ in range(12)]
    time.sleep(0.2)
    # (read without sending: the refusal is written and the socket closed at
    # once, a request sent into that would turn the close into a reset)
    r = http_fetch(www, http_port, b"")
    check(r.startswith(b"HTTP/1.0 503 ") and b"busy" in r, "13th session refused: %r" % r[:40])
    f = Ftp(ftpa, ftp_port)
    check(f.line().startswith("421 "), "13th session refused on FTP too")
    f.s.close()
    for s in idle:
        s.close()
    time.sleep(0.2)
    r = http_fetch(www, http_port, b"GET / HTTP/1.0\r\n\r\n")
    check_eq(r, FakeProxy.crlf(b"200 OK", HOME), "slots are free again")

    # ---- the proxy goes away
    f = Ftp(ftpa, ftp_port)
    f.line()
    proxy.stop()
    time.sleep(0.1)
    r = http_fetch(www, http_port, b"GET / HTTP/1.0\r\n\r\n")
    check(r.startswith(b"HTTP/1.0 502 ") and b"could not reach the archive" in r, "502 when the proxy is down: %r" % r[:60])
    first, data, last = f.get_pasv("LIST")
    check(first.startswith("451 ") and "archive" in first, "FTP LIST when the proxy is down: " + first)
    check(f.cmd("CWD pub").startswith("451 "), "FTP CWD when the proxy is down")
    check(f.cmd("NOOP").startswith("200 "), "FTP session survives")
    f.s.close()
    check_eq(dns_query(dns_port, "www.inode.com")[3], www, "DNS does not need the proxy")

    # ---- clean exit on SIGTERM
    check(proc.poll() is None, "still running at the end")
    proc.send_signal(signal.SIGTERM)
    try:
        check_eq(proc.wait(timeout=5), 0, "exit status after SIGTERM")
    except subprocess.TimeoutExpired:
        check(False, "did not exit on SIGTERM")

def timeouts(proc, proxy, dns_port, http_port, ftp_port):
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", http_port), timeout=1).close()
            break
        except OSError:
            time.sleep(0.05)
    www = dns_query(dns_port, "www.inode.com")[3]
    silent = dns_query(dns_port, "silent.example")[3]
    # a request head sent a byte at a time is not given more time for it
    t0 = time.time()
    s = socket.create_connection((www, http_port), timeout=10)
    try:
        for c in b"GET / HTTP/1.0\r\nX-Slow: " + b"a" * 60:
            s.sendall(bytes([c]))
            time.sleep(0.05)
        r = s.recv(100)
    except OSError:
        r = b""
    s.close()
    check(r == b"" and time.time() - t0 < 4, "trickled head dropped at the deadline (%.1f s)" % (time.time() - t0))
    # a client that connects and says nothing is dropped
    t0 = time.time()
    r = http_fetch(www, http_port, b"")
    check(r == b"" and 0.5 < time.time() - t0 < 4, "idle HTTP client dropped after the idle time (%.1f s)" % (time.time() - t0))
    # the proxy takes the request and never answers: a 502, not silence
    t0 = time.time()
    r = http_fetch(silent, http_port, b"GET / HTTP/1.0\r\n\r\n")
    check(r.startswith(b"HTTP/1.0 502 ") and time.time() - t0 < 4, "502 when the proxy stays silent: %r" % r[:30])
    # the same for FTP: the fetch is given up, the session goes on
    f = Ftp(silent, ftp_port)
    f.line()
    first, data, last = f.get_pasv("LIST")
    check(first.startswith("451 "), "FTP LIST when the proxy stays silent: " + first)
    f.s.close()
    # a data connection that never comes
    f = Ftp(dns_query(dns_port, "ftp.inode.com")[3], ftp_port)
    f.line()
    f.pasv()
    check(f.cmd("LIST").startswith("150 "), "LIST accepted")
    check(f.line().startswith("425 "), "425 when the client never opens the data connection")
    # and an idle control connection is told so and closed
    t0 = time.time()
    check_eq(f.line(), "421 Timeout.", "idle FTP session")
    check_eq(f.s.recv(10), b"", "closed after 421")
    check(time.time() - t0 < 4, "within the idle time")
    f.s.close()
    # a normal fetch still works with the short timeout
    r = http_fetch(www, http_port, b"GET /big HTTP/1.0\r\n\r\n")
    check_eq(r, FakeProxy.crlf(b"200 OK", BIG_HTTP), "fetch in the short-timeout run")
    proc.send_signal(signal.SIGTERM)
    check_eq(proc.wait(timeout=5), 0, "exit status of the second run")

if __name__ == "__main__":
    sys.exit(main())
