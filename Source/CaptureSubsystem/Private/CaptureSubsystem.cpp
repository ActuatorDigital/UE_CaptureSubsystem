// Copyright iraj mohtasham aurelion.net 2023
#include "CaptureSubsystem.h"
#include "Interfaces/IPluginManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY(LogCaptureSubsystem);

void FCaptureSubsystemModule::StartupModule()
{
	AVUtilLibrary = LoadLibrary(TEXT("avutil"), TEXT("57"));
	SWResampleLibrary = LoadLibrary(TEXT("swresample"), TEXT("4"));
	AVCodecLibrary = LoadLibrary(TEXT("avcodec"), TEXT("59"));
	AVFormatLibrary = LoadLibrary(TEXT("avformat"), TEXT("59"));
	SWScaleLibrary = LoadLibrary(TEXT("swscale"), TEXT("6"));
	PostProcLibrary = LoadLibrary(TEXT("postproc"), TEXT("56"));
	AVFilterLibrary = LoadLibrary(TEXT("avfilter"), TEXT("8"));
	AVDeviceLibrary = LoadLibrary(TEXT("avdevice"), TEXT("59"));
	Initialized = true;
}

void FCaptureSubsystemModule::ShutdownModule()
{
	if (!Initialized) return;
	if (AVDeviceLibrary) FPlatformProcess::FreeDllHandle(AVDeviceLibrary);
	if (AVFilterLibrary) FPlatformProcess::FreeDllHandle(AVFilterLibrary);
	if (PostProcLibrary) FPlatformProcess::FreeDllHandle(PostProcLibrary);
	if (SWScaleLibrary) FPlatformProcess::FreeDllHandle(SWScaleLibrary);
	if (AVFormatLibrary) FPlatformProcess::FreeDllHandle(AVFormatLibrary);
	if (AVCodecLibrary) FPlatformProcess::FreeDllHandle(AVCodecLibrary);
	if (SWResampleLibrary) FPlatformProcess::FreeDllHandle(SWResampleLibrary);
	if (AVUtilLibrary) FPlatformProcess::FreeDllHandle(AVUtilLibrary);
	Initialized = false;
}

void* FCaptureSubsystemModule::LoadLibrary(const FString& Name, const FString& Version)
{
	FString Directory;
	FString Prefix;
	FString Separator;
	FString Extension;
#if PLATFORM_MAC
	Directory = IPluginManager::Get().FindPlugin(TEXT("CaptureSubsystem"))->GetBaseDir() / TEXT("Source/ThirdParty/ffmpeg/lib/osx");
	Prefix = TEXT("lib");
	Separator = TEXT(".");
	Extension = TEXT(".dylib");
#elif PLATFORM_WINDOWS
	// FFMPEG.build.cs stages the DLLs alongside the project binaries.
	Directory = FPaths::ProjectDir() / TEXT("Binaries/Win64");
	Separator = TEXT("-");
	Extension = TEXT(".dll");
#endif
	if (Directory.IsEmpty()) return nullptr;
	const FString Path = Directory / (Prefix + Name + Separator + Version + Extension);
	void* Library = FPlatformProcess::GetDllHandle(*Path);
	if (!Library) UE_LOG(LogCaptureSubsystem, Error, TEXT("Cannot load FFmpeg library: %s"), *Path);
	return Library;
}

IMPLEMENT_MODULE(FCaptureSubsystemModule, CaptureSubsystem)
