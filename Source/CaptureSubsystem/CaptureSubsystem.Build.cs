// Copyright iraj mohtasham aurelion.net 2023
using UnrealBuildTool;

public class CaptureSubsystem : ModuleRules
{
    public CaptureSubsystem(ReadOnlyTargetRules Target) : base(Target)
    {
        PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });
        PrivateDependencyModuleNames.AddRange(new string[] { "FFMPEG", "RHI", "RenderCore", "Slate", "SlateCore", "Projects" });
    }
}
