# GakuAudio

基于 `miniaudio` 与 Windows Media Foundation 的 GakuLoader 原生音频能力插件，为 `GakuMotion` 与 `VideoExporter` 提供多格式本地音频播放、标签/封面解析、实时空间音效与离线混音渲染服务（`gakuaudio.core/v1`）。

## 核心能力

- **主流本地音频解码**：原生支持 `.wav`（PCM / IEEE Float / `WAVE_FORMAT_EXTENSIBLE`）、`.mp3`、`.flac`、`.ogg`，并通过 Windows Media Foundation 扩展支持 `.m4a`、`.aac`、`.opus`、`.wma`（严格限定本地文件）。
- **元数据与内嵌封面读取**：支持解析 ID3v2.2/2.3/2.4 (`TIT2`/`TPE1`/`TALB`/`APIC`)、ID3v1、FLAC (`STREAMINFO`/`VORBIS_COMMENT`/`PICTURE`)、OGG/Opus (`METADATA_BLOCK_PICTURE`)、MP4/M4A (`ilst` / `covr`) 及 WAV `LIST/INFO`。
- **实时音频效果节点 (`AudioDspGraph`)**：基于 `miniaudio` `ma_node_base` 构建的多段参数均衡、分频声场扩展、早反射 + 多抽头立体声混响与柔限幅管线，支持 **35 ms 无缝淡入淡出切换**：
  - `off`（原声）
  - `live`（演唱会现场）
  - `livehouse`（LiveHouse现场）
  - `bass`（极重低音）
  - `vocal`（清澈人声）
  - `concert_hall`（音乐厅）
  - `surround_3d`（3D环绕）
  - `acg`（纯净ACG）
- **离线渲染输出 (`renderOfflineWav`)**：供 `VideoExporter` 将曲目时间映射（倍速、音轨偏移、音量）与选定舞台音效烘焙至 48 kHz 立体声 WAV 后与视频流合流。

## 源码构建与测试

```powershell
powershell -NoProfile -File build.ps1 -Target All
```
