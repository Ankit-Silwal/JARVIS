# JARVIS

A tiny, **offline** voice launcher for Windows, written in plain C (Win32).
Press the **Copilot key**, say what you want, and JARVIS does it.

- **Open folders and files** by name: "open downloads", "open my resume"
- **Find files you lost** with fuzzy search: "find physics notes", then say "two"
- **Newest stuff fast**: "open latest photo", "open latest download", "open newest pdf in documents"
- **Fill Upload / Open dialogs** (Google Drive in Chrome, Gmail, Office...): "upload resume"
- **Web**: open Chrome, YouTube, Gmail, Drive, Classroom; Google / YouTube / Wikipedia searches; directions
- **Apps**: VS Code, Word, or any app in your Start Menu
- **Little things**: time, date, battery, volume, lock, screenshot
- **Works offline**: uses the speech recognizer and voices built into Windows. Nothing leaves your PC.
- **Small**: one `.exe` + one `jarvis.ini`. Nothing runs in the background when it's closed.

---

## Quick start

**You need:** Windows 10/11 and [MinGW-w64](https://www.mingw-w64.org/) (`gcc` and `windres` on your PATH).

1. Build:
   ```bat
   build.bat
   ```
   This makes `build\jarvis.exe`.
2. Put it somewhere permanent, for example `%LOCALAPPDATA%\Programs\JARVIS\`.
   Or run `tools\install.ps1` (copies the exe and creates a Start Menu shortcut).
3. Run `jarvis.exe` once. It creates `jarvis.ini` (your settings) next to the exe.
4. Set up the Copilot key (below).

## Copilot key setup (short version)

1. Install PowerToys: `winget install Microsoft.PowerToys`
2. PowerToys → **Keyboard Manager** → **Remap a shortcut** → press the Copilot key.
3. Action: **Run Program**, pick `jarvis.exe`, "If running": **Start another**.

Full step-by-step guide: **[docs/COPILOT_KEY.md](docs/COPILOT_KEY.md)**

Press the key once → JARVIS opens and listens. Press it again → mic on/off.

---

## What you can say

Talk after the mic dot turns blue. Short commands work best.

### Files and folders

| Say | What happens |
|---|---|
| "open downloads" / "open documents" / "open desktop" | Opens that folder (also pictures, videos, music, onedrive, this pc, recycle bin...) |
| "open my resume" | Finds the best match for "resume" and opens it |
| "open latest photo" | Opens your newest picture |
| "open latest download" | Opens the newest file in Downloads |
| "open newest pdf in documents" | Newest PDF inside Documents |
| "find physics notes" | Shows a numbered list of matches. Say **"two"** (or "number two") to open #2 |
| "show resume in folder" | Opens Explorer with the file selected |

### Upload / Open dialogs

Open the Upload window first (for example "Upload file" on Google Drive in Chrome), **then** press the Copilot key.

| Say | What happens |
|---|---|
| "upload resume" | Finds your resume and puts it into the Upload window |
| "go to downloads" | Moves the Upload window to the Downloads folder |

### Web

| Say | What happens |
|---|---|
| "open chrome" | Opens Chrome |
| "open youtube" / "open gmail" / "open drive" / "open classroom" | Opens that site (more in `jarvis.ini`) |
| "google black holes" | Google search |
| "youtube lofi music" | YouTube search |
| "wikipedia nepal" | Wikipedia search |
| "directions to kathmandu airport" | Google Maps directions |

### Apps and system

| Say | What happens |
|---|---|
| "open vs code" / "open word" | Opens the app |
| "open spotify" (any app name) | Finds it in your Start Menu and opens it |
| "what time is it" / "what's the date" / "battery" | JARVIS tells you |
| "volume up" / "volume down" / "mute" | Changes the volume |
| "lock" | Locks the PC |
| "screenshot" | Takes a screenshot |
| "reindex" | Re-scans your files now |
| "help" | Shows example commands |
| "cancel" | Closes JARVIS |

### Keyboard

You can also just type. Results show up live while you type.

| Key | Does |
|---|---|
| Enter | Open the selected result |
| Shift + Enter | Show it in its folder |
| Up / Down | Move through results |
| F2, Ctrl + Space, or the Copilot key again | Mic on / off |
| Esc | Close |

---

## Customize (`jarvis.ini`)

`jarvis.ini` sits next to `jarvis.exe`. Open it in Notepad. Changes apply the next time JARVIS opens.
Names on the left are what you **say** (lowercase). Delete the file to get the defaults back.

| Section | What it's for |
|---|---|
| `[general]` | `mode` (normal / background), `speak`, `listen_on_start`, `voice` (David / Zira), `voice_rate` (-10..10), `close_delay_ms`, `browser`, `index_max_age_min` |
| `[search]` | `roots` = extra folders to index (separate with `;`), `skip` = folder names never scanned |
| `[sites]` | spoken name = website |
| `[apps]` | spoken name = program, `ms-settings:` link, etc. |
| `[folders]` | spoken name = folder |

Examples - add your own:

```ini
[search]
roots=C:\Studies;D:\College

[sites]
moodle=https://moodle.myuni.edu
chess=https://www.chess.com

[apps]
discord=%LOCALAPPDATA%\Discord\Update.exe --processStart Discord.exe
obs=C:\Program Files\obs-studio\bin\64bit\obs64.exe

[folders]
projects=C:\Studies\Web Development
semester=D:\College\Semester 5
```

Now "open moodle", "open discord", "open projects" work.
Desktop, Documents, Downloads, Pictures, Videos, Music and OneDrive are always indexed.

---

## Privacy and performance

- **Nothing runs in the background.** JARVIS starts when you press the key and closes when it's done.
- **Nothing leaves your PC.** Speech recognition and voices are the ones built into Windows.
- File index: `%LOCALAPPDATA%\JARVIS\index.txt`. It loads in milliseconds. When it's older than
  `index_max_age_min` (default 30 min), JARVIS re-scans in the background while you use it.
- Log: `%LOCALAPPDATA%\JARVIS\jarvis.log` (kept small).

---

## Troubleshooting

| Problem | Fix |
|---|---|
| It doesn't hear me | Settings → Privacy & security → **Microphone** → turn on **"Let desktop apps access your microphone"**. Check the right mic is the default in Sound settings. |
| No speech recognition at all | JARVIS needs the Windows recognizer "Microsoft Speech Recognizer 8.0 for Windows (English - US)". Settings → Time & language → Language & region / Speech: make sure **English (United States)** is installed with speech. |
| It mishears me | Speak **after** the mic dot turns blue. Use short commands ("open downloads", not a full sentence). Quiet room helps. If available, run Control Panel → Speech Recognition → **Train your computer to better understand you** (not on every Windows build). You can always type instead. |
| File not found | Run `jarvis.exe --reindex`, or say "reindex". If the file is outside your normal folders, add its folder to `roots` in `[search]`. |
| Wrong app / site opens | Add or fix the name in `[apps]` / `[sites]`. |

### Test without doing anything

From a terminal in the JARVIS folder:

```bat
jarvis.exe --dry-run "open latest photo"
jarvis.exe --search resume
jarvis.exe --reindex
```

`--dry-run` prints what the command **would** do (nothing is opened). `--search` prints matching files.
Tip: use Command Prompt. In PowerShell add `| Out-String` at the end so it waits for the output.

Other options: `jarvis.exe --type` (open without listening), `jarvis.exe --background` (no window: just listen and talk; a small "Listening…" pill shows at the bottom and the bar appears only when you need to pick from a list), `jarvis.exe --normal` (always show the bar), `jarvis.exe "open downloads"` (run a command right away).

While the bar is open, say or type **"hide"** / **"background mode"** (or press **Ctrl+H**) to keep going in the background; say **"show window"** to bring it back. Set `mode=background` in `jarvis.ini` to always start hidden.

---

## Project layout

| File | What it does |
|---|---|
| `src/jarvis.h` | Shared declarations for all modules |
| `src/main.c` | Entry point, command-line flags, single instance (second launch = toggle mic) |
| `src/ui.c` | The search bar (mic dot, text box, results, keyboard) and the background-mode pill |
| `src/commands.c` | Understands commands and does the actions |
| `src/search.c` | File index + fuzzy search, Start Menu app search |
| `src/voice.c` | Offline speech recognition and text-to-speech (SAPI) on its own thread |
| `src/dialog.c` | Detects Upload/Open dialogs and fills in the file path |
| `src/config.c` | Loads `jarvis.ini` (creates it with defaults on first run) |
| `src/util.c` | String, path and log helpers |
| `res/` | Manifest, version info |
| `tools/` | Build helper, `install.ps1` |
| `docs/COPILOT_KEY.md` | Copilot key setup |

## Building and tests

```bat
build.bat     :: builds build\jarvis.exe
test.bat      :: runs the tests
```

Needs MinGW-w64 `gcc` + `windres` on PATH. Uses only Windows system libraries, no other dependencies.

## Architecture

- **One small exe, no service.** A named mutex keeps one instance; launching again posts "toggle mic" to the open window.
- **Voice on a worker thread.** SAPI recognizer + TTS live on their own thread and talk to the UI with window messages, so the overlay never freezes.
- **Command grammar.** `commands.c` turns text (spoken or typed) into an action: open, reveal, fill dialog, URL, app, or system action.
- **Cached file index.** A flat list of files saved to `index.txt`, loaded instantly, refreshed on a low-priority background thread only when stale.
- **Dialog filling.** The window that had focus before JARVIS opened is checked for a standard Open/Upload dialog; the path is typed into its "File name" box.

## License

MIT - see [LICENSE](LICENSE). Copyright (c) 2026 Ankit Silwal.
