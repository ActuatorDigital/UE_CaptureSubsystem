# CaptureSubsystem — UE 5.4 PC video-only fork

This fork of [irajsb/UE_CaptureSubsystem](https://github.com/irajsb/UE_CaptureSubsystem) targets Unreal Engine 5.4 on Windows PC (Win64). It records a Blueprint-selected `TextureRenderTarget2D` to a local H.264 MP4. **Video only: no audio capture or audio track.** Other platforms are not supported by this fork. The fork is currently local and has not been published to GitHub.

The plugin encodes the supplied render target; it does not capture a viewport, an XR swap-chain image, or an audio source.

## Blueprint use

1. Create or select a fixed-size render target with **BGRA8** or **RGBA16F** format, and supply image content to it (for example, with a `SceneCapture2D`). Keep the render target and its producer alive at the same dimensions until recording completes. HDR source values are converted to 8-bit SDR for MP4. If the selected output dimensions differ in aspect ratio from the render target, black letterbox/pillarbox bars preserve the source aspect rather than stretching the video.
2. On `VideoCaptureSubsystem` (Game Instance subsystem), call `StartCapture` with the render target, caller-selected `.mp4` filename, even output width and height, frames per second, and bitrate **in bits per second**. `StartCapture` returns success plus an error string. An existing file at the supplied path may be overwritten.
3. Call `StopCapture` once. It returns without waiting for the encoder; do not start another recording until `OnCaptureFinished` fires. That Blueprint event provides filename, error (empty on success), and dropped-frame count. `IsRecording` remains true while finalising.

The GPU copy uses UE 5.4's asynchronous texture readback. Frame copies into a bounded CPU queue occur on the render thread when readback is ready; FFmpeg colour conversion, H.264 encoding, and file writes run on a worker thread. If buffers fill, frames are dropped rather than blocking the game/render thread. The output is constant-frame-rate at the requested FPS; dropped frames may shorten playback relative to wall time. Finalisation errors are reported on the completion event. No audio stream is created.

## Limits and verification

- Output width and height must be even and in 2–3840; FPS must be 1–120; bitrate must be 1–100,000,000 bps. Source width × height must not exceed 3840². Extremely different aspect ratios that cannot fit at least 2 even pixels in both dimensions are rejected. Source resizing during capture is unsupported. Rendering still costs GPU time; no VR frame-time guarantee follows from background encoding.
- Tested with Unreal Engine 5.4.4 on Windows using a `SceneCapture2D` feeding a 1280×720 RGBA16F render target. Two consecutive recordings at 320×180/15 fps/1 Mbps and 640×360/30 fps/2 Mbps were playable. Invalid output dimensions and duplicate Start were rejected. This does **not** validate other image producers, packaged playback, source ownership on abrupt shutdown, or licence compliance of the bundled FFmpeg DLLs.
- The upstream plugin source is MIT-licensed. The bundled FFmpeg binaries have no separate provenance/licence record in this fork; establish their build flags and redistribution terms before shipping. Do not commit or distribute generated binaries as a consequence of this prototype test.
