// Copyright iraj mohtasham aurelion.net 2023

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
DECLARE_LOG_CATEGORY_EXTERN(LogCaptureSubsystem, Log, All);
class FCaptureSubsystemModule : public IModuleInterface
{
public:

	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;


private:
	void* LoadLibrary(const  FString& name, const FString& version);
	void* AVUtilLibrary = nullptr;
	void* SWResampleLibrary = nullptr;
	void* AVCodecLibrary = nullptr;
	void* SWScaleLibrary = nullptr;
	void* AVFormatLibrary = nullptr;
	void* PostProcLibrary = nullptr;
	void* AVFilterLibrary = nullptr;
	void* AVDeviceLibrary = nullptr;

	bool Initialized = false;
};
