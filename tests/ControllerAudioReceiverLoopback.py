#!/usr/bin/env python3
"""Actual companion TLS binary, private generated certs, null output only."""
import hashlib
import json
import os
from pathlib import Path
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time

binary = sys.argv[1]
receiver_env = os.environ.copy()
if len(sys.argv)>2:
    receiver_env['LD_PRELOAD']=sys.argv[2]

def frame(kind, generation=0, epoch=0, port=0, rate=0, pg=0, seq=0, first=0, source=0, payload=b'', stream=0):
    return struct.pack('>4sBBBBIIQQIIQQQQ', b'KCPA', 1, kind, stream, 2 if stream else 0, 72,
                       len(payload), generation, epoch, port, rate, pg, seq, first, source) + payload

def take(conn):
    def exact(n):
        data = b''
        while len(data) < n:
            chunk = conn.recv(n-len(data))
            if not chunk:
                raise RuntimeError('unexpected client close')
            data += chunk
        return data
    h = exact(72)
    fields = struct.unpack('>4sBBBBIIQQIIQQQQ', h)
    return fields, exact(fields[6])

with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    subprocess.run([binary, '--generate-identity', str(root/'receiver')], check=True,env=receiver_env)
    assert (root/'receiver/key.pem').stat().st_mode & 0o777 == 0o600
    assert (root/'receiver').stat().st_mode & 0o777 == 0o700
    subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-keyout',str(root/'host.key'),'-out',str(root/'host.pem'),'-days','1','-subj','/CN=loopback'],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    der=ssl.PEM_cert_to_DER_cert((root/'host.pem').read_text())
    pin=hashlib.sha256(der).hexdigest()
    ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version=ssl.TLSVersion.TLSv1_2
    ctx.load_cert_chain(root/'host.pem',root/'host.key')
    ctx.load_verify_locations(root/'receiver/cert.pem')
    ctx.verify_mode=ssl.CERT_REQUIRED
    listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen();listener.settimeout(6)
    payload=struct.pack('<ffff',.125,-.25,.5,-.75)
    expected=hashlib.sha256(payload).hexdigest()
    failures=[]
    def serve():
        try:
            sock,_=listener.accept();sock.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
            with ctx.wrap_socket(sock,server_side=True) as conn:
                conn.settimeout(4)
                h,p=take(conn);assert h[2]==1 and json.loads(p)['controller']==0
                conn.sendall(frame(7,7,9,payload=b'{"state":"preparing"}'))
                while True:
                    h,p=take(conn)
                    if h[2]==9:
                        sent=json.loads(p)['client_send_us'];now=time.monotonic_ns()//1000
                        conn.sendall(frame(10,payload=json.dumps(dict(client_send_us=sent,server_receive_us=now,server_send_us=now)).encode()))
                    elif h[2]==6:
                        assert h[7:9]==(7,9) and not p
                        break
                conn.sendall(frame(7,7,9,payload=b'{"state":"active"}'))
                conn.sendall(frame(2,7,9,rate=48000,pg=1,seq=1,stream=1))
                pcm=frame(3,7,9,rate=48000,pg=1,seq=2,source=time.monotonic_ns()//1000,payload=payload,stream=1)
                for i in range(0,len(pcm),3): conn.sendall(pcm[i:i+3])
                conn.sendall(frame(4,7,9,rate=48000,pg=1,seq=3,first=2,stream=1))
                conn.sendall(frame(5,7,9))
                time.sleep(.2)
        except Exception as error:
            failures.append(repr(error))
    server=threading.Thread(target=serve);server.start()
    config=dict(host='127.0.0.1',port=listener.getsockname()[1],pinned_host_sha256=pin,cert=str(root/'receiver/cert.pem'),key=str(root/'receiver/key.pem'),receiver_id='test',sink='null')
    path=root/'config.json';path.write_text(json.dumps(config));path.chmod(0o600)
    result=subprocess.run([binary,'--config',str(path),'--run-seconds','2','--diagnostics','--raw',str(root/'capture.kcpa')],capture_output=True,text=True,env=receiver_env,timeout=6)
    server.join(6);listener.close()
    assert not server.is_alive() and not failures,(failures,result.stdout,result.stderr)
    assert result.returncode==0,result.stderr
    assert expected in result.stdout,result.stdout
    capture=(root/'capture.kcpa').read_bytes();assert capture[72:]==payload,capture
    assert len(capture)==72+len(payload)
    assert (root/'capture.kcpa').stat().st_mode & 0o777 == 0o600
    print('ControllerAudioReceiverLoopback passed: real mutual TLS / fragmented PCM / exact SHA256 / private raw / null only')
    # Wrong pinned host must receive no application Hello despite a valid client cert.
    listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen();listener.settimeout(4)
    observed=[]
    def wrong_pin_server():
        sock,_=listener.accept()
        try:
            with ctx.wrap_socket(sock,server_side=True) as conn:
                conn.settimeout(3)
                try: observed.append(conn.recv(72))
                except (ssl.SSLError,ConnectionError): observed.append(b'')
        except ssl.SSLError:
            observed.append(b'')
    server=threading.Thread(target=wrong_pin_server);server.start()
    config['port']=listener.getsockname()[1];config['pinned_host_sha256']='0'*64
    path.write_text(json.dumps(config))
    wrong=subprocess.run([binary,'--config',str(path),'--run-seconds','1'],capture_output=True,text=True,env=receiver_env,timeout=4)
    server.join(4);listener.close()
    assert observed==[b''],observed
    assert 'pin/time mismatch' in wrong.stderr,wrong.stderr
    assert not wrong.stdout,'hash/data diagnostics must be explicit'
    path.chmod(0o644)
    private=subprocess.run([binary,'--config',str(path),'--status'],capture_output=True,text=True,env=receiver_env,timeout=2)
    assert private.returncode==1 and 'owner-only' in private.stderr,private.stderr
    print('ControllerAudioReceiverLoopback negative cases passed: wrong pin sends no Hello; private config enforced; hashes opt-in')
    path.chmod(0o600)
    config['pinned_host_sha256']=pin
    # A slow/incomplete TLS frame cannot retain readiness or extend the read deadline.
    listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen();listener.settimeout(5)
    failures=[]
    def stalled_server():
        try:
            sock,_=listener.accept();sock.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
            with ctx.wrap_socket(sock,server_side=True) as conn:
                conn.settimeout(4)
                take(conn)
                conn.sendall(b'K')
                h,p=take(conn)
                assert h[2]==9
                sent=json.loads(p)['client_send_us'];now=time.monotonic_ns()//1000
                # Do not interleave a complete Pong into the intentionally partial header.
                time.sleep(2.3)
        except Exception as error:
            failures.append(repr(error))
    server=threading.Thread(target=stalled_server);server.start()
    config['port']=listener.getsockname()[1];path.write_text(json.dumps(config))
    stalled=subprocess.run([binary,'--config',str(path),'--run-seconds','3'],capture_output=True,text=True,env=receiver_env,timeout=6)
    server.join(5);listener.close()
    assert not failures,failures
    assert 'partial frame deadline' in stalled.stderr,stalled.stderr
    print('ControllerAudioReceiverLoopback partial frame deadline passed')
