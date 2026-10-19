import ctypes as C
import time
import unittest
from unittest.mock import Mock
from controller import Controller, trigger_report, OFF

class OutputOwnershipTests(unittest.TestCase):
    def controller(self):
        c=Controller.__new__(Controller)
        c.pad=123
        c.s=Mock()
        c.output=None
        return c

    def test_cancel_sdl_before_acquiring_raw_output(self):
        c=self.controller(); owner=Mock(); events=[]
        c.s.SDL_GameControllerRumble.side_effect=lambda *a:events.append('sdl-stop')
        owner.acquire.side_effect=lambda:events.append('raw-acquire')
        c.attach_output(owner)
        self.assertEqual(events,['sdl-stop','raw-acquire'])
        self.assertIs(c.output,owner)

    def test_no_sdl_effect_write_during_raw_ownership(self):
        c=self.controller();owner=Mock();owner.owns_output=True;c.output=owner
        report=trigger_report(12,OFF,OFF)
        deadline=time.monotonic()+.1
        c.effects([1,2],report,100,deadline=deadline)
        c.stop()
        c.s.SDL_GameControllerRumble.assert_not_called()
        c.s.SDL_GameControllerSendEffect.assert_not_called()
        self.assertEqual(owner.controls.call_count,2)
        self.assertEqual(owner.controls.call_args_list[0].args[1],deadline)
        self.assertEqual(owner.controls.call_args.args[1],0)

    def test_shutdown_raw_before_sdl_feedback_restored(self):
        c=self.controller();owner=Mock();c.output=owner;events=[]
        owner.shutdown.side_effect=lambda:events.append('raw-stop')
        c.s.SDL_GameControllerRumble.side_effect=lambda *a:events.append('sdl-stop')
        c.detach_output()
        self.assertEqual(events,['raw-stop','sdl-stop'])
        self.assertIsNone(c.output)

    def test_failed_acquisition_keeps_sdl_available(self):
        c=self.controller();owner=Mock();owner.acquire.side_effect=OSError('denied')
        with self.assertRaises(OSError): c.attach_output(owner)
        self.assertIsNone(c.output)

if __name__=='__main__':unittest.main()

class AudioLifecycleTests(unittest.TestCase):
    def test_raw_owner_attached_before_audio_network_start(self):
        from companion import ControllerAudio
        events=[];c=Mock();c.serial='serial';sink=Mock();client=Mock()
        c.attach_output.side_effect=lambda x:events.append('attach')
        client.start.side_effect=lambda:events.append('network')
        audio=ControllerAudio(c,{},sink_factory=lambda **k:sink,client_factory=lambda *a:client)
        audio.start()
        self.assertEqual(events,['attach','network'])
        audio.close();client.close.assert_called_once();c.detach_output.assert_called_once()

    def test_failed_audio_start_releases_output(self):
        from companion import ControllerAudio
        c=Mock();sink=Mock();client=Mock();client.start.side_effect=RuntimeError('unavailable')
        audio=ControllerAudio(c,{},sink_factory=lambda **k:sink,client_factory=lambda *a:client)
        with self.assertRaises(RuntimeError):audio.start()
        c.detach_output.assert_called_once()

class InstanceOwnershipTests(unittest.TestCase):
    def test_second_instance_cannot_initialize_another_controller(self):
        import os,tempfile
        from pathlib import Path
        from companion import acquire_app_lock
        with tempfile.TemporaryDirectory() as d:
            config=Path(d)/'config.json'
            first=acquire_app_lock(config)
            try:
                with self.assertRaisesRegex(RuntimeError,'already running'):
                    acquire_app_lock(config)
            finally:os.close(first)
            second=acquire_app_lock(config);os.close(second)
