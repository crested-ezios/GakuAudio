// SPDX-License-Identifier: MIT
#include "../include/GakuPlugin.h"
#include "../include/GakuPaths.hpp"
#include "../include/GakuAudio.h"
#include "GakuAudioEngine.hpp"

#include <filesystem>
#include <memory>

namespace
{
	struct PluginContext
	{
		GakuHostApi host{};
		GakuLogService log{};
		GakuRuntimeService runtime{};
		GakuPathsV1 paths{};
		GakuAudio::AudioEngine engine;
		GakuAudioServiceV1 service{};
	};

	GakuStatus ApiOpenAndPlay(void* ctx, const char* pathUtf8, double startSeconds)
	{
		return ctx ? static_cast<PluginContext*>(ctx)->engine.OpenAndPlay(pathUtf8, startSeconds) : GAKU_E_INVALID_ARGUMENT;
	}
	GakuStatus ApiPause(void* ctx)
	{
		return ctx ? static_cast<PluginContext*>(ctx)->engine.Pause() : GAKU_E_INVALID_ARGUMENT;
	}
	GakuStatus ApiResumeAt(void* ctx, double seconds)
	{
		return ctx ? static_cast<PluginContext*>(ctx)->engine.ResumeAt(seconds) : GAKU_E_INVALID_ARGUMENT;
	}
	GakuStatus ApiSeek(void* ctx, double seconds, uint32_t whilePlaying)
	{
		return ctx ? static_cast<PluginContext*>(ctx)->engine.Seek(seconds, whilePlaying != 0) : GAKU_E_INVALID_ARGUMENT;
	}
	GakuStatus ApiStop(void* ctx)
	{
		return ctx ? static_cast<PluginContext*>(ctx)->engine.Stop() : GAKU_E_INVALID_ARGUMENT;
	}
	GakuStatus ApiObserve(void* ctx, GakuAudioObservationV1* output)
	{
		return ctx ? static_cast<PluginContext*>(ctx)->engine.Observe(output) : GAKU_E_INVALID_ARGUMENT;
	}
	GakuStatus ApiSetVolume(void* ctx, float volume)
	{
		return ctx ? static_cast<PluginContext*>(ctx)->engine.SetVolume(volume) : GAKU_E_INVALID_ARGUMENT;
	}
	GakuStatus ApiSetPlaybackRate(void* ctx, float rate)
	{
		return ctx ? static_cast<PluginContext*>(ctx)->engine.SetPlaybackRate(rate) : GAKU_E_INVALID_ARGUMENT;
	}
	GakuStatus ApiProbeFile(void* ctx, const char* pathUtf8, GakuAudioMetadataV1* output)
	{
		return ctx ? static_cast<PluginContext*>(ctx)->engine.ProbeFile(pathUtf8, output) : GAKU_E_INVALID_ARGUMENT;
	}
	GakuStatus ApiExtractCover(void* ctx, const char* pathUtf8, void* outBuffer, uint32_t bufferCapacity,
		uint32_t* outBytes, char* outMime32)
	{
		return ctx ? static_cast<PluginContext*>(ctx)->engine.ExtractCover(
			pathUtf8, outBuffer, bufferCapacity, outBytes, outMime32) : GAKU_E_INVALID_ARGUMENT;
	}
	uint32_t ApiGetPresetCount(void* ctx)
	{
		return ctx ? static_cast<PluginContext*>(ctx)->engine.GetPresetCount() : 0u;
	}
	GakuStatus ApiGetPreset(void* ctx, uint32_t index, GakuAudioEffectPresetV1* output)
	{
		return ctx ? static_cast<PluginContext*>(ctx)->engine.GetPreset(index, output) : GAKU_E_INVALID_ARGUMENT;
	}
	GakuStatus ApiGetActivePreset(void* ctx, char* outId, uint32_t capacity)
	{
		return ctx ? static_cast<PluginContext*>(ctx)->engine.GetActivePreset(outId, capacity) : GAKU_E_INVALID_ARGUMENT;
	}
	GakuStatus ApiSetActivePreset(void* ctx, const char* presetId)
	{
		return ctx ? static_cast<PluginContext*>(ctx)->engine.SetActivePreset(presetId) : GAKU_E_INVALID_ARGUMENT;
	}
	GakuStatus ApiRenderOfflineWav(void* ctx, const GakuAudioOfflineRenderRequestV1* request,
		char* errorBuffer, uint32_t errorCapacity)
	{
		return ctx ? static_cast<PluginContext*>(ctx)->engine.RenderOfflineWav(request, errorBuffer, errorCapacity) : GAKU_E_INVALID_ARGUMENT;
	}

