# RX3 Prototype for Mac

An experimental, unofficial prototype that runs the XDJ-RX3 player interface
on a Mac, in a small virtual machine. Not affiliated with, endorsed by or
sponsored by Pioneer DJ or AlphaTheta.

This download contains **no Pioneer/AlphaTheta files**. The first time you open
the app, it downloads AlphaTheta's official XDJ-RX3 v1.20 update and builds the
player on your own Mac.

## What you need

* A Mac with Apple silicon (M1 or later)
* macOS 26 or later
* [Docker Desktop](https://www.docker.com/products/docker-desktop/), for the
  one-time setup only (playback doesn't use it)
* About 15 GB of free disk space during setup, and an internet connection
* To load tracks: a USB drive exported from rekordbox
* Optional: a Pioneer DDJ-FLX4, connected to the Mac with USB

You don't need Homebrew, Python, QEMU or Xcode. They're included in the app.

## Install

1. Open the `.dmg` file.
2. Drag **RX3 Prototype** onto the **Applications** folder.
3. Eject the disk image.

## First launch (one time, 20–40 minutes)

1. Install Docker Desktop and open it once. Wait until it says it's running.
2. Open **Applications**, then open **RX3 Prototype**.
3. macOS will block it the first time, because this is a free test build
   without an Apple developer signature:
   * Click **Done** in the warning.
   * Open **System Settings → Privacy & Security**, scroll down, and click
     **Open Anyway** next to "RX3 Prototype". Confirm with your password.
4. Click **Start Setup**. A Terminal window shows the progress:
   * It checks your Mac and Docker.
   * It downloads the official firmware and AlphaTheta's open-source archives.
   * It builds the player (this is the slow part).
5. When it says **Done**, RX3 Prototype opens by itself.

If setup stops with an error, fix what it says (usually Docker isn't running
or there's not enough disk space), then open RX3 Prototype again to retry.

Once setup finishes you can quit Docker Desktop. Optionally, remove the
`rbpi4b-macos-tools` and `rbpi4b-build` images in Docker Desktop to get
about 5 GB back.

## Using it

1. Plug in your rekordbox USB **before** opening the app. It's found
   automatically. If more than one is connected, the app asks which to use.
2. Open **RX3 Prototype**. The player opens fullscreen and takes about 20–30
   seconds to boot.
3. Click **USB** in the player to browse playlists. Use the mouse/trackpad and
   keyboard, or the DDJ-FLX4 if you have one.
4. Sound plays through the Mac's current output device.
5. To quit, press **Control-Option-F** to leave fullscreen, then close the
   window (or press **Command-Q**).

Your USB is read-only. Cues and settings you change in the player are not saved.

## Troubleshooting

* **"RX3 Prototype is damaged" or "can't be opened":** use **Open Anyway** as in
  step 3 of the first launch. If that doesn't appear, run this in Terminal:
  `xattr -dr com.apple.quarantine "/Applications/RX3 Prototype.app"`
* **No tracks:** make sure the USB was exported from rekordbox. It must contain
  `PIONEER/rekordbox/export.pdb`. Then reopen the app with the USB plugged in.
* **FLX4 does nothing:** reconnect it with its USB-C port, then reopen the app.
* **Logs:** `~/Library/Logs/RX3 Prototype/console.log` and `midi-host.log`.
* **Start over:** delete `~/Library/Application Support/RX3Prototype`, and the
  next launch runs setup again.

## Uninstall

Delete **RX3 Prototype** from Applications, plus
`~/Library/Application Support/RX3Prototype` and `~/Library/Logs/RX3 Prototype`.

## Legal

Experimental and provided as-is. The firmware is downloaded from AlphaTheta and
stays on your Mac; don't share it. Make sure using it is allowed where you
live. Third-party licenses and source links are inside the app, under
`Contents/Resources/licenses`.
