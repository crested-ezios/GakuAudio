// SPDX-License-Identifier: MIT
#pragma once

#include "../deps/miniaudio/miniaudio.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace GakuAudio
{
	inline constexpr double kPi = 3.14159265358979323846;

	inline float DbToLinear(float db) noexcept
	{
		return (db <= -79.0f) ? 0.0f : std::pow(10.0f, db / 20.0f);
	}

	inline float FlushDenormal(float v) noexcept
	{
		return (std::fabs(v) < 1e-24f) ? 0.0f : v;
	}

	struct StereoBiquad
	{
		float b0{1.0f}, b1{0.0f}, b2{0.0f}, a1{0.0f}, a2{0.0f};
		float z1L{0.0f}, z2L{0.0f}, z1R{0.0f}, z2R{0.0f};
		bool bypass{true};

		void Reset() noexcept
		{
			z1L = z2L = z1R = z2R = 0.0f;
		}

		void SetBypass() noexcept
		{
			b0 = 1.0f; b1 = b2 = a1 = a2 = 0.0f;
			bypass = true;
		}

		void SetPeaking(double freq, double sampleRate, double q, double gainDb) noexcept
		{
			if (std::fabs(gainDb) < 0.01 || sampleRate <= 0.0 || freq <= 0.0 || freq >= sampleRate * 0.49)
			{
				SetBypass();
				return;
			}
			const double a = std::pow(10.0, gainDb / 40.0);
			const double w0 = 2.0 * kPi * freq / sampleRate;
			const double alpha = std::sin(w0) / (2.0 * (std::max)(0.1, q));
			const double cosW0 = std::cos(w0);
			const double norm = 1.0 + alpha / a;
			b0 = static_cast<float>((1.0 + alpha * a) / norm);
			b1 = static_cast<float>((-2.0 * cosW0) / norm);
			b2 = static_cast<float>((1.0 - alpha * a) / norm);
			a1 = static_cast<float>((-2.0 * cosW0) / norm);
			a2 = static_cast<float>((1.0 - alpha / a) / norm);
			bypass = false;
		}

		void SetLowShelf(double freq, double sampleRate, double q, double gainDb) noexcept
		{
			if (std::fabs(gainDb) < 0.01 || sampleRate <= 0.0 || freq <= 0.0 || freq >= sampleRate * 0.49)
			{
				SetBypass();
				return;
			}
			const double a = std::pow(10.0, gainDb / 40.0);
			const double w0 = 2.0 * kPi * freq / sampleRate;
			const double cosW0 = std::cos(w0);
			const double sinW0 = std::sin(w0);
			const double alpha = sinW0 / (2.0 * (std::max)(0.1, q));
			const double twoSqrtAAlpha = 2.0 * std::sqrt(a) * alpha;
			const double norm = (a + 1.0) + (a - 1.0) * cosW0 + twoSqrtAAlpha;
			b0 = static_cast<float>((a * ((a + 1.0) - (a - 1.0) * cosW0 + twoSqrtAAlpha)) / norm);
			b1 = static_cast<float>((2.0 * a * ((a - 1.0) - (a + 1.0) * cosW0)) / norm);
			b2 = static_cast<float>((a * ((a + 1.0) - (a - 1.0) * cosW0 - twoSqrtAAlpha)) / norm);
			a1 = static_cast<float>((-2.0 * ((a - 1.0) + (a + 1.0) * cosW0)) / norm);
			a2 = static_cast<float>(((a + 1.0) + (a - 1.0) * cosW0 - twoSqrtAAlpha) / norm);
			bypass = false;
		}

		void SetHighShelf(double freq, double sampleRate, double q, double gainDb) noexcept
		{
			if (std::fabs(gainDb) < 0.01 || sampleRate <= 0.0 || freq <= 0.0 || freq >= sampleRate * 0.49)
			{
				SetBypass();
				return;
			}
			const double a = std::pow(10.0, gainDb / 40.0);
			const double w0 = 2.0 * kPi * freq / sampleRate;
			const double cosW0 = std::cos(w0);
			const double sinW0 = std::sin(w0);
			const double alpha = sinW0 / (2.0 * (std::max)(0.1, q));
			const double twoSqrtAAlpha = 2.0 * std::sqrt(a) * alpha;
			const double norm = (a + 1.0) - (a - 1.0) * cosW0 + twoSqrtAAlpha;
			b0 = static_cast<float>((a * ((a + 1.0) + (a - 1.0) * cosW0 + twoSqrtAAlpha)) / norm);
			b1 = static_cast<float>((-2.0 * a * ((a - 1.0) + (a + 1.0) * cosW0)) / norm);
			b2 = static_cast<float>((a * ((a + 1.0) + (a - 1.0) * cosW0 - twoSqrtAAlpha)) / norm);
			a1 = static_cast<float>((2.0 * ((a - 1.0) - (a + 1.0) * cosW0)) / norm);
			a2 = static_cast<float>(((a + 1.0) - (a - 1.0) * cosW0 - twoSqrtAAlpha) / norm);
			bypass = false;
		}

		void SetLowPass(double freq, double sampleRate, double q = 0.7071) noexcept
		{
			if (sampleRate <= 0.0 || freq <= 0.0 || freq >= sampleRate * 0.49)
			{
				SetBypass();
				return;
			}
			const double w0 = 2.0 * kPi * freq / sampleRate;
			const double cosW0 = std::cos(w0);
			const double alpha = std::sin(w0) / (2.0 * (std::max)(0.1, q));
			const double norm = 1.0 + alpha;
			b0 = static_cast<float>(((1.0 - cosW0) * 0.5) / norm);
			b1 = static_cast<float>((1.0 - cosW0) / norm);
			b2 = static_cast<float>(((1.0 - cosW0) * 0.5) / norm);
			a1 = static_cast<float>((-2.0 * cosW0) / norm);
			a2 = static_cast<float>((1.0 - alpha) / norm);
			bypass = false;
		}

		inline void Process(float& l, float& r) noexcept
		{
			if (bypass) return;
			const float yL = b0 * l + z1L;
			z1L = FlushDenormal(b1 * l - a1 * yL + z2L);
			z2L = FlushDenormal(b2 * l - a2 * yL);
			l = yL;

			const float yR = b0 * r + z1R;
			z1R = FlushDenormal(b1 * r - a1 * yR + z2R);
			z2R = FlushDenormal(b2 * r - a2 * yR);
			r = yR;
		}
	};

	struct CombFilter
	{
		std::vector<float> buf;
		size_t idx{0};
		float filterStore{0.0f};
		float feedback{0.8f};
		float damp1{0.2f};
		float damp2{0.8f};

		void Init(size_t length)
		{
			buf.assign((std::max<size_t>)(length, 16), 0.0f);
			idx = 0;
			filterStore = 0.0f;
		}

		void Reset() noexcept
		{
			std::fill(buf.begin(), buf.end(), 0.0f);
			idx = 0;
			filterStore = 0.0f;
		}

		inline float Process(float input) noexcept
		{
			const float out = buf[idx];
			filterStore = FlushDenormal(out * damp2 + filterStore * damp1);
			buf[idx] = FlushDenormal(input + filterStore * feedback);
			if (++idx >= buf.size()) idx = 0;
			return out;
		}
	};

	struct AllpassFilter
	{
		std::vector<float> buf;
		size_t idx{0};
		float feedback{0.5f};

		void Init(size_t length)
		{
			buf.assign((std::max<size_t>)(length, 8), 0.0f);
			idx = 0;
		}

		void Reset() noexcept
		{
			std::fill(buf.begin(), buf.end(), 0.0f);
			idx = 0;
		}

		inline float Process(float input) noexcept
		{
			const float bufOut = buf[idx];
			const float out = -input + bufOut;
			buf[idx] = FlushDenormal(input + bufOut * feedback);
			if (++idx >= buf.size()) idx = 0;
			return out;
		}
	};

	struct EffectPresetParams
	{
		const char* id;
		const char* name;
		const char* category;
		const char* description;

		float bassShelfDb{0.0f};
		float trebleShelfDb{0.0f};
		std::array<float, 10> eq10Db{}; // 31.25, 62.5, 125, 250, 500, 1k, 2k, 4k, 8k, 16k
		float presenceDb{0.0f};
		float stereoWidth{1.0f};        // 1.0 = neutral, >1.0 = wider above 140Hz
		float haasDelayMs{0.0f};        // subtle stereo decorrelation on side/highs

		bool reverbEnabled{false};
		float dryDb{0.0f};
		float erDb{-80.0f};
		float wetDb{-80.0f};
		float preDelayMs{20.0f};
		float roomSize{0.7f};           // 0.2 .. 1.0
		float decaySeconds{1.5f};       // 0.3 .. 4.0
		float hfDamping{0.35f};         // 0.05 .. 0.85
		float diffusion{0.75f};         // 0.2 .. 0.9
		float wetWidth{1.3f};           // 0.0 .. 1.8
		float tcLowFreq{200.0f}, tcLowGainDb{0.0f}, tcLowQ{0.7f};
		float tcMidFreq{2000.0f}, tcMidGainDb{0.0f}, tcMidQ{0.5f};
		float tcHighFreq{8000.0f}, tcHighGainDb{0.0f}, tcHighQ{0.5f};
		float outputTrimDb{0.0f};
	};

	inline constexpr std::array<EffectPresetParams, 8> kBuiltinPresets = {{
		{
			"off", "原声", "Default", "直通模式，不附加额外均衡或空间混响",
			0.0f, 0.0f, {0,0,0,0,0,0,0,0,0,0}, 0.0f, 1.0f, 0.0f,
			false, 0.0f, -80.0f, -80.0f, 0.0f, 0.5f, 1.0f, 0.3f, 0.7f, 1.0f,
			200.0f, 0.0f, 0.7f, 2000.0f, 0.0f, 0.5f, 8000.0f, 0.0f, 0.5f, 0.0f
		},
		{
			"live", "演唱会现场", "Scene", "体育馆/巨蛋级演出声场，增强低频冲击力与宏大舞台混响",
			3.0f, 2.0f, {4.5f, 4.0f, 2.2f, 0.0f, -1.0f, 0.0f, 2.6f, 3.8f, 3.4f, 4.2f}, 1.5f, 1.28f, 1.4f,
			true, 0.0f, -16.0f, -15.5f, 32.0f, 0.88f, 2.7f, 0.38f, 0.85f, 1.55f,
			200.0f, -2.5f, 0.8f, 2000.0f, -1.5f, 0.5f, 9000.0f, 1.0f, 0.5f, -2.5f
		},
		{
			"livehouse", "LiveHouse现场", "Scene", "近场俱乐部热烈氛围，紧凑早期反射与近距离空间包围感",
			2.0f, 1.2f, {2.5f, 3.0f, 1.8f, 0.5f, -0.8f, 0.8f, 2.2f, 2.8f, 1.5f, 1.0f}, 1.2f, 1.20f, 0.8f,
			true, 0.0f, -17.0f, -18.5f, 24.0f, 0.70f, 1.45f, 0.48f, 0.80f, 1.45f,
			200.0f, -1.0f, 1.0f, 2000.0f, -3.0f, 0.5f, 8000.0f, -4.5f, 0.5f, -1.5f
		},
		{
			"bass", "极重低音", "EQ", "强力下潜低频与温暖胸腔共振，内置柔限幅防破音",
			3.2f, 0.5f, {6.0f, 4.8f, 3.2f, 2.0f, 0.0f, 0.0f, 0.0f, 0.5f, 0.5f, 0.0f}, 0.0f, 1.04f, 0.0f,
			false, 0.0f, -80.0f, -80.0f, 0.0f, 0.5f, 1.0f, 0.3f, 0.7f, 1.0f,
			200.0f, 0.0f, 0.7f, 2000.0f, 0.0f, 0.5f, 8000.0f, 0.0f, 0.5f, -3.5f
		},
		{
			"vocal", "清澈人声", "EQ", "收紧浑浊低频并提升人声临场与唇齿清晰度",
			-1.0f, 1.5f, {-2.0f, -1.5f, -0.5f, 1.6f, 3.6f, 3.8f, 2.0f, 1.2f, -0.5f, 1.0f}, 2.4f, 1.08f, 0.0f,
			true, 0.0f, -24.0f, -26.0f, 16.0f, 0.50f, 0.9f, 0.55f, 0.70f, 1.15f,
			220.0f, -4.0f, 0.8f, 2400.0f, 1.0f, 0.6f, 8000.0f, -2.0f, 0.5f, -1.2f
		},
		{
			"concert_hall", "音乐厅", "Scene", "古典音乐厅自然厅堂残响，宽阔声像与柔润高频衰减",
			1.0f, 0.5f, {1.5f, 1.0f, 0.5f, 0.0f, 0.0f, 0.5f, 1.0f, 1.2f, 0.5f, 0.0f}, 0.0f, 1.25f, 1.0f,
			true, 0.0f, -18.0f, -17.0f, 28.0f, 0.84f, 2.25f, 0.42f, 0.84f, 1.50f,
			180.0f, -2.0f, 0.8f, 2000.0f, -1.0f, 0.5f, 7500.0f, -3.0f, 0.5f, -1.5f
		},
		{
			"surround_3d", "3D环绕", "Spatial", "低频凝聚居中，中高频横向声场拓宽与轻微空间反射",
			1.5f, 1.5f, {2.0f, 1.5f, 0.5f, 0.0f, -0.5f, 0.0f, 1.0f, 1.8f, 2.0f, 2.2f}, 1.8f, 1.72f, 2.1f,
			true, -0.5f, -20.0f, -22.0f, 18.0f, 0.65f, 1.15f, 0.40f, 0.82f, 1.75f,
			200.0f, -3.0f, 0.8f, 2000.0f, 0.0f, 0.5f, 10000.0f, 1.5f, 0.5f, -2.0f
		},
		{
			"acg", "纯净ACG", "Genre", "二次元舞台向调音：弹性鼓点、通透女声与明亮高频延伸",
			1.8f, 2.4f, {3.5f, 4.8f, 1.5f, -0.5f, -0.8f, 1.8f, 4.2f, 1.5f, -1.0f, 3.6f}, 1.5f, 1.18f, 0.5f,
			true, 0.0f, -24.0f, -25.0f, 18.0f, 0.60f, 1.1f, 0.42f, 0.76f, 1.25f,
			200.0f, -2.0f, 0.8f, 2000.0f, -1.0f, 0.5f, 10000.0f, 1.5f, 0.5f, -2.0f
		}
	}};

	inline uint32_t FindPresetIndex(std::string_view id) noexcept
	{
		for (uint32_t i = 0; i < static_cast<uint32_t>(kBuiltinPresets.size()); ++i)
		{
			if (id == kBuiltinPresets[i].id) return i;
		}
		return 0;
	}

	struct DspChain
	{
		static constexpr std::array<double, 10> kEqFreqs = {
			31.25, 62.5, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0
		};
		static constexpr std::array<size_t, 8> kCombLengths44100 = {
			1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617
		};
		static constexpr std::array<size_t, 4> kAllpassLengths44100 = {
			556, 441, 341, 225
		};
		static constexpr size_t kStereoSpread44100 = 23;

		uint32_t sampleRate{48000};
		uint32_t presetIndex{0};
		bool isBypass{true};

		StereoBiquad lowShelf;
		StereoBiquad highShelf;
		std::array<StereoBiquad, 10> eq10;
		StereoBiquad presence;
		StereoBiquad crossoverLp;

		float stereoWidth{1.0f};
		std::vector<float> haasLineR;
		size_t haasIdx{0};
		size_t haasDelaySamples{0};

		bool reverbEnabled{false};
		float dryGain{1.0f};
		float erGain{0.0f};
		float wetGain{0.0f};
		float wetWidth{1.0f};
		float outputTrim{1.0f};

		std::vector<float> preDelayL, preDelayR;
		size_t preDelayIdx{0};
		size_t preDelaySamples{1};

		std::vector<float> erLineL, erLineR;
		size_t erIdx{0};
		std::array<size_t, 6> erTapsL{};
		std::array<size_t, 6> erTapsR{};

		std::array<CombFilter, 8> combL, combR;
		std::array<AllpassFilter, 4> apL, apR;
		StereoBiquad tcLow, tcMid, tcHigh;

		void Configure(uint32_t sr, uint32_t idx)
		{
			sampleRate = sr > 0 ? sr : 48000;
			presetIndex = idx < kBuiltinPresets.size() ? idx : 0;
			const auto& p = kBuiltinPresets[presetIndex];
			isBypass = (presetIndex == 0);

			lowShelf.Reset();
			highShelf.Reset();
			presence.Reset();
			crossoverLp.Reset();
			tcLow.Reset();
			tcMid.Reset();
			tcHigh.Reset();

			if (isBypass)
			{
				lowShelf.SetBypass();
				highShelf.SetBypass();
				for (auto& b : eq10) { b.Reset(); b.SetBypass(); }
				presence.SetBypass();
				crossoverLp.SetBypass();
				stereoWidth = 1.0f;
				haasDelaySamples = 0;
				reverbEnabled = false;
				dryGain = 1.0f;
				erGain = 0.0f;
				wetGain = 0.0f;
				outputTrim = 1.0f;
				return;
			}

			lowShelf.SetLowShelf(100.0, sampleRate, 0.707, p.bassShelfDb);
			highShelf.SetHighShelf(8000.0, sampleRate, 0.707, p.trebleShelfDb);
			for (size_t i = 0; i < 10; ++i)
			{
				eq10[i].Reset();
				eq10[i].SetPeaking(kEqFreqs[i], sampleRate, 1.414, p.eq10Db[i]);
			}
			presence.SetPeaking(3200.0, sampleRate, 0.9, p.presenceDb);
			crossoverLp.SetLowPass(140.0, sampleRate, 0.7071);

			stereoWidth = p.stereoWidth;
			const size_t maxHaas = static_cast<size_t>(sampleRate * 0.01) + 4;
			haasLineR.assign(maxHaas, 0.0f);
			haasIdx = 0;
			haasDelaySamples = static_cast<size_t>(std::clamp(p.haasDelayMs, 0.0f, 8.0f) * 0.001f * sampleRate);

			reverbEnabled = p.reverbEnabled;
			dryGain = DbToLinear(p.dryDb);
			erGain = DbToLinear(p.erDb);
			wetGain = DbToLinear(p.wetDb);
			wetWidth = p.wetWidth;
			outputTrim = DbToLinear(p.outputTrimDb);

			if (reverbEnabled)
			{
				const double scale = static_cast<double>(sampleRate) / 44100.0;
				preDelaySamples = (std::max<size_t>)(1, static_cast<size_t>(p.preDelayMs * 0.001f * sampleRate));
				preDelayL.assign(preDelaySamples + 16, 0.0f);
				preDelayR.assign(preDelaySamples + 16, 0.0f);
				preDelayIdx = 0;

				const size_t erCap = static_cast<size_t>(sampleRate * 0.06) + 16;
				erLineL.assign(erCap, 0.0f);
				erLineR.assign(erCap, 0.0f);
				erIdx = 0;
				static constexpr std::array<float, 6> kErMsL = {7.1f, 11.7f, 16.3f, 22.9f, 29.3f, 36.1f};
				static constexpr std::array<float, 6> kErMsR = {8.3f, 13.1f, 17.9f, 24.1f, 31.7f, 38.9f};
				const float rScale = std::clamp(p.roomSize, 0.35f, 1.15f);
				for (size_t i = 0; i < 6; ++i)
				{
					erTapsL[i] = (std::min)(erCap - 1, (std::max<size_t>)(1, static_cast<size_t>(kErMsL[i] * rScale * 0.001f * sampleRate)));
					erTapsR[i] = (std::min)(erCap - 1, (std::max<size_t>)(1, static_cast<size_t>(kErMsR[i] * rScale * 0.001f * sampleRate)));
				}

				const size_t spread = static_cast<size_t>(kStereoSpread44100 * scale);
				const float roomMul = 0.65f + 0.45f * std::clamp(p.roomSize, 0.2f, 1.0f);
				for (size_t i = 0; i < 8; ++i)
				{
					const size_t lenL = static_cast<size_t>(kCombLengths44100[i] * scale * roomMul);
					const size_t lenR = lenL + spread;
					combL[i].Init(lenL);
					combR[i].Init(lenR);
					// RT60 feedback: g = 10^(-3 * delaySec / decaySeconds)
					const double delaySec = static_cast<double>(lenL) / static_cast<double>(sampleRate);
					const float fb = static_cast<float>(std::clamp(std::pow(10.0, (-3.0 * delaySec) / (std::max)(0.25f, p.decaySeconds)), 0.45, 0.95));
					combL[i].feedback = fb;
					combR[i].feedback = fb;
					combL[i].damp1 = std::clamp(p.hfDamping, 0.02f, 0.85f);
					combR[i].damp1 = combL[i].damp1;
					combL[i].damp2 = 1.0f - combL[i].damp1;
					combR[i].damp2 = combL[i].damp2;
				}
				for (size_t i = 0; i < 4; ++i)
				{
					const size_t lenL = static_cast<size_t>(kAllpassLengths44100[i] * scale);
					const size_t lenR = lenL + spread;
					apL[i].Init(lenL);
					apR[i].Init(lenR);
					apL[i].feedback = std::clamp(p.diffusion * 0.7f, 0.2f, 0.65f);
					apR[i].feedback = apL[i].feedback;
				}

				tcLow.SetPeaking(p.tcLowFreq, sampleRate, p.tcLowQ, p.tcLowGainDb);
				tcMid.SetPeaking(p.tcMidFreq, sampleRate, p.tcMidQ, p.tcMidGainDb);
				tcHigh.SetHighShelf(p.tcHighFreq, sampleRate, p.tcHighQ, p.tcHighGainDb);
			}
		}

		static inline float SoftLimit(float x) noexcept
		{
			constexpr float kThresh = 0.88f;
			constexpr float kCeiling = 0.97f;
			const float ax = std::fabs(x);
			if (ax <= kThresh) return x;
			const float excess = (ax - kThresh) / (kCeiling - kThresh);
			const float compressed = kThresh + (kCeiling - kThresh) * std::tanh(excess);
			return x >= 0.0f ? compressed : -compressed;
		}

		inline void ProcessSample(float inL, float inR, float& outL, float& outR) noexcept
		{
			if (isBypass)
			{
				outL = inL;
				outR = inR;
				return;
			}

			float l = inL;
			float r = inR;

			lowShelf.Process(l, r);
			highShelf.Process(l, r);
			for (size_t i = 0; i < 10; ++i) eq10[i].Process(l, r);
			presence.Process(l, r);

			if (std::fabs(stereoWidth - 1.0f) > 0.005f || haasDelaySamples > 0)
			{
				float lowL = l, lowR = r;
				crossoverLp.Process(lowL, lowR);
				float hiL = l - lowL;
				float hiR = r - lowR;

				if (haasDelaySamples > 0 && !haasLineR.empty())
				{
					const size_t readIdx = (haasIdx + haasLineR.size() - haasDelaySamples) % haasLineR.size();
					const float delayedR = haasLineR[readIdx];
					haasLineR[haasIdx] = hiR;
					if (++haasIdx >= haasLineR.size()) haasIdx = 0;
					hiR = 0.72f * hiR + 0.28f * delayedR;
				}

				const float mid = 0.5f * (hiL + hiR);
				const float side = 0.5f * (hiL - hiR) * stereoWidth;
				l = lowL + (mid + side);
				r = lowR + (mid - side);
			}

			if (reverbEnabled)
			{
				const float dryL = l;
				const float dryR = r;

				// Early reflections
				float erL = 0.0f, erR = 0.0f;
				if (!erLineL.empty())
				{
					erLineL[erIdx] = dryL;
					erLineR[erIdx] = dryR;
					static constexpr std::array<float, 6> kErWeights = {0.32f, -0.26f, 0.21f, -0.17f, 0.13f, -0.09f};
					for (size_t i = 0; i < 6; ++i)
					{
						const size_t rIdxL = (erIdx + erLineL.size() - erTapsL[i]) % erLineL.size();
						const size_t rIdxR = (erIdx + erLineR.size() - erTapsR[i]) % erLineR.size();
						erL += erLineL[rIdxL] * kErWeights[i];
						erR += erLineR[rIdxR] * kErWeights[i];
					}
					if (++erIdx >= erLineL.size()) erIdx = 0;
				}

				// Pre-delay into comb tank
				float pdL = dryL, pdR = dryR;
				if (!preDelayL.empty())
				{
					const size_t readIdx = (preDelayIdx + preDelayL.size() - preDelaySamples) % preDelayL.size();
					pdL = preDelayL[readIdx];
					pdR = preDelayR[readIdx];
					preDelayL[preDelayIdx] = dryL;
					preDelayR[preDelayIdx] = dryR;
					if (++preDelayIdx >= preDelayL.size()) preDelayIdx = 0;
				}

				const float tankInL = (0.7f * pdL + 0.3f * pdR) * 0.125f;
				const float tankInR = (0.7f * pdR + 0.3f * pdL) * 0.125f;
				float wetL = 0.0f, wetR = 0.0f;
				for (size_t i = 0; i < 8; ++i)
				{
					wetL += combL[i].Process(tankInL);
					wetR += combR[i].Process(tankInR);
				}
				for (size_t i = 0; i < 4; ++i)
				{
					wetL = apL[i].Process(wetL);
					wetR = apR[i].Process(wetR);
				}
				tcLow.Process(wetL, wetR);
				tcMid.Process(wetL, wetR);
				tcHigh.Process(wetL, wetR);

				const float wetM = 0.5f * (wetL + wetR);
				const float wetS = 0.5f * (wetL - wetR) * wetWidth;
				wetL = wetM + wetS;
				wetR = wetM - wetS;

				l = dryL * dryGain + erL * erGain + wetL * wetGain;
				r = dryR * dryGain + erR * erGain + wetR * wetGain;
			}

			outL = SoftLimit(l * outputTrim);
			outR = SoftLimit(r * outputTrim);
		}
	};

	class DspProcessor
	{
	public:
		void Init(uint32_t sampleRate, uint32_t initialPreset = 0)
		{
			sampleRate_ = sampleRate > 0 ? sampleRate : 48000;
			currentSlot_ = 0;
			const uint32_t safePreset = initialPreset < kBuiltinPresets.size() ? initialPreset : 0;
			targetPreset_.store(safePreset, std::memory_order_release);
			activePreset_ = safePreset;
			chains_[0].Configure(sampleRate_, safePreset);
			chains_[1].Configure(sampleRate_, 0);
			crossfadeTotal_ = (std::max<uint32_t>)(1, (sampleRate_ * 35u) / 1000u);
			crossfadeRemaining_ = 0;
		}

		void RequestPreset(uint32_t presetIdx) noexcept
		{
			const uint32_t safe = presetIdx < kBuiltinPresets.size() ? presetIdx : 0;
			targetPreset_.store(safe, std::memory_order_release);
		}

		void RequestPresetById(std::string_view id) noexcept
		{
			RequestPreset(FindPresetIndex(id));
		}

		uint32_t ActivePresetIndex() const noexcept
		{
			return targetPreset_.load(std::memory_order_acquire);
		}

		void ProcessStereo(const float* inInterleaved, float* outInterleaved, size_t frameCount)
		{
			const uint32_t desired = targetPreset_.load(std::memory_order_acquire);
			if (desired != activePreset_)
			{
				const uint32_t nextSlot = 1u - currentSlot_;
				chains_[nextSlot].Configure(sampleRate_, desired);
				currentSlot_ = nextSlot;
				activePreset_ = desired;
				crossfadeRemaining_ = crossfadeTotal_;
			}

			auto& active = chains_[currentSlot_];
			auto& prev = chains_[1u - currentSlot_];

			if (crossfadeRemaining_ == 0 && active.isBypass)
			{
				if (inInterleaved && outInterleaved && inInterleaved != outInterleaved)
					std::memcpy(outInterleaved, inInterleaved, frameCount * 2 * sizeof(float));
				return;
			}

			for (size_t i = 0; i < frameCount; ++i)
			{
				const float inL = inInterleaved ? inInterleaved[i * 2] : 0.0f;
				const float inR = inInterleaved ? inInterleaved[i * 2 + 1] : 0.0f;
				float aL = 0.0f, aR = 0.0f;
				active.ProcessSample(inL, inR, aL, aR);
				if (crossfadeRemaining_ > 0)
				{
					float pL = 0.0f, pR = 0.0f;
					prev.ProcessSample(inL, inR, pL, pR);
					const float alpha = 1.0f - (static_cast<float>(crossfadeRemaining_) / static_cast<float>(crossfadeTotal_));
					aL = pL + (aL - pL) * alpha;
					aR = pR + (aR - pR) * alpha;
					--crossfadeRemaining_;
				}
				outInterleaved[i * 2] = aL;
				outInterleaved[i * 2 + 1] = aR;
			}
		}

	private:
		uint32_t sampleRate_{48000};
		std::array<DspChain, 2> chains_;
		uint32_t currentSlot_{0};
		uint32_t activePreset_{0};
		std::atomic<uint32_t> targetPreset_{0};
		uint32_t crossfadeTotal_{1680};
		uint32_t crossfadeRemaining_{0};
	};

	struct DspEffectNode
	{
		ma_node_base base{};
		DspProcessor* processor{nullptr};
	};

	namespace detail
	{
		inline void DspNodeOnProcess(ma_node* pNode, const float** ppFramesIn, ma_uint32* pFrameCountIn,
			float** ppFramesOut, ma_uint32* pFrameCountOut)
		{
			auto* node = reinterpret_cast<DspEffectNode*>(pNode);
			const ma_uint32 frames = pFrameCountOut ? *pFrameCountOut : 0;
			if (pFrameCountIn) *pFrameCountIn = frames;
			if (!ppFramesOut || !ppFramesOut[0] || frames == 0) return;

			const float* in = (ppFramesIn && ppFramesIn[0]) ? ppFramesIn[0] : nullptr;
			if (node && node->processor)
				node->processor->ProcessStereo(in, ppFramesOut[0], frames);
			else if (in)
				std::memcpy(ppFramesOut[0], in, static_cast<size_t>(frames) * 2 * sizeof(float));
			else
				std::memset(ppFramesOut[0], 0, static_cast<size_t>(frames) * 2 * sizeof(float));
		}

		inline ma_node_vtable g_dspNodeVtable = {
			DspNodeOnProcess,
			nullptr,
			1,
			1,
			MA_NODE_FLAG_CONTINUOUS_PROCESSING | MA_NODE_FLAG_ALLOW_NULL_INPUT
		};
	} // namespace detail

	inline ma_result InitDspEffectNode(ma_node_graph* graph, DspEffectNode& node, DspProcessor& processor, uint32_t sampleRate, uint32_t initialPreset)
	{
		processor.Init(sampleRate, initialPreset);
		node.processor = &processor;
		const ma_uint32 inCh[1] = {2};
		const ma_uint32 outCh[1] = {2};
		ma_node_config cfg = ma_node_config_init();
		cfg.vtable = &detail::g_dspNodeVtable;
		cfg.pInputChannels = inCh;
		cfg.pOutputChannels = outCh;
		return ma_node_init(graph, &cfg, nullptr, &node.base);
	}
} // namespace GakuAudio
