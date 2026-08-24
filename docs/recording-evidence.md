# What the recording proves, measured rather than described

Two checks run against the published film. Both read the file; neither reads
this document. Re-run them and compare.

    ./scripts/check-video.py <film.mp4>
    ./scripts/check-transcript.py recordings/lp8-demo.srt <film.mp4>

The film is a release asset of this repository
([`lp8-demo.mp4`](https://github.com/edenbd1/lp-0008-autonomous-agent-module/releases/download/demo-v1/lp8-demo.mp4))
and is also on YouTube. The transcript beside it is
[`recordings/lp8-demo.srt`](../recordings/lp8-demo.srt) — 298 cues, 2,784 words,
the narration as spoken.

## Measured 2026-08-24

`check-video.py` samples 24 frames across the film, reads them through OCR, and
asserts on the text. The host it requires is read out of `scripts/demo.sh`, not
written into the checker, so it cannot go on passing against an endpoint the
demo stopped using:

```
lp8-demo.mp4
  ok    1317s, 136 MB, video 3024,1964
  ok    audio aac, mean -28.3 dB, peak -5.5 dB
  ok    18 silence(s) over 6s, longest 48s (limit 90s)
  ok    the public testnet is on screen: explorer.testnet.lez.logos.co, testnet.lez.logos.co
  ok    none of 5 forbidden string(s) appears in any frame
  ok    RISC0_DEV_MODE=0 is on screen: the proving is real
  ok    4 illustrative use case(s) run on screen
```

The five forbidden strings are `localhost`, `127.0.0.1`, `0.0.0.0`, `localnet`
and `dev_mode=1`. A film of a local chain fails on the first four; a film of
mocked proving fails on the last.

`check-transcript.py` ties the transcript to that same file:

```
lp8-demo.srt: 298 cue(s), lp8-demo.mp4: 1317.3 s
  ok    structure, and the last cue lands 3.0 s before the end
  ok    r0vm                       spoken at 827s, and on screen there
  ok    settlement transaction     spoken at 1188s, and on screen there
  ok    balance                    spoken at 548s, and on screen there
  ok    explorer                   spoken at 1306s, and on screen there
  transcript matches the film: structure, fit, and 4 anchor(s) tied to the picture.
```

The anchors are the part that matters. The narration is spoken, so its words are
never on screen — but what it *talks about* is, and each anchor requires the
picture to show it while the line is being said. A transcript written from
memory passes every structural check and fails these.

Mutation-tested, because a check nobody has seen fail is not a check. Against
this same film: a transcript truncated to two thirds is caught ("the last cue
ends 577.6 s before the film does"); one cue moved to overlap its neighbour is
caught ("cue 51 starts before cue 50 ends"); and the transcript of a *different*
film of ours is caught the same way as the truncated one.

## What this does not establish

- **Not every frame.** `check-video.py` samples 24; `check-transcript.py` looks
  at up to five frames per anchor. Neither claims the other 32,000 frames.
- **Not the wording.** Nothing here checks the transcript's sentences against
  the audio. Structure, fit and four anchors are what is proved; that the
  narration says what a listener hears is not.
- **An OCR quirk we route around and have not diagnosed.** Tesseract renders `0`
  as `@` in this terminal font, so the prover line reads back as
  `RISC@_DEV_MODE=0 r@vm 1129% cpu`; both checkers allow for it, and
  `check-video.py` normalises `dev_mode=[@oO]` before matching. Separately, on
  the machine these were run on, tesseract returns an *empty string* for images
  written under some temporary directories, silently. `check-transcript.py`
  probes three frames first and refuses to report on the transcript if it reads
  nothing anywhere, because the alternative is a checker blaming a file for a
  tool that could not see. Why tesseract does that has not been diagnosed, and
  is not guessed at here.