	GakuStatus Activate(const GakuHostApi* host, void** pluginState)
	{
		if (!host || !pluginState || host->abiVersion != GAKU_ABI_VERSION || host->structSize < sizeof(GakuHostApi) ||
		    !host->queryService || !host->registerService)
			return GAKU_E_INVALID_ARGUMENT;

		auto ctx = std::make_unique<PluginContext>();
		ctx->host = *host;
		host->queryService(host->hostContext, GAKU_SERVICE_LOG, GAKU_SERVICE_VERSION, sizeof(ctx->log), &ctx->log);
		host->queryService(host->hostContext, GAKU_SERVICE_RUNTIME, GAKU_SERVICE_VERSION, sizeof(ctx->runtime), &ctx->runtime);

		std::filesystem::path configRoot;
		if (GakuPaths::Query(host, ctx->paths) == GAKU_OK)
		{
			GakuPaths::GetRoot(ctx->paths, GAKU_PATH_CONFIGS, configRoot);
		}
		if (configRoot.empty() && ctx->runtime.localRoot && ctx->runtime.localRoot[0])
			configRoot = std::filesystem::path(ctx->runtime.localRoot) / L"Configs" / L"gakuaudio";
		if (!configRoot.empty())
			ctx->engine.SetConfigPath(configRoot / L"config.json");

		ctx->service.abiVersion = GAKU_AUDIO_VERSION;
		ctx->service.structSize = sizeof(GakuAudioServiceV1);
		ctx->service.context = ctx.get();
		ctx->service.openAndPlay = &ApiOpenAndPlay;
		ctx->service.pause = &ApiPause;
		ctx->service.resumeAt = &ApiResumeAt;
		ctx->service.seek = &ApiSeek;
		ctx->service.stop = &ApiStop;
		ctx->service.observe = &ApiObserve;
		ctx->service.setVolume = &ApiSetVolume;
		ctx->service.setPlaybackRate = &ApiSetPlaybackRate;
		ctx->service.probeFile = &ApiProbeFile;
		ctx->service.extractCover = &ApiExtractCover;
		ctx->service.getPresetCount = &ApiGetPresetCount;
		ctx->service.getPreset = &ApiGetPreset;
		ctx->service.getActivePreset = &ApiGetActivePreset;
		ctx->service.setActivePreset = &ApiSetActivePreset;
		ctx->service.renderOfflineWav = &ApiRenderOfflineWav;

		const GakuStatus reg = host->registerService(host->hostContext, GAKU_SERVICE_AUDIO_CORE,
			sizeof(GakuAudioServiceV1), &ctx->service);
		if (reg != GAKU_OK) return reg;

		if (ctx->log.write)
			ctx->log.write(ctx->log.context, GAKU_LOG_INFO, "[GakuAudio] activated; service gakuaudio.core/v1 registered.");
		*pluginState = ctx.release();
		return GAKU_OK;
	}

	void Shutdown(void* pluginState)
	{
		auto* ctx = static_cast<PluginContext*>(pluginState);
		if (!ctx) return;
		ctx->engine.Shutdown();
		delete ctx;
	}
}

extern "C" GAKU_PLUGIN_EXPORT GakuStatus GakuPlugin_Query(uint32_t requestedAbi, uint32_t outputSize, GakuPluginDescriptor* output)
{
	if (!output || requestedAbi != GAKU_ABI_VERSION) return GAKU_E_VERSION;
	if (outputSize < sizeof(GakuPluginDescriptor)) return GAKU_E_SIZE;
	GakuPluginDescriptor desc{};
	desc.abiVersion = GAKU_ABI_VERSION;
	desc.structSize = sizeof(GakuPluginDescriptor);
	desc.id = "gakuaudio";
	desc.name = "GakuAudio";
	desc.version = "0.1.0";
	desc.activate = &Activate;
	desc.shutdown = &Shutdown;
	*output = desc;
	return GAKU_OK;
}
