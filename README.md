# JARVIS

A tiny, **offline** voice launcher for Windows, written in plain C (Win32).
Press the **Copilot key**, say what you want, and JARVIS does it.

- **Open folders and files** by name: "open downloads", "open my resume"
- **Find files you lost** with fuzzy search: "find physics notes", then say "two"
- **Newest stuff fast**: "open latest photo", "open latest download", "open newest pdf in documents"
- **Fill Upload / Open dialogs** (Google Drive in Chrome, Gmail, Office...): "upload resume"
- **Web**: open Chrome, YouTube, Gmail, Drive, Classroom; Google / YouTube / Wikipedia searches; directions;
  pick the browser: "open youtube in brave", "google black holes in firefox"
- **Apps**: VS Code, Word, or any app in your Start Menu
- **Little things**: time, date, battery, volume, lock, screenshot
- **Works offline**: uses the speech recognizer and voices built into Windows. Nothing leaves your PC.
- **Stays until you close it**: ask one thing after another. Close with Esc, the **×** button, or say "close".
- **Small**: one `.exe` + one `jarvis.ini`. Nothing runs in the background once you close it.

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

Press the key once → JARVIS opens and listens. Press it again → mic on/off (or, if JARVIS is
behind another window, it comes back to the front and listens).

JARVIS stays open after answering (`stay_open=1`, the default): after a spoken command it listens
for the next one; after a typed one you can just type again. Opening an app or website doesn't close
it; it steps behind that window instead of covering it. Close it with **Esc**, **Alt+F4**, the small
**×** at the right of the box, or by saying **"close"** / **"exit"** / **"bye"** / **"that's all"**.
Set `stay_open=0` to close after every answer instead.

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

### Documents

Everyday file jobs. Select a file in Explorer (or on the Desktop) **then** press the Copilot key and say
"this", or name the file. "it" / "this" also mean the last file JARVIS opened or made.

