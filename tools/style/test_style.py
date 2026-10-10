#!/usr/bin/env python3
"""Tests for the style tool.

    python3 tools/style/test_style.py

  fixtures   every fixtures/NAME.in.EXT becomes fixtures/NAME.out.EXT, and an .out file is
             left as it is (a second run changes nothing)
  proof      code_fingerprint() sees every change to the code and none to the comments or
             the whitespace -- checked against deliberate mutants, as tools/fncmp is
  refusal    a stage that changes the code is refused, and the file is never written
  command    --check and --fix: exit codes, and what is written

To add a rule's fixture: write NAME.in.c, run the tool on it, read the .out file it gives
against docs/STYLE.md, and commit both once it is right.
"""
import glob
import io
import os
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import order                                                     # noqa: E402
import restyle                                                   # noqa: E402
import style                                                     # noqa: E402

FIXTURES = os.path.join(HERE, 'fixtures')


def read(path):
    with open(path, encoding='utf-8', newline='') as handle:
        return handle.read()


def fixture_pairs():
    for source in sorted(glob.glob(os.path.join(FIXTURES, '*.in.*'))):
        yield source, source.replace('.in.', '.out.')


class Fixtures(unittest.TestCase):

    def test_there_are_fixtures(self):
        self.assertGreaterEqual(len(list(fixture_pairs())), 6)

    def test_input_becomes_output(self):
        for source, expected in fixture_pairs():
            with self.subTest(fixture=os.path.basename(source)):
                name = os.path.basename(expected).replace('.out.', '.')
                self.assertEqual(style.style_text(read(source), name), read(expected))

    def test_output_is_left_alone(self):
        for _, expected in fixture_pairs():
            with self.subTest(fixture=os.path.basename(expected)):
                name = os.path.basename(expected).replace('.out.', '.')
                self.assertEqual(style.style_text(read(expected), name), read(expected))


BASE = '#define LIMIT 9\nINT Value(INT x)\n{\n    if (x > LIMIT)\n        return "big";\n    return x + 1;\n}\n'

SAME_CODE = [
    ('a comment added', BASE.replace('{\n', '{\n    /* note */\n', 1)),
    ('a comment changed', BASE.replace('{\n', '{ // first\n', 1).replace('// first', '// second')),
    ('re-indented', BASE.replace('    return x', '        return x')),
    ('a line joined', BASE.replace('{\n    if', '{ if')),
    ('a brace moved', BASE.replace(')\n{', ') {')),
]

CHANGED_CODE = [
    ('an operator', BASE.replace('x + 1', 'x - 1')),
    ('a constant', BASE.replace('x + 1', 'x + 2')),
    ('a string literal', BASE.replace('"big"', '"bog"')),
    ('the space in a string', BASE.replace('"big"', '"b ig"')),
    ('two tokens swapped', BASE.replace('x + 1', '1 + x')),
    ('a token dropped', BASE.replace('return x + 1;', 'return x;')),
    ('a directive joined to code', BASE.replace('9\nINT', '9 INT')),
    ('a macro value', BASE.replace('LIMIT 9', 'LIMIT 8')),
    ('code moved into a comment', BASE.replace('    return x + 1;', '    /* return x + 1; */')),
]


class Proof(unittest.TestCase):

    def test_comments_and_whitespace_are_not_code(self):
        for label, variant in SAME_CODE:
            with self.subTest(label):
                self.assertEqual(restyle.code_fingerprint(BASE), restyle.code_fingerprint(variant))

    def test_every_mutant_is_seen(self):
        for label, variant in CHANGED_CODE:
            with self.subTest(label):
                self.assertNotEqual(variant, BASE, 'the mutant must differ from the base')
                self.assertNotEqual(restyle.code_fingerprint(BASE), restyle.code_fingerprint(variant))


class Refusal(unittest.TestCase):

    SOURCE = '/* r.c -- r. */\n\n#include "r.h"\n\nINT R(VOID) {\n    return 1;\n}\n'

    def test_a_layout_stage_that_changes_code_is_refused(self):
        broken = lambda text: (text.replace('return 1', 'return 2'), 0)
        with mock.patch.object(restyle, 'restyle_braces', broken):
            with self.assertRaises(style.Unproven):
                style.style_text(self.SOURCE, 'r.c')

    def test_an_order_stage_that_changes_code_is_refused(self):
        broken = lambda text: (text.replace('return 1', 'return 2'), [], [])
        with mock.patch.object(order, 'reorder_c', broken):
            with self.assertRaises(style.Unproven):
                style.style_text(self.SOURCE, 'r.c')

    def test_dropping_a_declaration_that_is_not_reported_is_refused(self):
        broken = lambda text: (text.replace('INT R(VOID)', 'VOID R(VOID)'), ['INT'], [])
        with mock.patch.object(order, 'reorder_c', broken):
            with self.assertRaises(style.Unproven):
                style.style_text(self.SOURCE, 'r.c')

    def test_a_refused_file_is_not_written(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, 'r.c')
            with open(path, 'w') as handle:
                handle.write(self.SOURCE)
            broken = lambda text: (text.replace('return 1', 'return 2'), 0)
            with mock.patch.object(restyle, 'restyle_braces', broken), \
                    redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                self.assertEqual(style.main(['--fix', path]), 1)
            self.assertEqual(read(path), self.SOURCE)


class Command(unittest.TestCase):

    def run_main(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = style.main(list(argv))
        return code, out.getvalue()

    def copy_fixture(self, folder, name):
        target = os.path.join(folder, name.replace('.in.', '.').replace('.out.', '.'))
        shutil.copy(os.path.join(FIXTURES, name), target)
        return target

    def test_check_passes_a_file_in_the_house_style(self):
        with tempfile.TemporaryDirectory() as folder:
            path = self.copy_fixture(folder, 'braces.out.c')
            self.assertEqual(self.run_main('--check', path), (0, ''))

    def test_check_fails_with_the_diff_and_writes_nothing(self):
        with tempfile.TemporaryDirectory() as folder:
            path = self.copy_fixture(folder, 'braces.in.c')
            code, diff = self.run_main('--check', path)
            self.assertEqual(code, 1)
            self.assertIn('+    if (value < low)\n', diff)
            self.assertEqual(read(path), read(os.path.join(FIXTURES, 'braces.in.c')))

    def test_fix_writes_the_house_style(self):
        with tempfile.TemporaryDirectory() as folder:
            path = self.copy_fixture(folder, 'braces.in.c')
            self.assertEqual(self.run_main('--fix', path)[0], 0)
            self.assertEqual(read(path), read(os.path.join(FIXTURES, 'braces.out.c')))

    def test_the_default_files_leave_out_the_fixtures(self):
        files = style.tracked_files()
        self.assertTrue(any(p.startswith('src/') for p in files))
        self.assertFalse([p for p in files if p.startswith(style.FIXTURES)])

    def test_a_mode_is_required(self):
        self.assertEqual(self.run_main('x.c')[0], 2)
        self.assertEqual(self.run_main('--check', '--fix')[0], 2)
        self.assertEqual(self.run_main('--chek')[0], 2)


if __name__ == '__main__':
    unittest.main()
