# Voice recognition: what was tried, and why Whisper

Outcome (2026-11-03): JARVIS now uses **Whisper** (whisper.cpp) when installed, see the README section
"Speech recognition: Whisper". This file keeps the research so nobody has to redo it.

## The problem

JARVIS used SAPI 5 with "Microsoft Speech Recognizer 8.0 for Windows (English - US)" (`MS-1033-80-DESK`),
free dictation and a command grammar active together. Real commands from the log came out as "and",
"i know", "oakland's cities from", "women on", "open on those". The dictation model is from 2006-era
Windows and copes badly with accents and rooms; the command grammar only held verbs plus `[sites]` /
`[apps]` / `[folders]` names, so "find physics notes" could only match through dictation.

## What's on a Windows 11 PC

- SAPI desktop recognizer: `MS-1033-80-DESK` only (en-US).
- OneCore recognizer: `MS-1033-110-WINMO-DNN`, "Microsoft Speech Recognition Engine - en-US Embedded DNN
  v11.1" (`HKLM\SOFTWARE\Microsoft\Speech_OneCore\Recognizers\Tokens`). The newer neural engine, used
  by Win+H and Cortana. Grammar only, no free dictation offline.
- Windows "Online speech recognition" (Settings → Privacy → Speech) was **off**; with it on, WinRT
  dictation goes to Microsoft's servers.

## Experiments (probe programs in `tools/sr_probe/`)

| Try | Result |
|---|---|
| Desktop SAPI + the DNN token (`ISpRecognizer::SetRecognizer`) | Loads, but `LoadDictation` → `0x8004503A`, `SetRuleState` → `0x80045001`. Unusable. |
| OneCore SAPI classes (`sapi_onecore.dll`: `SpInprocRecognizer` `{8A44FFB7-8140-432F-BEC5-1B5FF725BAC8}` etc.) + DNN | Context is created, but `GetRule` → `0x8004503B` and `LoadCmdFromFile` (SRGS) → `0x8004503A`. Undocumented API, dropped. |
| Desktop SAPI 8.0 with a "verb + dictation slot" grammar vs. plain dictation, on TTS-generated WAVs | Both near-perfect on clean synthetic audio (useless as a benchmark); the grammar variant still produced "find physics and oates". |
| WinRT `Windows.Media.SpeechRecognition.SpeechRecognizer`, default dictation constraint | Compiles; `RecognizeAsync` → `0x80045509` (online speech recognition off). Offline it only supports list / SRGS constraints, which can't take free words (file names, search terms). |
| **whisper.cpp b5454 `whisper-bin-x64.zip`** (CPU build) | `base.en`: 6/6 test phrases right, ~0.7 s per command on a 12-thread laptop. `small.en-q5_1`: 6/6, ~0.9 s with the audio context sized to the clip (`-ac`), 1.6 s without. Live mic test through JARVIS: "what time is it" heard and run. |

Build the probes (MinGW):

```bat
gcc -municode -O1 tools\sr_probe\srtest.c -o build\srtest.exe -specs=tools\nomanifest.specs -lole32 -luuid
gcc -municode -O1 -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 tools\sr_probe\rt.c -o build\rt.exe -specs=tools\nomanifest.specs -lole32 -luuid -lruntimeobject
```

## Whisper notes

- Pinned downloads (SHA-256 in `tools/get_whisper.ps1`): `whisper-bin-x64.zip` from the ggml-org
  GitHub release b5454 (hash matches GitHub's published digest), models from
  `huggingface.co/ggerganov/whisper.cpp` (hashes match the repo's LFS metadata).
- Only `whisper-cli.exe`, `whisper.dll` and `ggml*.dll` are installed (the zip also carries
  `whisper-server`, `stream`, SDL2, llama, parakeet; not needed).
- `-ac` (audio context) sized to the clip: `(seconds * 50 + 128)` rounded up to 64, min 384. At 256 the
  model started repeating text ("convert my resume to pdf convert my resume to pdf"), so 384 is the floor.
- `--prompt` with example commands + the ini names steers spelling ("pdf", "vs code", "gmail").
- `-sns` (suppress non-speech tokens) and dropping `[...]` / `(...)` tags, plus the classic
  silence hallucinations ("Thank you.", "you"), in `whisper_clean`.
- `whisper-server` (model kept loaded) was ~0.7 s vs ~0.9 s for the CLI with `-ac`: not worth a
  resident 400 MB process for a launcher that should use nothing while idle.
- Future: whisper.cpp's own Silero VAD (`--vad`) could replace the energy VAD; a multilingual model
  with `language=auto` works through the same settings.
