// SPDX-License-Identifier: MIT
#include "GakuAudio.h"
#include "GkAudioMetadata.hpp"
#include "GakuAudioEngine.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
	std::vector<uint8_t> MakeTestWavPcm16(uint32_t sampleRate, uint32_t channels, double seconds, double freqHz, bool extensible = false)
	{
		const uint32_t totalFrames = static_cast<uint32_t>(sampleRate * seconds);
		const uint16_t bps = 16;
		const uint16_t blockAlign = static_cast<uint16_t>(channels * (bps / 8));
		const uint32_t dataBytes = totalFrames * blockAlign;
		const uint32_t fmtSize = extensible ? 40u : 16u;
		const uint32_t riffSize = 4u + (8u + fmtSize) + (8u + dataBytes);

		std::vector<uint8_t> out;
		out.reserve(riffSize + 8);
		auto pushBytes = [&](const void* p, size_t n)
		{
			const auto* b = static_cast<const uint8_t*>(p);
			out.insert(out.end(), b, b + n);
		};
		auto pushU16 = [&](uint16_t v) { pushBytes(&v, 2); };
		auto pushU32 = [&](uint32_t v) { pushBytes(&v, 4); };

		pushBytes("RIFF", 4);
		pushU32(riffSize);
		pushBytes("WAVEfmt ", 8);
		pushU32(fmtSize);
		pushU16(extensible ? 0xFFFEu : 1u);
		pushU16(static_cast<uint16_t>(channels));
		pushU32(sampleRate);
		pushU32(sampleRate * blockAlign);
		pushU16(blockAlign);
		pushU16(bps);
		if (extensible)
		{
			pushU16(22);
			pushU16(bps);
			pushU32(3); // stereo mask
			pushU16(1); // PCM subformat tag
			const uint8_t guidRest[14] = {0x00,0x00,0x00,0x00,0x10,0x00,0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71};
			pushBytes(guidRest, 14);
		}
		pushBytes("data", 4);
		pushU32(dataBytes);
		for (uint32_t i = 0; i < totalFrames; ++i)
		{
			const float sample = 0.45f * std::sinf(static_cast<float>(2.0 * GakuAudio::kPi * freqHz * i / sampleRate));
			const int16_t q = static_cast<int16_t>(std::lrintf(sample * 32767.0f));
			for (uint32_t c = 0; c < channels; ++c) pushU16(static_cast<uint16_t>(q));
		}
		return out;
	}

	std::vector<uint8_t> MakeSyntheticId3v2Mp3(const std::string& title, const std::string& artist, const std::vector<uint8_t>& pngCover)
	{
		auto syncsafe = [](uint32_t v, uint8_t out[4])
		{
			out[0] = static_cast<uint8_t>((v >> 21) & 0x7F);
			out[1] = static_cast<uint8_t>((v >> 14) & 0x7F);
			out[2] = static_cast<uint8_t>((v >> 7) & 0x7F);
			out[3] = static_cast<uint8_t>(v & 0x7F);
		};
		auto be32 = [](uint32_t v, std::vector<uint8_t>& dst)
		{
			dst.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
			dst.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
			dst.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
			dst.push_back(static_cast<uint8_t>(v & 0xFF));
		};
		std::vector<uint8_t> frames;
		auto addTextFrame = [&](const char id[4], const std::string& val)
		{
			frames.insert(frames.end(), id, id + 4);
			be32(static_cast<uint32_t>(1 + val.size()), frames);
			frames.push_back(0); frames.push_back(0);
			frames.push_back(3); // UTF-8
			frames.insert(frames.end(), val.begin(), val.end());
		};
		addTextFrame("TIT2", title);
		addTextFrame("TPE1", artist);

		std::vector<uint8_t> apic;
		apic.push_back(0); // encoding
		const char* mime = "image/png";
		apic.insert(apic.end(), mime, mime + std::strlen(mime) + 1);
		apic.push_back(3); // Front cover
		apic.push_back(0); // empty description
		apic.insert(apic.end(), pngCover.begin(), pngCover.end());
		frames.insert(frames.end(), {'A','P','I','C'});
		be32(static_cast<uint32_t>(apic.size()), frames);
		frames.push_back(0); frames.push_back(0);
		frames.insert(frames.end(), apic.begin(), apic.end());

		std::vector<uint8_t> mp3;
		mp3.insert(mp3.end(), {'I','D','3', 3, 0, 0});
		uint8_t ss[4]{};
		syncsafe(static_cast<uint32_t>(frames.size()), ss);
		mp3.insert(mp3.end(), ss, ss + 4);
		mp3.insert(mp3.end(), frames.begin(), frames.end());

		// Append 10 CBR MPEG1 Layer3 128kbps 44100Hz stereo frames (417 bytes each)
		// Header: 0xFF 0xFB 0x90 0x00 (sync, MPEG1, L3, no crc, 128kbps=9, 44100=0, pad=0)
		for (int f = 0; f < 10; ++f)
		{
			mp3.push_back(0xFF);
			mp3.push_back(0xFB);
			mp3.push_back(0x90);
			mp3.push_back(0x00);
			mp3.resize(mp3.size() + 413, 0x00);
		}
		return mp3;
	}

	std::vector<uint8_t> MakeSyntheticFlac(uint32_t sampleRate, uint64_t totalSamples,
		const std::string& title, const std::string& artist, const std::vector<uint8_t>& jpegCover)
	{
		std::vector<uint8_t> out = {'f','L','a','C'};
		// STREAMINFO (type 0, 34 bytes, not last)
		out.push_back(0x00);
		out.push_back(0); out.push_back(0); out.push_back(34);
		std::vector<uint8_t> si(34, 0);
		si[10] = static_cast<uint8_t>((sampleRate >> 12) & 0xFF);
		si[11] = static_cast<uint8_t>((sampleRate >> 4) & 0xFF);
		si[12] = static_cast<uint8_t>(((sampleRate & 0x0F) << 4) | ((1) << 1) | 0); // ch=2
		si[13] = static_cast<uint8_t>((15 << 4) | ((totalSamples >> 32) & 0x0F));
		si[14] = static_cast<uint8_t>((totalSamples >> 24) & 0xFF);
		si[15] = static_cast<uint8_t>((totalSamples >> 16) & 0xFF);
		si[16] = static_cast<uint8_t>((totalSamples >> 8) & 0xFF);
		si[17] = static_cast<uint8_t>(totalSamples & 0xFF);
		out.insert(out.end(), si.begin(), si.end());

		// VORBIS_COMMENT (type 4)
		std::vector<uint8_t> vc;
		auto le32 = [&](uint32_t v)
		{
			vc.push_back(static_cast<uint8_t>(v & 0xFF));
			vc.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
			vc.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
			vc.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
		};
		le32(4); vc.insert(vc.end(), {'t','e','s','t'});
		le32(2);
		std::string c1 = "TITLE=" + title;
		std::string c2 = "ARTIST=" + artist;
		le32(static_cast<uint32_t>(c1.size())); vc.insert(vc.end(), c1.begin(), c1.end());
		le32(static_cast<uint32_t>(c2.size())); vc.insert(vc.end(), c2.begin(), c2.end());
		out.push_back(0x04);
		out.push_back(static_cast<uint8_t>((vc.size() >> 16) & 0xFF));
		out.push_back(static_cast<uint8_t>((vc.size() >> 8) & 0xFF));
		out.push_back(static_cast<uint8_t>(vc.size() & 0xFF));
		out.insert(out.end(), vc.begin(), vc.end());

		// PICTURE (type 6, last=1)
		std::vector<uint8_t> pic;
		auto be32 = [&](uint32_t v)
		{
			pic.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
			pic.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
			pic.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
			pic.push_back(static_cast<uint8_t>(v & 0xFF));
		};
		be32(3);
		std::string mime = "image/jpeg";
		be32(static_cast<uint32_t>(mime.size()));
		pic.insert(pic.end(), mime.begin(), mime.end());
		be32(0); // desc
		be32(64); be32(64); be32(24); be32(0);
		be32(static_cast<uint32_t>(jpegCover.size()));
		pic.insert(pic.end(), jpegCover.begin(), jpegCover.end());
		out.push_back(0x86);
		out.push_back(static_cast<uint8_t>((pic.size() >> 16) & 0xFF));
		out.push_back(static_cast<uint8_t>((pic.size() >> 8) & 0xFF));
		out.push_back(static_cast<uint8_t>(pic.size() & 0xFF));
		out.insert(out.end(), pic.begin(), pic.end());
		return out;
	}

	double ComputeRms(const std::vector<float>& stereo)
	{
		double sumSq = 0.0;
		for (float v : stereo) sumSq += static_cast<double>(v) * static_cast<double>(v);
		return std::sqrt(sumSq / static_cast<double>((std::max<size_t>)(1, stereo.size())));
	}
}

