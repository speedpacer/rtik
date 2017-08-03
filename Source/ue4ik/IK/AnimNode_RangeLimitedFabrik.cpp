// Copyright(c) Henry Cooney 2017

#include "AnimNode_RangeLimitedFabrik.h"
#include "AnimationRuntime.h"
#include "Animation/AnimInstanceProxy.h"
#include "Components/SkeletalMeshComponent.h"
#include "IK/RangeLimitedFABRIK.h"

FAnimNode_RangeLimitedFabrik::FAnimNode_RangeLimitedFabrik()
	: EffectorTransform(FTransform::Identity)
	, EffectorTransformSpace(BCS_ComponentSpace)
	, EffectorRotationSource(BRS_KeepLocalSpaceRotation)
	, Precision(1.f)
	, MaxIterations(10)
	, bEnableDebugDraw(false)
{
}

FVector FAnimNode_RangeLimitedFabrik::GetCurrentLocation(FCSPose<FCompactPose>& MeshBases, const FCompactPoseBoneIndex& BoneIndex)
{
	return MeshBases.GetComponentSpaceTransform(BoneIndex).GetLocation();
}

void FAnimNode_RangeLimitedFabrik::EvaluateSkeletalControl_AnyThread(FComponentSpacePoseContext& Output, TArray<FBoneTransform>& OutBoneTransforms)
{
	const FBoneContainer& BoneContainer = Output.Pose.GetPose().GetBoneContainer();

	// Update EffectorLocation if it is based off a bone position
	FTransform CSEffectorTransform = EffectorTransform;
	FAnimationRuntime::ConvertBoneSpaceTransformToCS(Output.AnimInstanceProxy->GetComponentTransform(),
		Output.Pose, CSEffectorTransform, EffectorTransformBone.GetCompactPoseIndex(BoneContainer), 
		EffectorTransformSpace);
	
	FVector CSEffectorLocation = CSEffectorTransform.GetLocation();

#if WITH_EDITOR
	CachedEffectorCSTransform = CSEffectorTransform;
#endif	

	int32 NumChainLinks = IKChain->Chain.Num();
	if (NumChainLinks < 2)
	{
		return;
	}

	// Maximum length of skeleton segment at full extension
	float MaximumReach = 0;

	// Gather bone transforms and constraints
	TArray<FTransform> SourceCSTransforms;
	TArray<UIKBoneConstraint*> Constraints;
	SourceCSTransforms.Reserve(NumChainLinks);
	Constraints.Reserve(NumChainLinks);
	for (int32 i = 0; i < NumChainLinks; ++i)
	{
		SourceCSTransforms.Add(Output.Pose.GetComponentSpaceTransform(IKChain->Chain[i].BoneIndex));
		Constraints.Add(IKChain->Chain[i].Constraint);
	}

	TArray<FTransform> DestCSTransforms;

	ACharacter* Character = Cast<ACharacter>(Output.AnimInstanceProxy->GetSkelMeshComponent()->GetOwner());
	bool bBoneLocationUpdated = FRangeLimitedFABRIK::SolveRangeLimitedFABRIK(
		SourceCSTransforms,
		Constraints,
		CSEffectorTransform.GetLocation(),
		DestCSTransforms,
		Precision,
		MaxIterations,
		Character
	);

	// Special handling for tip bone's rotation.
	int32 TipBoneIndex = NumChainLinks - 1;
	switch (EffectorRotationSource)
	{
	case BRS_KeepLocalSpaceRotation:
		if (NumChainLinks > 1)
		{
			DestCSTransforms[TipBoneIndex] = Output.Pose.GetLocalSpaceTransform(IKChain->Chain[TipBoneIndex].BoneIndex) *
				DestCSTransforms[TipBoneIndex - 1];
		}
		break;
	case BRS_CopyFromTarget:
		DestCSTransforms[TipBoneIndex].SetRotation(CSEffectorTransform.GetRotation());
		break;
	case BRS_KeepComponentSpaceRotation:
		// Don't change the orientation at all
		break;
	default:
		break;
	}

	// Commit the changes, if there were any
	if (bBoneLocationUpdated)
	{
		OutBoneTransforms.Reserve(NumChainLinks);

		for (int32 i = 0; i < NumChainLinks; ++i)
		{
			OutBoneTransforms.Add(FBoneTransform(IKChain->Chain[i].BoneIndex, DestCSTransforms[i]));
		}
	}
}

