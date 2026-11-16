# Example videos

## Real footage: a waterfall

`localcut-nature.mp4` is a six-second scenic clip with the original waterfall audio (H.264/AAC, 25 fps). It is an excerpt of [Bigora waterfall](https://commons.wikimedia.org/wiki/File:Bigora_waterfall.webm), by Termininja, published under CC0-1.0. See the adjacent `.license` file.

Import it with **选择素材**, then try splitting it at two seconds, adding a caption and exporting MP4. It can be edited manually or through your configured AI assistant.

## Synthetic codec test

`localcut-demo.mp4` is a synthetic FFmpeg test pattern with a 440 Hz sine tone. It contains no personal footage, third-party music or voices.

- 640 × 360, 25 fps, 150 frames, 6 seconds.
- H.264 video and AAC audio.

Import it with **选择素材**. After connecting your own Codex account, try:

```text
把 localcut-demo.mp4 放到空时间线，保留原声。
在第 2 秒分成两段，总时长保持 6 秒。
```

Inspect the two clips, try Undo/Redo, then export MP4. Expected: the same six-second pattern and tone, with an editable cut at two seconds. If you are testing manual editing, perform the same steps on the timeline without connecting the assistant.

The synthetic demo is provided under CC0-1.0.
