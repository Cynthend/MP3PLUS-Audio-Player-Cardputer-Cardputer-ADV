<div align="center">

MP3PLUS Audio Player for M5Stack Cardputer-Adv & Cardputer

<b>🇬🇧 English</b> | <a href="README.ru.md">🇷🇺 Русский</a> | <a href="README.es.md">🇪🇸 Español</a>

License: GPL v3

Built for people who actually carry their entire music library in their pocket. No PSRAM, no lag, no compromises.

</div>

Why MP3PLUS Audio Player?
I was missing a lot of key features and used the principle: “If you want something done right, do it yourself", and i did.
Most Cardputer's audio players hit a wall around 256–2,000 tracks, choke on Cyrillic filenames, or freeze up when you hit shuffle or scroll. MP3+ was engineered from the ground up to squeeze every last drop out of the ESP32-S3's 320 KB usable SRAM: I tried my best!

Up to 4,096 tracks per folder - filenames are indexed cleanly on microSD (/.mp3plus.idx) with a 16-slot RAM hot cache. Tested with ~1,000+ real tracks in a single directory with zero hiccups.
0 ms Instant Shuffle (S) - non-destructive Fisher-Yates permutation in RAM. Turn it on to shuffle; turn it off to jump right back to your exact original track order without touching your files.
Formats: MP3, M4A (AAC), WAV - smooth dual-core decoding on Core 0 with zero PSRAM required.
Hardware Amp Boost (A) 100% / 150% (+6 dB) / 200% (+11 dB) direct ES8311 DAC gain control on Cardputer-Adv, plus native 3.5mm headphone jack auto-switching.
2-in-1 Hardware Support - auto-detects M5Stack Cardputer-Adv and original M5Stack Cardputer from a single .bin.
5-Zone AGC Visualizer (V) real-time 3-band integer DSP with volume-aware Auto Gain Control so bars stay lively even at low volume (03). Modes: BARS, PEAK, WAVE, OFF.
Full Cyrillic (UTF-8) Support — Russian & English track names render cleanly across 3 font sizes (G).
Pocket Mode (G0 side button) - control playback with the screen off: 1× Play/Pause, 2× Next, 3× Previous.
Battery & Eye Friendly - 6 color themes (T), clean 20%–100% brightness steps, Screen Sleep timer, 160 MHz CPU Eco mode while sleeping, and automatic NVS settings save.
Quick Install (M5Launcher)
Grab the latest .bin
Drop it onto your FAT32 microSD card (e.g. /MP3PLUS.bin).
Boot M5Launcher on your Cardputer, pick the .bin, and hit Install (or Run).
Put your music folders anywhere on the SD card and press F to browse.
Controls
Key	Action
Enter / Space	Play / Pause / Open folder (Space in folder browser plays the whole folder)
; / .	Navigate up / down (hold to fast-scroll)
N / B	Next / Previous track
/ / ,	Seek forward / backward (5s or 10s, hold to scrub)
[ / ] or - / =	Volume down / up (0–21)
A	Cycle Hardware Amp Boost (100% / 150% / 200%)
S	Toggle Instant Non-Destructive Shuffle
L	Cycle Loop Mode (NORM / RP1X / RPINF / RPALL)
R	Toggle Reverse playback order (FWD ↔ REV)
F	Open Folder Browser
T / V / G	Cycle Theme / Visualizer / Font Size
M / I / Esc	Settings / Hotkeys Help / Back
G0 (side button)	Pocket Mode: 1× Play/Pause · 2× Next · 3× Prev

Custom Features & Support:
This project is 100% free and open-source — and always will be. Unfortunately, bills can't pay themselves 💀

If MP3+ turned your Cardputer into your favorite pocket DAP, you can toss a coffee my way, or even commission custom features, bespoke boot animations, themes, or tailored firmware builds for a small fee. Feel free to ask.

Donate / Support: 
- USDT (TRC20): TF7kf8QxKoXrHq8ixnaYG5zDXBBuRP5FWb 
- BTC: bc1qef8762lcc72zxz5w6kac4vztpjuw5vxlxaal5c


Custom Dev & Commissions: Farbender@protonmail.com

Credits & License:
Powered by ESP32-audioI2S & M5Unified/M5GFX. Released under the GNU General Public License v3.0 (
GPL-3.0).
