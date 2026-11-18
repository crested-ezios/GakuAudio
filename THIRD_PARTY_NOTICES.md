# Third-Party Notices

This repository includes or interacts with the following third-party software:

- **miniaudio** (Public Domain / MIT-0) — https://github.com/mackron/miniaudio (`deps/miniaudio/miniaudio.h`)
- **stb_vorbis** (Public Domain / MIT) — https://github.com/nothings/stb (`deps/miniaudio/stb_vorbis.c`)
- **GakuLoader Plugin SDK** (MIT License) — `include/GakuPlugin.h`, `include/GakuPaths.h`, `include/GakuPaths.hpp`

DSP architecture reference note:
- Preset categories and signal-chain topology (`BT` shelf + 10-band EQ + stereoizer + early-reflection/tank reverb + 3-band wet tone control) were inspired conceptually by the audio effect feature analysis in `AllenHeartcore/audioeffect-ncm`. All DSP filters, reverb algorithms, and preset gains in `GakuAudio` are independently authored clean-room MIT implementations with no copied source code or encrypted upstream assets.
