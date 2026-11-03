# Voice recognition research (work in progress)

Status as of 2026-11-03. Nothing in `src/` has changed yet. This file records what was tested,
so the work can continue from here.

## The problem

JARVIS uses SAPI 5 with **"Microsoft Speech Recognizer 8.0 for Windows (English - US)"**
(`MS-1033-80-DESK`), with free dictation and the command grammar active together. Real
commands from the log (`%LOCALAPPDATA%\JARVIS\jarvis.log`) came out as "and", "i know",
"oakland's cities from", "women on", "open on those". The free-dictation grammar wins and
returns guesses. The command grammar (`cmd_grammar` in `src/commands.c`) only holds verbs plus
`[sites]`/`[apps]`/`[folders]` names, so anything like "find physics notes" can only match
through dictation.

## What this PC has

- SAPI desktop recognizer: `MS-1033-80-DESK` only (en-US).
- OneCore recognizer: `MS-1033-110-WINMO-DNN`, "Microsoft Speech Recognition Engine - en-US
  Embedded DNN v11.1" (`HKLM\SOFTWARE\Microsoft\Speech_OneCore\Recognizers\Tokens`). This is
  the newer neural engine, but it supports grammars only, not standalone dictation.
- Microphone access for desktop apps: Allow.
- Windows "Online speech recognition" privacy setting: **off** (no `HasAccepted` under
  `HKCU\Software\Microsoft\Speech_OneCore\Settings\OnlineSpeechPrivacy`).

## Experiments (probe sources in `tools/sr_probe/`)

| Try | Result |
|---|---|
| Desktop SAPI + DNN token (`SetRecognizer`) | Loads, but `LoadDictation` gives `0x8004503A` and `SetRuleState` gives `0x80045001`. Not usable. |
| OneCore SAPI classes (`SpInprocRecognizer` `{8A44FFB7-8140-432F-BEC5-1B5FF725BAC8}` etc.) + DNN | Context is created, but `GetRule` gives `0x8004503B` and `LoadCmdFromFile` (SRGS) gives `0x8004503A`. Undocumented, so dropped. |
| Desktop SAPI 8.0, grammar "verb + dictation slot" vs. free dictation, on TTS-generated WAVs | Both are near-perfect on clean TTS audio, so TTS audio can't show the difference. A real voice is needed to compare. The grammar version wrote "find physics and oates". |
| WinRT `Windows.Media.SpeechRecognition.SpeechRecognizer`, default (dictation) constraint | Compiles OK; `RecognizeAsync` gives `0x80045509` because the online speech privacy setting is off. MinGW's header has no `SpeechRecognitionTopicConstraint`, but it isn't needed (empty constraint list = dictation). Offline WinRT supports only list/SRGS constraints, which can't capture free words like file names or search terms. |
| whisper.cpp `b5454` `whisper-bin-x64.zip` + `ggml-base.en.bin` (148 MB) | Downloaded, but running it was **blocked** by the Claude Code safety check (code from the internet). Needs the user's OK. |

Build the probes (MinGW):

```bat
gcc -municode -O1 tools\sr_probe\srtest.c -o build\srtest.exe -specs=tools\nomanifest.specs -lole32 -luuid
gcc -municode -O1 -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 tools\sr_probe\rt.c -o build\rt.exe -specs=tools\nomanifest.specs -lole32 -luuid -lruntimeobject
```

## Plan (best option first)

1. **Whisper (offline, the most accurate option).** `tools/get_whisper.ps1` downloads
   whisper.cpp + an English model (`base.en`, or `small.en` for better accents) into
   `%LOCALAPPDATA%\JARVIS\whisper`. JARVIS records the mic (waveIn, 16 kHz mono, energy-based
   end-of-speech), then transcribes with `whisper-server.exe` (model loaded once, kept running
   while JARVIS is open, killed on exit) using `--prompt` filled with command words and app,
   site and folder names to steer it toward commands. Setting: `[voice] engine=auto|whisper|windows|sapi`.
   **Blocked until the user allows running the whisper binaries.**
2. **WinRT online dictation** (`engine=windows`): the same engine as Win+H voice typing. Needs
   Settings → Privacy & security → Speech → **Online speech recognition: On**. Audio goes to
   Microsoft, so this breaks the "offline" promise and must be opt-in.
3. **SAPI fallback, improved**: drop the always-on free dictation and use a grammar of
   verb + dictation slot, plus fixed phrases, plus index/app names; keep free dictation only as
   a low-weight fallback; reject results below a confidence threshold ("Didn't catch that")
   instead of running garbage.

## Other requests still to do

- Stop talking now (Esc while speaking, "stop" / "shut up" / "be quiet", click the reply).
- Mute JARVIS's voice: a toggle that is saved in `jarvis.ini` (`speak=`), "mute jarvis" /
  "unmute jarvis" / "voice off", a speaker button in the bar, Ctrl+M. ("mute" alone already
  means system volume.)
- Option to remove what it says: clear the reply text ("clear"), and a short-replies mode.
- Extra features: timers/reminders, calculator, media play/pause/next, "repeat that",
  shutdown/restart/sleep with yes/no confirmation, quick notes, command history (Up arrow).
- Work on `voice/*` and `feature/*` branches, merge `--no-ff` into `main`, push.
