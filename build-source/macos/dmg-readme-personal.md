# RX3 Prototype for Mac (personal edition)

**Do not share this file.** This edition contains decrypted, patched
XDJ-RX3 firmware. It's for your own Macs only. To give the prototype to
someone else, send the *shareable* DMG instead. It builds the firmware on the
recipient's Mac from AlphaTheta's official download.

## What you need

* A Mac with Apple silicon (M1 or later), macOS 26 or later
* Nothing else to install. QEMU, the guest system and the player are all inside
  the app.
* To load tracks: a USB drive exported from rekordbox. A DDJ-FLX4 is optional.

## Install

1. Open the `.dmg` file.
2. Drag **RX3 Prototype** onto **Applications**.
3. Open it from Applications. If you copied the DMG from another Mac and macOS
   blocks it, go to **System Settings → Privacy & Security → Open Anyway**.

## Using it

1. Plug in your rekordbox USB first. It's found automatically.
2. Open **RX3 Prototype**. The player boots fullscreen in about 20–30 seconds.
3. To quit, press **Control-Option-F** to leave fullscreen, then close the
   window (or press **Command-Q**).

Logs are in `~/Library/Logs/RX3 Prototype/`. App data is in
`~/Library/Application Support/RX3Prototype`, which you can delete at any time.
