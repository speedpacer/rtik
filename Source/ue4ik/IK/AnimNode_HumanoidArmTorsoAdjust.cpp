// Copyright(c) Henry Cooney 2017

#include "AnimNode_HumanoidArmTorsoAdjust.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimInstanceProxy.h"
#include "AnimationRuntime.h"
#include "IK/Constraints.h"
#include "IK/RangeLimitedFABRIK.h"
#include "Utility/AnimUtil.h"

#if WITH_EDITOR
#include "Utility/DebugDrawUtil.h"
#endif

DECLARE_CYCLE_STAT(TEXT("IK Humanoid Arm Torso Adjust"), STAT_HumanoidArmTorsoAdjust_Eval, STATGROUP_Anim);

void FAnimNode_HumanoidArmTorsoAdjust::Initialize(const FAnimationInitializeContext & Context)
{
	Super::Initialize(Context);
	BaseComponentPose.Initialize(Context);
}

void FAnimNode_HumanoidArmTorsoAdjust::CacheBones(const FAnimationCacheBonesContext & Context)
{
	Super::CacheBones(Context);
	BaseComponentPose.CacheBones(Context);
}

void FAnimNode_HumanoidArmTorsoAdjust::UpdateInternal(const FAnimationUpdateContext & Context)
{
	BaseComponentPose.Update(Context);
	DeltaTime = Context.GetDeltaTime();	
}

