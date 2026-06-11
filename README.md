## GTA: Chinatown Wars Nintendo Switch port

This is a wrapper/port of the Android version of Grand Theft Auto: Chinatown Wars (v4.4.243).
It loads the original game binary, patches it and runs it.
It's basically as if we emulate a minimalist Android environment in which we natively run the original Android binary as is.

This is a based on [max_nx](https://github.com/fgsfdsfgs/max_nx) adapted for CTW

### How to install

You're going to need:
* `.apk` file for version 4.4.243. There is no separate `.obb`: all the game data lives in the APK's `assets` folder.

The file [can be obtained](https://stackoverflow.com/questions/11012976/how-do-i-get-the-apk-of-an-installed-app-without-root-access) from your phone if you have a copy of the game installed.
It can be opened or extracted with anything that can extract `.zip` files.

To install:
1. Create a folder called `gtactw` in the `switch` folder on your SD card.
2. Extract **the contents of** the `assets` folder from your `.apk` to `/switch/gtactw/` (this includes `game.pak`, `dxt.bin`, `AVConfig.json`, the `.gxt` text files, the `.mp3` music and the `.mp4` movies). You can skip the `flutter_assets`, `dexopt` and `rockstar` folders.
3. Extract `lib/arm64-v8a/libGame.so` **and** `lib/arm64-v8a/libopenal.so` from your `.apk` to `/switch/gtactw/`.
4. Copy `gtactw_nx.nro` into `/switch/gtactw/`.

### Notes

This will not work in applet/album mode. Use a game override (hold R on a title) or forwarder.

Save games and the game's own settings (`Adjustable.cfg`) are stored in `/switch/gtactw/`.

The port has an extra config file, located at `/switch/gtactw/config.txt`. It is created when you
first run the game:
* `screen_width` / `screen_height` — render resolution; `-1` picks 1280x720 in handheld and 1920x1080 docked
* `language` — `0` English; other values select the other localizations (FR/DE/IT/ES/JA)
* `touchscreen` — `1` enables touch input in handheld mode
* `trilinear_filter` / `disable_mipmaps` — texture filtering tweaks

### How to build

You're going to need devkitA64 and the following libraries:
* `switch-mesa`
* `switch-libdrm_nouveau`
* `switch-sdl2`
* `switch-mpg123`
* `switch-ffmpeg`
* `devkitpro-pkgbuild-helpers`
* [openal-soft](https://github.com/fgsfdsfgs/openal-soft)

```
source $DEVKITPRO/switchvars.sh
make
```

### Credits

* TheOfficialFloW for the original gtactw_vita port;
* fgsfds for max_nx, which this port is based on;

### Legal

This project has no direct affiliation with Take-Two Interactive Software, Inc. or Rockstar Games, Inc.
"Grand Theft Auto" and "Grand Theft Auto: Chinatown Wars" are trademarks of their respective owners. All Rights Reserved.

No assets or program code from the original game or its Android port are included in this project.
We do not condone piracy in any way, shape or form and encourage users to legally own the original game.

Unless specified otherwise, the source code provided in this repository is licenced under the MIT License.
Please see the accompanying LICENSE file.
