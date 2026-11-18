// SPDX-License-Identifier: MIT
#pragma once

#include "../include/GakuAudio.h"
#include "../include/GkAudioMetadata.hpp"
#include "../deps/miniaudio/miniaudio.h"
#include "AudioDspGraph.hpp"
#include "AudioMfDecoder.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace GakuAudio
{
	class AudioEngine
	{
	public:
		AudioEngine() = default;
		~AudioEngine() { Shutdown(); }

		void SetConfigPath(const std::filesystem::path& path)
		{
			std::lock_guard<std::mutex> lock(mutex_);
			configPath_ = path;
			LoadConfigLocked();
		}

		void Shutdown()
		{
			std::lock_guard<std::mutex> lock(mutex_);
			CloseSourceLocked();
			if (dspNodeInitialized_)
			{
				ma_node_uninit(&dspNode_, nullptr);
				dspNodeInitialized_ = false;
			}
			if (engineInitialized_)
			{
				ma_engine_uninit(&engine_);
				engineInitialized_ = false;
			}
		}

		GakuStatus OpenAndPlay(const char* pathUtf8, double startSeconds)
		{
			if (!pathUtf8 || !pathUtf8[0] || GakuAudioMeta::IsNetworkPath(pathUtf8))
				return GAKU_E_INVALID_ARGUMENT;

			const std::filesystem::path filePath = GakuAudioMeta::PathFromUtf8(pathUtf8);
			if (filePath.empty()) return GAKU_E_INVALID_ARGUMENT;

			std::lock_guard<std::mutex> lock(mutex_);
			if (!EnsureEngineLocked()) return GAKU_E_UNAVAILABLE;

			CloseSourceLocked();
			lastPathUtf8_ = pathUtf8;
			lastErrorCode_ = 0;

			const std::wstring wide = filePath.wstring();
			ma_decoder_config decCfg = ma_decoder_config_init(ma_format_f32, 2, 0);
			if (ma_decoder_init_file_w(wide.c_str(), &decCfg, &decoder_) == MA_SUCCESS)
			{
				sourceKind_ = SourceKind::MiniaudioDecoder;
				activeDataSource_ = &decoder_.ds;
				ma_format fmt = ma_format_unknown;
				ma_uint32 ch = 0, sr = 0;
				ma_decoder_get_data_format(&decoder_, &fmt, &ch, &sr, nullptr, 0);
				ma_uint64 len = 0;
				ma_decoder_get_length_in_pcm_frames(&decoder_, &len);
				sourceSampleRate_ = sr;
				sourceChannels_ = ch;
				sourceTotalFrames_ = len;
			}
			else if (InitMfDataSource(wide, mfSource_))
			{
				sourceKind_ = SourceKind::MediaFoundation;
				activeDataSource_ = &mfSource_.ds;
				sourceSampleRate_ = mfSource_.sampleRate;
				sourceChannels_ = mfSource_.channels;
				sourceTotalFrames_ = mfSource_.totalFrames;
			}
			else
			{
				state_ = GAKU_AUDIO_FAILED;
				lastErrorCode_ = -1;
				return GAKU_E_UNAVAILABLE;
			}

			if (sourceSampleRate_ == 0 || sourceChannels_ == 0)
			{
				CloseSourceLocked();
				state_ = GAKU_AUDIO_FAILED;
				lastErrorCode_ = -2;
				return GAKU_E_INTERNAL;
			}

			const double safeStart = (std::isfinite(startSeconds) && startSeconds > 0.0) ? startSeconds : 0.0;
			const ma_uint64 startFrame = static_cast<ma_uint64>(safeStart * static_cast<double>(sourceSampleRate_));

			ma_sound_config sc = ma_sound_config_init_2(&engine_);
			sc.pDataSource = activeDataSource_;
			sc.pInitialAttachment = &dspNode_.base;
			sc.initialAttachmentInputBusIndex = 0;
			sc.channelsOut = 2;
			sc.flags = MA_SOUND_FLAG_NO_SPATIALIZATION;
			sc.initialSeekPointInPCMFrames = startFrame;

			const ma_result sr = ma_sound_init_ex(&engine_, &sc, &sound_);
			if (sr != MA_SUCCESS)
			{
				CloseSourceLocked();
				state_ = GAKU_AUDIO_FAILED;
				lastErrorCode_ = static_cast<int32_t>(sr);
				return GAKU_E_INTERNAL;
			}
			soundInitialized_ = true;
			ma_sound_set_volume(&sound_, volume_);
			ma_sound_set_pitch(&sound_, playbackRate_);
			const ma_result playRc = ma_sound_start(&sound_);
			if (playRc != MA_SUCCESS)
			{
				CloseSourceLocked();
				state_ = GAKU_AUDIO_FAILED;
				lastErrorCode_ = static_cast<int32_t>(playRc);
				return GAKU_E_INTERNAL;
			}

			if (++sourceEpoch_ == 0) sourceEpoch_ = 1;
			state_ = GAKU_AUDIO_PLAYING;
			return GAKU_OK;
		}

		GakuStatus Pause()
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (!soundInitialized_) return GAKU_E_UNAVAILABLE;
			ma_sound_stop(&sound_);
			state_ = GAKU_AUDIO_PAUSED;
			return GAKU_OK;
		}

		GakuStatus ResumeAt(double seconds)
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (!soundInitialized_)
			{
				if (lastPathUtf8_.empty()) return GAKU_E_UNAVAILABLE;
				const std::string pathCopy = lastPathUtf8_;
				// Reopen cleanly at requested offset.
				return OpenAndPlayImplLocked(pathCopy.c_str(), seconds);
			}
			const double safeSec = (std::isfinite(seconds) && seconds > 0.0) ? seconds : 0.0;
			const ma_uint64 targetFrame = static_cast<ma_uint64>(safeSec * static_cast<double>(sourceSampleRate_));
			ma_sound_seek_to_pcm_frame(&sound_, targetFrame);
			ma_sound_set_volume(&sound_, volume_);
			ma_sound_set_pitch(&sound_, playbackRate_);
			const ma_result rc = ma_sound_start(&sound_);
			if (rc != MA_SUCCESS)
			{
				state_ = GAKU_AUDIO_FAILED;
				lastErrorCode_ = static_cast<int32_t>(rc);
				return GAKU_E_INTERNAL;
			}
			state_ = GAKU_AUDIO_PLAYING;
			lastErrorCode_ = 0;
			return GAKU_OK;
		}

		GakuStatus Seek(double seconds, bool whilePlaying)
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (!soundInitialized_) return GAKU_E_UNAVAILABLE;
			const double safeSec = (std::isfinite(seconds) && seconds > 0.0) ? seconds : 0.0;
			const ma_uint64 targetFrame = static_cast<ma_uint64>(safeSec * static_cast<double>(sourceSampleRate_));
			const ma_result seekRc = ma_sound_seek_to_pcm_frame(&sound_, targetFrame);
			if (seekRc != MA_SUCCESS)
			{
				lastErrorCode_ = static_cast<int32_t>(seekRc);
				return GAKU_E_INTERNAL;
			}
			if (whilePlaying)
			{
				ma_sound_start(&sound_);
				state_ = GAKU_AUDIO_PLAYING;
			}
			else
			{
				ma_sound_stop(&sound_);
				state_ = GAKU_AUDIO_PAUSED;
			}
			lastErrorCode_ = 0;
			return GAKU_OK;
		}

		GakuStatus Stop()
		{
			std::lock_guard<std::mutex> lock(mutex_);
			CloseSourceLocked();
			state_ = GAKU_AUDIO_STOPPED;
			lastErrorCode_ = 0;
			return GAKU_OK;
		}

		GakuStatus Observe(GakuAudioObservationV1* output)
		{
			if (!output || output->structSize < sizeof(GakuAudioObservationV1))
				return GAKU_E_INVALID_ARGUMENT;
			std::lock_guard<std::mutex> lock(mutex_);
			GakuAudioObservationV1 obs{};
			obs.abiVersion = GAKU_AUDIO_VERSION;
			obs.structSize = sizeof(GakuAudioObservationV1);
			obs.sourceOpen = soundInitialized_ ? 1u : 0u;
			obs.errorCode = lastErrorCode_;
			obs.sourceEpoch = sourceEpoch_;
			obs.sampleRate = sourceSampleRate_;
			obs.channels = sourceChannels_;
			obs.durationSeconds = (sourceSampleRate_ > 0 && sourceTotalFrames_ > 0)
				? static_cast<double>(sourceTotalFrames_) / static_cast<double>(sourceSampleRate_)
				: 0.0;

			if (soundInitialized_)
			{
				ma_uint64 cursor = 0;
				if (ma_sound_get_cursor_in_pcm_frames(&sound_, &cursor) == MA_SUCCESS && sourceSampleRate_ > 0)
					obs.positionSeconds = static_cast<double>(cursor) / static_cast<double>(sourceSampleRate_);
				if (state_ == GAKU_AUDIO_PLAYING && ma_sound_at_end(&sound_))
					state_ = GAKU_AUDIO_STOPPED;
				obs.confirmed = (lastErrorCode_ == 0) ? 1u : 0u;
			}
			else
			{
				obs.confirmed = (state_ == GAKU_AUDIO_STOPPED && lastErrorCode_ == 0) ? 1u : 0u;
			}
			obs.state = state_;
			*output = obs;
			return GAKU_OK;
		}

		GakuStatus SetVolume(float volume)
		{
			if (!std::isfinite(volume)) return GAKU_E_INVALID_ARGUMENT;
			std::lock_guard<std::mutex> lock(mutex_);
			volume_ = std::clamp(volume, 0.0f, 2.0f);
			if (soundInitialized_) ma_sound_set_volume(&sound_, volume_);
			return GAKU_OK;
		}

		GakuStatus SetPlaybackRate(float rate)
		{
			if (!std::isfinite(rate)) return GAKU_E_INVALID_ARGUMENT;
			std::lock_guard<std::mutex> lock(mutex_);
			playbackRate_ = std::clamp(rate, 0.1f, 5.0f);
			if (soundInitialized_) ma_sound_set_pitch(&sound_, playbackRate_);
			return GAKU_OK;
		}

		GakuStatus ProbeFile(const char* pathUtf8, GakuAudioMetadataV1* output)
		{
			if (!pathUtf8 || !pathUtf8[0] || !output || output->structSize < sizeof(GakuAudioMetadataV1))
				return GAKU_E_INVALID_ARGUMENT;
			if (GakuAudioMeta::IsNetworkPath(pathUtf8))
				return GAKU_E_INVALID_ARGUMENT;

			GakuAudioMeta::Metadata meta = GakuAudioMeta::ProbeFileUtf8(pathUtf8);
			const auto fsPath = GakuAudioMeta::PathFromUtf8(pathUtf8);
			if (!meta.valid && !fsPath.empty() && GakuAudioMeta::IsSupportedAudioExtension(fsPath.extension().string()))
			{
				uint32_t sr = 0, ch = 0;
				double dur = 0.0;
				if (ProbeViaMediaFoundation(fsPath.wstring(), sr, ch, dur))
				{
					meta.valid = true;
					meta.sampleRate = sr;
					meta.channels = ch;
					meta.durationSeconds = dur;
					meta.codec = GakuAudioMeta::LowerAscii(fsPath.extension().string());
					if (!meta.codec.empty() && meta.codec[0] == '.') meta.codec.erase(meta.codec.begin());
				}
			}

			GakuAudioMetadataV1 out{};
			out.abiVersion = GAKU_AUDIO_VERSION;
			out.structSize = sizeof(GakuAudioMetadataV1);
			out.valid = meta.valid ? 1u : 0u;
			out.sampleRate = meta.sampleRate;
			out.channels = meta.channels;
			out.hasCover = !meta.coverBytes.empty() ? 1u : 0u;
			out.coverByteSize = static_cast<uint32_t>((std::min<size_t>)(meta.coverBytes.size(), 0xFFFFFFFFu));
			out.durationSeconds = meta.durationSeconds;
			CopyBounded(out.codec, sizeof(out.codec), meta.codec);
			CopyBounded(out.title, sizeof(out.title), meta.title);
			CopyBounded(out.artist, sizeof(out.artist), meta.artist);
			CopyBounded(out.album, sizeof(out.album), meta.album);
			CopyBounded(out.coverMime, sizeof(out.coverMime), meta.coverMime);
			*output = out;
			return meta.valid ? GAKU_OK : GAKU_E_UNAVAILABLE;
		}

		GakuStatus ExtractCover(const char* pathUtf8, void* outBuffer, uint32_t bufferCapacity,
			uint32_t* outBytes, char* outMime32)
		{
			if (!pathUtf8 || !pathUtf8[0] || !outBytes || GakuAudioMeta::IsNetworkPath(pathUtf8))
				return GAKU_E_INVALID_ARGUMENT;
			const GakuAudioMeta::Metadata meta = GakuAudioMeta::ProbeFileUtf8(pathUtf8);
			if (meta.coverBytes.empty())
			{
				*outBytes = 0;
				if (outMime32) outMime32[0] = '\0';
				return GAKU_E_UNAVAILABLE;
			}
			const uint32_t req = static_cast<uint32_t>(meta.coverBytes.size());
			*outBytes = req;
			if (outMime32) CopyBounded(outMime32, 32, meta.coverMime);
			if (!outBuffer || bufferCapacity < req) return GAKU_E_SIZE;
			std::memcpy(outBuffer, meta.coverBytes.data(), req);
			return GAKU_OK;
		}

		uint32_t GetPresetCount() const noexcept
		{
			return static_cast<uint32_t>(kBuiltinPresets.size());
		}

		GakuStatus GetPreset(uint32_t index, GakuAudioEffectPresetV1* output) const noexcept
		{
			if (!output || output->structSize < sizeof(GakuAudioEffectPresetV1))
				return GAKU_E_INVALID_ARGUMENT;
			if (index >= kBuiltinPresets.size()) return GAKU_E_INVALID_ARGUMENT;
			const auto& p = kBuiltinPresets[index];
			GakuAudioEffectPresetV1 item{};
			item.abiVersion = GAKU_AUDIO_VERSION;
			item.structSize = sizeof(GakuAudioEffectPresetV1);
			CopyBounded(item.id, sizeof(item.id), p.id);
			CopyBounded(item.name, sizeof(item.name), p.name);
			CopyBounded(item.category, sizeof(item.category), p.category);
			CopyBounded(item.description, sizeof(item.description), p.description);
			*output = item;
			return GAKU_OK;
		}

		GakuStatus GetActivePreset(char* outId, uint32_t capacity) const noexcept
		{
			if (!outId || capacity == 0) return GAKU_E_INVALID_ARGUMENT;
			const uint32_t idx = dspProcessor_.ActivePresetIndex();
			const char* id = idx < kBuiltinPresets.size() ? kBuiltinPresets[idx].id : "off";
			CopyBounded(outId, capacity, id);
			return GAKU_OK;
		}

		GakuStatus SetActivePreset(const char* presetId)
		{
			if (!presetId || !presetId[0]) return GAKU_E_INVALID_ARGUMENT;
			uint32_t foundIdx = 0;
			bool found = false;
			for (uint32_t i = 0; i < static_cast<uint32_t>(kBuiltinPresets.size()); ++i)
			{
				if (std::strcmp(presetId, kBuiltinPresets[i].id) == 0)
				{
					foundIdx = i;
					found = true;
					break;
				}
			}
			if (!found) return GAKU_E_INVALID_ARGUMENT;
			dspProcessor_.RequestPreset(foundIdx);
			std::lock_guard<std::mutex> lock(mutex_);
			activePresetId_ = kBuiltinPresets[foundIdx].id;
			SaveConfigLocked();
			return GAKU_OK;
		}

		GakuStatus RenderOfflineWav(const GakuAudioOfflineRenderRequestV1* req, char* errBuf, uint32_t errCap)
		{
			auto setErr = [&](const char* msg)
			{
				if (errBuf && errCap > 0) CopyBounded(errBuf, errCap, msg ? msg : "");
			};
			if (!req || req->structSize < sizeof(GakuAudioOfflineRenderRequestV1) ||
			    !req->inputPathUtf8 || !req->inputPathUtf8[0] ||
			    !req->outputWavPathUtf8 || !req->outputWavPathUtf8[0])
			{
				setErr("Invalid offline render request");
				return GAKU_E_INVALID_ARGUMENT;
			}
			if (GakuAudioMeta::IsNetworkPath(req->inputPathUtf8) || GakuAudioMeta::IsNetworkPath(req->outputWavPathUtf8))
			{
				setErr("Network paths are not supported");
				return GAKU_E_INVALID_ARGUMENT;
			}

			const std::filesystem::path inPath = GakuAudioMeta::PathFromUtf8(req->inputPathUtf8);
			const std::filesystem::path outPath = GakuAudioMeta::PathFromUtf8(req->outputWavPathUtf8);
			if (inPath.empty() || outPath.empty())
			{
				setErr("Invalid UTF-8 audio file path");
				return GAKU_E_INVALID_ARGUMENT;
			}

			// Decode entire track cleanly into float32 stereo at source sample rate.
			ma_decoder dec{};
			MfDataSource mfSrc{};
			ma_data_source* srcPtr = nullptr;
			uint32_t srcSr = 0;
			bool useMaDec = false;

			ma_decoder_config decCfg = ma_decoder_config_init(ma_format_f32, 2, 0);
			if (ma_decoder_init_file_w(inPath.wstring().c_str(), &decCfg, &dec) == MA_SUCCESS)
			{
				ma_uint32 ch = 0;
				ma_decoder_get_data_format(&dec, nullptr, &ch, &srcSr, nullptr, 0);
				srcPtr = &dec.ds;
				useMaDec = true;
			}
			else if (InitMfDataSource(inPath.wstring(), mfSrc))
			{
				srcSr = mfSrc.sampleRate;
				srcPtr = &mfSrc.ds;
			}
			else
			{
				setErr("Failed to decode input audio file");
				return GAKU_E_UNAVAILABLE;
			}

			auto cleanupDec = [&]()
			{
				if (useMaDec) ma_decoder_uninit(&dec);
				else UninitMfDataSource(mfSrc);
			};

			if (srcSr == 0)
			{
				cleanupDec();
				setErr("Invalid decoded audio sample rate");
				return GAKU_E_INTERNAL;
			}

			uint32_t srcChannels = 2;
			if (!useMaDec) srcChannels = mfSrc.channels;

			std::vector<float> sourceStereo;
			std::vector<float> chunk(8192 * srcChannels);
			while (true)
			{
				ma_uint64 readFrames = 0;
				const ma_result rc = ma_data_source_read_pcm_frames(srcPtr, chunk.data(), 8192, &readFrames);
				if (readFrames > 0)
				{
					const size_t base = sourceStereo.size();
					sourceStereo.resize(base + static_cast<size_t>(readFrames) * 2);
					for (ma_uint64 i = 0; i < readFrames; ++i)
					{
						if (srcChannels == 1)
						{
							const float s = chunk[i];
							sourceStereo[base + i * 2] = s;
							sourceStereo[base + i * 2 + 1] = s;
						}
						else
						{
							sourceStereo[base + i * 2] = chunk[i * srcChannels];
							sourceStereo[base + i * 2 + 1] = chunk[i * srcChannels + 1];
						}
					}
				}
				if (rc != MA_SUCCESS || readFrames == 0) break;
			}
			cleanupDec();

			const uint64_t totalSrcFrames = sourceStereo.size() / 2;
			if (totalSrcFrames == 0)
			{
				setErr("Decoded audio contains zero frames");
				return GAKU_E_UNAVAILABLE;
			}

			const uint32_t outSr = req->targetSampleRate > 0 ? req->targetSampleRate : 48000u;
			const double effectiveRate = std::clamp(
				(std::isfinite(req->audioSpeed) && req->audioSpeed > 0.0f ? static_cast<double>(req->audioSpeed) : 1.0) *
				(std::isfinite(req->playbackSpeed) && req->playbackSpeed > 0.0f ? static_cast<double>(req->playbackSpeed) : 1.0),
				0.1, 5.0);
			const double offsetSec = std::isfinite(req->trimOffsetSeconds) ? req->trimOffsetSeconds : 0.0;
			const double srcDuration = static_cast<double>(totalSrcFrames) / static_cast<double>(srcSr);
			double outDuration = (std::isfinite(req->timelineDurationSeconds) && req->timelineDurationSeconds > 0.0)
				? req->timelineDurationSeconds
				: (std::max)(0.1, srcDuration / effectiveRate + (std::max)(0.0, offsetSec));
			const uint64_t totalOutFrames = static_cast<uint64_t>(std::llround(outDuration * static_cast<double>(outSr)));
			const float vol = std::isfinite(req->volume) ? std::clamp(req->volume, 0.0f, 2.0f) : 1.0f;

			uint32_t presetIdx = dspProcessor_.ActivePresetIndex();
			if (req->effectPresetId && req->effectPresetId[0])
				presetIdx = FindPresetIndex(req->effectPresetId);

			DspProcessor offlineDsp;
			offlineDsp.Init(outSr, presetIdx);

			std::error_code ec;
			if (!outPath.parent_path().empty())
				std::filesystem::create_directories(outPath.parent_path(), ec);
			std::filesystem::path tmpPath = outPath;
			tmpPath += L".tmp";

			std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
			if (!out)
			{
				setErr("Failed to create output WAV file");
				return GAKU_E_INTERNAL;
			}

			// Write 44-byte RIFF/WAVE header (16-bit PCM stereo)
			const uint32_t dataBytes = static_cast<uint32_t>((std::min<uint64_t>)(totalOutFrames * 4ULL, 0x7FFFFFFFu));
			const uint32_t riffSize = 36u + dataBytes;
			const uint16_t audioFmt = 1, ch = 2, blockAlign = 4, bps = 16;
			const uint32_t byteRate = outSr * 4u, fmtSize = 16u;
			out.write("RIFF", 4);
			out.write(reinterpret_cast<const char*>(&riffSize), 4);
			out.write("WAVEfmt ", 8);
			out.write(reinterpret_cast<const char*>(&fmtSize), 4);
			out.write(reinterpret_cast<const char*>(&audioFmt), 2);
			out.write(reinterpret_cast<const char*>(&ch), 2);
			out.write(reinterpret_cast<const char*>(&outSr), 4);
			out.write(reinterpret_cast<const char*>(&byteRate), 4);
			out.write(reinterpret_cast<const char*>(&blockAlign), 2);
			out.write(reinterpret_cast<const char*>(&bps), 2);
			out.write("data", 4);
			out.write(reinterpret_cast<const char*>(&dataBytes), 4);

			constexpr size_t kBlock = 4096;
			std::vector<float> dryBlock(kBlock * 2);
			std::vector<float> wetBlock(kBlock * 2);
			std::vector<int16_t> pcmBlock(kBlock * 2);

			uint64_t frameCursor = 0;
			const uint64_t writableFrames = dataBytes / 4u;
			while (frameCursor < writableFrames)
			{
				const size_t n = static_cast<size_t>((std::min<uint64_t>)(kBlock, writableFrames - frameCursor));
				for (size_t i = 0; i < n; ++i)
				{
					const double timelineSec = static_cast<double>(frameCursor + i) / static_cast<double>(outSr);
					const double srcSec = (timelineSec - offsetSec) * effectiveRate;
					float l = 0.0f, r = 0.0f;
					if (srcSec >= 0.0)
					{
						const double pos = srcSec * static_cast<double>(srcSr);
						const uint64_t idx0 = static_cast<uint64_t>(pos);
						if (idx0 + 1 < totalSrcFrames)
						{
							const float frac = static_cast<float>(pos - static_cast<double>(idx0));
							const float l0 = sourceStereo[idx0 * 2], r0 = sourceStereo[idx0 * 2 + 1];
							const float l1 = sourceStereo[(idx0 + 1) * 2], r1 = sourceStereo[(idx0 + 1) * 2 + 1];
							l = (l0 + (l1 - l0) * frac) * vol;
							r = (r0 + (r1 - r0) * frac) * vol;
						}
						else if (idx0 < totalSrcFrames)
						{
							l = sourceStereo[idx0 * 2] * vol;
							r = sourceStereo[idx0 * 2 + 1] * vol;
						}
					}
					dryBlock[i * 2] = l;
					dryBlock[i * 2 + 1] = r;
				}
				offlineDsp.ProcessStereo(dryBlock.data(), wetBlock.data(), n);
				for (size_t i = 0; i < n * 2; ++i)
				{
					const float clamped = std::clamp(wetBlock[i], -1.0f, 1.0f);
					pcmBlock[i] = static_cast<int16_t>(std::lrintf(clamped * 32767.0f));
				}
				out.write(reinterpret_cast<const char*>(pcmBlock.data()), static_cast<std::streamsize>(n * 4));
				frameCursor += n;
			}
			out.flush();
			const bool writeOk = out.good();
			out.close();
			if (!writeOk)
			{
				std::filesystem::remove(tmpPath, ec);
				setErr("Failed while writing offline WAV file");
				return GAKU_E_INTERNAL;
			}
			if (!MoveFileExW(tmpPath.c_str(), outPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			{
				std::filesystem::remove(tmpPath, ec);
				setErr("Failed to commit offline WAV file");
				return GAKU_E_INTERNAL;
			}
			return GAKU_OK;
		}

	private:
		enum class SourceKind { None, MiniaudioDecoder, MediaFoundation };

		static void CopyBounded(char* dst, size_t cap, std::string_view src) noexcept
		{
			if (!dst || cap == 0) return;
			const size_t n = (std::min)(cap - 1, src.size());
			if (n > 0) std::memcpy(dst, src.data(), n);
			dst[n] = '\0';
		}

		GakuStatus OpenAndPlayImplLocked(const char* pathUtf8, double startSeconds)
		{
			// Internal helper called under mutex_ when reopening from ResumeAt.
			if (!pathUtf8 || !pathUtf8[0] || GakuAudioMeta::IsNetworkPath(pathUtf8))
				return GAKU_E_INVALID_ARGUMENT;
			const std::filesystem::path filePath = GakuAudioMeta::PathFromUtf8(pathUtf8);
			if (filePath.empty() || !EnsureEngineLocked()) return GAKU_E_UNAVAILABLE;
			CloseSourceLocked();
			lastPathUtf8_ = pathUtf8;
			lastErrorCode_ = 0;
			const std::wstring wide = filePath.wstring();
			ma_decoder_config decCfg = ma_decoder_config_init(ma_format_f32, 2, 0);
			if (ma_decoder_init_file_w(wide.c_str(), &decCfg, &decoder_) == MA_SUCCESS)
			{
				sourceKind_ = SourceKind::MiniaudioDecoder;
				activeDataSource_ = &decoder_.ds;
				ma_format fmt = ma_format_unknown;
				ma_uint32 ch = 0, sr = 0;
				ma_decoder_get_data_format(&decoder_, &fmt, &ch, &sr, nullptr, 0);
				ma_uint64 len = 0;
				ma_decoder_get_length_in_pcm_frames(&decoder_, &len);
				sourceSampleRate_ = sr;
				sourceChannels_ = ch;
				sourceTotalFrames_ = len;
			}
			else if (InitMfDataSource(wide, mfSource_))
			{
				sourceKind_ = SourceKind::MediaFoundation;
				activeDataSource_ = &mfSource_.ds;
				sourceSampleRate_ = mfSource_.sampleRate;
				sourceChannels_ = mfSource_.channels;
				sourceTotalFrames_ = mfSource_.totalFrames;
			}
			else
			{
				state_ = GAKU_AUDIO_FAILED;
				lastErrorCode_ = -1;
				return GAKU_E_UNAVAILABLE;
			}
			const double safeStart = (std::isfinite(startSeconds) && startSeconds > 0.0) ? startSeconds : 0.0;
			ma_sound_config sc = ma_sound_config_init_2(&engine_);
			sc.pDataSource = activeDataSource_;
			sc.pInitialAttachment = &dspNode_.base;
			sc.initialAttachmentInputBusIndex = 0;
			sc.channelsOut = 2;
			sc.flags = MA_SOUND_FLAG_NO_SPATIALIZATION;
			sc.initialSeekPointInPCMFrames = static_cast<ma_uint64>(safeStart * static_cast<double>(sourceSampleRate_));
			if (ma_sound_init_ex(&engine_, &sc, &sound_) != MA_SUCCESS)
			{
				CloseSourceLocked();
				state_ = GAKU_AUDIO_FAILED;
				return GAKU_E_INTERNAL;
			}
			soundInitialized_ = true;
			ma_sound_set_volume(&sound_, volume_);
			ma_sound_set_pitch(&sound_, playbackRate_);
			if (ma_sound_start(&sound_) != MA_SUCCESS)
			{
				CloseSourceLocked();
				state_ = GAKU_AUDIO_FAILED;
				return GAKU_E_INTERNAL;
			}
			if (++sourceEpoch_ == 0) sourceEpoch_ = 1;
			state_ = GAKU_AUDIO_PLAYING;
			return GAKU_OK;
		}

		bool EnsureEngineLocked()
		{
			if (engineInitialized_) return true;
			ma_engine_config engCfg = ma_engine_config_init();
			engCfg.channels = 2;
			if (ma_engine_init(&engCfg, &engine_) != MA_SUCCESS)
				return false;
			engineInitialized_ = true;
			const uint32_t sr = ma_engine_get_sample_rate(&engine_);
			const uint32_t presetIdx = FindPresetIndex(activePresetId_);
			if (InitDspEffectNode(ma_engine_get_node_graph(&engine_), dspNode_, dspProcessor_, sr, presetIdx) == MA_SUCCESS)
			{
				dspNodeInitialized_ = true;
				ma_node_attach_output_bus(&dspNode_.base, 0, ma_engine_get_endpoint(&engine_), 0);
			}
			else
			{
				ma_engine_uninit(&engine_);
				engineInitialized_ = false;
				return false;
			}
			return true;
		}

		void CloseSourceLocked()
		{
			if (soundInitialized_)
			{
				ma_sound_stop(&sound_);
				ma_sound_uninit(&sound_);
				soundInitialized_ = false;
			}
			if (sourceKind_ == SourceKind::MiniaudioDecoder)
			{
				ma_decoder_uninit(&decoder_);
			}
			else if (sourceKind_ == SourceKind::MediaFoundation)
			{
				UninitMfDataSource(mfSource_);
			}
			sourceKind_ = SourceKind::None;
			activeDataSource_ = nullptr;
			sourceSampleRate_ = 0;
			sourceChannels_ = 0;
			sourceTotalFrames_ = 0;
		}

		void LoadConfigLocked()
		{
			if (configPath_.empty()) return;
			std::ifstream in(configPath_);
			if (!in) return;
			std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
			const size_t keyPos = content.find("\"activePreset\"");
			if (keyPos == std::string::npos) return;
			const size_t colon = content.find(':', keyPos);
			const size_t q1 = colon != std::string::npos ? content.find('"', colon) : std::string::npos;
			const size_t q2 = q1 != std::string::npos ? content.find('"', q1 + 1) : std::string::npos;
			if (q2 != std::string::npos && q2 > q1 + 1)
			{
				const std::string id = content.substr(q1 + 1, q2 - q1 - 1);
				const uint32_t idx = FindPresetIndex(id);
				activePresetId_ = kBuiltinPresets[idx].id;
				dspProcessor_.RequestPreset(idx);
			}
		}

		void SaveConfigLocked()
		{
			if (configPath_.empty()) return;
			std::error_code ec;
			if (!configPath_.parent_path().empty())
				std::filesystem::create_directories(configPath_.parent_path(), ec);
			std::filesystem::path tmpPath = configPath_;
			tmpPath += L".tmp";
			const std::string payload = "{\"version\":1,\"activePreset\":\"" + activePresetId_ + "\"}\n";
			HANDLE h = CreateFileW(tmpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h == INVALID_HANDLE_VALUE)
			{
				DeleteFileW(tmpPath.c_str());
				h = CreateFileW(tmpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
			}
			if (h == INVALID_HANDLE_VALUE) return;
			DWORD written = 0;
			const BOOL ok = WriteFile(h, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr) &&
			                written == payload.size() && FlushFileBuffers(h);
			CloseHandle(h);
			if (ok)
				MoveFileExW(tmpPath.c_str(), configPath_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
			else
				DeleteFileW(tmpPath.c_str());
		}

		mutable std::mutex mutex_;
		ma_engine engine_{};
		bool engineInitialized_{false};
		DspProcessor dspProcessor_;
		DspEffectNode dspNode_{};
		bool dspNodeInitialized_{false};

		SourceKind sourceKind_{SourceKind::None};
		ma_decoder decoder_{};
		MfDataSource mfSource_{};
		ma_data_source* activeDataSource_{nullptr};
		ma_sound sound_{};
		bool soundInitialized_{false};

		uint32_t sourceSampleRate_{0};
		uint32_t sourceChannels_{0};
		uint64_t sourceTotalFrames_{0};
		uint64_t sourceEpoch_{0};
		uint32_t state_{GAKU_AUDIO_STOPPED};
		int32_t lastErrorCode_{0};
		float volume_{1.0f};
		float playbackRate_{1.0f};
		std::string lastPathUtf8_;
		std::string activePresetId_{"off"};
		std::filesystem::path configPath_;
	};
} // namespace GakuAudio
