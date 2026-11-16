# Source, licenses and third-party components

LocalCut is an independent modified version of Kdenlive. It is not an official KDE or OpenAI distribution. The original source notices, AUTHORS, COPYING, LICENSES and REUSE metadata are retained. The upstream README is retained as [UPSTREAM_README.md](UPSTREAM_README.md).

The source baseline is KDE/kdenlive commit `ef54398259f2af1d29095a798cab48ca4c52f141` (version declarations: 26.08.2), followed by LocalCut assistant, media, template and portability changes. This public repository starts with a reviewed source snapshot; the original private development history and personal test data are not included.

## Application and runtime

| Component | Source / license information |
| --- | --- |
| Kdenlive | [Source](https://invent.kde.org/multimedia/kdenlive), GPL-3.0-only and per-file notices in this repository |
| Qt | [Open-source obligations](https://www.qt.io/development/open-source-lgpl-obligations), individual modules have their own licenses |
| KDE Frameworks | [Sources](https://invent.kde.org/frameworks), individual library notices apply |
| MLT | [Source](https://github.com/mltframework/mlt), library and module licenses apply |
| FFmpeg | [License and source requirements](https://ffmpeg.org/legal.html); the pinned Windows build is GPL enabled |
| 7zr | [7-Zip source and releases](https://github.com/ip7z/7zip); extractor is obtained from its publisher |

The repository and LocalCut application overlay do not bundle Qt/KDE/MLT/FFmpeg binary distributions. The installer fetches exact third-party artifacts directly from KDE, FFmpeg-Builds and 7-Zip, using the URLs and SHA-256 values recorded in `distribution/windows.json`. The downloaded files retain their upstream notices. FFmpeg-Builds provides its [build recipes](https://github.com/FFmpeg-Builds/FFmpeg-Builds); its GPL build must not be described as an LGPL-only build.

The FFmpeg pin deliberately selects a month-end release. The publisher retains daily builds for only 14 days and month-end builds for two years. This dependency must be updated and revalidated before its retention period ends; third-party URLs are not a promise of permanent availability.

For the pinned KDE package, the installer copies its verified `libogg.dll` to `ogg.dll` because the included SoX audio library imports that name. The source and bytes are unchanged; the expected digest is checked. No arbitrary DLL is borrowed from a developer's computer.

If you redistribute a combined runtime yourself, you take on the relevant notice, exact corresponding source and redistribution obligations for all included components. A generic upstream homepage is not a substitute for the corresponding sources. Do not distribute an FFmpeg build configured with `--enable-nonfree` as if it were the verified GPL build. The runtime checker rejects that configuration.

## Optional local models

| Project | Use in LocalCut | Upstream |
| --- | --- | --- |
| Robust Video Matting | Human foreground/background separation | [Source and GPL-3.0 license](https://github.com/PeterL1n/RobustVideoMatting) |
| Spleeter | Two-stem vocal separation | [Source and MIT license](https://github.com/deezer/spleeter) |
| SadTalker | Photo plus speech talking portrait | [Pinned source](https://github.com/OpenTalker/SadTalker/tree/cd4c0465ae0b54a6f85af57f5c65fec9fe23e7f8), Apache-2.0 at that revision |
| facexlib and related face models | Face detection and landmarks | [Source and notices](https://github.com/xinntao/facexlib) |
| Whisper | Local transcription | [Source and MIT license](https://github.com/openai/whisper) |

Weights are not included in Git or the application overlay. Download manifests identify the original artifacts and checksums. Where the publisher did not publish a digest, the manifest explicitly labels the digest as locally computed from the official download. A code license does not automatically cover every model, training dataset, voice, likeness or input asset. Review the original weight and dataset terms for your use.

## External services and assets

Codex authentication and usage belong to the user's own account. Public Hugging Face Spaces, video generation APIs and Wikimedia Commons have their own terms and availability. No account, API key, entitlement, paid material library or unlimited free cloud quota is included.

Commons results can require attribution and share-alike notices. Preserve the author, original asset URL and license returned by the download tool. The templates shipped here are LocalCut's editing instructions, not copied paid template libraries.

## Public demonstration footage

`examples/localcut-nature.mp4` uses [Bigora waterfall](https://commons.wikimedia.org/wiki/File:Bigora_waterfall.webm), filmed by Termininja and published under [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/). The sample is a six-second H.264/AAC excerpt of that footage. No personal media or commercial stock library is included.

The README animation (`docs/assets/localcut-demo.gif`) shows the same footage exported from LocalCut with a Chinese caption. The desktop image (`docs/assets/localcut-desktop.jpg`) is a real capture of that project in the public Windows preview. The GIF is silent; both the MP4 sample and the captioned export (`docs/assets/localcut-demo.mp4`) retain the original waterfall audio. The synthetic color-bar clip remains available solely as a reproducible codec/test fixture.
