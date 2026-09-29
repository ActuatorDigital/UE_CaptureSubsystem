// Copyright iraj mohtasham aurelion.net 2023
#include "VideoCaptureSubsystem.h"

#include "CaptureSubsystem.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"
#include "TextureResource.h"
#include "Math/Color.h"
#include "Containers/Ticker.h"
#include <atomic>

extern "C"
{
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/imgutils.h"
#include "libswscale/swscale.h"
}

namespace
{
constexpr int32 MaxQueuedFrames = 4;
constexpr int32 MaxPendingReadbacks = 3;
constexpr int32 MaxDimension = 3840;

FIntPoint FitVideoContent(FIntPoint SourceSize, FIntPoint OutputSize)
{
	if (int64(SourceSize.X) * OutputSize.Y > int64(SourceSize.Y) * OutputSize.X)
	{
		return FIntPoint(OutputSize.X, int32(int64(OutputSize.X) * SourceSize.Y / SourceSize.X) & ~1);
	}
	return FIntPoint(int32(int64(OutputSize.Y) * SourceSize.X / SourceSize.Y) & ~1, OutputSize.Y);
}

FString VideoError(const char* Operation, int Code)
{
	char Description[AV_ERROR_MAX_STRING_SIZE] = {};
	av_strerror(Code, Description, sizeof(Description));
	return FString::Printf(TEXT("%hs: %hs"), Operation, Description);
}

struct FVideoFrame
{
	TArray<uint8> Pixels;
};
}

// Render-thread GPU copies, worker-thread codec/file ownership, game-thread completion.
class FVideoCaptureSession final : public FRunnable
{
public:
	FVideoCaptureSession(FTextureRenderTargetResource* InResource, FIntPoint InSourceSize, EPixelFormat InFormat,
		FString InFilename, FIntPoint InOutputSize, int32 InFPS, int32 InBitrate)
		: Resource(InResource), SourceFormat(InFormat), Filename(MoveTemp(InFilename)), OutputSize(InOutputSize), FPS(InFPS), Bitrate(InBitrate)
	{
		Width = InSourceSize.X;
		Height = InSourceSize.Y;
		Wake = FPlatformProcess::GetSynchEventFromPool(false);
	}

	~FVideoCaptureSession() override
	{
		StopRequested.store(true);
		EncodingStop.store(true);
		Wake->Trigger();
		if (Thread)
		{
			Thread->WaitForCompletion();
			delete Thread;
		}
		FPlatformProcess::ReturnSynchEventToPool(Wake);
	}

	bool Start()
	{
		Thread = FRunnableThread::Create(this, TEXT("VideoCaptureEncoder"));
		return Thread != nullptr;
	}

	void Stop()
	{
		StopRequested.store(true);
	}

	void Pump_RenderThread(bool bAllowNewFrame)
	{
		if (Finished.load()) return;

		for (int32 Index = 0; Index < PendingReadbacks.Num();)
		{
			FRHIGPUTextureReadback& Readback = *PendingReadbacks[Index];
			if (!Readback.IsReady()) break; // preserve capture order

			int32 PitchInPixels = 0;
			int32 BufferHeight = 0;
			const uint8* Data = static_cast<const uint8*>(Readback.Lock(PitchInPixels, &BufferHeight));
			if (Data && PitchInPixels >= Width && BufferHeight >= Height && QueuedFrames.load() < MaxQueuedFrames)
			{
				FVideoFrame Frame;
				const int32 PixelBytes = SourceFormat == PF_FloatRGBA ? sizeof(FFloat16Color) : sizeof(FColor);
				Frame.Pixels.SetNumUninitialized(Width * Height * PixelBytes);
				for (int32 Row = 0; Row < Height; ++Row)
				{
					FMemory::Memcpy(Frame.Pixels.GetData() + Row * Width * PixelBytes,
						Data + Row * PitchInPixels * PixelBytes, Width * PixelBytes);
				}
				QueuedFrames.fetch_add(1);
				Frames.Enqueue(MoveTemp(Frame));
				Wake->Trigger();
			}
			else
			{
				DroppedFrames.fetch_add(1);
			}
			if (Data) Readback.Unlock();
			PendingReadbacks.RemoveAt(Index);
		}

		if (StopRequested.load())
		{
			if (PendingReadbacks.IsEmpty())
			{
				EncodingStop.store(true);
				Wake->Trigger();
			}
			return;
		}

		if (!bAllowNewFrame) return;
		const double Now = FPlatformTime::Seconds();
		if (Now - LastCaptureTime < 1.0 / FPS) return;
		LastCaptureTime = Now;
		if (PendingReadbacks.Num() >= MaxPendingReadbacks || QueuedFrames.load() >= MaxQueuedFrames)
		{
			DroppedFrames.fetch_add(1);
			return;
		}
		const FTextureRHIRef Texture = Resource->GetRenderTargetTexture();
		if (!Texture || Texture->GetFormat() != SourceFormat || Texture->IsMultisampled() ||
			Texture->GetSizeX() != Width || Texture->GetSizeY() != Height)
		{
			SourceInvalidated.store(true);
			StopRequested.store(true);
			return;
		}
		auto Readback = MakeUnique<FRHIGPUTextureReadback>(TEXT("VideoCaptureReadback"));
		FRHICommandListImmediate& Commands = GRHICommandList.GetImmediateCommandList();
		Readback->EnqueueCopy(Commands, Texture.GetReference());
		PendingReadbacks.Add(MoveTemp(Readback));
	}

