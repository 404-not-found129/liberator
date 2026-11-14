#!/usr/bin/env python3
# The owner's answer about start reports from macOS 27: asked by the app, passed by setup, written onto the macOS 27
# data volume by the Golden Gate script, and taken back by a later "no". The function is run as it stands in the
# script, against a made-up volume.
import pathlib, re, subprocess, sys, tempfile

root = pathlib.Path(__file__).resolve().parents[2]
gg = (root / "mac/Resources/nullmoth-golden-gate.sh").read_text()
setup = (root / "mac/Resources/nullmoth-setup.sh").read_text()
main = (root / "mac/Sources/main.swift").read_text()
failed = 0

def arm(name, ok, detail=""):
    global failed
    print(("ok   " if ok else "FAIL ") + name + (f" ({detail})" if detail and not ok else ""))
    failed += 0 if ok else 1

start = gg.index("report_choice() {")
body = gg[start:gg.index("\n}\n", start) + 3]

def run(report, tag, pre=()):
    with tempfile.TemporaryDirectory() as d:
        vol = pathlib.Path(d) / "vol"; rep = vol / "Library/NullMoth/report"
        for f in pre:
            rep.mkdir(parents=True, exist_ok=True); (rep / f).write_text("x\n")
        script = 'ok() { echo "OK $*"; }; note() { echo "NOTE $*"; }\n' + body + f'\nREPORT="$1"; KITTAG="$2"; report_choice "$3"\n'
        out = subprocess.run(["/bin/bash", "-c", script, "x", report, tag, str(vol)], capture_output=True, text=True)
        have = {p.name: p.read_text() for p in rep.iterdir()} if rep.is_dir() else {}
        return out.returncode, out.stdout, have

rc, out, have = run("yes", "v3.0.0-beta.3")
arm("a yes writes the consent file and the kit's release tag", rc == 0 and "consent" in have and have.get("kit-tag") == "v3.0.0-beta.3\n", str(have))
arm("and says so", "start reports from macOS 27: on" in out, out)
rc, out, have = run("no", "v3.0.0-beta.3")
arm("a no writes no consent file", rc == 0 and "consent" not in have, str(have))
rc, out, have = run("", "v3.0.0-beta.3")
arm("no answer at all is a no", rc == 0 and "consent" not in have, str(have))
rc, out, have = run("no", "v3.0.0-beta.3", pre=("consent",))
arm("a later no takes an earlier yes back", rc == 0 and "consent" not in have and "off" in out, str(have))
rc, out, have = run("yes", "v3; rm -rf /")
arm("a tag that is not a plain release name is not written", rc == 0 and "consent" in have and "kit-tag" not in have, str(have))
rc, out, have = run("YES", "v3.0.0-beta.3")
arm("control: only the exact word yes turns reports on", "consent" not in have, str(have))

arm("the script takes --report and --kit-tag", "--report) REPORT=${2:-}; shift;; --kit-tag) KITTAG=${2:-}; shift;;" in gg)
arm("the choice is written right after the system package is installed",
    re.search(r'\$sinst" --payload "\$SPAY" --system "\$SYS" --data "\$DATA" --esp "\$ROOT" --apply \|\| stop [^\n]*\n  report_choice "\$DATA"\n', gg) is not None)
arm("setup passes the answer on the finish step only, with the tag from the kit pin",
    '$([ "$1" = finish ] && echo "--report $([ -n "${GGREPORT:-}" ] && echo yes || echo no) --kit-tag ' in setup and "--report-consent) GGREPORT=1;;" in setup)
ask = main.find('r.messageText = "Tell NullMoth how macOS 27 starts on this PC?"')
arm("the app asks before the one-button run and sends the option only on Send",
    ask > 0 and 'if r.runModal() == .alertFirstButtonReturn { extra.append("--report-consent") }' in main[ask:ask + 1500]
    and 'r.addButton(withTitle: "Send"); r.addButton(withTitle: "Don\'t send")' in main[ask:ask + 1500])
print(f"{failed} failed"); sys.exit(1 if failed else 0)
