using UnrealBuildTool;
using System.IO;

public class CafeBazaarBilling : ModuleRules
{
	public CafeBazaarBilling(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Json",
			"JsonUtilities",
			"UMG",
			"Slate",
			"SlateCore"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Projects"
		});

		if (Target.Platform == UnrealTargetPlatform.Android)
		{
			PrivateDependencyModuleNames.Add("Launch");

			string PluginRootDir = Path.Combine(ModuleDirectory, "..", "..");
			string UPLFilePath = Path.Combine(PluginRootDir, "CafeBazaarBilling_UPL.xml");

			AdditionalPropertiesForReceipt.Add("AndroidPlugin", UPLFilePath);
		}
	}
}
