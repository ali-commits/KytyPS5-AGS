"""Actual KCPA session + native adapter with a paced original-driver queue model."""
import math,struct,unittest
import audio_client as audio
from native_audio import NativeAudio
from test_native_audio import Driver,feedback
from test_audio_client import wire,decoded,state

class PacedDriver(Driver):
    def __init__(self,clock):
        super().__init__();self.clock=clock;self.next_send=clock()+1024/48000;self.stalled=False
    def drain(self):
        if self.stalled:return
        while self.next_send<=self.clock()+1e-9:
            for s in self.streams.values():s['frames']=max(0,s['frames']-1024)
            self.next_send+=1024/48000
    def kyty_bt_open(self,*args):
        handle=super().kyty_bt_open(*args);self.streams[handle]['frames']=0;return handle
    def kyty_bt_queue(self,handle,samples,frames):
        self.drain();self.streams[handle]['frames']+=frames
        return math.ceil(self.streams[handle]['frames']*1e6/48000)
    def kyty_bt_queued_us(self,handle):
        self.drain();return math.ceil(self.streams[handle]['frames']*1e6/48000)

class NativeSessionTests(unittest.TestCase):
    def test_continuous_native_packets_do_not_double_count_transport_tail(self):
        now=[10.0];driver=PacedDriver(lambda:now[0]);sink=NativeAudio(driver=driver,clock=lambda:now[0])
        self.assertTrue(sink.acquire());sink.controls(feedback(),10.2)
        session=audio.Session(sink);session.clock.sample(9999900,10000000,9999950,9999950)
        session.receive(*state('preparing'),10000000);session.receive(*state('active'),10000000)
        session.receive(*decoded(wire(2,7,9,rate=48000,pg=1,seq=1,stream=1)),10000000)
        try:
            for n in range(400):
                now[0]=10+n*.01;driver.now=700000+n*10000
                sink.controls(feedback(),now[0]+.2);clock=int(now[0]*1e6)
                session.tick(clock)
                session.receive(*decoded(wire(3,7,9,rate=48000,pg=1,seq=n+2,first=n*480,source=clock,payload=struct.pack('<ff',.03,-.04)*480,stream=1)),clock)
        finally:sink.shutdown()

    def test_stalled_native_queue_still_expires_with_fresh_input(self):
        now=[10.0];driver=PacedDriver(lambda:now[0]);sink=NativeAudio(driver=driver,clock=lambda:now[0])
        self.assertTrue(sink.acquire());sink.controls(feedback(),10.2)
        session=audio.Session(sink);session.clock.sample(9999900,10000000,9999950,9999950)
        session.receive(*state('preparing'),10000000);session.receive(*state('active'),10000000)
        session.receive(*decoded(wire(2,7,9,rate=48000,pg=1,seq=1,stream=1)),10000000)
        try:
            for n in range(6):
                now[0]=10+n*.01;driver.now=700000+n*10000;sink.controls(feedback(),now[0]+.2)
                clock=int(now[0]*1e6);session.tick(clock)
                session.receive(*decoded(wire(3,7,9,rate=48000,pg=1,seq=n+2,first=n*480,source=clock,payload=struct.pack('<ff',.03,-.04)*480,stream=1)),clock)
            driver.stalled=True
            with self.assertRaisesRegex(audio.ProtocolError,'queued sample delivery deadline'):
                for n in range(6,25):
                    now[0]=10+n*.01;driver.now=700000+n*10000;sink.controls(feedback(),now[0]+.2)
                    session.tick(int(now[0]*1e6))
        finally:sink.shutdown()
