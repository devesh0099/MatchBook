#!/usr/bin/env python3
"""Screenshot every view in both themes, so the design can be checked by eye.

Local development only.

    python3 design/shots.py [outdir]              # npm run start on :3000
    BASE=https://localhost:8443 python3 design/shots.py [outdir]   # compose

Certificate errors are ignored: Caddy issues its own for `localhost`, so a
compose dry run is always self-signed.
"""
import os, sys, pathlib
from playwright.sync_api import sync_playwright

BASE = os.environ.get("BASE", "http://localhost:3000")
OUT = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "/tmp/shots")
OUT.mkdir(parents=True, exist_ok=True)

# There is no login: identity is a roster pick in localStorage, so a headless
# browser has to seed it the same way the picker does.
IDENTITY = '{"id":1,"handle":"a.mehra"}'

VIEWS = [
    ("editor", "/editor", False),
    ("submissions", "/submissions", True),
    ("sub-compile-failed", "/submissions/1", True),
    ("sub-diverged", "/submissions/2", True),
    ("sub-timeout", "/submissions/3", True),
    ("sub-held", "/submissions/4", True),
    ("sub-benchmarking", "/submissions/7", True),
    ("sub-done", "/submissions/8", True),
    ("leaderboard", "/leaderboard", True),
    ("spec", "/spec", False),
]

with sync_playwright() as pw:
    # The system Chrome, rather than downloading a pinned build. This is a
    # local dev convenience, not part of any pipeline, so matching Playwright's
    # bundled revision buys nothing.
    browser = pw.chromium.launch(executable_path="/usr/bin/google-chrome")
    try:
        for theme in ("light", "dark"):
            ctx = browser.new_context(
                viewport={"width": 1440, "height": 900}, ignore_https_errors=True
            )
            ctx.add_init_script(
                f"localStorage.setItem('mebench.participant', '{IDENTITY}');"
                f"localStorage.setItem('mebench.theme', '{theme}');"
            )
            page = ctx.new_page()
            for name, path, full in VIEWS:
                page.goto(BASE + path, wait_until="networkidle")
                page.wait_for_timeout(1200)  # Monaco and recharts settle late
                page.screenshot(path=str(OUT / f"{theme}-{name}.png"), full_page=full)
                print(f"{theme}-{name}.png")
            ctx.close()
    finally:
        browser.close()
print(f"\nwrote to {OUT}")
