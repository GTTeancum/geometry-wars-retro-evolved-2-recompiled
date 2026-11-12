Geometry Wars: Retro Evolved 2 for the original Xbox - installer
https://github.com/GTTeancum/geometry-wars-retro-evolved-2-recompiled

This download contains no game files. The installer prepares the game from
your own Geometry Wars: Retro Evolved 2 Xbox Live Arcade package (title ID
584108FF, game version 0.0.1.2).

You need:
  - Windows 10 or 11
  - ffmpeg (https://ffmpeg.org) on the PATH, or ffmpeg.exe next to
    install.ps1; without it the game installs with no sound
  - an original Xbox that runs homebrew, stock 64 MB

To install:
  1. Open a command prompt in this folder and run
       install.cmd "<your LIVE package>" "<new output folder>"
     The output folder must be new or empty. It takes a few minutes.
  2. Copy the output folder to the Xbox's hard disk (with FTP, for example
     to E:\Games\Geometry Wars 2).
  3. Start default.xbe from your dashboard.

In game: hold LT + RT + Back + Black on any controller, or pick Return to
dash on the main menu, to go back to the dashboard. Saves are kept in
E:\UDATA\584108FF.

Licensed under the GNU General Public License v2.0; the source is on the
project page. dump_image.exe and rexruntime.dll are built with ReXGlue
(https://github.com/rexglue/rexglue-sdk, BSD-3-Clause).
