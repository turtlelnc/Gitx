#!/usr/bin/env python3
"""End-to-end test for the gitx AI toolchain against a local mock LLM server.

Usage: python3 tests/ai_smoke.py <path-to-gitx-binary>
Skips (exit 0 with a notice) when the binary reports AI unavailable.
"""
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

GITX = sys.argv[1] if len(sys.argv) > 1 else "gitx"
PORT = 18990
MOCK = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mock_llm.py")


def find_free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def run(args, cwd=None, env=None, input_text=None):
    merged = dict(os.environ)
    merged.setdefault("GITX_AI_KEY", "test-key")
    if env:
        merged.update(env)
    return subprocess.run(
        [GITX] + args,
        cwd=cwd,
        env=merged,
        input=input_text,
        text=True,
        capture_output=True,
        timeout=60,
    )


def main():
    # Skip when AI support is not compiled in.
    version = run(["--version"]).stdout
    if "no AI" in version.lower():
        print("skip: gitx built without AI support")
        return 0

    port = find_free_port()
    mock = subprocess.Popen([sys.executable, MOCK, str(port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(0.5)
        root = tempfile.mkdtemp(prefix="gitx-ai-smoke-")
        repo = os.path.join(root, "repo")
        try:
            r = run(["start", repo])
            if r.returncode != 0:
                print(f"FAIL: gitx start: {r.stderr}")
                return 1

            cfg = os.path.join(repo, ".gitx", "config.toml")
            with open(cfg, "a") as f:
                f.write(f'\n[ai]\nprovider = "custom"\nbase_url = "http://127.0.0.1:{port}/v1"\nmodel = "mock"\n')

            for name, email in [("Tester", "tester@example.com")]:
                subprocess.run(["git", "-C", repo, "config", "user.name", name], check=True)
                subprocess.run(["git", "-C", repo, "config", "user.email", email], check=True)

            with open(os.path.join(repo, "app.js"), "w") as f:
                f.write("function addUser() { return 1; }\n")

            # save --ai: accept the generated message.
            r = run(["save", "--ai"], cwd=repo, input_text="y\n")
            if r.returncode != 0:
                print(f"FAIL: save --ai: {r.stderr}\nstdout: {r.stdout}")
                return 1
            if "已创建提交" not in r.stdout:
                print(f"FAIL: save --ai did not commit:\n{r.stdout}")
                return 1

            # explain: ask about the first commit.
            r = run(["history", "1"], cwd=repo)
            commit = r.stdout.split()[0] if r.stdout.strip() else ""
            r = run(["explain", commit], cwd=repo)
            if r.returncode != 0:
                print(f"FAIL: explain: {r.stderr}")
                return 1
            if "mock" not in r.stdout and "feat(" not in r.stdout:
                print(f"FAIL: explain unexpected output:\n{r.stdout}")

            # review --ai: review a new change.
            with open(os.path.join(repo, "admin.js"), "w") as f:
                f.write("function addAdmin() { return 2; }\n")
            r = run(["review", "--ai"], cwd=repo)
            if r.returncode != 0:
                print(f"FAIL: review --ai: {r.stderr}")
                return 1

            print("ok: AI save/explain/review end-to-end")
            return 0
        finally:
            shutil.rmtree(root, ignore_errors=True)
    finally:
        mock.terminate()
        mock.wait()


if __name__ == "__main__":
    sys.exit(main())
