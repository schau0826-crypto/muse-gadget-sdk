#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Host tests for UTF-8 caption wrapping and bounded caption copies."""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class CaptionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            raise unittest.SkipTest("C compiler not available")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.tmp.name) / "muse_caption_harness"
        proc = subprocess.run(
            [
                *cc,
                "-include",
                str(ROOT / "tests" / "host_compat.h"),
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT / "components" / "muse"),
                str(ROOT / "tests" / "muse_caption_harness.c"),
                str(ROOT / "components" / "muse" / "muse_chat_text.c"),
                str(ROOT / "components" / "muse" / "muse_text.c"),
                "-o",
                str(cls.binary),
            ],
            cwd=ROOT,
            text=True,
            capture_output=True,
        )
        if proc.returncode:
            raise AssertionError(proc.stdout + proc.stderr)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_harness(self, args: list[str], data: bytes) -> bytes:
        proc = subprocess.run([str(self.binary), *args], input=data, capture_output=True)
        self.assertEqual(proc.returncode, 0, msg=proc.stderr.decode(errors="replace"))
        return proc.stdout

    def caption(self, text: str, cols: int, lines: int, at: int = 0, cap: int = 768) -> tuple[bool, str]:
        out = self.run_harness(
            ["caption", str(cap), str(cols), str(lines), str(at)], text.encode("utf-8")
        )
        shown, _, body = out.partition(b"\n")
        self.assertIn(shown, (b"0", b"1"))
        return shown == b"1", body.decode("utf-8")

    def test_cjk_punctuation_wraps_without_dropping_characters(self) -> None:
        text = "甲乙（丙丁）戊己。庚辛『壬癸』子丑！"
        shown, page = self.caption(text, cols=3, lines=10)
        self.assertTrue(shown)
        rows = page.split("\n")
        self.assertEqual("".join(rows), text)
        no_line_start = set("，。！？：；、）》】」』”’）〕］｝〉〗〙〛…,.!?:;)]}>")
        no_line_end = set("（〔［｛〈《「『【〖〘〚“‘([{<")
        for row in rows:
            self.assertTrue(row)
            self.assertNotIn(row[0], no_line_start, msg=row)
            self.assertNotIn(row[-1], no_line_end, msg=row)

    def test_english_words_and_ideographic_space_wrap(self) -> None:
        shown, english = self.caption("The quick brown fox", cols=10, lines=3)
        self.assertTrue(shown)
        self.assertEqual(english, "The quick\nbrown fox")

        shown, cjk = self.caption("甲乙　丙丁", cols=3, lines=3)
        self.assertTrue(shown)
        self.assertEqual(cjk, "甲乙\n丙丁")

    def test_captions_scroll_a_line_at_a_time_and_accept_offsets_inside_utf8(self) -> None:
        text = "甲乙丙丁戊己庚辛壬癸子丑"
        rows = lambda at: self.caption(text, cols=3, lines=3, at=at)[1].split("\n")
        self.assertEqual(rows(0), ["甲乙丙", "丁戊己", "庚辛壬"])
        self.assertEqual(rows(3 * 3 + 1), ["甲乙丙", "丁戊己", "庚辛壬"])   # 2nd line: still the top
        self.assertEqual(rows(6 * 3 + 1), ["丁戊己", "庚辛壬", "癸子丑"])   # 3rd line: one up
        self.assertEqual(rows(11 * 3 + 2), ["丁戊己", "庚辛壬", "癸子丑"])  # the end fills the page

        shown, one = self.caption(text, cols=3, lines=1, at=6 * 3)
        self.assertTrue(shown)
        self.assertEqual(one, "庚辛壬")

    def test_long_cjk_pages_fit_bounded_output(self) -> None:
        text = "汉" * 320
        shown, first = self.caption(text, cols=16, lines=5, cap=768)
        self.assertTrue(shown)
        self.assertEqual(len(first.split("\n")), 5)
        self.assertEqual("".join(first.split("\n")), text[:80])
        shown, later = self.caption(text, cols=16, lines=5, at=80 * 3 + 1, cap=768)
        self.assertTrue(shown)
        self.assertEqual("".join(later.split("\n")), text[64:144])   # line 5 said: lines 4-8

    def test_bullet_does_not_sit_alone_before_hanzi(self) -> None:
        shown, page = self.caption("- 查东西：搜新闻、查资料、比价、做调研", cols=10, lines=5)
        self.assertTrue(shown)
        self.assertTrue(page.startswith("- 查东西"), msg=page)

    def plain(self, text: str, speech: bool = False) -> str:
        return self.run_harness(["speech" if speech else "plain", "4096"], text.encode()).decode()

    def test_markdown_shows_as_plain_text(self) -> None:
        md = (
            "## 推荐\n按常用程度排：\n- **查东西**：搜新闻\n* `code` 和 _下划线_ 保留\n---\n"
            "> 引用\n这周 GitHub 热榜：[OpenShell](https://github.com/NVIDIA/OpenShell)，"
            "详见 https://www.example.com/a/b?c=1。\n| 名称 | 星标 |\n|---|:-:|\n| A | 1 |"
        )
        self.assertEqual(
            self.plain(md),
            "推荐\n按常用程度排：\n- 查东西：搜新闻\n- code 和 _下划线_ 保留\n"
            "引用\n这周 GitHub 热榜：OpenShell，详见 example.com。\n  名称   星标  \n  A   1  ",
        )

    def test_speech_drops_urls_code_and_adds_pauses(self) -> None:
        md = "## 推荐\n- **查东西**\n看 [链接](https://x.com/y) 或 <https://a.b/c>。\n```\nrm -rf /\n```\n好"
        self.assertEqual(self.plain(md, speech=True), "推荐。\n查东西。\n看 链接 或 。\n好")

    def test_streaming_link_hides_its_partial_target(self) -> None:
        self.assertEqual(self.plain("热榜：[OpenShell](https://github.com/NVI"), "热榜：OpenShell")

    def test_speech_units_and_offsets(self) -> None:
        units = lambda s: int(self.run_harness(["units", "1"], s.encode()))
        self.assertEqual(units("汉字"), 12)
        self.assertEqual(units("ab 12"), 8)
        self.assertEqual(units("好，"), 9)
        self.assertEqual(units("🙂 "), 0)
        offset = lambda s, u: int(self.run_harness(["offset", str(u)], s.encode()))
        self.assertEqual(offset("汉字ab", 0), 0)
        self.assertEqual(offset("汉字ab", 6), 3)
        self.assertEqual(offset("汉字ab", 13), 6)   # partway through "a"
        self.assertEqual(offset("汉字ab", 99), 8)

    def test_ascii_standin_widths_and_dropped_emoji_match_caption_normalization(self) -> None:
        for base in ("a" * 18, "字" * 18):
            text = base + "…—"
            shown, page = self.caption(text, cols=19, lines=4)
            self.assertTrue(shown)
            self.assertEqual("".join(page.split("\n")), text)
            for row in page.split("\n"):
                cells = sum({"…": 3, "—": 2}.get(ch, 1) for ch in row)
                self.assertLessEqual(cells, 19, msg=row)

        text = "a" * 18 + "🙂" + "b"
        shown, page = self.caption(text, cols=19, lines=2)
        self.assertTrue(shown)
        self.assertEqual(page, text)

    def test_tail_and_copy_never_split_utf8(self) -> None:
        tail = self.run_harness(["tail", "256"], ("汉" * 35).encode())
        self.assertEqual(tail.decode(), "汉" * 32)

        self.assertEqual(self.run_harness(["copy", "5"], ("abc汉".encode())), b"abc")
        self.assertEqual(self.run_harness(["copy", "7"], "abc汉".encode()), "abc汉".encode())
        self.assertEqual(self.run_harness(["copy", "16"], b"abc\xe4\xbd"), b"abc")
        self.assertEqual(self.run_harness(["copy", "16"], b"abc\xe4A"), b"abc")

    def test_delta_append_and_tiny_caption_capacity_keep_whole_characters(self) -> None:
        data = "甲".encode() + b"\1" + "乙丙".encode()
        self.assertEqual(self.run_harness(["append", "7"], data).decode(), "甲乙")
        data = "甲".encode() + b"\1" + b"\xe4\xbd"
        self.assertEqual(self.run_harness(["append", "16"], data).decode(), "甲")

        shown, page = self.caption("汉字", cols=2, lines=1, cap=4)
        self.assertFalse(shown)
        self.assertEqual(page, "")
        shown, page = self.caption("汉字", cols=2, lines=1, cap=7)
        self.assertTrue(shown)
        self.assertEqual(page, "汉字")


if __name__ == "__main__":
    unittest.main()
