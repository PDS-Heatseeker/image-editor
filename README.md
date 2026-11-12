<p align="center"><img src="docs/icon.png" width="112" alt="Image Shrinker icon"></p>

<h1 align="center">Image Shrinker</h1>

<p align="center">Bulk-resize and compress images on Windows and get them back in one zip.<br>
A single 1.6 MB exe: no installer, no runtime, and it keeps itself up to date.</p>

<p align="center"><img src="docs/screenshot.png" width="720" alt="Image Shrinker window"></p>

## Download

Get **`ImageShrinker.exe`** from the [latest release](../../releases/latest) and run it.
The app isn't code-signed, so the first time Windows shows *"Windows protected your PC"*: click **More info → Run anyway**.

## What it does

- **Add images**: drag files or whole folders onto the window, or use *Add images… / Add folder…*. Reads JPG, PNG, WebP, BMP and GIF.
- **See before you save**: every file shows its current size and an estimated new size, with the change in green (smaller) or red (bigger), plus a running total.
- **Size**: shrink the longest side to a preset (default 1920 px), or tick *Custom size* to set a width and/or height. With *Maintain aspect ratio* on, typing one fills in the other (for the selected image) and every image keeps its shape. With it off, images are set to exactly that width × height.
- **Format**: keep each image's own format, or convert all to JPG, JPEG, PNG, WebP, WebP lossless, TIFF or BMP. The quality slider (default 90) applies to JPG and WebP.
- **Save**: everything goes into one zip. Photos are rotated upright, camera metadata is removed, wide-gamut colour profiles are kept, and an image is never made larger just by re-saving it.
- **Light on resources**: one image in memory at a time and low-priority background threads.

## Updates

When the app starts it checks this repository's latest release. If there's a newer version, it downloads it, checks it against the SHA-256 checksum GitHub publishes for the file, and installs it when you close the app (or right away with *Restart now*).

### Publishing a new version

1. Push your changes to `main`.
2. On GitHub go to **Releases → Draft a new release**, create a tag such as **`v1.3.0`** (it must be higher than the current version), add notes, and click **Publish release**.
3. The *Build and release* workflow builds `ImageShrinker.exe` with that version number and attaches it to the release (about 2 minutes, see the **Actions** tab).

Every installed copy picks it up the next time it starts.

## Building it yourself

From Linux or macOS (or WSL) with Python 3 and git:

```sh
pip install ziglang==0.17.1      # the C compiler used to target Windows
scripts/fetch_deps.sh            # downloads the pinned libraries into deps/
scripts/build.sh 1.3.0           # -> out/ImageShrinker.exe
```

| Path | What's in it |
|---|---|
| `src/gui.c` | Window, file list, options, background threads |
| `src/core.c` | Decode → resize → encode pipeline, size estimates |
| `src/update.c` | Self-update from GitHub Releases |
| `src/zipw.c` | Zip writer |
| `src/test_cli.c`, `src/test_est.c` | Command-line test tools for the pipeline |

Third-party libraries and their licenses: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
