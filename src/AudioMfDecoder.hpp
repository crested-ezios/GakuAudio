// SPDX-License-Identifier: MIT
#pragma once

#include "../deps/miniaudio/miniaudio.h"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace GakuAudio
{
	using Microsoft::WRL::ComPtr;

	struct MfScopeInit
	{
		bool comInitialized{false};
		bool mfStarted{false};

		MfScopeInit()
		{
			const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			comInitialized = (co == S_OK || co == S_FALSE);
			if (SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)))
				mfStarted = true;
		}
		~MfScopeInit()
		{
			if (mfStarted) MFShutdown();
			if (comInitialized) CoUninitialize();
		}
	};

	inline bool ProbeViaMediaFoundation(const std::wstring& widePath,
		uint32_t& outSampleRate, uint32_t& outChannels, double& outDurationSeconds)
	{
		outSampleRate = 0;
		outChannels = 0;
		outDurationSeconds = 0.0;
		if (widePath.empty()) return false;

		MfScopeInit mf;
		if (!mf.mfStarted) return false;

		ComPtr<IMFSourceReader> reader;
		if (FAILED(MFCreateSourceReaderFromURL(widePath.c_str(), nullptr, &reader)) || !reader)
			return false;

		ComPtr<IMFMediaType> partial;
		if (FAILED(MFCreateMediaType(&partial))) return false;
		partial->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
		partial->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
		if (FAILED(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, partial.Get())))
			return false;

		ComPtr<IMFMediaType> actual;
		if (FAILED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &actual)) || !actual)
			return false;

		UINT32 sr = 0, ch = 0;
		actual->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
		actual->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
		if (sr == 0 || ch == 0) return false;

		PROPVARIANT var;
		PropVariantInit(&var);
		double durSec = 0.0;
		if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &var)))
		{
			ULONGLONG dur100ns = 0;
			if (SUCCEEDED(PropVariantToUInt64(var, &dur100ns)))
				durSec = static_cast<double>(dur100ns) / 10000000.0;
			PropVariantClear(&var);
		}
		outSampleRate = sr;
		outChannels = ch;
		outDurationSeconds = durSec;
		return durSec > 0.0;
	}

	struct MfDataSource
	{
		ma_data_source_base ds{};
		ComPtr<IMFSourceReader> reader;
		uint32_t sampleRate{0};
		uint32_t channels{0};
		uint64_t totalFrames{0};
		uint64_t cursorFrames{0};
		std::vector<float> pending;
		size_t pendingReadIndex{0};
		bool mfStarted{false};
		bool comInitialized{false};
		bool initialized{false};
	};

	namespace detail
	{
		inline ma_result MfRead(ma_data_source* pDataSource, void* pFramesOut, ma_uint64 frameCount, ma_uint64* pFramesRead)
		{
			auto* self = reinterpret_cast<MfDataSource*>(pDataSource);
			if (!self || !self->initialized) return MA_INVALID_OPERATION;
			if (pFramesRead) *pFramesRead = 0;
			if (frameCount == 0) return MA_SUCCESS;

			auto* out = static_cast<float*>(pFramesOut);
			const uint32_t ch = self->channels;
			ma_uint64 written = 0;

			while (written < frameCount)
			{
				const size_t availSamples = self->pending.size() - self->pendingReadIndex;
				const size_t availFrames = availSamples / ch;
				if (availFrames > 0)
				{
					const ma_uint64 take = (std::min)(frameCount - written, static_cast<ma_uint64>(availFrames));
					if (out)
					{
						std::memcpy(out + written * ch,
							self->pending.data() + self->pendingReadIndex,
							static_cast<size_t>(take) * ch * sizeof(float));
					}
					self->pendingReadIndex += static_cast<size_t>(take) * ch;
					written += take;
					self->cursorFrames += take;
					if (self->pendingReadIndex >= self->pending.size())
					{
						self->pending.clear();
						self->pendingReadIndex = 0;
					}
					continue;
				}

				DWORD streamIdx = 0;
				DWORD flags = 0;
				LONGLONG timestamp = 0;
				ComPtr<IMFSample> sample;
				const HRESULT hr = self->reader->ReadSample(
					static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM),
					0, &streamIdx, &flags, &timestamp, &sample);
				if (FAILED(hr))
				{
					if (pFramesRead) *pFramesRead = written;
					return written > 0 ? MA_SUCCESS : MA_ERROR;
				}
				if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0)
				{
					if (pFramesRead) *pFramesRead = written;
					return written > 0 ? MA_SUCCESS : MA_AT_END;
				}
				if (!sample) continue;

				ComPtr<IMFMediaBuffer> buffer;
				if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || !buffer) continue;
				BYTE* raw = nullptr;
				DWORD curLen = 0;
				if (SUCCEEDED(buffer->Lock(&raw, nullptr, &curLen)) && raw && curLen >= sizeof(float) * ch)
				{
					const size_t floatCount = curLen / sizeof(float);
					const size_t usable = (floatCount / ch) * ch;
					const auto* fptr = reinterpret_cast<const float*>(raw);
					self->pending.assign(fptr, fptr + usable);
					self->pendingReadIndex = 0;
					buffer->Unlock();
				}
				else if (raw)
				{
					buffer->Unlock();
				}
			}

			if (pFramesRead) *pFramesRead = written;
			return MA_SUCCESS;
		}

		inline ma_result MfSeek(ma_data_source* pDataSource, ma_uint64 frameIndex)
		{
			auto* self = reinterpret_cast<MfDataSource*>(pDataSource);
			if (!self || !self->initialized || !self->reader || self->sampleRate == 0)
				return MA_INVALID_OPERATION;

			const LONGLONG target100ns = static_cast<LONGLONG>(
				(static_cast<double>(frameIndex) * 10000000.0) / static_cast<double>(self->sampleRate));
			PROPVARIANT pos;
			PropVariantInit(&pos);
			pos.vt = VT_I8;
			pos.hVal.QuadPart = target100ns;
			const HRESULT hr = self->reader->SetCurrentPosition(GUID_NULL, pos);
			PropVariantClear(&pos);
			if (FAILED(hr)) return MA_ERROR;
			self->pending.clear();
			self->pendingReadIndex = 0;
			self->cursorFrames = frameIndex;
			return MA_SUCCESS;
		}

		inline ma_result MfGetDataFormat(ma_data_source* pDataSource, ma_format* pFormat,
			ma_uint32* pChannels, ma_uint32* pSampleRate, ma_channel* pChannelMap, size_t channelMapCap)
		{
			auto* self = reinterpret_cast<MfDataSource*>(pDataSource);
			if (!self || !self->initialized) return MA_INVALID_OPERATION;
			if (pFormat) *pFormat = ma_format_f32;
			if (pChannels) *pChannels = self->channels;
			if (pSampleRate) *pSampleRate = self->sampleRate;
			if (pChannelMap)
				ma_channel_map_init_standard(ma_standard_channel_map_default, pChannelMap, channelMapCap, self->channels);
			return MA_SUCCESS;
		}

		inline ma_result MfGetCursor(ma_data_source* pDataSource, ma_uint64* pCursor)
		{
			auto* self = reinterpret_cast<MfDataSource*>(pDataSource);
			if (!self || !self->initialized || !pCursor) return MA_INVALID_OPERATION;
			*pCursor = self->cursorFrames;
			return MA_SUCCESS;
		}

		inline ma_result MfGetLength(ma_data_source* pDataSource, ma_uint64* pLength)
		{
			auto* self = reinterpret_cast<MfDataSource*>(pDataSource);
			if (!self || !self->initialized || !pLength) return MA_INVALID_OPERATION;
			*pLength = self->totalFrames;
			return MA_SUCCESS;
		}

		inline ma_data_source_vtable g_mfDataSourceVtable = {
			MfRead,
			MfSeek,
			MfGetDataFormat,
			MfGetCursor,
			MfGetLength,
			nullptr,
			0
		};
	} // namespace detail

	inline void UninitMfDataSource(MfDataSource& src)
	{
		src.reader.Reset();
		src.pending.clear();
		src.pendingReadIndex = 0;
		if (src.initialized)
		{
			ma_data_source_uninit(&src.ds);
			src.initialized = false;
		}
		if (src.mfStarted)
		{
			MFShutdown();
			src.mfStarted = false;
		}
		if (src.comInitialized)
		{
			CoUninitialize();
			src.comInitialized = false;
		}
		src.sampleRate = 0;
		src.channels = 0;
		src.totalFrames = 0;
		src.cursorFrames = 0;
	}

	inline bool InitMfDataSource(const std::wstring& widePath, MfDataSource& src)
	{
		UninitMfDataSource(src);
		if (widePath.empty()) return false;

		const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		src.comInitialized = (co == S_OK || co == S_FALSE);
		if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE)))
		{
			UninitMfDataSource(src);
			return false;
		}
		src.mfStarted = true;

		if (FAILED(MFCreateSourceReaderFromURL(widePath.c_str(), nullptr, &src.reader)) || !src.reader)
		{
			UninitMfDataSource(src);
			return false;
		}

		ComPtr<IMFMediaType> partial;
		if (FAILED(MFCreateMediaType(&partial)))
		{
			UninitMfDataSource(src);
			return false;
		}
		partial->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
		partial->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
		partial->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
		partial->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);

		if (FAILED(src.reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, partial.Get())))
		{
			// Retry without forcing channel count if source decoder rejects stereo upmix.
			ComPtr<IMFMediaType> partial2;
			if (FAILED(MFCreateMediaType(&partial2))) { UninitMfDataSource(src); return false; }
			partial2->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
			partial2->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
			if (FAILED(src.reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, partial2.Get())))
			{
				UninitMfDataSource(src);
				return false;
			}
		}

		ComPtr<IMFMediaType> actual;
		if (FAILED(src.reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &actual)) || !actual)
		{
			UninitMfDataSource(src);
			return false;
		}

		UINT32 sr = 0, ch = 0;
		actual->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
		actual->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
		if (sr == 0 || ch == 0)
		{
			UninitMfDataSource(src);
			return false;
		}

		PROPVARIANT var;
		PropVariantInit(&var);
		uint64_t total = 0;
		if (SUCCEEDED(src.reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &var)))
		{
			ULONGLONG dur100ns = 0;
			if (SUCCEEDED(PropVariantToUInt64(var, &dur100ns)))
				total = static_cast<uint64_t>((static_cast<double>(dur100ns) / 10000000.0) * sr + 0.5);
			PropVariantClear(&var);
		}

		ma_data_source_config cfg = ma_data_source_config_init();
		cfg.vtable = &detail::g_mfDataSourceVtable;
		if (ma_data_source_init(&cfg, &src.ds) != MA_SUCCESS)
		{
			UninitMfDataSource(src);
			return false;
		}

		src.sampleRate = sr;
		src.channels = ch;
		src.totalFrames = total;
		src.cursorFrames = 0;
		src.initialized = true;
		return true;
	}
} // namespace GakuAudio
