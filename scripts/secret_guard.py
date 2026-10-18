#!/usr/bin/env python3
"""
Pre-commit guard: refuse a commit whose STAGED diff carries a credential-shaped secret.

WHY (2026-10-18). This repository is PUBLIC. The Commander's access code and the brains'
PINs live in the private MayDay-AI repo and are deliberately absent here, but a header
pasted into a doc, a curl example, or a test fixture would publish them in one keystroke.
A guard that only exists in the other repo protects nothing here.

WHAT IT LOOKS FOR (staged, added lines only; text files only):
    X-Commander-Code: 123        "X-Commander-Code": "123"
    X-Pin: 1234                  pin=1234        commander_code = 123
Placeholders are allowed: <code>, YOUR_PIN, ***, ..., xxxx, or any value containing
letters other than a digit. Anything that looks like a real numeric credential blocks.

BYPASS: `git commit --no-verify` still works, by git's design. Say why when you use it.

Install (once per clone):
    python scripts/secret_guard.py --install
Self-test:
    python scripts/secret_guard.py --selftest
"""

import argparse
import os
import re
import subprocess
import sys

# Header / assignment shapes that carry a literal value. A C++ call like
# http.addHeader("X-Commander-Code", s_code) has a comma, not : or =, so it does not match.
PATTERNS = [
    ("access code header", re.compile(r"X-Commander-Code[\"']?\s*[:=]\s*[\"']?([0-9]{2,})", re.I)),
    ("PIN header",         re.compile(r"X-Pin[\"']?\s*[:=]\s*[\"']?([0-9]{3,})", re.I)),
    ("pin assignment",     re.compile(r"\bpin\b\s*[:=]\s*[\"']?([0-9]{3,})", re.I)),
    ("commander code var", re.compile(r"\bcommander_code\b\s*[:=]\s*[\"']?([0-9]{2,})", re.I)),
]

PLACEHOLDER = re.compile(r"^(<.*>|\*+|\.+|x+|your\w*|\$\{.*\}|%.*%)$", re.I)


# This file's own self-test corpus is made of credential-shaped examples, so scanning it
# would make the guard block every commit that touches the guard. It is exempt by path -
# an allowlist of exactly one file, named here so it cannot grow quietly.
SELF_PATH = "scripts/secret_guard.py"


def scan_text(text, skip_paths=(SELF_PATH,)):
    """(pattern label, redacted line) for every added line that carries a credential."""
    found = []
    current = ""
    for line in text.splitlines():
        if line.startswith("+++ b/"):
            current = line[6:].strip().replace("\\", "/")
            continue
        if any(current.endswith(p) for p in skip_paths):
            continue
        if not line.startswith("+") or line.startswith("+++"):
            continue
        body = line[1:]
        for label, pattern in PATTERNS:
            match = pattern.search(body)
            if not match:
                continue
            value = match.group(1)
            if PLACEHOLDER.match(value):
                continue
            redacted = body[:match.start(1)] + "[REDACTED]" + body[match.end(1):]
            found.append((label, redacted.strip()[:120]))
    return found


def staged_diff():
    return subprocess.run(["git", "diff", "--cached", "--unified=0", "--no-color"],
                          capture_output=True, text=True, errors="replace").stdout or ""


def check():
    hits = scan_text(staged_diff())
    if not hits:
        return 0
    print("secret_guard: refusing this commit - a staged line looks like a credential:")
    for label, line in hits:
        print(f"  {label}: {line}")
    print("\nThe code and PINs belong in the private MayDay-AI repo, never here.")
    print("Remove the value (or use a placeholder like <code>), then commit again.")
    print("Real emergency? `git commit --no-verify` - and say why in the message.")
    return 1


def install():
    hook_dir = subprocess.run(["git", "rev-parse", "--git-path", "hooks"],
                              capture_output=True, text=True).stdout.strip()
    if not hook_dir:
        print("not a git repository")
        return 1
    os.makedirs(hook_dir, exist_ok=True)
    path = os.path.join(hook_dir, "pre-commit")
    script = os.path.abspath(__file__).replace("\\", "/")
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("#!/bin/sh\n"
                     "# Installed by scripts/secret_guard.py --install (2026-10-18).\n"
                     f'python "{script}" || exit 1\n')
    if os.name != "nt":
        os.chmod(path, 0o755)
    print(f"installed {path}")
    print("Windows ignores the shebang but git runs hooks through sh, so this works here.")
    return 0


def _selftest():
    cases = [
        ("header with a literal code", "X-Commander-Code: 123", True),
        ("quoted json header", '"X-Commander-Code": "123"', True),
        ("pin header", "X-Pin: 4321", True),
        ("pin assignment", "pin=4821", True),
        ("commander_code var", "commander_code = 123", True),
        ("placeholder angle", "X-Commander-Code: <code>", False),
        ("placeholder words", "X-Pin: YOUR_PIN", False),
        ("placeholder stars", "X-Commander-Code: ***", False),
        ("cpp addHeader call", 'http.addHeader("X-Commander-Code", s_code);', False),
        ("prose mentioning it", "send the X-Commander-Code header on each request", False),
    ]
    fails = 0
    for name, line, want in cases:
        got = bool(scan_text("+" + line))
        ok = got == want
        fails += 0 if ok else 1
        print(f"  {'ok  ' if ok else 'FAIL'} {name:<28} blocked={got} (want {want})")
    print(f"SELFTEST {'OK' if not fails else 'FAILED'} ({len(cases)} checks)")
    return 1 if fails else 0


def main(argv=None):
    parser = argparse.ArgumentParser(description="Refuse commits that stage a credential")
    parser.add_argument("--install", action="store_true", help="write .git/hooks/pre-commit")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)
    if args.selftest:
        return _selftest()
    if args.install:
        return install()
    return check()


if __name__ == "__main__":
    raise SystemExit(main())
