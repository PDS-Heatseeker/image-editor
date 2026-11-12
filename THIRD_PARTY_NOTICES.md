# Third-party software

Image Shrinker is built with these libraries (downloaded by `scripts/fetch_deps.sh`, compiled in statically):

| Library | Used for | License |
|---|---|---|
| [mozjpeg](https://github.com/mozilla/mozjpeg) (libjpeg-turbo) | JPEG reading and writing | IJG License + BSD-3-Clause + zlib |
| [libwebp](https://github.com/webmproject/libwebp) | WebP reading and writing | BSD-3-Clause |
| [lodepng](https://github.com/lvandeve/lodepng) | PNG writing, compression for TIFF | zlib |
| [stb_image](https://github.com/nothings/stb) | PNG / BMP / GIF reading | MIT or public domain |
| [stb_image_resize2](https://github.com/nothings/stb_image_resize) | Resizing | MIT or public domain |

This software is based in part on the work of the Independent JPEG Group.

The full license texts are in each library's repository (`LICENSE.md` / `README.ijg` for mozjpeg,
`COPYING` for libwebp, `LICENSE` for lodepng and stb). Keep this notice with any copy of the exe you share.
