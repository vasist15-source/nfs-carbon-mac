Need for Speed Carbon - Setup

This app builds the native Mac version of Need for Speed: Carbon from YOUR OWN copy of the PC game.
It contains no game files. Everything happens on your Mac; nothing is uploaded.

You need:
  - An Apple Silicon Mac (M1 or later).
  - Your own installed PC game folder: Need for Speed: Carbon, version 1.4, with an unwrapped (no-DVD)
    NFSC.exe: the folder with NFSC.exe, msvcr71.dll, GLOBAL, TRACKS, CARS and so on.
  - About 10 GB free, and some time for the first build (the one-time analysis of NFSC.exe is the long part).

Opening the app the first time:
  This app is not notarized by Apple, so macOS blocks it the first time. Open it once, click Done,
  then go to System Settings > Privacy & Security and click "Open Anyway" next to its name.

Then:
  1. Choose your game folder, or drop it on the window. The app checks NFSC.exe first and changes
     nothing if it can't be used.
  2. If asked, install Apple's Command Line Tools (free; the app waits for them).
  3. Optional: tick "Controller button icons" to download the controller button pictures, and
     "HD menus, HUD and fonts" for the Remastered UI texture pack (adds about 300 MB to the app).
  4. Click Build. When it finishes, click "Open the game".

In the game: F10 (fn+F10 on a Mac keyboard) opens the settings: full screen, borderless or windowed,
frame-rate limit, widescreen and controller options. Resolutions are in Options > Video.

Your saves and settings: ~/Library/Application Support/NFS Carbon/
The setup's own files (safe to delete after building; an update is quicker with them): ~/Library/NFSC-Native-Setup/

This text is also in the Setup app (Read Me button).
Source and help: https://github.com/vasist15-source/nfs-carbon-mac
This project is not affiliated with or endorsed by Electronic Arts.