void FAnimNode_RangeLimitedFabrik::EnforceROMConstraint(FCSPose<FCompactPose>& Pose, 
	FIKBone& ChildBone, int32 ChildIndex)
{
	/*
	if (ChildBone.ConstraintMode == EIKROMConstraintMode::IKROM_No_Constraint)
	{
		return;
	}
	FRangeLimitedFABRIKChainLink& ChildLink = Chain[ChildIndex];
	FVector ChildLoc = ChildLink.BoneCSTransform.GetLocation();

	FVector ChildDirection;
	FVector ParentDirection;

	// Step 1: determine the forward directions of the parent and child
	if (ChildBone.bUseParentBoneDirection)
	{
		// Find the parent location -- either by looking at the chain, or going to the skeletal parent (if root)
		FTransform ParentTransform;
		if (ChildIndex < 1)
		{
			if (ChildLink.BoneIndex < 1)
			{
				// This bone is the root of the entire skeleton! So just use its default transform.
				ParentTransform = Pose.GetComponentSpaceTransform(ChildLink.BoneIndex);
			}
			else
			{
				// At root of chain -- look at skeleton instead
				FCompactPoseBoneIndex ParentBoneIndex = Pose.GetPose().GetParentBoneIndex(ChildLink.BoneIndex);
				ParentTransform = Pose.GetComponentSpaceTransform(ParentBoneIndex);
			}
		} 
		else
		{
			// Use the parent link transform
			ParentTransform = Chain[ChildIndex - 1].BoneCSTransform;
		}

	}
	
*/

/*
	else if (ChildBone.ConstraintMode == EIKROMConstraintMode::IKROM_Pitch_And_Yaw)
	{
		
		
	}
*/
}

void FAnimNode_RangeLimitedFabrik::UpdateParentRotation(FTransform& ParentTransform, const FIKBone& ParentBone,
	FTransform& ChildTransform, const FIKBone& ChildBone, FCSPose<FCompactPose>& Pose) const
{
	
	// Calculate pre-translation vector between this bone and child
	FTransform OldParentTransform = Pose.GetComponentSpaceTransform(ParentBone.BoneIndex);
	FTransform OldChildTransform = Pose.GetComponentSpaceTransform(ChildBone.BoneIndex);
	FVector OldDir = (OldChildTransform.GetLocation() - OldParentTransform.GetLocation()).GetUnsafeNormal();

	// Get vector from the post-translation bone to it's child
	FVector NewDir = (ChildTransform.GetLocation() -
		ParentTransform.GetLocation()).GetUnsafeNormal();
	
	// Calculate axis of rotation from pre-translation vector to post-translation vector
	FVector RotationAxis = FVector::CrossProduct(OldDir, NewDir).GetSafeNormal();
	float RotationAngle = FMath::Acos(FVector::DotProduct(OldDir, NewDir));
	FQuat DeltaRotation = FQuat(RotationAxis, RotationAngle);
	// We're going to multiply it, in order to not have to re-normalize the final quaternion, it has to be a unit quaternion.
	checkSlow(DeltaRotation.IsNormalized());
	
	// Calculate absolute rotation and set it
	ParentTransform.SetRotation(DeltaRotation * OldParentTransform.GetRotation());
	ParentTransform.NormalizeRotation();
}


bool FAnimNode_RangeLimitedFabrik::IsValidToEvaluate(const USkeleton* Skeleton, const FBoneContainer& RequiredBones)
{

	if (IKChain == nullptr)
	{
#if ENABLE_IK_DEBUG_VERBOSE
		UE_LOG(LogIK, Warning, TEXT("AnimNode_RangeLimitedFabrik was not valid to evaluate -- an input wrapper object was null"));		
#endif ENABLE_IK_DEBUG_VERBOSE
		return false;
	}

	if (IKChain->Chain.Num() < 2)
	{
		return false;
	}

	// Allow evaluation if all parameters are initialized and TipBone is child of RootBone
	return
		(
			Precision > 0
			&& IKChain->IsValid(RequiredBones)
		);
}

void FAnimNode_RangeLimitedFabrik::InitializeBoneReferences(const FBoneContainer& RequiredBones)
{

	if (IKChain == nullptr)
	{
#if ENABLE_IK_DEBUG
		UE_LOG(LogIK, Warning, TEXT("Could not initialize FAnimNode_RangeLimitedFabrik -- An input wrapper object was null"));
#endif // ENABLE_IK_DEBUG
		return;
	}

	IKChain->InitIfInvalid(RequiredBones);
	size_t NumBones = IKChain->Chain.Num();

	if (NumBones < 2)
	{
		return;
	}
	
	EffectorTransformBone = IKChain->Chain[NumBones - 1].BoneRef;
	EffectorTransformBone.Initialize(RequiredBones);
}

void FAnimNode_RangeLimitedFabrik::GatherDebugData(FNodeDebugData& DebugData)
{
	FString DebugLine = DebugData.GetNodeName(this);

	DebugData.AddDebugItem(DebugLine);
	ComponentPose.GatherDebugData(DebugData);
}