int main()
{
	// 1. Metadata & cover parsing tests
	{
		const auto wavExt = MakeTestWavPcm16(48000, 2, 1.0, 440.0, true);
		const auto mWav = GakuAudioMeta::ProbeBytes(wavExt, ".wav");
		assert(mWav.valid);
		assert(mWav.codec == "wav");
		assert(mWav.sampleRate == 48000 && mWav.channels == 2);
		assert(std::fabs(mWav.durationSeconds - 1.0) < 0.01);

		const std::vector<uint8_t> fakePng = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A, 1, 2, 3, 4};
		const auto mp3 = MakeSyntheticId3v2Mp3("Campus Anthem", "Saki Hanami", fakePng);
		const auto mMp3 = GakuAudioMeta::ProbeBytes(mp3, ".mp3");
		assert(mMp3.valid);
		assert(mMp3.codec == "mp3");
		assert(mMp3.sampleRate == 44100 && mMp3.channels == 2);
		assert(mMp3.title == "Campus Anthem");
		assert(mMp3.artist == "Saki Hanami");
		assert(mMp3.coverMime == "image/png");
		assert(mMp3.coverBytes == fakePng);
		assert(mMp3.durationSeconds > 0.2 && mMp3.durationSeconds < 0.4);

		const std::vector<uint8_t> fakeJpg = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J','F','I','F'};
		const auto flac = MakeSyntheticFlac(48000, 48000 * 3, "Hoshikuzu", "Temari", fakeJpg);
		const auto mFlac = GakuAudioMeta::ProbeBytes(flac, ".flac");
		assert(mFlac.valid);
		assert(mFlac.codec == "flac");
		assert(mFlac.sampleRate == 48000 && mFlac.channels == 2);
		assert(std::fabs(mFlac.durationSeconds - 3.0) < 0.01);
		assert(mFlac.title == "Hoshikuzu");
		assert(mFlac.artist == "Temari");
		assert(mFlac.coverMime == "image/jpeg");
		assert(mFlac.coverBytes == fakeJpg);

		assert(GakuAudioMeta::IsNetworkPath("https://example.com/song.mp3"));
		assert(GakuAudioMeta::IsNetworkPath("\\\\server\\share\\song.flac"));
		assert(!GakuAudioMeta::IsNetworkPath("D:\\Music\\song.flac"));
	}

	// 2. DSP presets, bass boost, reverb tail, limiter & smooth crossfade
	{
		constexpr uint32_t sr = 48000;
		constexpr size_t frames = 48000;
		std::vector<float> sine60(frames * 2, 0.0f);
		for (size_t i = 0; i < frames; ++i)
		{
			const float s = 0.25f * std::sinf(static_cast<float>(2.0 * GakuAudio::kPi * 60.0 * i / sr));
			sine60[i * 2] = s;
			sine60[i * 2 + 1] = s;
		}
		std::vector<float> outOff(frames * 2, 0.0f), outBass(frames * 2, 0.0f);

		GakuAudio::DspProcessor dsp;
		dsp.Init(sr, GakuAudio::FindPresetIndex("off"));
		dsp.ProcessStereo(sine60.data(), outOff.data(), frames);
		assert(std::fabs(outOff[1000] - sine60[1000]) < 1e-5f);

		dsp.Init(sr, GakuAudio::FindPresetIndex("bass"));
		dsp.ProcessStereo(sine60.data(), outBass.data(), frames);
		const double rmsOff = ComputeRms(outOff);
		const double rmsBass = ComputeRms(outBass);
		assert(rmsBass > rmsOff * 1.45); // significant 60Hz bass gain even after headroom trim
		for (float v : outBass) assert(std::fabs(v) <= 0.98f);

		// Impulse response reverb tail test for live and livehouse
		for (const char* presetId : {"live", "livehouse"})
		{
			GakuAudio::DspProcessor revDsp;
			revDsp.Init(sr, GakuAudio::FindPresetIndex(presetId));
			std::vector<float> imp(48000 * 2, 0.0f), revOut(48000 * 2, 0.0f);
			imp[0] = 0.9f; imp[1] = 0.9f;
			revDsp.ProcessStereo(imp.data(), revOut.data(), 48000);
			double tailEnergy = 0.0;
			for (size_t i = 10000; i < 40000; ++i)
				tailEnergy += static_cast<double>(revOut[i * 2]) * revOut[i * 2];
			assert(tailEnergy > 1e-4);
		}

		// Crossfade discontinuity check
		GakuAudio::DspProcessor xfDsp;
		xfDsp.Init(sr, GakuAudio::FindPresetIndex("off"));
		std::vector<float> buf(2048 * 2);
		xfDsp.ProcessStereo(sine60.data(), buf.data(), 1024);
		xfDsp.RequestPresetById("live");
		xfDsp.ProcessStereo(sine60.data() + 2048, buf.data() + 2048, 1024);
		for (size_t i = 1; i < 2048; ++i)
		{
			const float diff = std::fabs(buf[i * 2] - buf[(i - 1) * 2]);
			assert(diff < 0.08f);
		}
	}

	// 3. Offline WAV rendering via AudioEngine
	{
		const auto tempDir = std::filesystem::temp_directory_path() / "gakuaudio_offline_test";
		std::filesystem::create_directories(tempDir);
		const auto inWav = tempDir / "input.wav";
		const auto outWav = tempDir / "rendered.wav";
		{
			const auto wavBytes = MakeTestWavPcm16(44100, 2, 1.5, 523.25, false);
			std::ofstream f(inWav, std::ios::binary | std::ios::trunc);
			f.write(reinterpret_cast<const char*>(wavBytes.data()), static_cast<std::streamsize>(wavBytes.size()));
		}

		GakuAudio::AudioEngine engine;
		assert(engine.GetPresetCount() == 8);
		assert(engine.SetActivePreset("livehouse") == GAKU_OK);
		char activeId[64]{};
		assert(engine.GetActivePreset(activeId, sizeof(activeId)) == GAKU_OK);
		assert(std::string(activeId) == "livehouse");

		auto toUtf8 = [](const std::filesystem::path& p)
		{
			const auto u8 = p.u8string();
			return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
		};

		GakuAudioMetadataV1 probed{};
		probed.structSize = sizeof(probed);
		const std::string inPathUtf8 = toUtf8(inWav);
		const std::string outPathUtf8 = toUtf8(outWav);
		assert(engine.ProbeFile(inPathUtf8.c_str(), &probed) == GAKU_OK);
		assert(probed.valid == 1 && probed.sampleRate == 44100 && std::fabs(probed.durationSeconds - 1.5) < 0.02);

		GakuAudioOfflineRenderRequestV1 req{};
		req.abiVersion = GAKU_AUDIO_VERSION;
		req.structSize = sizeof(req);
		req.inputPathUtf8 = inPathUtf8.c_str();
		req.outputWavPathUtf8 = outPathUtf8.c_str();
		req.effectPresetId = "live";
		req.trimOffsetSeconds = 0.25;
		req.timelineDurationSeconds = 2.0;
		req.audioSpeed = 1.0f;
		req.playbackSpeed = 1.0f;
		req.volume = 0.8f;
		req.targetSampleRate = 48000;
		char err[256]{};
		assert(engine.RenderOfflineWav(&req, err, sizeof(err)) == GAKU_OK);

		const auto renderedMeta = GakuAudioMeta::ProbeFilePath(outWav);
		assert(renderedMeta.valid);
		assert(renderedMeta.sampleRate == 48000 && renderedMeta.channels == 2);
		assert(std::fabs(renderedMeta.durationSeconds - 2.0) < 0.02);

		std::error_code ec;
		std::filesystem::remove_all(tempDir, ec);
	}

	std::printf("GakuAudio tests passed!\n");
	return 0;
}
