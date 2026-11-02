# Make the Copilot key open JARVIS

Uses **PowerToys Keyboard Manager** (free, from Microsoft). Takes about 5 minutes.

## 1. Install PowerToys

Open PowerShell or Command Prompt and run:

```bat
winget install Microsoft.PowerToys
```

Or get it from the Microsoft Store. Then open **PowerToys** from the Start Menu.

In PowerToys → **General**, turn on **Run at startup**. The shortcut only works while PowerToys is running.

## 2. Know where jarvis.exe is

Example: `C:\Users\<you>\AppData\Local\Programs\JARVIS\jarvis.exe`
(`tools\install.ps1` puts it there.)

## 3. Add the shortcut

1. PowerToys → **Keyboard Manager** → turn **Enable Keyboard Manager** on.
2. Click **Remap a shortcut**.
3. Click **Add shortcut remapping** (the **+**).
4. **Shortcut (left side):** click **Select**, then press the **Copilot key**. Click OK.
   - On most laptops it shows up as **Win + Shift + F23**. That's normal.
5. **Action (right side):** choose **Run Program** and fill in:

| Field | Set to |
|---|---|
| App | full path to `jarvis.exe` |
| Args | leave empty |
| Start in | the folder of `jarvis.exe` |
| Elevation | **Normal** |
| If running | **Start another** |
| Visibility | **Normal** |

6. Click **OK** / **Save**. If it warns that a key is not remapped / is a system key, click **Continue anyway**.

Press the Copilot key. JARVIS should open and start listening.

## How the second press works

- **First press:** JARVIS opens and listens.
- **Press again while it's open:** turns the mic **off / on**. It does **not** open a second window.

That's why "If running" is **Start another**: the new copy sees JARVIS is already open,
tells it to toggle the mic, and quits immediately.

## No Copilot key?

Use any free shortcut instead, for example **Ctrl + Alt + J**. Same steps, just press
that combo in step 4. (Win + J may already be used by Windows, so avoid it.)

## Simpler option without PowerToys (may not work)

Windows 11 has **Settings → Personalization → Text input → Customize Copilot key on keyboard**.
It lets you pick **Search** or a **custom app** for the key.

Catch: on many Windows builds it only lists apps that are MSIX-signed (Store-style apps),
so a plain `jarvis.exe` usually can't be chosen. **PowerToys is the reliable way.**

## Troubleshooting

| Problem | Fix |
|---|---|
| The Copilot app still opens | Settings → Personalization → Text input → **Customize Copilot key on keyboard** → choose **Search** (or another option). Then redo the PowerToys remap. |
| Nothing happens | Is PowerToys running (tray icon)? Is Keyboard Manager enabled? Is the path to `jarvis.exe` correct? |
| Works, then stops after reboot | Turn on **Run at startup** in PowerToys → General. |
| Key shows something other than Win + Shift + F23 | Fine - use whatever PowerToys recorded when you pressed it. |
| JARVIS opens but doesn't hear you | See Troubleshooting in the [README](../README.md#troubleshooting) (microphone permission). |
