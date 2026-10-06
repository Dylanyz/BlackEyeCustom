// SPDX-License-Identifier: Apache-2.0

#include "BlackEyeContract.h"

#include "CineCameraActor.h"
#include "Components/SkeletalMeshComponent.h"
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
	const TCHAR* SimpleTargetStructName = TEXT("BlackEyeSimpleTarget");               // BlackEyeSimpleTarget.h:23
	const FName TargetActorPropertyName(TEXT("Actor"));                              // BlackEyeSimpleTarget.h:47
	const FName TargetComponentPropertyName(TEXT("ComponentName"));                  // BlackEyeSimpleTarget.h:50
	const FName TargetBonePropertyName(TEXT("BoneName"));                            // BlackEyeSimpleTarget.h:59

	UScriptStruct* GetSimpleTargetStruct()
	{
		return FindObject<UScriptStruct>(FTopLevelAssetPath(BlackEyeScriptPackage, SimpleTargetStructName));
	}

	/** Reads FBlackEyeSimpleTarget::Actor from one target struct instance. */
	AActor* ReadTargetActor(const UScriptStruct* TargetStruct, const void* TargetValue)
	{
		const FSoftObjectProperty* ActorProp = FindFProperty<FSoftObjectProperty>(TargetStruct, TargetActorPropertyName);
		return ActorProp ? Cast<AActor>(ActorProp->GetPropertyValue_InContainer(TargetValue).Get()) : nullptr;
	}

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

bool BlackEyeContract::SnapNow(AActor* Camera)
{
	if (!IsBlackEyeCamera(Camera))
	{
		return false;
	}
	UFunction* SnapNow = Camera->FindFunction(SnapNowFunctionName);
	if (!SnapNow || SnapNow->NumParms != 0)
	{
		return false;
	}
	Camera->ProcessEvent(SnapNow, nullptr);
	return true;
}

void BlackEyeContract::GetSubjectActors(const AActor* Camera, TArray<AActor*>& OutActors)
{
	const UClass* Base = GetCameraBaseClass();
	const UScriptStruct* TargetStruct = GetSimpleTargetStruct();
	if (!Camera || !Base || !TargetStruct || !Camera->IsA(Base))
	{
		return;
	}

	for (const FName& ComponentName : { FollowPropertyName, LookAtPropertyName })
	{
		const FObjectPropertyBase* ComponentProp = CastField<FObjectPropertyBase>(Base->FindPropertyByName(ComponentName));
		const UObject* Component = ComponentProp ? ComponentProp->GetObjectPropertyValue_InContainer(Camera) : nullptr;
		if (!Component)
		{
			continue;
		}
		for (TFieldIterator<FStructProperty> It(Component->GetClass()); It; ++It)
		{
			if (It->Struct && It->Struct->IsChildOf(TargetStruct))
			{
				if (AActor* Subject = ReadTargetActor(It->Struct, It->ContainerPtrToValuePtr<void>(Component)))
				{
					OutActors.AddUnique(Subject);
				}
			}
		}
	}
}

bool BlackEyeContract::GetFirstLookAtSubjectPoint(const AActor* Camera, FVector& OutWorld)
{
	const UClass* Base = GetCameraBaseClass();
	const UScriptStruct* TargetStruct = GetSimpleTargetStruct();
	const FObjectPropertyBase* LookAtProp = Base ? CastField<FObjectPropertyBase>(Base->FindPropertyByName(LookAtPropertyName)) : nullptr;
	const UObject* LookAt = (LookAtProp && Camera && Camera->IsA(Base)) ? LookAtProp->GetObjectPropertyValue_InContainer(Camera) : nullptr;
	if (!LookAt || !TargetStruct)
	{
		return false;
	}
	for (TFieldIterator<FStructProperty> It(LookAt->GetClass()); It; ++It)
	{
		if (!It->Struct || !It->Struct->IsChildOf(TargetStruct))
		{
			continue;
		}
		// The first target struct declared is Target_0 (BlackEyeLookAtComponent.h:38).
		const void* Target = It->ContainerPtrToValuePtr<void>(LookAt);
		const AActor* Subject = ReadTargetActor(It->Struct, Target);
		if (!Subject)
		{
			return false;
		}
		const FStrProperty* CompProp = FindFProperty<FStrProperty>(It->Struct, TargetComponentPropertyName);
		const FStrProperty* BoneProp = FindFProperty<FStrProperty>(It->Struct, TargetBonePropertyName);
		const FString CompName = CompProp ? CompProp->GetPropertyValue_InContainer(Target) : FString();
		const FString Bone = BoneProp ? BoneProp->GetPropertyValue_InContainer(Target) : FString();

		// Black Eye resolves an empty component name to the actor's first skeletal mesh (see /ue-blackeye known issues).
		const USceneComponent* Comp = nullptr;
		TArray<USceneComponent*> Comps;
		Subject->GetComponents(Comps);
		for (const USceneComponent* C : Comps)
		{
			if (CompName.IsEmpty() ? C->IsA<USkeletalMeshComponent>() : C->GetName() == CompName)
			{
				Comp = C;
				break;
			}
		}
		OutWorld = Comp ? (Bone.IsEmpty() ? Comp->GetComponentLocation() : Comp->GetSocketLocation(FName(*Bone)))
		                : Subject->GetActorLocation();
		return true;
	}
	return false;
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

	// 5. The subject struct and its Actor field, read to find which skeletal meshes the bake must refresh.
	const UScriptStruct* TargetStruct = GetSimpleTargetStruct();
	bAll &= Check(5, TEXT("subject struct"),
	              TargetStruct && FindFProperty<FSoftObjectProperty>(TargetStruct, TargetActorPropertyName) != nullptr
	                           && FindFProperty<FStrProperty>(TargetStruct, TargetComponentPropertyName) != nullptr
	                           && FindFProperty<FStrProperty>(TargetStruct, TargetBonePropertyName) != nullptr,
	              FString::Printf(TEXT("%s.%s::Actor/ComponentName/BoneName"), BlackEyeScriptPackage, SimpleTargetStructName));

	UE_LOG(LogBlackEyeCustom, Display, TEXT("[BlackEyeCustom] selftest %s"), bAll ? TEXT("PASSED") : TEXT("FAILED"));
	return bAll;
}
