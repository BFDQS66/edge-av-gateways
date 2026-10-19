"""Focused tests for continuous and quiet microphone input."""

import importlib.util
from pathlib import Path
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "scripts/live_translate.py"
SPEC = importlib.util.spec_from_file_location("live_translate", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class AudioSegmenterTests(unittest.TestCase):
    def setUp(self):
        self.segmenter = MODULE.AudioSegmenter()
        self.frame = b"\0" * MODULE.FRAME_BYTES

    def feed(self, levels):
        return [result for level in levels
                if (result := self.segmenter.feed(self.frame, level))]

    def test_continuous_voice_is_emitted_before_silence(self):
        results = self.feed([180.0] * 270)
        self.assertGreaterEqual(len(results), 2)
        self.assertEqual([reason for _, reason in results[:2]],
                         ["max_duration", "max_duration"])
        self.assertTrue(all(len(pcm) <= MODULE.MAX_FRAMES * MODULE.FRAME_BYTES
                            for pcm, _ in results))

    def test_short_phrase_ends_after_silence(self):
        self.assertFalse(self.feed([500.0] * 15 + [45.0] * MODULE.SILENCE_FRAMES))
        results = self.feed([45.0] * (MODULE.SHORT_SILENCE_FRAMES - MODULE.SILENCE_FRAMES))
        self.assertEqual(len(results), 1)
        self.assertEqual(results[0][1], "silence")

    def test_weak_short_burst_is_held_for_context(self):
        self.assertFalse(self.feed([120.0] * 12 + [45.0] * 20))
        results = self.feed([45.0] * (MODULE.WEAK_MIN_FRAMES - 32))
        self.assertEqual(len(results), 1)
        self.assertEqual(results[0][1], "max_duration")

    def test_weak_voice_fallback_and_silence_rejection(self):
        self.assertFalse(self.feed([45.0] * 100))
        results = self.feed([90.0] * MODULE.MAX_FRAMES)
        self.assertEqual(len(results), 1)
        self.assertEqual(results[0][1], "low_level_fallback")

    def test_ungated_retry_requires_long_audible_window(self):
        self.assertTrue(MODULE.should_retry_ungated("max_duration", 12, 82, 45))
        self.assertFalse(MODULE.should_retry_ungated("silence", 12, 82, 45))
        self.assertFalse(MODULE.should_retry_ungated("max_duration", 2, 82, 45))
        self.assertFalse(MODULE.should_retry_ungated("max_duration", 12, 55, 45))

    def test_ungated_text_filter_rejects_short_noise_hallucinations(self):
        self.assertFalse(MODULE.plausible_fallback("그.", "zh"))
        self.assertFalse(MODULE.plausible_fallback("嗯", "zh"))
        self.assertTrue(MODULE.plausible_fallback("我们的日子为什么一去不复返呢", "zh"))
        self.assertFalse(MODULE.plausible_fallback("hello", "en"))
        self.assertTrue(MODULE.plausible_fallback("Please close the door", "en"))

    def test_gate_text_filter_rejects_punctuation_and_wrong_language(self):
        self.assertFalse(MODULE.usable_transcript(".", "en"))
        self.assertFalse(MODULE.usable_transcript("。", "zh"))
        self.assertFalse(MODULE.usable_transcript("그.", "zh"))
        self.assertTrue(MODULE.usable_transcript("Before5 clock.", "en"))
        self.assertTrue(MODULE.usable_transcript("请关闭摄像头。", "zh"))


if __name__ == "__main__":
    unittest.main()
