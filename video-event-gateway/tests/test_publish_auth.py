import importlib.util
from pathlib import Path
import unittest


SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"


def module(name):
    spec = importlib.util.spec_from_file_location(name, SCRIPTS / f"{name}.py")
    loaded = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(loaded)
    return loaded


auth = module("publish_auth")
config = module("configure_publish_auth")


class PublishAuthTest(unittest.TestCase):
    def test_only_gate_source_and_stream_pass(self):
        valid = {"ip": "192.168.1.101", "app": "approved", "stream": "obs",
                 "schema": "rtmp", "protocol": "rtmp", "params": "token=" + "x" * 32}
        self.assertEqual(auth.decision(valid, "192.168.1.101", "x" * 32)["code"], 0)
        for key, wrong in [("ip", "192.168.1.100"), ("app", "live"),
                           ("stream", "bypass"), ("schema", "rtsp"),
                           ("protocol", "rtsp"), ("params", "token=fake")]:
            changed = dict(valid)
            changed[key] = wrong
            self.assertNotEqual(auth.decision(changed, "192.168.1.101", "x" * 32)["code"], 0)
        self.assertNotEqual(auth.decision(None, "192.168.1.101", "x" * 32)["code"], 0)

    def test_config_preserves_other_values_and_is_idempotent(self):
        sample = "[api]\nsecret=private\n[hook]\nenable=0\non_publish=\ntimeoutSec=10\n[rtmp]\nport=11936\n"
        output = config.configure(sample, "http://127.0.0.1:18993/on_publish")
        self.assertIn("secret=private", output)
        self.assertIn("enable=1\n", output)
        self.assertIn("on_publish=http://127.0.0.1:18993/on_publish", output)
        self.assertEqual(config.configure(output, "http://127.0.0.1:18993/on_publish"), output)


if __name__ == "__main__":
    unittest.main()