	bool IsFinished() const { return Finished.load(); }
	bool IsStopRequested() const { return StopRequested.load(); }
	const FString& GetFilename() const { return Filename; }
	const FString& GetError() const { return Error; }
	int32 GetDroppedFrames() const { return DroppedFrames.load(); }

	uint32 Run() override
	{
		Error = Encode();
		Finished.store(true);
		return 0;
	}

private:
	FString Encode()
	{
		AVFormatContext* Format = nullptr;
		AVCodecContext* CodecContext = nullptr;
		SwsContext* Converter = nullptr;
		AVFrame* Picture = nullptr;
		AVPacket* Packet = nullptr;
		FString Failure;
		int32 WrittenFrames = 0;
		int Result = 0;
		const FIntPoint ContentSize = FitVideoContent(FIntPoint(Width, Height), OutputSize);
		const int32 Left = ((OutputSize.X - ContentSize.X) / 4) * 2;
		const int32 Top = ((OutputSize.Y - ContentSize.Y) / 4) * 2;

		do
		{
			const FString Directory = FPaths::GetPath(Filename);
			if (!FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*Directory))
			{
				Failure = TEXT("Could not create video output directory");
				break;
			}
			Result = avformat_alloc_output_context2(&Format, nullptr, "mp4", TCHAR_TO_UTF8(*Filename));
			if (Result < 0 || !Format) { Failure = VideoError("MP4 output", Result); break; }

			const AVCodec* Encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
			if (!Encoder) { Failure = TEXT("No H.264 encoder in bundled FFmpeg"); break; }
			CodecContext = avcodec_alloc_context3(Encoder);
			if (!CodecContext) { Failure = TEXT("Could not allocate video encoder"); break; }
			CodecContext->width = OutputSize.X;
			CodecContext->height = OutputSize.Y;
			CodecContext->pix_fmt = AV_PIX_FMT_YUV420P;
			CodecContext->time_base = {1, FPS};
			CodecContext->framerate = {FPS, 1};
			CodecContext->bit_rate = Bitrate;
			if (Format->oformat->flags & AVFMT_GLOBALHEADER) CodecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
			Result = avcodec_open2(CodecContext, Encoder, nullptr);
			if (Result < 0) { Failure = VideoError("Open H.264 encoder", Result); break; }

			AVStream* Stream = avformat_new_stream(Format, nullptr);
			if (!Stream) { Failure = TEXT("Could not create video stream"); break; }
			Stream->time_base = CodecContext->time_base;
			Result = avcodec_parameters_from_context(Stream->codecpar, CodecContext);
			if (Result < 0) { Failure = VideoError("Video stream", Result); break; }
			Result = avio_open(&Format->pb, TCHAR_TO_UTF8(*Filename), AVIO_FLAG_WRITE);
			if (Result < 0) { Failure = VideoError("Open video file", Result); break; }
			Result = avformat_write_header(Format, nullptr);
			if (Result < 0) { Failure = VideoError("Write MP4 header", Result); break; }

			Converter = sws_getContext(Width, Height, AV_PIX_FMT_BGRA, ContentSize.X, ContentSize.Y,
				AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
			Picture = av_frame_alloc();
			Packet = av_packet_alloc();
			if (!Converter || !Picture || !Packet) { Failure = TEXT("Could not allocate video frame converter"); break; }
			Picture->format = CodecContext->pix_fmt;
			Picture->width = OutputSize.X;
			Picture->height = OutputSize.Y;
			Result = av_frame_get_buffer(Picture, 32);
			if (Result < 0) { Failure = VideoError("Allocate video frame", Result); break; }

			auto DrainPackets = [&]() -> bool
			{
				while (true)
				{
					int Code = avcodec_receive_packet(CodecContext, Packet);
					if (Code == AVERROR(EAGAIN) || Code == AVERROR_EOF) return true;
					if (Code < 0) { Failure = VideoError("Encode video", Code); return false; }
					av_packet_rescale_ts(Packet, CodecContext->time_base, Stream->time_base);
					Packet->stream_index = Stream->index;
					Code = av_interleaved_write_frame(Format, Packet);
					av_packet_unref(Packet);
					if (Code < 0) { Failure = VideoError("Write video packet", Code); return false; }
				}
			};

			while (!EncodingStop.load() || QueuedFrames.load() > 0)
			{
				FVideoFrame Frame;
				if (!Frames.Dequeue(Frame)) { Wake->Wait(20); continue; }
				QueuedFrames.fetch_sub(1);
				Result = av_frame_make_writable(Picture);
				if (Result < 0) { Failure = VideoError("Prepare video frame", Result); break; }
				if (ContentSize != OutputSize)
				{
					const ptrdiff_t PictureStrides[4] = {Picture->linesize[0], Picture->linesize[1], Picture->linesize[2], 0};
					Result = av_image_fill_black(Picture->data, PictureStrides, AV_PIX_FMT_YUV420P,
						AVCOL_RANGE_MPEG, OutputSize.X, OutputSize.Y);
					if (Result < 0) { Failure = VideoError("Fill video bars", Result); break; }
				}
				uint8* ContentPlanes[4] = {
					Picture->data[0] + Top * Picture->linesize[0] + Left,
					Picture->data[1] + (Top / 2) * Picture->linesize[1] + Left / 2,
					Picture->data[2] + (Top / 2) * Picture->linesize[2] + Left / 2,
					nullptr
				};
				TArray<FColor> SDRPixels;
				const uint8* SourcePixels = Frame.Pixels.GetData();
				if (SourceFormat == PF_FloatRGBA)
				{
					SDRPixels.SetNumUninitialized(Width * Height);
					for (int32 PixelIndex = 0; PixelIndex < Width * Height; ++PixelIndex)
					{
						FFloat16Color HDRPixel;
						FMemory::Memcpy(&HDRPixel, SourcePixels + PixelIndex * sizeof(FFloat16Color), sizeof(FFloat16Color));
						SDRPixels[PixelIndex] = FLinearColor(HDRPixel.R.GetFloat(), HDRPixel.G.GetFloat(),
							HDRPixel.B.GetFloat(), HDRPixel.A.GetFloat()).ToFColor(true);
					}
					SourcePixels = reinterpret_cast<const uint8*>(SDRPixels.GetData());
				}
				const uint8* SourcePlanes[1] = {SourcePixels};
				const int SourceStrides[1] = {Width * int32(sizeof(FColor))};
				sws_scale(Converter, SourcePlanes, SourceStrides, 0, Height, ContentPlanes, Picture->linesize);
				Picture->pts = WrittenFrames++;
				Result = avcodec_send_frame(CodecContext, Picture);
				if (Result < 0) { Failure = VideoError("Submit video frame", Result); break; }
				if (!DrainPackets()) break;
			}
			if (!Failure.IsEmpty()) break;
			if (WrittenFrames == 0) { Failure = TEXT("No video frames were captured"); break; }
			Result = avcodec_send_frame(CodecContext, nullptr);
			if (Result < 0 || !DrainPackets())
			{
				if (Failure.IsEmpty()) Failure = VideoError("Flush video encoder", Result);
				break;
			}
			Result = av_write_trailer(Format);
			if (Result < 0) Failure = VideoError("Finalise MP4", Result);
		} while (false);

		av_packet_free(&Packet);
		av_frame_free(&Picture);
		sws_freeContext(Converter);
		avcodec_free_context(&CodecContext);
		if (Format)
		{
			if (Format->pb) avio_closep(&Format->pb);
			avformat_free_context(Format);
		}
		if (SourceInvalidated.load() && Failure.IsEmpty())
		{
			Failure = TEXT("Source render target changed during capture");
		}
		return Failure;
	}

