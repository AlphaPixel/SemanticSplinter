#!/usr/bin/env python3
"""Regenerates the golden corpus from the reference tools (PLAN.md Phase 2, text.md section 11).

Inputs are generated deterministically from fixed seeds; outputs come from GNU diffutils (diff, diff3) and
Git (git merge-file) found on PATH. Run it only to extend the corpus or after a deliberate change of oracle:

    python tests/golden/generate.py            # writes tests/golden/*.json

The checked-in files were produced with GNU diffutils 3.12 and Git 2.4x. The tools' outputs are byte
streams with LF line ends on every platform, so the corpus is platform independent. Labels are always
given, so no timestamps appear.
"""
import json
import os
import random
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

DIFF_OPTIONS = [
    [], ['-u'], ['-c'], ['-U1'], ['-C0'], ['-U0'], ['-u', '-U5'], ['-e'], ['-f'], ['-n'], ['-y'],
    ['-y', '-W', '60'], ['-y', '-t', '-W', '50'], ['-y', '--left-column', '-W', '40'],
    ['-y', '--suppress-common-lines'], ['-b', '-u'], ['-w'], ['-B'], ['-B', '-u'], ['-uB', '-U1'],
    ['-i', '-u'], ['-E'], ['-Z', '-u'], ['-p', '-u'], ['-F', '^[a-z]', '-c'], ['-I', '^#', '-u'],
    ['-T', '-u'], ['-t', '-c'], ['--suppress-blank-empty', '-u'], ['-d', '-u'], ['-q'], ['-s'],
    ['--strip-trailing-cr', '-u'], ['--horizon-lines=2', '-U0'], ['-wB', '-c'],
]
DIFF3_OPTIONS = [
    ['-m'], ['-m', '-E'], ['-m', '-e'], ['-m', '-x'], ['-m', '-3'], [], ['-e'], ['-E'], ['-A'], ['-x'],
    ['-3'], ['-T'], ['-A', '-i'],
]
GIT_OPTIONS = [[], ['--diff3'], ['--zdiff3'], ['--ours'], ['--theirs'], ['--union'], ['--marker-size=3']]

VOCAB = ['a', 'b', 'c', '{', '}', '', '  x', 'x  ', '\tx', 'int f()', 'return 0;', 'b b', 'B', '#c', ' ',
         'foo  bar', 'Foo Bar', 'x\r']
WORDS = 'the quick brown fox jumps over lazy dog merge conflict line text file change'.split()


def random_lines(r):
    kind = r.randrange(3)
    n = r.randint(0, 30)
    if kind == 0:
        return [r.choice(VOCAB) if r.random() < 0.7 else 'L%d' % r.randint(0, 40) for _ in range(n)]
    if kind == 1:  # source-like
        out, depth = [], 0
        for _ in range(n):
            t = r.random()
            if t < 0.15:
                out.append('    ' * depth + 'if (x%d) {' % r.randint(0, 9))
                depth += 1
            elif t < 0.3 and depth:
                depth -= 1
                out.append('    ' * depth + '}')
            elif t < 0.4:
                out.append('')
            else:
                out.append('    ' * depth + 'v%d = f(%d);' % (r.randint(0, 9), r.randint(0, 3)))
        return out
    return [' '.join(r.choice(WORDS) for _ in range(r.randint(0, 6))) for _ in range(n)]


def mutate(r, lines):
    out = list(lines)
    for _ in range(r.randint(0, 6)):
        op = r.random()
        if op < 0.3 and out:
            del out[r.randrange(len(out))]
        elif op < 0.55:
            out.insert(r.randint(0, len(out)), r.choice(['a', 'b', '', 'new', '  x', '}', '#c', 'L%d' % r.randint(0, 40)]))
        elif op < 0.7 and len(out) > 2:  # move a block
            i = r.randrange(len(out) - 1)
            block = out[i:i + 2]
            del out[i:i + 2]
            j = r.randint(0, len(out))
            out[j:j] = block
        elif out:
            i = r.randrange(len(out))
            out[i] = out[i] + r.choice(['', ' ', 'z', '\t', '  '])
    return out


