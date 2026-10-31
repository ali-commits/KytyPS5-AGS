import struct
import unittest
from reference_check import run_reference

class Clock:
    def __init__(self):self.now=10.0
    def __call__(self):return self.now
    def sleep(self,seconds):self.now+=max(0,seconds)
class Sink:
    def __init__(self,**kwargs):self.samples=[];self.closed=False;self.reports_accepted=0
    def acquire(self):return True
    def prepare(self):return True
    def controls(self,*a):return True
    def open(self,h):self.kind=h['kind'];return True
    def pcm(self,h,payload,now):self.samples.extend(struct.unpack('<'+'f'*(len(payload)//4),payload));self.reports_accepted+=1;return True
    def close(self,p):self.closed=True
    def shutdown(self):self.closed=True
class Controller:
    serial='example'
    def __init__(self):self.output=None;self.detached=False
    def snapshot(self):return {'connected':True}
    def attach_output(self,output):self.output=output;output.acquire()
    def detach_output(self):self.output.shutdown();self.output=None;self.detached=True
class Tests(unittest.TestCase):
    def run_kind(self,kind,cancel=lambda:False):
        clock=Clock();controller=Controller();sink=Sink()
        result=run_reference(controller,kind,duration=.1,canceled=cancel,sink_factory=lambda **k:sink,clock=clock,sleep=clock.sleep)
        self.assertTrue(controller.detached);self.assertTrue(sink.closed)
        return result,sink
    def test_speaker_is_bounded_signed_pcm_and_cleanup(self):
        result,sink=self.run_kind(2)
        self.assertLessEqual(max(map(abs,sink.samples)),.041)
        self.assertTrue(any(x<0 for x in sink.samples));self.assertEqual(result['kind'],'speaker')
    def test_haptics_vary_and_cancel_is_neutral(self):
        result,sink=self.run_kind(1)
        self.assertTrue(any(abs(x)>.001 for x in sink.samples));self.assertLessEqual(max(map(abs,sink.samples)),.121)
        result,sink=self.run_kind(1,lambda:True)
        self.assertTrue(result['canceled']);self.assertEqual(sink.samples,[])
    def test_bad_kind_or_long_duration_never_acquires(self):
        for kind,duration in ((0,1),(2,3)):
            c=Controller()
            with self.assertRaises(ValueError):run_reference(c,kind,duration=duration)
            self.assertIsNone(c.output)