	FTextureRenderTargetResource* Resource; // valid while SourceTarget is retained and not resized
	EPixelFormat SourceFormat;
	FString Filename;
	FIntPoint OutputSize;
	int32 FPS;
	int32 Bitrate;
	int32 Width;
	int32 Height;
	FEvent* Wake = nullptr;
	FRunnableThread* Thread = nullptr;
	TQueue<FVideoFrame, EQueueMode::Spsc> Frames;
	std::atomic<int32> QueuedFrames{0};
	std::atomic<int32> DroppedFrames{0};
	std::atomic<bool> StopRequested{false};
	std::atomic<bool> EncodingStop{false};
	std::atomic<bool> Finished{false};
	std::atomic<bool> SourceInvalidated{false};
	TArray<TUniquePtr<FRHIGPUTextureReadback>> PendingReadbacks; // render thread only
	double LastCaptureTime = 0; // render thread only
	FString Error; // published when Finished becomes true
};

bool UVideoCaptureSubsystem::StartCapture(UTextureRenderTarget2D* Source, const FString& Filename,
	FIntPoint OutputSize, int32 FramesPerSecond, int32 Bitrate, FString& Error)
{
	Error.Reset();
	if (Session) { Error = TEXT("Recording or finalisation is already in progress"); return false; }
	if (!Source || Filename.IsEmpty() || !Filename.EndsWith(TEXT(".mp4"), ESearchCase::IgnoreCase))
	{
		Error = TEXT("A render target and an MP4 filename are required");
		return false;
	}
	if (OutputSize.X < 2 || OutputSize.Y < 2 || OutputSize.X > MaxDimension || OutputSize.Y > MaxDimension ||
		(OutputSize.X & 1) || (OutputSize.Y & 1) || FramesPerSecond < 1 || FramesPerSecond > 120 ||
		Bitrate <= 0 || Bitrate > 100000000 || Source->SizeX < 2 || Source->SizeY < 2 ||
		int64(Source->SizeX) * Source->SizeY > int64(MaxDimension) * MaxDimension)
	{
		Error = TEXT("Invalid source size, even output size (max 3840), FPS (1-120), or bitrate (1-100000000 bps)");
		return false;
	}
	const FIntPoint ContentSize = FitVideoContent(FIntPoint(Source->SizeX, Source->SizeY), OutputSize);
	if (ContentSize.X < 2 || ContentSize.Y < 2)
	{
		Error = TEXT("Output size cannot fit the source aspect ratio at even YUV420 dimensions");
		return false;
	}
	FTextureRenderTargetResource* Resource = Source->GameThread_GetRenderTargetResource();
	if (!Resource || (Source->GetFormat() != PF_B8G8R8A8 && Source->GetFormat() != PF_FloatRGBA))
	{
		Error = TEXT("Source must be an initialised BGRA8 or RGBA16F render target");
		return false;
	}
	Session = MakeShared<FVideoCaptureSession, ESPMode::ThreadSafe>(Resource, FIntPoint(Source->SizeX, Source->SizeY),
		Source->GetFormat(), FPaths::ConvertRelativePathToFull(Filename), OutputSize, FramesPerSecond, Bitrate);
	if (!Session->Start())
	{
		Session.Reset();
		Error = TEXT("Could not start video encoder thread");
		return false;
	}
	SourceTarget = Source; // keep render target/resource alive until completion
	TSharedPtr<FVideoCaptureSession, ESPMode::ThreadSafe> Recording = Session;
	BackBufferHandle = FSlateApplication::Get().GetRenderer()->OnBackBufferReadyToPresent().AddLambda(
		[Recording](SWindow&, const FTexture2DRHIRef&) { Recording->Pump_RenderThread(true); });
	PollHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &UVideoCaptureSubsystem::PollCompletion));
	return true;
}