void FAnimNode_HumanoidArmTorsoAdjust::EvaluateSkeletalControl_AnyThread(FComponentSpacePoseContext & Output, TArray<FBoneTransform>& OutBoneTransforms)
{
	SCOPE_CYCLE_COUNTER(STAT_HumanoidArmTorsoAdjust_Eval);

#if ENABLE_ANIM_DEBUG
	check(Output.AnimInstanceProxy->GetSkelMeshComponent());
#endif
	check(OutBoneTransforms.Num() == 0);

	// Input pin pointers are checked in IsValid -- don't need to check here

	USkeletalMeshComponent* SkelComp   = Output.AnimInstanceProxy->GetSkelMeshComponent();
	FMatrix ToCS = SkelComp->GetComponentToWorld().ToMatrixNoScale().Inverse();
	const USkeletalMeshSocket* TorsoPivotSocket = SkelComp->GetSocketByName(TorsoPivotSocketName);

	if (TorsoPivotSocket == nullptr)
	{
#if ENABLE_IK_DEBUG_VERBOSE
		UE_LOG(LogIK, Warning, TEXT("Could not evaluate humanoid arm torso adjustment -- torso pivot socket named %s could not be found"),
			*TorsoPivotSocketName.ToString());
#endif
		return;
	}

	if (Arm->Chain.Num() < 1)
	{
		return;
	}

	// Get skeleton axes
	FVector ForwardAxis = FIKUtil::GetSkeletalMeshComponentAxis(*SkelComp, SkeletonForwardAxis);
	FVector UpAxis      = FIKUtil::GetSkeletalMeshComponentAxis(*SkelComp, SkeletonUpAxis);
	FVector LeftAxis   = FVector::CrossProduct(ForwardAxis, UpAxis);

	// Create two artificial 'bones'. The spine bone goes from the torso pivot to the neck,
	// the shoulder bone goes from the neck to  the shoulder ball joint.
	FTransform ShoulderCS = Output.Pose.GetComponentSpaceTransform(Arm->Chain[0].BoneIndex);

	// Artificial bones don't need rotaions as they won't be rendered. 
	FTransform PivotCS(ToCS.TransformPosition(TorsoPivotSocket->GetSocketLocation(SkelComp)));

	// Neck is directly above pivot, at shoulder height
	FTransform NeckCS(PivotCS.GetLocation() + (ShoulderCS.GetLocation() - PivotCS.GetLocation()).ProjectOnTo(UpAxis));

	// Set up augmented chain -- pivot and neck preceed the arm chain
	int32 NumBones = Arm->Chain.Num() + 2;
	TArray<FTransform> CSTransforms;
	CSTransforms.Reserve(NumBones);
	CSTransforms.Add(PivotCS);
	CSTransforms.Add(NeckCS);

	for (FIKBone& Bone : Arm->Chain.BonesRootToEffector)
	{
		CSTransforms.Add(Output.Pose.GetComponentSpaceTransform(Bone.BoneIndex));
	}
	
#if ENABLE_IK_DEBUG
	if (!LeftAxis.IsNormalized())
	{
		UE_LOG(LogIK, Warning, TEXT("Could not evaluate Humanoid Arm Torso Adjustment - Skeleton Forward Axis and Skeleton Up Axis were not orthogonal"));
		return;
	}
#endif 

	// Set up torso pitch constraint, allowing torso to bend forward / backward
	FPlanarRotation TorsoPitchConstraint;
	TorsoPitchConstraint.RotationAxis = -1 * LeftAxis;
	TorsoPitchConstraint.ForwardDirection = UpAxis;
	TorsoPitchConstraint.FailsafeDirection = UpAxis;

	// Bend degree inputs are measured from the waist bone. Convert them to be measured at the torso pivot.
	FVector WaistCS = Output.Pose.GetComponentSpaceTransform(WaistBone.BoneIndex).GetLocation();

	float ForwardBendLen = FMath::Tan(FMath::DegreesToRadians(MaxForwardBendDegrees)) * 
		(NeckCS.GetLocation() - WaistCS).Size();
	float ForwardBendDegreesFromPivot = FMath::RadiansToDegrees(FMath::Atan(ForwardBendLen / 
		(NeckCS.GetLocation() - PivotCS.GetLocation()).Size()));
	
	float BackwardBendLen = FMath::Tan(FMath::DegreesToRadians(MaxBackwardBendDegress)) * 
		(NeckCS.GetLocation() - WaistCS).Size();
	float BackwardBendDegreesFromPivot = FMath::RadiansToDegrees(FMath::Atan(BackwardBendLen / 
		(NeckCS.GetLocation() - PivotCS.GetLocation()).Size()));

	TorsoPitchConstraint.MinDegrees = -BackwardBendDegreesFromPivot;
	TorsoPitchConstraint.MaxDegrees = ForwardBendDegreesFromPivot;
	TorsoPitchConstraint.bEnabled = true;
	TorsoPitchConstraint.bEnableDebugDraw = false;

	// Set up torso twist constraint, allowing a twist around the direction of the pivot-neck vector
	FPlanarRotation TorsoTwistConstraint;
	TorsoTwistConstraint.ForwardDirection = -LeftAxis;	
	TorsoTwistConstraint.FailsafeDirection = -LeftAxis;
	TorsoTwistConstraint.MinDegrees = -MaxBackwardTwistDegrees;
	TorsoTwistConstraint.MaxDegrees = MaxForwardTwistDegrees;
	TorsoTwistConstraint.bEnabled = true;
	TorsoPitchConstraint.bEnableDebugDraw = true;

	// Setup lambda wil run before constraint eval. It sets the rotation axis to the direction of the previous bone 
	// (the pivot-to-neck bone), and determines 'up' direction by the cross product.	
	TorsoTwistConstraint.SetupFn = [&TorsoTwistConstraint](
		int32 Index,
		const TArray<FTransform>& ReferenceCSTransforms,
		const TArray<FIKBoneConstraint*>& Constraints,
		TArray<FTransform>& CSTransforms
		)
	{
		FVector PivotLoc = CSTransforms[Index - 1].GetLocation();
		FVector NeckLoc = CSTransforms[Index].GetLocation();		
		TorsoTwistConstraint.RotationAxis = -1 * (NeckLoc - PivotLoc).GetUnsafeNormal();
	};

	// Add constraints
	TArray<FIKBoneConstraint*> Constraints;
	Constraints.Reserve(NumBones);
	Constraints.Add(&TorsoPitchConstraint);
	Constraints.Add(&TorsoTwistConstraint);
	for (FIKBone& Bone : Arm->Chain.BonesRootToEffector)
	{
		Constraints.Add(Bone.GetConstraint());
	}

	// Run FABRIK and pray
	FVector EffectorTargetCS = ToCS.TransformPosition(EffectorWorldTarget);
	TArray<FTransform> DestCSTransforms;
	FRangeLimitedFABRIK::SolveRangeLimitedFABRIK(
		CSTransforms,
		Constraints,
		EffectorTargetCS,
		DestCSTransforms,
		Precision,
		MaxIterations,
		Cast<ACharacter>(SkelComp->GetOwner())
	);
	
#if WITH_EDITOR
	if (bEnableDebugDraw)
	{
		UWorld* World = SkelComp->GetWorld();		
		FMatrix ToWorld = SkelComp->ComponentToWorld.ToMatrixNoScale();

		// Draw chain before adjustment, in yellow
		for (int32 i = 0; i < NumBones - 1; ++i)
		{
			FVector ParentLoc = ToWorld.TransformPosition(CSTransforms[i].GetLocation());
			FVector ChildLoc = ToWorld.TransformPosition(CSTransforms[i+1].GetLocation());
			FDebugDrawUtil::DrawLine(World, ParentLoc, ChildLoc, FColor(255, 255, 0));
			FDebugDrawUtil::DrawSphere(World, ChildLoc, FColor(255, 255, 0), 3.0f);
		}		

		// Draw chain after adjustment, in cyan
		for (int32 i = 0; i < NumBones - 1; ++i)
		{
			FVector ParentLoc = ToWorld.TransformPosition(DestCSTransforms[i].GetLocation());
			FVector ChildLoc = ToWorld.TransformPosition(DestCSTransforms[i+1].GetLocation());
			FDebugDrawUtil::DrawLine(World, ParentLoc, ChildLoc, FColor(0, 255, 255));
			FDebugDrawUtil::DrawSphere(World, ChildLoc, FColor(0, 255, 255), 3.0f);
		}

		FVector Base = ToWorld.GetOrigin();
		FDebugDrawUtil::DrawVector(World, Base, ForwardAxis, FColor(255, 0, 0));
		FDebugDrawUtil::DrawVector(World, Base, LeftAxis, FColor(0, 255, 0));
		FDebugDrawUtil::DrawVector(World, Base, UpAxis, FColor(0, 0, 255));

	}
#endif // WITH_EDITOR
}

