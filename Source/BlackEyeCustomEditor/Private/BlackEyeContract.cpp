// SPDX-License-Identifier: Apache-2.0

#include "BlackEyeContract.h"

#include "CineCameraActor.h"
#include "Interfaces/IPluginManager.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY(LogBlackEyeCustom);

namespace
{
	// Names as declared in Black Eye 2.0.7. Keep this list complete: it is the dependency surface.
	const TCHAR* BlackEyePluginName = TEXT("Black_Eye");
	const TCHAR* BlackEyeScriptPackage = TEXT("/Script/Black_Eye");
	const TCHAR* CameraBaseClassName = TEXT("BlackEyeCineCameraActorBase");          // BlackEyeCineCameraActorBase.h:22
	const FName SnapNowFunctionName(TEXT("SnapComponentsToTargetsNow"));             // BlackEyeCineCameraActorBase.h:78
	const FName LookAtPropertyName(TEXT("LookAt"));                                  // BlackEyeCineCameraActorBase.h:64
	const FName FollowPropertyName(TEXT("Follow"));                                  // BlackEyeCineCameraActorBase.h:61

	bool Check(int32 Index, const TCHAR* What, bool bGood, const FString& Detail = FString())
	{
		UE_LOG(LogBlackEyeCustom, Display, TEXT("[BlackEyeCustom] selftest %d %s%s%s: %s"), Index, What,
		       Detail.IsEmpty() ? TEXT("") : TEXT(" "), *Detail, bGood ? TEXT("ok") : TEXT("FAIL"));
		return bGood;
	}
}

UClass* BlackEyeContract::GetCameraBaseClass()
{
	return FindObject<UClass>(FTopLevelAssetPath(BlackEyeScriptPackage, CameraBaseClassName));
}

bool BlackEyeContract::IsBlackEyeCamera(const AActor* Actor)
{
	const UClass* Base = GetCameraBaseClass();
	return Actor && Base && Actor->IsA(Base);
}

FString BlackEyeContract::GetInstalledVersion()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindEnabledPlugin(BlackEyePluginName);
	return Plugin.IsValid() ? Plugin->GetDescriptor().VersionName : FString();
}

bool BlackEyeContract::RunSelfTest()
{
	bool bAll = true;

	// 1. Black Eye is enabled. A different version is allowed but flagged: the file:line citations
	//    across this repo are pinned to TestedVersion.
	const FString Version = GetInstalledVersion();
	bAll &= Check(1, TEXT("Black_Eye plugin enabled"), !Version.IsEmpty(),
	              Version.IsEmpty() ? FString(TEXT("(not enabled in this project)")) : FString::Printf(TEXT("v%s"), *Version));
	if (!Version.IsEmpty() && Version != TestedVersion)
	{
		UE_LOG(LogBlackEyeCustom, Warning,
		       TEXT("[BlackEyeCustom] Black Eye v%s installed, tested against v%s: re-run the update checklist in .claude/refs/maintenance.md"),
		       *Version, TestedVersion);
	}

	// 2. The camera base class resolves by path and is still a CineCameraActor, which is what lets the
	//    bake read CineCameraComponent state without knowing Black Eye's own types.
	UClass* Base = GetCameraBaseClass();
	bAll &= Check(2, TEXT("camera base class"), Base && Base->IsChildOf(ACineCameraActor::StaticClass()),
	              FString::Printf(TEXT("%s.%s"), BlackEyeScriptPackage, CameraBaseClassName));

	// 3. The snap entry point exists and still takes no parameters (called through ProcessEvent).
	const UFunction* SnapNow = Base ? Base->FindFunctionByName(SnapNowFunctionName) : nullptr;
	bAll &= Check(3, TEXT("UFUNCTION"), SnapNow && SnapNow->NumParms == 0, SnapNowFunctionName.ToString());

	// 4. The Follow and LookAt component pointers, read to find the bake's subjects.
	for (const FName& Name : { FollowPropertyName, LookAtPropertyName })
	{
		const FProperty* Prop = Base ? Base->FindPropertyByName(Name) : nullptr;
		bAll &= Check(4, TEXT("component property"), CastField<FObjectPropertyBase>(Prop) != nullptr, Name.ToString());
	}

	UE_LOG(LogBlackEyeCustom, Display, TEXT("[BlackEyeCustom] selftest %s"), bAll ? TEXT("PASSED") : TEXT("FAILED"));
	return bAll;
}
