#!/usr/bin/env python3
"""x86 test for sipecho: scripted SIP UAC + RTP sender (stdlib only).
Sends REGISTER, INVITE, ACK, then RTP at 50 pps with deliberate faults
(one dropped seq, one PT 13 packet, one 40 ms stall), checks every reply."""
import socket, struct, sys, time
SIP=("127.0.0.1", int(sys.argv[1])); MODE=sys.argv[2]; EXPECT=open(sys.argv[3],'rb').read() if len(sys.argv)>3 else b''
sip=socket.socket(socket.AF_INET, socket.SOCK_DGRAM); sip.bind(("127.0.0.1",0)); sip.settimeout(2)
rtp=socket.socket(socket.AF_INET, socket.SOCK_DGRAM); rtp.bind(("127.0.0.1",0)); rtp.settimeout(1)
me=sip.getsockname()[1]
def req(method, cseq, body=b'', totag=''):
    h=(f"{method} sip:modem@127.0.0.1:{SIP[1]} SIP/2.0\r\nVia: SIP/2.0/UDP 127.0.0.1:{me};branch=z9hG4bK{cseq}\r\n"
       f"From: <sip:fxs1@127.0.0.1>;tag=abc\r\nTo: <sip:modem@127.0.0.1>{totag}\r\nCall-ID: test1\r\nCSeq: {cseq} {method}\r\n"
       f"Contact: <sip:fxs1@127.0.0.1:{me}>\r\nMax-Forwards: 70\r\n")
    if body: h+="Content-Type: application/sdp\r\n"
    sip.sendto((h+f"Content-Length: {len(body)}\r\n\r\n").encode()+body, SIP)
def resp():
    d=sip.recv(4096).decode(); return int(d.split()[1]), d
fails=0
def check(name, ok, extra=''):
    global fails; fails+=0 if ok else 1; print(("PASS " if ok else "FAIL ")+name, extra)
req("REGISTER",1); c,d=resp(); check("REGISTER -> 200 with Expires", c==200 and "Expires" in d)
sdp=(f"v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=-\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\nm=audio {rtp.getsockname()[1]} RTP/AVP 0 8 101\r\n"
     "a=rtpmap:0 PCMU/8000\r\na=rtpmap:8 PCMA/8000\r\na=rtpmap:101 telephone-event/8000\r\na=ptime:20\r\n").encode()
req("INVITE",2,sdp); c,d=resp()
port=int(d.split("m=audio ")[1].split()[0]) if "m=audio" in d else 0
check("INVITE -> 200, SDP offers only PT 0", c==200 and "RTP/AVP 0\r\n" in d and ";tag=" in d, f"rtp port {port}")
req("ACK",2,totag=";tag=v90d")
N=250; seq=1000; ts=5000; bad=0; got=0; rseq=None; rts=None; txpos=0; contig=True
t0=time.monotonic()
for i in range(N):
    if i==100: seq+=1; ts+=160              # one lost packet
    if i==150: time.sleep(0.04)             # one stall
    pt=13 if i==120 else 0
    pay=bytes([13]) if pt==13 else bytes(((i*7+k)&0xFF) for k in range(160))
    rtp.sendto(struct.pack("!BBHII",0x80,pt,seq&0xFFFF,ts,0x1234)+pay,("127.0.0.1",port))
    try: r=rtp.recv(2048)
    except socket.timeout: bad+=1; continue
    got+=1; v,rpt,s,t,_=struct.unpack("!BBHII",r[:12]); rp=r[12:]
    if rseq is not None and (s!=(rseq+1)&0xFFFF or t!=(rts+160)&0xFFFFFFFF): contig=False
    rseq,rts=s,t
    if rpt!=0 or len(rp)!=160: bad+=1
    elif MODE=="echo" and pt==0 and rp!=pay: bad+=1
    elif MODE=="echo" and pt!=0 and rp!=b'\xff'*160: bad+=1
    elif MODE=="file":
        exp=(EXPECT[txpos:txpos+160]+b'\xff'*160)[:160]; txpos+=160
        if rp!=exp: bad+=1
    seq+=1; ts+=160
    time.sleep(max(0, t0+(i+1)*0.02-time.monotonic()))
check(f"one reply per packet ({got}/{N})", got==N)
check("reply seq/timestamp contiguous (no slip on our side)", contig)
check(f"reply payloads correct for mode {MODE}", bad==0, f"bad={bad}")
req("BYE",3,totag=";tag=v90d"); c,d=resp(); check("BYE -> 200", c==200)
body=b"v=0\r\nm=image 4000 udptl t38\r\n"; req("INVITE",4,body,totag=";tag=v90d"); c,d=resp(); check("T.38-only INVITE -> 488", c==488)
sys.exit(1 if fails else 0)