bool FAnimNode_HumanoidArmTorsoAdjust::IsValidToEvaluate(const USkeleton * Skeleton, const FBoneContainer & RequiredBones)
{
	
	if (Arm == nullptr)
	{
#if ENABLE_IK_DEBUG_VERBOSE
		UE_LOG(LogIK, Warning, TEXT("Humaonid Arm Torso Adjust was not valid to evaluate - an input wrapper was null"));		
#endif ENABLE_IK_DEBUG_VERBOSE
		return false;		
	}

	if (!Arm->IsValid(RequiredBones))
	{
#if ENABLE_IK_DEBUG_VERBOSE
		UE_LOG(LogIK, Warning, TEXT("Humaonid Arm Torso Adjust was not valid to evaluate - arm chain was not valid"));		
#endif ENABLE_IK_DEBUG_VERBOSE
		return false;				
	}

	if (!WaistBone.IsValid(RequiredBones))
	{
#if ENABLE_IK_DEBUG_VERBOSE
		UE_LOG(LogIK, Warning, TEXT("Humaonid Arm Torso Adjust was not valid to evaluate - Waist bone was not valid"));		
#endif ENABLE_IK_DEBUG_VERBOSE
		return false;				
	}

	return true;
}

void FAnimNode_HumanoidArmTorsoAdjust::InitializeBoneReferences(const FBoneContainer& RequiredBones)
{
	if (Arm == nullptr)
	{
#if ENABLE_IK_DEBUG
		UE_LOG(LogIK, Warning, TEXT("Coud not initialize humanoid arm torso adjust - An input wrapper object was null"));
#endif // ENABLE_IK_DEBUG
		return;
	}
	
	if (!Arm->InitBoneReferences(RequiredBones))
	{
#if ENABLE_IK_DEBUG
		UE_LOG(LogIK, Warning, TEXT("Could not initialize arm chain in humanoid arm torso adjust"));
#endif // ENABLE_IK_DEBUG
		return;
	}

	if (!WaistBone.Init(RequiredBones))
	{
#if ENABLE_IK_DEBUG
		UE_LOG(LogIK, Warning, TEXT("Could not initialize waist bone in humanoid arm torso adjust"));
#endif // ENABLE_IK_DEBUG
		return;
	}

}