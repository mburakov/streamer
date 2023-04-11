# Streamer

This is a lightweight framebuffer streamer. It continuously captures framebuffer with 60 fps rate, converts it into YUV colorspace using GLSL shader, compresses resulting buffers into HEVC bitstream using VA-API libavcodec backend, and sends the resulting bitstream to a connected client over tcp connection. Everything is done hardware-accelerated and zero-copy. Streamer receives input events from a connected client over the same tcp connection, and forwards those to kernel using uhid api. This streaming server is accompanied by a lightweight [receiver](https://github.com/mburakov/receiver) client.

## Building on Linux

Streamer depends on following libraries:
* egl
* gbm
* glesv2
* libavcodec
* libavutil
* libdrm
* libva

Once you have these installed, just
```
make
```

## Building anywhere else

I don't care about any other platforms except Linux, so you are on your own. Moreover, I don't really expect it would work anywhere else.

## Running

There are couple of things streamer implies. I.e. that the system supports KMS and that it's actually possible to access framebuffers via libdrm. This is certainly the case with Intel and AMD when using opensource drivers. This is certainly not the case with Nvidia and proprietary drivers. Same stands for hardware-accelerated colorspace conversion. I don't really expect Nvidia to support [EGL_EXT_image_dma_buf_import](https://registry.khronos.org/EGL/extensions/EXT/EGL_EXT_image_dma_buf_import.txt) and related infrastructure. Hardware-accelerated video decoding is achieved by means of libavcodec and VA-API, and Nvidia does not support the latter. I guess you got the point already - no Nvidia please.

Since streamer requires read access to framebuffer and read-write access to uhid device, it is recommended to run it as root. Provide listening port number on commandline as a single argument:
```
sudo ./streamer 1337
```

After starting, streamer would wait for incoming connections from [receiver](https://github.com/mburakov/receiver) on the specified port. Streamer does not do capturing until receiver is conencted.

## What about Steam Link?

For a long time I was suffering from various issues with Steam Link:
* client builtin into usual desktop steam client does not support hardware-accelerated video decoding,
* decicated steam link application is redistributed as a flatpak, and requires to install related bloatware,
* streaming occasionally crashes on server-side bringing down Steam together with streamed game,
* using a mouse wheel makes server-side Steam to crash immediately together with streamed game,
* only back screen is streamed unless server-side Steam is running in big picture mode,
* one of recent updates broke streaming entirely, producing black screen even when running server-side in big picture mode,
* many other minor issues...

## What about Sunshine/Moonlight?

I tried this one too. Issues there are not as severe as with Steam Link, but still:
* relies on Qt, boost and other questionable technologies,
* does not properly propagate mouse wheel events making it pretty much unusable,
* video quality occasionally degrades into a blurry brownish mess and never recovers,
* extremely thin Moonlight video-related configuration options are seem to be ignored entirely by Sunshine,
* Sunshine crashed on me when I attempted to stream a game running with Proton.

## But there's no audio

This is correct, audio streaming is not supported as of today. Actually pulseaudio has quite a decent implementation of forwarding audio streams over network. When using module-native-protocol-tcp combined with module-tunnel-sink I only get minor distortions when connecting bluetooth headset. Connecting wired headphones produces seamless experience. That said, I might consider adding audio streaming support to the project.

## Fancy features support status

There are no fancy features in streamer. There's no bitrate control - libavcodec configuration selects constant image quality over constant bitrate. There's no frame pacing - because I personally consider it useless for low-latency realtime streaming. There's no network discovery. There's no automatic reconnection. There's no codec selection. There's no fancy configuration interface. There are no options at all. I might consider implementing some of that in the future - or might not, because it works perfectly fine for my use-case in its current state.

At the same time, it addresses all of the issues listed above for Steam Link and Sunshine/Moonlight. No issues with controls, no issue with video quality, no issues with screen capturing. On top of that instant startup and shutdown both on server- and client-side.

## Where is toolbox?

Note, that I don't use github for actual development anymore - it's just a mirror these days. Instead, I self-host git repos on https://burakov.eu. Read-only access is provided via cgit, i.e.: https://burakov.eu/streamer.git. Same stands for toolbox submodule, which is fetched via https using git commandline. You can as well access the code of toolbox directly using your browser: https://burakov.eu/toolbox.git.
