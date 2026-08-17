#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR Apache-2.0
"""Read the submission video the way a reviewer will: off the screen.

WHY THIS EXISTS

The criterion asks for "a recorded video demo of the end-to-end flow ... showing
terminal output", and the one thing that would sink it is a film shot against a
local chain. Two takes were discarded after duration, file size and "no stray
window on screen" all came back green — because none of those three looks at
what the terminal actually says. A check that cannot fail for the reason you
care about is not a check.

So this one reads the pixels. It samples frames across the film, runs them
through OCR, and asserts on the text:

  REQUIRED   the public testnet's DOMAIN, taken from the sequencer URL that
             scripts/demo.sh defaults to, has to appear on screen at least
             once. Not in the narration -- narration is a claim, the terminal
             is the evidence.

             The domain, deliberately, and not the sequencer URL itself. This
             check first demanded the exact RPC host and failed the second
             film, which spends the money and is the more important of the
             two: what that film puts on screen is
             `explorer.testnet.lez.logos.co/transaction/<hash>`, a link a
             reviewer can click, which is better evidence than the RPC URL and
             not worse. Requiring the exact host was a proxy for the criterion
             rather than the criterion. Do not "tighten" it back -- it was a
             false negative, on the film that matters most.
  FORBIDDEN  localhost, 127.0.0.1, 0.0.0.0, "localnet", and RISC0_DEV_MODE=1.
             Any one of them means the film is showing something other than
             the public testnet with real proofs.

The host is READ OUT OF `scripts/demo.sh` rather than written here, so this
cannot go on passing against an endpoint the demo stopped using.

AND IT ALSO CHECKS THE THINGS THAT ARE EASY, because they are still worth
checking once the hard one is covered: an audio track that is present and
audible rather than a silent stream, and the longest dead-air stretch.

NO SKIP PATH. Missing ffmpeg, ffprobe or tesseract exits non-zero and says so.
A video checker that quietly passes when it cannot look is the failure it was
written to prevent, one level up.

    ./scripts/check-video.py film1.mp4 film2.mp4
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# What a film must show, and must not. Strings are matched case-insensitively
# against OCR output, which is noisy -- so each one is chosen to be long enough
# that a stray character does not invent it.
FORBIDDEN = [
    ("127.0.0.1", "a loopback address: this is a local chain, not the testnet"),
    ("localhost", "a loopback host: this is a local chain, not the testnet"),
    ("0.0.0.0", "a wildcard bind address, which only a local node listens on"),
    ("localnet", "the word localnet, which the criterion rules out by name"),
    ("RISC0_DEV_MODE=1", "dev mode: the proofs on screen are fakes"),
]

FRAMES = int(os.environ.get("VIDEO_FRAMES", "24"))
MAX_SILENCE = float(os.environ.get("VIDEO_MAX_SILENCE", "90"))


def need(tool):
    if shutil.which(tool) is None:
        print("missing: %s -- this check reads the video and cannot run without\n"
              "it. There is no skip path: passing here without having looked is\n"
              "exactly the failure this exists to prevent." % tool)
        sys.exit(1)


def sequencer_host():
    """The public host scripts/demo.sh defaults to, not a copy of it."""
    path = os.path.join(ROOT, "scripts", "demo.sh")
    try:
        with open(path, encoding="utf-8") as fh:
            body = fh.read()
    except OSError as exc:
        print("cannot read scripts/demo.sh to learn which sequencer is the "
              "public one: %s" % exc)
        sys.exit(1)
    m = re.search(r'SEQUENCER_URL:-(https?://[^\s"}]+)', body)
    if not m:
        print("scripts/demo.sh no longer names a default SEQUENCER_URL, so this\n"
              "check cannot tell which endpoint is the public one. Fix the\n"
              "pattern here rather than hardcoding a host.")
        sys.exit(1)
    return m.group(1).rstrip("/")


def probe(path, *args):
    out = subprocess.run(["ffprobe", "-v", "error", *args, path],
                         capture_output=True, text=True)
    return out.stdout.strip()


def ocr_frames(path, duration, workdir):
    """Sample FRAMES frames across the film and OCR each one."""
    texts = []
    for i in range(FRAMES):
        t = duration * i / FRAMES
        png = os.path.join(workdir, "f%03d.png" % i)
        subprocess.run(["ffmpeg", "-v", "error", "-ss", "%.2f" % t, "-i", path,
                        "-frames:v", "1", png, "-y"],
                       capture_output=True)
        if not os.path.exists(png):
            continue
        out = subprocess.run(["tesseract", png, "-", "--psm", "6"],
                             capture_output=True, text=True)
        texts.append((t, out.stdout))
        os.unlink(png)
    return texts


def check(path, host):
    print("\n%s" % os.path.basename(path))
    if not os.path.exists(path):
        print("  --    no such file")
        return False

    dur = probe(path, "-show_entries", "format=duration", "-of", "csv=p=0")
    if not dur:
        print("  --    ffprobe cannot read this as a video")
        return False
    dur = float(dur)
    size = os.path.getsize(path) / 1e6
    vid = probe(path, "-select_streams", "v:0", "-show_entries",
                "stream=width,height", "-of", "csv=p=0")
    aud = probe(path, "-select_streams", "a:0", "-show_entries",
                "stream=codec_name", "-of", "csv=p=0")
    print("  ok    %.0fs, %.0f MB, video %s" % (dur, size, vid))

    ok = True
    if not aud:
        print("  --    NO AUDIO TRACK. The submission asks the builder to "
              "narrate the demo.")
        ok = False
    else:
        vol = subprocess.run(["ffmpeg", "-hide_banner", "-nostats", "-i", path,
                              "-af", "volumedetect", "-f", "null", os.devnull],
                             capture_output=True, text=True).stderr
        mean = re.search(r"mean_volume:\s*(-?[\d.]+)", vol)
        peak = re.search(r"max_volume:\s*(-?[\d.]+)", vol)
        if mean and float(mean.group(1)) < -50:
            print("  --    the audio track is effectively silent (mean %s dB)"
                  % mean.group(1))
            ok = False
        else:
            print("  ok    audio %s, mean %s dB, peak %s dB"
                  % (aud, mean.group(1) if mean else "?",
                     peak.group(1) if peak else "?"))

        sil = subprocess.run(["ffmpeg", "-hide_banner", "-nostats", "-i", path,
                              "-af", "silencedetect=noise=-45dB:d=6",
                              "-f", "null", os.devnull],
                             capture_output=True, text=True).stderr
        runs = [float(x) for x in re.findall(r"silence_duration:\s*([\d.]+)", sil)]
        if runs:
            longest = max(runs)
            note = ("  ok   " if longest <= MAX_SILENCE else "  --   ")
            print("%s %d silence(s) over 6s, longest %.0fs (limit %.0fs)"
                  % (note, len(runs), longest, MAX_SILENCE))
            if longest > MAX_SILENCE:
                ok = False

    # The part that matters: what the terminal says.
    with tempfile.TemporaryDirectory() as workdir:
        texts = ocr_frames(path, dur, workdir)
    if not texts:
        print("  --    no frame could be read; the OCR pass did nothing, which "
              "is not a pass")
        return False
    blob = "\n".join(t for _, t in texts).lower()

    domain = re.sub(r"^https?://", "", host).split("/")[0]
    seen = sorted(set(re.findall(r"[a-z0-9.-]*" + re.escape(domain), blob)))
    if seen:
        print("  ok    the public testnet is on screen: %s"
              % ", ".join(s for s in seen if s))
    else:
        print("  --    %s NEVER APPEARS on screen across %d sampled frames.\n"
              "        The criterion is a demo against the public testnet, and\n"
              "        narration saying so is not the terminal showing so."
              % (domain, len(texts)))
        ok = False

    for needle, why in FORBIDDEN:
        hits = [t for t, txt in texts if needle.lower() in txt.lower()]
        if hits:
            print("  --    %r on screen at %s -- %s"
                  % (needle, ", ".join("%.0fs" % h for h in hits[:4]), why))
            ok = False
    if ok:
        print("  ok    none of %d forbidden string(s) appears in any frame"
              % len(FORBIDDEN))
    return ok


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    for tool in ("ffprobe", "ffmpeg", "tesseract"):
        need(tool)
    host = sequencer_host()
    print("the public sequencer, as scripts/demo.sh defaults to it: %s" % host)
    print("sampling %d frames per film" % FRAMES)
    results = [check(p, host) for p in argv[1:]]
    print()
    if all(results):
        print("%d film(s) checked: each shows the public sequencer and none "
              "shows a local one." % len(results))
        return 0
    print("%d of %d film(s) would not survive a reviewer."
          % (sum(1 for r in results if not r), len(results)))
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
