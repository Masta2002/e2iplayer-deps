# e2iplayer-deps

Native helper binaries and the C subtitle parser bundled with
[E2iPlayer](https://github.com/oe-mirrors/e2iplayer). Shipped as the
`enigma2-plugin-extensions-e2iplayer-deps` package, built from the
OE/OpenATV feed recipe.

## Github status

[![build deps](https://github.com/oe-mirrors/e2iplayer-deps/actions/workflows/ci.yml/badge.svg)](https://github.com/oe-mirrors/e2iplayer-deps/actions/workflows/ci.yml)
[![Github last commit](https://img.shields.io/github/last-commit/oe-mirrors/e2iplayer-deps)](https://github.com/oe-mirrors/e2iplayer-deps/commits)
[![GitHub Activity](https://img.shields.io/github/commit-activity/y/oe-mirrors/e2iplayer-deps.svg?label=commits)](https://github.com/oe-mirrors/e2iplayer-deps/commits)
[![Issues](https://img.shields.io/github/issues/oe-mirrors/e2iplayer-deps?color=blue)](https://github.com/oe-mirrors/e2iplayer-deps/issues)
![Platform](https://img.shields.io/badge/Platform-Enigma2-orange.svg)

**Component versions** (bumped by hand alongside each component's own version string)

[![hlsdl](https://img.shields.io/badge/hlsdl-v0.36-blue)](hlsdl/)
[![e2isubparser](https://img.shields.io/badge/e2isubparser-0.9-blue)](e2isubparser/)
[![lsdir](https://img.shields.io/badge/lsdir-1.0-blue)](lsdir/)
[![cmdwrap](https://img.shields.io/badge/cmdwrap-2-blue)](cmdwrap/)
[![f4mdump](https://img.shields.io/badge/f4mdump-v0.82-blue)](f4mdump/)

## SonarCloud status

Fills in once a `SONAR_TOKEN` secret is added and the project is created
at sonarcloud.io (org `oe-mirrors`, key `oe-mirrors_e2iplayer-deps`) - see
the header of `.github/workflows/sonarcloud.yml`.

[![Quality Gate Status](https://sonarcloud.io/api/project_badges/measure?project=oe-mirrors_e2iplayer-deps&metric=alert_status)](https://sonarcloud.io/summary/new_code?id=oe-mirrors_e2iplayer-deps)
[![Vulnerabilities](https://sonarcloud.io/api/project_badges/measure?project=oe-mirrors_e2iplayer-deps&metric=vulnerabilities)](https://sonarcloud.io/summary/new_code?id=oe-mirrors_e2iplayer-deps)
[![Security Rating](https://sonarcloud.io/api/project_badges/measure?project=oe-mirrors_e2iplayer-deps&metric=security_rating)](https://sonarcloud.io/summary/new_code?id=oe-mirrors_e2iplayer-deps)
[![Bugs](https://sonarcloud.io/api/project_badges/measure?project=oe-mirrors_e2iplayer-deps&metric=bugs)](https://sonarcloud.io/summary/new_code?id=oe-mirrors_e2iplayer-deps)
[![Code Smells](https://sonarcloud.io/api/project_badges/measure?project=oe-mirrors_e2iplayer-deps&metric=code_smells)](https://sonarcloud.io/summary/new_code?id=oe-mirrors_e2iplayer-deps)
[![Duplicated Lines (%)](https://sonarcloud.io/api/project_badges/measure?project=oe-mirrors_e2iplayer-deps&metric=duplicated_lines_density)](https://sonarcloud.io/summary/new_code?id=oe-mirrors_e2iplayer-deps)
[![Reliability Rating](https://sonarcloud.io/api/project_badges/measure?project=oe-mirrors_e2iplayer-deps&metric=reliability_rating)](https://sonarcloud.io/summary/new_code?id=oe-mirrors_e2iplayer-deps)
[![Maintainability Rating](https://sonarcloud.io/api/project_badges/measure?project=oe-mirrors_e2iplayer-deps&metric=sqale_rating)](https://sonarcloud.io/summary/new_code?id=oe-mirrors_e2iplayer-deps)

[![SonarQube Cloud](https://sonarcloud.io/images/project_badges/sonarcloud-light.svg)](https://sonarcloud.io/summary/new_code?id=oe-mirrors_e2iplayer-deps)

---

See [BUILDING.md](BUILDING.md) for how to build the components outside the
OE feed.

---

## lsdir

Fast directory lister used by the plugin's file browser. `lsdir --version`
prints the version string.

## cmdwrap

Command-line argument splitter.

## e2isubparser

Python C-extension for text subtitle parsing (SRT / SSA / TTML / EBU-TT).
The TTML path uses a vendored [Expat](https://libexpat.github.io/) 2.8.5
(`e2isubparser/src/expat`, the `lib/` sources of the release tarball).

## f4mdump

Simple F4M / HDS (Adobe HTTP Dynamic Streaming) dumper. It shells out to
`wget` for the transfers rather than linking an HTTP library.

```
List bitrates:  f4mdump "/path/to/wget [extra params]" "http://url.to/Manifest.f4m"
Download:       f4mdump "/path/to/wget [extra params]" "http://url.to/Manifest.f4m" "/path/to/outfile" [bitrate]
```

## hlsdl

Downloads VOD and live HLS (`.m3u8`) streams to a single file. Handles
MPEG-2 Transport Stream and fragmented MP4 / CMAF segments, `EXT-X-MAP`
initialization segments (fMP4 and TS; a repeated map is written once, a new
one after a discontinuity also in live streams), `EXT-X-BYTERANGE`, discontinuities,
AES-128 / SAMPLE-AES decryption (each segment with the key, IV and method -
including `METHOD=NONE` - in force where it stands), and TS segments disguised
as images (an image header in front of the MPEG-TS data is cut off).
`EXT-X-KEY` and `EXT-X-MAP` attributes may come in any order. A segment that
cannot be fetched (an empty body counts as failed), decrypted or written fails
a VOD download (exit code 1, resumable with `-R`), as does an init segment
whose response does not match its `BYTERANGE`; with `-I` a VOD download skips
segments that cannot be fetched (decryption and write errors stay fatal). A live download skips a
segment it cannot fetch or decrypt and stops when the output cannot be written.
A separate audio rendition (`EXT-X-MEDIA TYPE=AUDIO` or `-a`, TS or packed
audio with an ID3 timestamp) is muxed into TS output for VOD and live streams;
with fMP4 the video track is written alone.

Requires `libcurl` and `libcrypto`.

```
Linux:    make && make install && make clean
Windows:  hlsdl/msvc/BUID_WINDOWS.txt
```

### Usage

`hlsdl [options] url`

```
-b ... Automatically choose the best quality.
-W ... Choose largest width lower or equal than this.
-H ... Choose largest height lower or equal than this.
-A ... Select audio language.
-v ... Verbose more information.
-q ... Print less to the console (status lines, errors and warnings; -qq: nothing).
-o ... Choose name of output file ("-" alias for stdout).
-f ... Force overwriting the output file.
-R ... Resume an interrupted VOD download (keeps a <output>.hlsdl.resume sidecar).
-F ... Force ignore detection of DRM.
-u ... Set custom HTTP User-Agent header.
-h ... Set custom HTTP header.
-p ... Set proxy uri.
-C ... Cookie file (old Netscape / Mozilla format).
-k ... Allow to replace part of AES key uri - old.
-n ... Allow to replace part of AES key uri - new.
-K ... Force AES key value (hexstring).
-d ... Print the openssl decryption command.
-t ... Print the links to the .ts files.
-s ... Set live start offset in seconds.
-i ... Set live stream download duration in seconds.
-e ... Set refresh delay in seconds.
-r ... Set max retries at open.
-w ... Set max download segment retries.
-I ... Skip VOD segments that still fail after the retries (the output has gaps; not with -R).
-a ... Set additional url to the audio media playlist (VOD and live, TS only).
-c ... Treat HTTP 206 as 200 even without a range request.
```

Not handled (E2iPlayer routes these through its own ffmpeg/gstreamer
pipeline): Widevine / PlayReady / FairPlay (CENC / cbcs), real muxing, and
bit-exact remuxing across codec parameter changes.

---

### 📜 License Information [![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)

This repository ships more than one license:

| Component | License |
|---|---|
| `hlsdl` | MIT — see [hlsdl/LICENSE](hlsdl/LICENSE) |
| `lsdir`, `cmdwrap`, `e2isubparser`, `f4mdump` | GPLv3, released as part of [E2iPlayer](https://github.com/oe-mirrors/e2iplayer) — see its [LICENSE.txt](https://github.com/oe-mirrors/e2iplayer/blob/python3/LICENSE.txt) |
| vendored Expat (in `e2isubparser`) | MIT |
| vendored VLC / FFmpeg subtitle snippets (in `e2isubparser`) | LGPL 2.1+ |
| vendored librtmp from rtmpdump v2.6 (in `f4mdump`) | LGPL 2.1 — see [f4mdump/ext/librtmp/COPYING](f4mdump/ext/librtmp/COPYING) |
| vendored TinyXML-2 11.0.0 (in `f4mdump`) | zlib |

The E2iPlayer-authored components are free software; you can redistribute
them and/or modify them under the terms of the GNU General Public License
as published by the Free Software Foundation. Vendored third-party code
keeps its own upstream license.

<img width="127" height="51" alt="GPLv3 logo" src="https://www.gnu.org/graphics/gplv3-127x51.png" />

---

### 🤝 Contributing

Pull requests welcome. Fork, branch, commit with a clear message, open a
PR. CI (`build deps`) runs on every push and PR.
