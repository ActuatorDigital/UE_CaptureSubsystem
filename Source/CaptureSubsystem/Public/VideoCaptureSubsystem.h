// Copyright iraj mohtasham aurelion.net 2023
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Containers/Ticker.h"
#include "VideoCaptureSubsystem.generated.h"

class UTextureRenderTarget2D;
class FVideoCaptureSession;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnVideoCaptureFinished, const FString&, Filename, const FString&, Error, int32, DroppedFrames);

/** Video-only capture from a fixed-size BGRA8 or RGBA16F render target. The caller owns the source camera. */
UCLASS()
class CAPTURESUBSYSTEM_API UVideoCaptureSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()
public:
	/** The filename may replace an existing file. Returns false without touching it on invalid input. */
	UFUNCTION(BlueprintCallable, Category = "Capture")
	bool StartCapture(UTextureRenderTarget2D* Source, const FString& Filename, FIntPoint OutputSize,
		int32 FramesPerSecond, int32 Bitrate, FString& Error);

	/** Returns immediately; wait for OnCaptureFinished before starting another recording. */
	UFUNCTION(BlueprintCallable, Category = "Capture")
	void StopCapture();

	UFUNCTION(BlueprintPure, Category = "Capture")
	bool IsRecording() const;

	UPROPERTY(BlueprintAssignable, Category = "Capture")
	FOnVideoCaptureFinished OnCaptureFinished;

	virtual void Deinitialize() override;

private:
	bool PollCompletion(float DeltaTime);
	TSharedPtr<FVideoCaptureSession, ESPMode::ThreadSafe> Session;
	FDelegateHandle BackBufferHandle;
	FTSTicker::FDelegateHandle PollHandle;
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> SourceTarget;
};
