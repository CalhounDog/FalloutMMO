using UnrealBuildTool;
using System.Collections.Generic;

[SupportedPlatforms(UnrealPlatformClass.Server)]
public class RealisticProjectilePhysicServerTarget : TargetRules
{
    public RealisticProjectilePhysicServerTarget(TargetInfo Target)
    {
        Type = TargetType.Server;
        bUsesSteam = false;
    }

    //
    // TargetRules interface.
    //

    public override void SetupBinaries(
        TargetInfo Target,
        ref List<UEBuildBinaryConfiguration> OutBuildBinaryConfigurations,
        ref List<string> OutExtraModuleNames
        )
    {
        OutExtraModuleNames.Add("RealisticProjectilePhysic");
    }
}