void UVideoCaptureSubsystem::StopCapture()
{
	if (Session) Session->Stop();
}

bool UVideoCaptureSubsystem::IsRecording() const
{
	return Session.IsValid(); // includes finalisation
}

bool UVideoCaptureSubsystem::PollCompletion(float)
{
	if (!Session) return false;
	if (!Session->IsFinished())
	{
		if (BackBufferHandle.IsValid() && Session->IsStopRequested())
		{
			TSharedPtr<FVideoCaptureSession, ESPMode::ThreadSafe> Recording = Session;
			ENQUEUE_RENDER_COMMAND(PumpVideoReadback)([Recording](FRHICommandListImmediate&)
			{
				Recording->Pump_RenderThread(false);
			});
		}
		return true;
	}

	FSlateApplication::Get().GetRenderer()->OnBackBufferReadyToPresent().Remove(BackBufferHandle);
	BackBufferHandle.Reset();
	const FString Filename = Session->GetFilename();
	FString Error = Session->GetError();
	const int32 Dropped = Session->GetDroppedFrames();
	if (Dropped > 0)
	{
		UE_LOG(LogCaptureSubsystem, Warning, TEXT("Video capture dropped %d frames: %s"), Dropped, *Filename);
	}
	Session.Reset();
	SourceTarget = nullptr;
	OnCaptureFinished.Broadcast(Filename, Error, Dropped);
	return false;
}

void UVideoCaptureSubsystem::Deinitialize()
{
	if (BackBufferHandle.IsValid())
	{
		FSlateApplication::Get().GetRenderer()->OnBackBufferReadyToPresent().Remove(BackBufferHandle);
		BackBufferHandle.Reset();
	}
	if (PollHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(PollHandle);
		PollHandle.Reset();
	}
	if (Session)
	{
		Session->Stop();
		FlushRenderingCommands(); // shutdown only: release queued RHI callbacks before the source disappears
		Session.Reset();
	}
	SourceTarget = nullptr;
	Super::Deinitialize();
}