def text(lines, final_newline):
    s = '\n'.join(lines)
    return s + '\n' if lines and final_newline else s


FIXED_PAIRS = [
    ('', ''), ('', 'a\n'), ('a\n', ''), ('a', 'a\n'), ('a\n', 'a'), ('a\nb\nc\n', 'a\nb\nb\nc\n'),
    ('x\na\n', 'z\nx\na\na\n'), ('a\r\nb\r\n', 'a\r\nc\r\n'), ('a\rb\rc\n', 'a\rB\rc\n'),
    ('same\n' * 20, 'same\n' * 19 + 'diff\n'), ('a\nb\nc\nd\n', 'd\nc\nb\na\n'),
    ('.\nx\n', 'y\n.\n.\n'), ('﻿bom\n', 'bom\n'),
]


def run(cmd, cwd):
    p = subprocess.run(cmd, cwd=cwd, capture_output=True)
    return p.stdout.decode('latin-1'), p.returncode


def write(path, content):
    with open(path, 'wb') as f:
        f.write(content.encode('latin-1'))


def main():
    for tool in (['diff', '--version'], ['diff3', '--version'], ['git', '--version']):
        subprocess.run(tool, check=True, capture_output=True)
    with tempfile.TemporaryDirectory() as tmp:
        # Two-way.
        r = random.Random(20261009)
        pairs = [(a.encode('utf-8').decode('latin-1'), b.encode('utf-8').decode('latin-1')) for a, b in FIXED_PAIRS]
        for _ in range(45):
            a = random_lines(r)
            pairs.append((text(a, r.random() < 0.9), text(mutate(r, a), r.random() < 0.9)))
        cases = []
        for a, b in pairs:
            write(os.path.join(tmp, 'a'), a)
            write(os.path.join(tmp, 'b'), b)
            runs = []
            for opts in DIFF_OPTIONS:
                out, rc = run(['diff'] + opts + ['--label', 'A', '--label', 'B', 'a', 'b'], tmp)
                runs.append({'args': opts, 'stdout': out, 'rc': rc})
            cases.append({'files': {'a': a, 'b': b}, 'runs': runs})
        dump('diff.json', cases)

        # Three-way.
        r = random.Random(20261010)
        triples = []
        for _ in range(40):
            o = random_lines(r)
            triples.append((text(o, True), text(mutate(r, o), r.random() < 0.9), text(mutate(r, o), r.random() < 0.9)))
        d3, git = [], []
        for o, m, y in triples:
            for name, content in (('older', o), ('mine', m), ('yours', y)):
                write(os.path.join(tmp, name), content)
            runs = []
            for opts in DIFF3_OPTIONS:
                out, rc = run(['diff3'] + opts + ['-L', 'M', '-L', 'O', '-L', 'Y', 'mine', 'older', 'yours'], tmp)
                runs.append({'args': opts, 'stdout': out, 'rc': rc})
            d3.append({'files': {'mine': m, 'older': o, 'yours': y}, 'runs': runs})
            runs = []
            for opts in GIT_OPTIONS:
                out, rc = run(['git', 'merge-file', '-p'] + opts + ['-L', 'M', '-L', 'O', '-L', 'Y', 'mine', 'older', 'yours'], tmp)
                runs.append({'args': opts, 'stdout': out, 'rc': rc})
            git.append({'files': {'mine': m, 'older': o, 'yours': y}, 'runs': runs})
        dump('diff3.json', d3)
        dump('merge-file.json', git)


def dump(name, cases):
    with open(os.path.join(HERE, name), 'w', encoding='utf-8', newline='\n') as f:
        json.dump(cases, f, ensure_ascii=True, indent=0)
        f.write('\n')
    print(name, sum(len(c['runs']) for c in cases), 'runs', file=sys.stderr)


if __name__ == '__main__':
    main()