| Say | What happens |
|---|---|
| "convert resume to pdf" / "make this a pdf" / "save this as pdf" | Word, PowerPoint, Excel, text or image file → PDF |
| "convert physics notes to word" / "pdf to word resume" | PDF → editable `.docx` (Word's PDF reflow) |
| "convert this doc to pdf **named** final report" | Picks the output name (also "name it X", "call it X", "as X") |
| "split physics notes at page 5" | `physics notes part 1.pdf` (pages 1-5) and `part 2.pdf` (6 to the end). Also "up to / until / after page five", "into two parts at page 5" |
| "split this at page 5 **and save it in downloads**" | Puts the output in another folder ("put it in desktop", "save to documents", or any folder name) |
| "extract pages 3 to 7 from notes" / "take pages 2-4 of this pdf" | Those pages as a new PDF |
| "merge notes and homework" / "merge these" | One PDF (`notes merged.pdf`); "merge these" uses every selected PDF |
| "how many pages does this have" | Counts the pages |
| "rename this to final report" | Renames (the extension is kept if you don't say one) |
| "move this to downloads" / "copy resume to desktop" | Moves / copies the file |
| "delete this" / "remove resume" | Asks "Move it to the Recycle Bin?" - say **yes** or **no**. Always the Recycle Bin, never permanent. Drives and your main folders are refused |

- Nothing is ever overwritten: if `report.pdf` exists the new one is `report (2).pdf`.
- When JARVIS isn't sure which file you mean it shows a list: say **"two"** and the job runs on #2.
- Conversions use Microsoft Office in the background (a hidden copy that is closed afterwards; your open
  Word windows are not touched). Split / extract / merge / page count use Python with PyMuPDF
  (`pip install pymupdf`); JARVIS finds Anaconda or python.org installs, or set `python=` under `[docs]`.
- Long jobs run in the background: JARVIS says "Converting…" and tells you when it's done, with the
  new file in the list (say "one" to open it).

### Upload / Open dialogs

Open the Upload window first (for example "Upload file" on Google Drive in Chrome), **then** press the Copilot key.

| Say | What happens |
|---|---|
| "upload resume" | Finds your resume and puts it into the Upload window |
| "go to downloads" | Moves the Upload window to the Downloads folder |

### Web

| Say | What happens |
|---|---|
| "open chrome" / "open brave" / "open firefox" | Opens that browser |
| "open youtube" / "open gmail" / "open drive" / "open classroom" | Opens that site (more in `jarvis.ini`) |
| "open youtube in brave" / "youtube lofi in edge" | Opens it in that browser (chrome, brave, edge, firefox, opera). Without a browser name, `browser=` in `jarvis.ini` is used |
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
| "clear" / "clear screen" | Empties the box, the reply and the list (also **Ctrl+L**) |
| "help" | Shows example commands |
| "close" / "exit" / "bye" | Closes JARVIS ("never mind" just cancels) |

### JARVIS's voice

| Say / do | What happens |
|---|---|
| **Esc**, the **Copilot key** or **F2** while JARVIS talks, or click the reply | Stops talking right away (Copilot key / F2 then listen to you; a second Esc closes) |
| "mute jarvis" / "stop talking" / "be quiet" / "voice off" | JARVIS stops speaking. Replies are only shown. Saved in `jarvis.ini` (`speak=0`) |
| "unmute jarvis" / "voice on" / "talk to me" | Speaks again (`speak=1`) |
| The speaker button next to **×**, or **Ctrl+M** | Mute / unmute |
| "talk less" / "only answers" | Stops saying confirmations like "Opening Downloads." Answers, questions and problems are still spoken (`speak_confirm=0`) |
| "talk more" | Says everything again (`speak_confirm=1`) |

"mute" on its own still mutes the PC's sound, like the volume key.

### Keyboard

You can also just type. Results show up live while you type.

| Key | Does |
|---|---|
| Enter | Open the selected result |
| Shift + Enter | Show it in its folder |
| Up / Down | Move through results |
| F2, Ctrl + Space, or the Copilot key again | Mic on / off (while JARVIS talks: stop it and listen) |
| Ctrl + M | Mute / unmute JARVIS's voice |
| Ctrl + L | Clear the box and the results |
| Esc | Stop talking; if it isn't talking, close JARVIS |
| Alt + F4, or the × button | Close JARVIS |

---

## Customize (`jarvis.ini`)

`jarvis.ini` sits next to `jarvis.exe`. Open it in Notepad. Changes apply the next time JARVIS opens.
Names on the left are what you **say** (lowercase). Delete the file to get the defaults back.

| Section | What it's for |
|---|---|
| `[general]` | `mode` (normal / background), `stay_open` (1 = stay until you close it, 0 = close after each answer), `speak` (0 = muted), `speak_confirm` (0 = "talk less"), `listen_on_start`, `voice` (David / Zira), `voice_rate` (-10..10), `close_delay_ms`, `browser`, `index_max_age_min` |
| `[search]` | `roots` = extra folders to index (separate with `;`), `skip` = folder names never scanned |
| `[sites]` | spoken name = website |
| `[apps]` | spoken name = program, `ms-settings:` link, etc. |
| `[folders]` | spoken name = folder |
| `[docs]` | `python` = python.exe with PyMuPDF for PDF split / merge (empty = find it) |

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

- **Nothing runs once you close it.** While open and idle it uses no timers and no polling
  (with `stay_open=0` it closes by itself after each answer).
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

In background mode with `stay_open=1`, JARVIS keeps running after it answers: it listens once more,
then waits quietly with a **tray icon** ("JARVIS — background"). Press the Copilot key to talk again,
left-click the icon to show the bar, or right-click it for **Show** / **Exit**. Say "close" or
"exit" to end it.

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
| `src/docs.c` | Document jobs (Office COM, Python + PyMuPDF), file ops, Explorer selection |
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

`test.bat` skips the slow real document test (Word, Excel, PyMuPDF on throwaway files in `%TEMP%`);
run it with `set JARVIS_DOCS_TEST=1` first.

Needs MinGW-w64 `gcc` + `windres` on PATH. Uses only Windows system libraries, no other dependencies.

## Architecture

- **One small exe, no service.** A named mutex keeps one instance; launching again posts "toggle mic" to the open window.
- **Voice on a worker thread.** SAPI recognizer + TTS live on their own thread and talk to the UI with window messages, so the overlay never freezes.
- **Command grammar.** `commands.c` turns text (spoken or typed) into an action: open, reveal, fill dialog, URL, app, or system action.
- **Cached file index.** A flat list of files saved to `index.txt`, loaded instantly, refreshed on a low-priority background thread only when stale.
- **Dialog filling.** The window that had focus before JARVIS opened is checked for a standard Open/Upload dialog; the path is typed into its "File name" box.

## License

MIT - see [LICENSE](LICENSE). Copyright (c) 2026 Ankit Silwal.
