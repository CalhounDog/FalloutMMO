// Copyright 2017 Mipmap Games

using UnrealBuildTool;
using System.Collections.Generic;

public class RealisticProjectilePhysicTarget : TargetRules
{
	public RealisticProjectilePhysicTarget(TargetInfo Target)
	{
		Type = TargetType.Game;
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
		OutExtraModuleNames.AddRange( new string[] { "RealisticProjectilePhysic" } );
	}
}
