// Copyright 2017 Mipmap Games

using UnrealBuildTool;
using System.Collections.Generic;

public class RealisticProjectilePhysicEditorTarget : TargetRules
{
	public RealisticProjectilePhysicEditorTarget(TargetInfo Target)
	{
		Type = TargetType.Editor;
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
