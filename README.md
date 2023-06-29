# Spotlight


<img src="artifacts/logo.png" align="left" style="display: inline; width: 250px; height: auto; margin-right: 30px; margin-bottom: 30px">

<p name="introduction" style="padding-bottom: 90px">
<img src="https://img.shields.io/badge/Made%20with%20C-A8B9CC?logo=C&style=for-the-badge&labelColor=111111" />
<br><br>
<b>Spotlight</b> is an open-source program designed to provide Linux users with a powerful circular screen capture and recording solution, inspired by NVIDIA's ShadowPlay software.
</p>

> **NOTE**
> Spotlight only works on X11, and does not support Wayland.

> **WARNING**
> This application is likely not a good fit for systems with low resources, as it takes up a bunch of memory to buffer frames and audio in a circular buffer.
>
> If regularly have 4-8 GiB of memory free, you should be fine.
> For reference; 1080p 30 FPS with 45 second buffer take up 4.3GiB of memory. Your milage may vary.

# Table of Contents


# Features

**Spotlight** is currently in development, and as such, not all features are implemented yet. The following is a list of features that are currently implemented.

- Configurable circular video and audio buffer
- Configurable real-time video rescaling
- Audio through PulseAudio
- Separating audio devices into separate audio tracks

# Installation

## Dependencies

*Spotlight* depends on the following libraries:
- [ffmpeg](https://ffmpeg.org/)
- [libConfuse](https://github.com/libconfuse/libconfuse)
- libX11
- libXext
- pulse and pulse-simple

### Arch

```bash
pacman -S confuse libx11 libxext libpulse ffmpeg
```

## Compiling from source

```bash
git clone https://github.com/27justin/spotlight
cd spotlight
make
make install
```

You can find the config file at `~/.config/spotlight/config.cfg`.

# Usage

## Configuration

Spotlight uses a configuration file to determine the behavior of the program. By default, Spotlight looks for the configuration file at `~/.config/spotlight/config.cfg`. You can find the default configuration [here](artifacts/config.cfg)

## Running

```bash
spotlight
```

## Saving the window

Spotlight always keeps `window-size` seconds of video and audio data in memory. The `window-size` is defined in the configuration file.

You can tell Spotlight to save the window by sending it a `SIGUSR1` signal.

```bash
pkill -USR1 spotlight
```

How you send this signal is up to you.

### Using a keybind

Configure a hotkey through your window manager, some form of hotkey daemonm, or any way you like really.
An example would be to use `sxhkd`:

```bash
# ~/.config/sxhkd/sxhkdrc
super + shift + s
    # Send SIGUSR1 to spotlight
    pkill -USR1 spotlight
```


