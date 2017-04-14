// Copyright 2017 Mipmap Games

#include "RealisticProjectilePhysic.h"
#include "RealisticProjectileComponent.h"
#include "RealisticProjectileComponent.h"
#include "EngineUtils.h"
#include "DrawDebugHelpers.h"




DEFINE_LOG_CATEGORY(ProjectilePhysics);

const float URealisticProjectileComponent::MIN_TICK_TIME = 0.0002f;

/** Utility function to cast a Netquantized fvec (the type found in fhitresult) to fvec */
static FVector ToFVector(FVector_NetQuantize InVec)
{
	FVector result;
	result.X = InVec.X;
	result.Y = InVec.Y;
	result.Z = InVec.Z;
	return result;
}



/** Utility function - Get the first hit out of two FHitResult arrays - entry and exit hits */
static FHitResult GetFirstHit(TArray<FHitResult> MoveEntryHits, TArray<FHitResult> MoveExitHits, FVector OrigLocation, bool &OutIsEntryHit);

void DoDrawDebugLine(FVector StartLocation, FVector NewLocation, float Velocity);

URealisticProjectileComponent::URealisticProjectileComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	static ConstructorHelpers::FObjectFinder<UDataTable> MatProps(*DataTablePath);
	MaterialPropertiesTable = MatProps.Object;

	bUpdateOnlyIfRendered = false;
	bInitialVelocityInLocalSpace = true;
	bForceSubStepping = false;

	Velocity = FVector(1.f, 0.f, 0.f);

	Bounciness = 0.6f;
	Friction = 0.2f;



	bWantsInitializeComponent = true;

	MaxSimulationTimeStep = 0.05f;
	//	MaxSimulationIterations = 8;


}

void URealisticProjectileComponent::PostLoad()
{
	Super::PostLoad();

	const int32 LinkerUE4Ver = GetLinkerUE4Version();

	if (LinkerUE4Ver < VER_UE4_REFACTOR_PROJECTILE_MOVEMENT)
	{
		// Old code used to treat Bounciness as Friction as well.
		Friction = FMath::Clamp(1.f - Bounciness, 0.f, 1.f);

		// Old projectiles probably don't want to use this behavior by default.
		bInitialVelocityInLocalSpace = false;
	}
}

void URealisticProjectileComponent::InitializeComponent()
{
	Super::InitializeComponent();

	if (Velocity.SizeSquared() > 0.f)
	{
		// InitialSpeed > 0 overrides initial velocity magnitude.
		if (InitialSpeed > 0.f)
		{
			Velocity = Velocity.GetSafeNormal() * InitialSpeed;
		}

		if (bInitialVelocityInLocalSpace)
		{
			SetVelocityInLocalSpace(Velocity);
		}

		if (bRotationFollowsVelocity)
		{
			if (UpdatedComponent)
			{
				UpdatedComponent->SetWorldRotation(Velocity.Rotation());
			}
		}

		UpdateComponentVelocity();

		if (UpdatedPrimitive && UpdatedPrimitive->IsSimulatingPhysics())
		{
			UpdatedPrimitive->SetPhysicsLinearVelocity(Velocity);
		}
	}
	// Set up trajectory initial conditions
	FVector Loc0 = FVector::ZeroVector;
	AActor* ActorOwner = UpdatedComponent->GetOwner();
	if (IsValid(ActorOwner))
	{
		Loc0 = ActorOwner->GetActorLocation();
	}
	const UWorld* MyWorld = GetWorld();
	CurrentPenetrationDeceleration /= PenetrationModifier;
	if (!MyWorld) InitialConditions = FTrajectoryInitialConditions(Velocity, Loc0, 0.f, FRotator::ZeroRotator);
	else SetInitialConditions(Velocity, Loc0, MyWorld->GetTimeSeconds(), FRotator::ZeroRotator, 1.f);


	MoveEntryHits.SetNum(32, false);
	MoveExitHits.SetNum(32, false);

	float pitch = FMath::FRandRange(AngularVelocityMin.Pitch, AngularVelocityMax.Pitch);
	float yaw = FMath::FRandRange(AngularVelocityMin.Yaw, AngularVelocityMax.Yaw);
	float roll = FMath::FRandRange(AngularVelocityMin.Roll, AngularVelocityMax.Roll);
	AngularVelocity = FRotator(pitch, yaw, roll);
}


/** This is where the magic happens - the main function of this class */
void URealisticProjectileComponent::TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction *ThisTickFunction)
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_ProjectileMovementComponent_TickComponent);

	// skip if don't want component updated when not rendered or updated component can't move
	if (HasStoppedSimulation())
	{
		return;
	}
	if (ShouldSkipUpdate(DeltaTime)) return;

	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!IsValid(UpdatedComponent))
	{
		return;
	}

	AActor* ActorOwner = UpdatedComponent->GetOwner();
	if (!ActorOwner || !CheckStillInWorld())
	{
		return;
	}

	if (UpdatedComponent->IsSimulatingPhysics())
	{
		return;
	}

	const UWorld* MyWorld = GetWorld();
	if (!MyWorld)
	{
		return;
	}

	float TickRemainingTime = DeltaTime;
	int32 Iterations = 0;

	// k should be +ve so use -gravity
	k = 0.5f * -GetGravityZ() / TerminalVelocity;
	Vinf = FVector(0.f, 0.f, -TerminalVelocity);

	const float GameTime = MyWorld->GetTimeSeconds();

	while (TickRemainingTime > 0.f && !ActorOwner->IsPendingKill() && !HasStoppedSimulation() && Iterations < 500)
	{
		Iterations++;
		// subdivide long ticks to more closely follow parabolic trajectory
		const float TimeTick = DeltaTime;// fminf(DeltaTime / 4.f, TickRemainingTime);
		TickRemainingTime -= TimeTick;

		FHitResult Hit(1.f);
		const FVector OldVelocity = Velocity;

		float LaunchDeltaTime = (GameTime + DeltaTime - TickRemainingTime) - InitialConditions.t0;

		if (LaunchDeltaTime < 0.f)
		{
			//UE_LOG(ProjectilePhysics, Error, TEXT("Calculated time since projectile launch is -ve. CalculatedTime=%f"),LaunchDeltaTime)
			//UE_LOG(ProjectilePhysics, Error, TEXT("GameTime=%f, TickDeltaTime=%f, TickRemainingTime=%f, t0=%f"),GameTime,DeltaTime,TickRemainingTime,InitialConditions.t0)
			return;
		}


		const FVector OrigLocation = UpdatedComponent->GetComponentLocation();
		const FRotator OrigRotation = UpdatedComponent->GetComponentRotation();

		if (ObjectsPenetrated.Num() > 0 && LaunchDeltaTime > InitialConditions.PenRestDeltaTime)
		{
			// Object is currently penetrating and may stop on this hit, 
			// provided it doesn't encounter another object first
			TickRemainingTime += LaunchDeltaTime - InitialConditions.PenRestDeltaTime;
			// Set dt to the rest-time so newlocation is the furthest it will go
			LaunchDeltaTime = InitialConditions.PenRestDeltaTime;

		}
		FVector NewLocation = ComputeNewLocation(InitialConditions, LaunchDeltaTime);
		const FRotator NewRelativeRotation = InitialConditions.InitialRotation + AngularVelocity * LaunchDeltaTime;
		FRotator FollowVelocityRot = OldVelocity.Rotation() + (NewRelativeRotation);
		const FRotator NewRotation = (bRotationFollowsVelocity && !OldVelocity.IsNearlyZero(0.01f)) ? FollowVelocityRot : ActorOwner->GetActorRotation();

		float SubTickTimeRemaining = 0.f;
		MoveEntryHits.Reset();
		MoveExitHits.Reset();

		DoSweep(OrigLocation, NewLocation, NewRotation, MoveEntryHits, MyWorld, true);

		// Check for overlaps IE. hit a player pawn
		//Array of components that start overlapping this tick
		TArray<UPrimitiveComponent*> BeginOverlapping;
		TArray<UPrimitiveComponent*> StillOverlapping;
		//Go through and find the components beginning to overlap
		for (int32 Index = MoveEntryHits.Num() - 1; Index >= 0; --Index)
		{
			FHitResult CurrentHit = MoveEntryHits[Index];
			UPrimitiveComponent* OtherComponent = CurrentHit.GetComponent();
			ECollisionResponse CR = UpdatedPrimitive->GetCollisionResponseToComponent(OtherComponent);
			if (CR < ECollisionResponse::ECR_Block) {
				int32 iFound;
				bool found = OverlappedComponents.Find(CurrentHit.GetComponent(), iFound);
				if (!found)
				{
					BeginOverlapping.Emplace(CurrentHit.GetComponent());
					OnComponentBeginOverlap.Broadcast(CurrentHit, InitialConditionHistory.Num() - 1, GameTime - InitialConditions.t0, LaunchDeltaTime);
				}
				else
				{
					StillOverlapping.Emplace(CurrentHit.GetComponent());
				}
				MoveEntryHits.RemoveAt(Index);
			}
		}
		OverlappedComponents.Reset();
		for (auto& elem : BeginOverlapping)
		{
			OverlappedComponents.Emplace(elem);
		}
		for (auto& elem : StillOverlapping)
		{
			OverlappedComponents.Emplace(elem);
		}

		//// Now remove all starting hits from MoveEntryHits
		for (int32 Index = MoveEntryHits.Num() - 1; Index >= 0; --Index)
		{
			if (MoveEntryHits[Index].bStartPenetrating) MoveEntryHits.RemoveAt(Index);
		}
		if (ObjectsPenetrated.Num() > 0) // We are inside another object at the start of the tick
		{
			DoSweep(NewLocation, OrigLocation, NewRotation, MoveExitHits, MyWorld);
			//Remove Any starting hits
			/*for (int32 hitIndex = MoveExitHits.Num() - 1; hitIndex >= 0; --hitIndex)
			{
			if (!MoveExitHits[hitIndex].bStartPenetrating) MoveExitHits.RemoveAt(hitIndex);
			}*/

			// Deal with hits
			if (MoveEntryHits.Num() > 0 || MoveExitHits.Num() > 0)
			{
				// We have to deal with a hit
				// But we only care about the first one encountered (closest one)
				bool IsEntryHit = true;
				FHitResult HitToDealWith = GetFirstHit(MoveEntryHits, MoveExitHits, OrigLocation, IsEntryHit);

				// Now we can deal with the closest hit
				if (IsEntryHit)
				{
					// The new location should be nudged into the object slightly or it will
					// collide again and again every hit. 1 Unreal unit is enough
					FVector ActualNewLocation = ToFVector(HitToDealWith.Location) -
						ToFVector(HitToDealWith.ImpactNormal);
					//UpdatedComponent->SetWorldLocationAndRotationNoPhysics(ActualNewLocation, NewRotation);
					ActorMove(ActualNewLocation, NewRotation);
					// Use hit location to calculate flight time rather than ActualNewLocation as 
					// technically the projectile may never get to actualnewlocation in some scenarios.
					float FlightTime = ComputeTimeOfFlight(ToFVector(HitToDealWith.Location));
					SubTickTimeRemaining = LaunchDeltaTime - FlightTime + .0001f;
					TickRemainingTime += SubTickTimeRemaining + .0001f;
					Velocity = ComputeVelocity(FlightTime);
					//ApplyImpulse(HitToDealWith, Velocity);

					OnProjectileAnyHit.Broadcast(HitToDealWith, Velocity);
					OnProjectilePenetrate.Broadcast(HitToDealWith, Velocity);


					// Add this hit to the list of objects currently penetrated
					ObjectsPenetrated.Emplace(HitToDealWith);
					CurrentPenetrationDeceleration = GetNewPenetrationDeceleration(HitToDealWith);

					SetInitialConditions(Velocity, ActualNewLocation, InitialConditions.t0 + FlightTime,
						NewRelativeRotation, CurrentPenetrationDeceleration);
					NewLocation = ActualNewLocation;
				}
				else //HitToDealWith is an exit hit
				{
					FVector ActualNewLocation = ToFVector(HitToDealWith.Location) + ToFVector(HitToDealWith.ImpactNormal);

					//UpdatedComponent->SetWorldLocationAndRotationNoPhysics(ActualNewLocation, NewRotation);
					ActorMove(ActualNewLocation, NewRotation);
					float FlightTime = ComputeTimeOfFlight(ToFVector(HitToDealWith.Location));
					SubTickTimeRemaining = LaunchDeltaTime - FlightTime;
					TickRemainingTime += SubTickTimeRemaining;
					Velocity = ComputeVelocity(FlightTime);

					OnProjectileAnyHit.Broadcast(HitToDealWith, Velocity);
					OnPenetrationExit.Broadcast(HitToDealWith, Velocity);

					//Find the matching entryhit
					for (int32 Index = ObjectsPenetrated.Num() - 1; Index >= 0; --Index)
					{
						if (HitToDealWith.GetActor() == ObjectsPenetrated[Index].GetActor())
						{
							ObjectsPenetrated.RemoveAt(Index);
						}
					}
					if (ObjectsPenetrated.Num() == 0)
					{
						Velocity = AdjustDirection(Velocity, ToFVector(Hit.ImpactNormal));
						CurrentPenetrationDeceleration = 1.f;
					}
					SetInitialConditions(Velocity, ActualNewLocation, InitialConditions.t0 + FlightTime,
						NewRelativeRotation, CurrentPenetrationDeceleration);
					NewLocation = ActualNewLocation;
				}
			}
			else // No hits
			{
				if (fabsf(LaunchDeltaTime - InitialConditions.PenRestDeltaTime) < 0.01f)
				{
					// The projectile stopped inside an object
					Hit = ObjectsPenetrated[0];
					TryEmbed(Hit, NewLocation);
					StopSimulating(Hit);
					TickRemainingTime = -1000.f;
				}
				else
				{
					//UpdatedComponent->SetWorldLocationAndRotationNoPhysics(NewLocation, NewRotation);
					ActorMove(NewLocation, NewRotation);
				}
			}
		}



		else // We are in air
		{
			if (MoveEntryHits.Num() > 0)
			{
				Hit = MoveEntryHits[0];

				// Decide whether to bounce or penetrate:
				bool DoBounce = DecideBouncePenetrate(Hit);
				if (DoBounce)
				{
					FVector ActualNewLocation = ToFVector(Hit.Location) + ToFVector(Hit.ImpactNormal);
					ActorMove(ActualNewLocation, NewRotation);
					float FlightTime = ComputeTimeOfFlight(ToFVector(Hit.Location));
					TickRemainingTime += LaunchDeltaTime - FlightTime;
					Velocity = ComputeVelocity(FlightTime);

					OnProjectileAnyHit.Broadcast(Hit, Velocity);
					OnProjectileBounce.Broadcast(Hit, Velocity);

					NewLocation = ActualNewLocation;

					FVector NormalVelocity = FVector::DotProduct(Velocity, ToFVector(Hit.ImpactNormal)) * ToFVector(Hit.ImpactNormal);
					FVector PlaneVel = Velocity - NormalVelocity;
					Velocity = PlaneVel - NormalVelocity * Bounciness;
					Velocity = AdjustDirection(Velocity, ToFVector(Hit.ImpactNormal));
					SetInitialConditions(Velocity, ActualNewLocation, InitialConditions.t0 + FlightTime,
						NewRelativeRotation, 1.f);

					if (Velocity.Size() < 500.f) StopSimulating(Hit);
				}
				else // Penetrate
				{
					// Nudge the projectile into the object so it is initially overlapping next tick
					// Failure to do this would mean a recursive loop of hitting the object
					FVector ActualNewLocation = ToFVector(Hit.Location) - ToFVector(Hit.ImpactNormal);
					//UpdatedComponent->SetWorldLocationAndRotationNoPhysics(ActualNewLocation, NewRotation);
					ActorMove(ActualNewLocation, NewRotation);
					// We hit something, so reset the initial conditions for trajectory calculation
					float FlightTime = ComputeTimeOfFlight(ToFVector(Hit.Location));
					TickRemainingTime += LaunchDeltaTime - FlightTime;
					Velocity = ComputeVelocity(FlightTime);
					//ApplyImpulse(Hit, Velocity);

					OnProjectileAnyHit.Broadcast(Hit, Velocity);
					OnProjectilePenetrate.Broadcast(Hit, Velocity);

					// Add this hit to the list of objects we are currently 'inside of'
					ObjectsPenetrated.Emplace(Hit);
					CurrentPenetrationDeceleration = GetNewPenetrationDeceleration(Hit);
					SetInitialConditions(Velocity, ActualNewLocation, InitialConditions.t0 + FlightTime,
						NewRelativeRotation, CurrentPenetrationDeceleration);
					NewLocation = ActualNewLocation;
				}
			}
			else //No hit
			{
				Velocity = ComputeVelocity(LaunchDeltaTime);
				//UpdatedComponent->SetWorldLocationAndRotationNoPhysics(NewLocation, NewRotation);
				ActorMove(NewLocation, NewRotation);
			}
		}
		UpdateComponentVelocity();
		DoDrawDebugLine(OrigLocation, NewLocation, OldVelocity.Size());
	}
}

void URealisticProjectileComponent::SetVelocityInLocalSpace(FVector NewVelocity)
{
	if (IsValid(UpdatedComponent))
	{
		Velocity = UpdatedComponent->GetComponentToWorld().TransformVectorNoScale(NewVelocity);
	}
}

FVector URealisticProjectileComponent::ComputeVelocity(float DeltaTime)
{
	if (ObjectsPenetrated.Num() > 0) //	We are inside an object - constant deceleration, ignore gravity.
	{
		float NewSpeed_SI = (CurrentPenetrationDeceleration * DeltaTime) + (InitialConditions.v0length / 100.f);
		if (NewSpeed_SI < 0.f) NewSpeed_SI = 0.f;
		float ratio = 100.f * NewSpeed_SI / InitialConditions.v0length;
		FVector NewVelocity = InitialConditions.InitialVelocity * ratio;
		return LimitVelocity(NewVelocity);
	}
	else // We are in air - drag and gravity are in effect
	{
		float denominator = (1 + k * DeltaTime);
		denominator *= denominator;
		FVector numerator = InitialConditions.InitialVelocity + k * DeltaTime * (2.f + k * DeltaTime) * Vinf;
		FVector NewVelocity = numerator / denominator;
		return NewVelocity;
	}


}

FVector URealisticProjectileComponent::LimitVelocity(FVector NewVelocity) const
{
	const float CurrentMaxSpeed = GetMaxSpeed();
	if (CurrentMaxSpeed > 0.f)
	{
		NewVelocity = NewVelocity.GetClampedToMaxSize(CurrentMaxSpeed);
	}

	return ConstrainDirectionToPlane(NewVelocity);
}

FVector URealisticProjectileComponent::ComputeNewLocation(FTrajectoryInitialConditions TIC, float DeltaTime) const
{
	//We are in air (or a fluid) - deceleration proportional to velocity
	if (TIC.PenDeceleration > 0.f)
	{
		float denominator = 1 + k * DeltaTime;
		FVector numerator = (TIC.InitialVelocity + k * DeltaTime * Vinf) * DeltaTime;
		FVector result1 = (numerator / denominator) + TIC.InitialWorldLocation;
		return result1;
	}
	else //We are in a solid object - constant deceleration and don't bother with gravity
	{
		// x(t) = 0.5at^2 + vt + x0
		FVector xt = (((0.5f * (TIC.PenDeceleration * DeltaTime)) * DeltaTime) * TIC.v0direction) +
			((TIC.InitialVelocity / 100.f) * DeltaTime);
		return (100.f * xt) + TIC.InitialWorldLocation;
	}
}

float URealisticProjectileComponent::GetGravityZ() const
{
	// TODO: apply buoyancy if in water
	//return ShouldApplyGravity() ? Super::GetGravityZ() * ProjectileGravityScale : 0.f;

	return CurrentGravity;
}

void URealisticProjectileComponent::StopSimulating(const FHitResult& HitResult)
{
	SetUpdatedComponent(NULL);
	OnProjectileStop.Broadcast(HitResult);
}

bool URealisticProjectileComponent::CheckStillInWorld()
{
	if (!UpdatedComponent)
	{
		return false;
	}

	const UWorld* MyWorld = GetWorld();
	if (!MyWorld)
	{
		return false;
	}

	// check the variations of KillZ
	AWorldSettings* WorldSettings = MyWorld->GetWorldSettings(true);
	if (!WorldSettings->bEnableWorldBoundsChecks)
	{
		return true;
	}
	AActor* ActorOwner = UpdatedComponent->GetOwner();
	if (!IsValid(ActorOwner))
	{
		return false;
	}
	if (ActorOwner->GetActorLocation().Z < WorldSettings->KillZ)
	{
		UDamageType const* DmgType = WorldSettings->KillZDamageType ? WorldSettings->KillZDamageType->GetDefaultObject<UDamageType>() : GetDefault<UDamageType>();
		ActorOwner->FellOutOfWorld(*DmgType);
		return false;
	}
	// Check if box has poked outside the world
	else if (UpdatedComponent && UpdatedComponent->IsRegistered())
	{
		const FBox&	Box = UpdatedComponent->Bounds.GetBox();
		if (Box.Min.X < -HALF_WORLD_MAX || Box.Max.X > HALF_WORLD_MAX ||
			Box.Min.Y < -HALF_WORLD_MAX || Box.Max.Y > HALF_WORLD_MAX ||
			Box.Min.Z < -HALF_WORLD_MAX || Box.Max.Z > HALF_WORLD_MAX)
		{
			//UE_LOG(LogProjectileMovement, Warning, TEXT("%s is outside the world bounds!"), *ActorOwner->GetName());
			ActorOwner->OutsideWorldBounds();
			// not safe to use physics or collision at this point
			ActorOwner->SetActorEnableCollision(false);
			FHitResult Hit(1.f);
			StopSimulating(Hit);
			return false;
		}
	}
	return true;
}

bool URealisticProjectileComponent::ShouldUseSubStepping() const
{
	return bForceSubStepping || (GetGravityZ() != 0.f);
}


float URealisticProjectileComponent::GetSimulationTimeStep(float RemainingTime, int32 Iterations) const
{
	// no less than MIN_TICK_TIME (to avoid potential divide-by-zero during simulation).
	return FMath::Max(MIN_TICK_TIME, RemainingTime);
}

/** Sweep the updated component geometry, returning all hitresults in the parameter
* Does not move the updated component */
bool URealisticProjectileComponent::DoSweep(FVector StartLocation,
	FVector EndLocation,
	FRotator Rot,
	TArray<struct FHitResult> &OutHits,
	const UWorld* World,
	bool KeepOverlaps)
{
	if (!IsValid(UpdatedPrimitive)) return false;

	FComponentQueryParams CQP = FComponentQueryParams();
	CQP.AddIgnoredComponent(UpdatedPrimitive);
	CQP.bFindInitialOverlaps = true;
	CQP.bReturnPhysicalMaterial = true;
	CQP.bTraceComplex = false;

	bool r = World->ComponentSweepMulti(
		OutHits,			//results
		UpdatedPrimitive,	//Component to sweep
		StartLocation,			//start location
		EndLocation,			//end location
		Rot,
		CQP					//Parameters
		);

	if (!KeepOverlaps)
	{
		for (int32 Index = OutHits.Num() - 1; Index >= 0; --Index)
		{
			UPrimitiveComponent* OtherComponent = OutHits[Index].GetComponent();
			ECollisionResponse CR = UpdatedPrimitive->GetCollisionResponseToComponent(OtherComponent);
			if (CR < ECollisionResponse::ECR_Block) OutHits.RemoveAt(Index);
		}
	}
	return OutHits.Num() > 0;
}

/** Used when a hit is encountered during a tick
* Needed to ensure the InitialConditions are deterministic on different machines with different tick rates
* See Carpentier paper for explanation of equation. The function will subtract the current wind offset
* from the given parameter. */
float URealisticProjectileComponent::ComputeTimeOfFlight(FVector HitLocation) const
{
	FVector deltaLoc = HitLocation - InitialConditions.InitialWorldLocation;
	if (ObjectsPenetrated.Num() == 0) // We are in air - use drag eq from Carpentier paper
	{
		FVector AbsDeltaLoc = deltaLoc * deltaLoc.GetSignVector();
		// Find on which axis change in location is largest, as projectile may be shot directly along an axis
		FVector AxisToUse = FVector::ZeroVector;
		if (AbsDeltaLoc.X > AbsDeltaLoc.Y && AbsDeltaLoc.X > AbsDeltaLoc.Z) AxisToUse = FVector(1.f, 0.f, 0.f);
		else if (AbsDeltaLoc.Y > AbsDeltaLoc.Z && AbsDeltaLoc.Y > AbsDeltaLoc.Z) AxisToUse = FVector(0.f, 1.f, 0.f);
		else AxisToUse = FVector(0.f, 0.f, 1.f);

		FVector pVec = deltaLoc * AxisToUse;
		float p = pVec.X + pVec.Y + pVec.Z; // Signed magnitude of the axis we are interested in - delta location
		FVector v0Vec = InitialConditions.InitialVelocity * AxisToUse;
		float v0 = v0Vec.X + v0Vec.Y + v0Vec.Z; // Signed magnitude of initial velocity

		if (AxisToUse.Z < 0.5f)
		{
			// Easy mode - we don't have to deal with acceleration due to gravity / multiple answers
			// t = p / (v0 - k.p)
			return p / (v0 - k * p);
		}
		else
		{
			//Hard mode - there are two possible answers
			// 0 = k.Vinf.t^2 + (v0 - p.k)t - p
			float a2 = 2.f * k * -TerminalVelocity; // 2 * 'a' term
			float b = v0 - p * k;
			float c = -p;
			//solve quadratic formula
			float t1 = (-b + sqrtf(b*b - 2.f * a2 * c)) / a2;
			float t2 = (-b - sqrtf(b*b - 2.f * a2 * c)) / a2;
			// Maybe one answer is in -ve time
			if (t1 < 0.f) return t2;
			if (t2 < 0.f) return t1;
			// So they are both in +ve time
			// assuming gravity is pointing down like normal
			if (GetGravityZ() < 0.f)
			{
				// A +ve velocity means projectile is going up in the air and the answer must be the earlier time
				if (p > 0.f) return (t1 < t2) ? t1 : t2;
				else return (t1 < t2) ? t2 : t1;
			}
			else
			{
				//Gravity is all topsy-turvy.
				if (p < 0.f) return (t1 < t2) ? t1 : t2;
				else return (t1 < t2) ? t2 : t1;
			}
		}
	}
	else // We are in an object, use constant deceleration
	{
		float p = deltaLoc.Size() / 100.f;
		float v0 = InitialConditions.InitialVelocity.Size() / 100.f;
		float accel = -CurrentPenetrationDeceleration;

		// solve 0 = 0.5at^2 + v0t - deltaLocation   using quadratic formula for +ve t
		// a = 0.5 * deceleration
		// b = v0
		// c = -delta location
		float numerator = -v0 + sqrtf(v0 * v0 - 2.f * accel * -p);
		return numerator / accel;
	}
}

/** If conditions are met, turn collision off and attach to hit component,
* otherwise start simulating regular physics*/
void URealisticProjectileComponent::TryEmbed(FHitResult LastHit, FVector RestLocation)
{
	UPrimitiveComponent* HitComponent = LastHit.GetComponent();

	FVector TotalPenetrationVector = ToFVector(LastHit.Location) - InitialConditions.PenRestLocation;
	float PenDistance = TotalPenetrationVector.Size();
	// Get the collision velocity
	// eq(1) v = at + v0
	// set v=0 for velocity at rest-time and rearrange
	// eq(2) t = -v0/a
	// eq(3) p = 0.5at^2 + v0.t
	// Substitute eq(2) into eq(3) for position at rest time
	// p = 0.5a(v0^2/a^2) + v0(-v0/a)
	// p = 0.5v0^2/a - v0^2/a
	// p = -0.5v0^2/a
	// 
	// Rearrange to find v0 (initial velocity) given a position
	// v0 = sqrt(-2.a.p) where a is a -ve number
	// Use SI units
	float SI_ImpactSpeed = sqrtf(-.02f * CurrentPenetrationDeceleration * PenDistance);
	float ImpactSpeed = 100.f * SI_ImpactSpeed;
	FVector ImpactVelocity = ImpactSpeed * InitialConditions.v0direction;
	// We only want perpendicular penetration depth so 'glancing' arrows do not embed

	UE_LOG(ProjectilePhysics, Warning, TEXT("PenDistance %f"), PenDistance)
	if (PenDistance > EmbedDepth)
	{
		// Embed projectile
		OnProjectileEmbed.Broadcast(LastHit, ImpactSpeed);
		if (IsValid(UpdatedPrimitive) && IsValid(UpdatedComponent))
		{
			UpdatedPrimitive->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			if (IsValid(HitComponent))
			{
				UpdatedComponent->AttachToComponent(HitComponent, FAttachmentTransformRules(EAttachmentRule::KeepRelative, false), LastHit.BoneName);
			}
		}
		ApplyImpulse(LastHit, Velocity);

	}
	else
	{
		// Bounce projectile
		Velocity = ImpactVelocity.MirrorByVector(ToFVector(LastHit.ImpactNormal)) * Bounciness;
		float one_minus_bounciness = fmaxf(1.f - Bounciness, 1.f);
		ApplyImpulse(LastHit, Velocity * one_minus_bounciness);
		FVector WorldLocation = ToFVector(LastHit.Location) + ToFVector(LastHit.ImpactNormal) * 3.f;
		FRotator Rot = UpdatedComponent->GetComponentRotation();
		bRotationFollowsVelocity = false;
		//UpdatedPrimitive->SetWorldLocationAndRotationNoPhysics(HitLocation,Rot);
		ActorMove(WorldLocation, Rot);
		UpdatedPrimitive->SetSimulatePhysics(true);
		UpdatedPrimitive->SetPhysicsLinearVelocity(Velocity, false);

		OnProjectileBounce.Broadcast(LastHit, Velocity);
	}
}

/** Apply impulse to other object after hit*/
void URealisticProjectileComponent::ApplyImpulse(FHitResult& Hit, FVector ImpactVelocity) const
{
	UPrimitiveComponent* HitComponent = Hit.GetComponent();
	if (IsValid(HitComponent) && HitComponent->GetOwnerRole() == ROLE_Authority)
	{
		if (IsValid(UpdatedPrimitive) && fabsf(ImpulseScale) > SMALL_NUMBER && HitComponent->IsSimulatingPhysics())
		{
			//Impart momentum
			float Mass = UpdatedPrimitive->CalculateMass();
			FVector Impulse = Mass * ImpactVelocity * ImpulseScale;
			HitComponent->AddImpulseAtLocation(Impulse, ToFVector(Hit.ImpactPoint));
		}
	}
}

/** Utility Function to move an actor */
void URealisticProjectileComponent::ActorMove(FVector NewLocation, FRotator NewRotation)
{
	if (IsValid(UpdatedComponent))
	{
		//FVector Delta = NewLocation - UpdatedComponent->GetComponentLocation();
		//UpdatedPrimitive->MoveComponent(Delta,NewRotation.Quaternion(), false,(FHitResult*)nullptr,MOVECOMP_NoFlags, ETeleportType::TeleportPhysics);
		AActor* OwnerActor = UpdatedComponent->GetOwner();
		if (IsValid(OwnerActor)) OwnerActor->SetActorLocationAndRotation(
			NewLocation,
			NewRotation,
			false,
			(FHitResult*)nullptr,
			ETeleportType::TeleportPhysics);
	}
}

/** When we are in an object - update the deceleration experienced due to that object's toughness */
float URealisticProjectileComponent::GetNewPenetrationDeceleration(FHitResult Hit)
{
	if (ObjectsPenetrated.Num() == 0) return 1.f;
	UPhysicalMaterial* PM = Hit.PhysMaterial.Get();
	FString hitmaterial = PM->GetName();
	/*if(GEngine){
	GEngine->AddOnScreenDebugMessage(-1,
	15.f,
	FColor::Green,
	hitmaterial);
	}*/

	if (IsValid(MaterialPropertiesTable))
	{
		static const FString ContextString(TEXT("GENERAL"));

		FMaterialProperties* LookupRow = MaterialPropertiesTable->FindRow<FMaterialProperties>(
			*hitmaterial,
			ContextString);

		if (LookupRow)
		{
			return LookupRow->Toughness * -1000.f / PenetrationModifier;
		}
	}
	return DEFAULT_TOUGHNESS * -1000.f / PenetrationModifier;
}

void URealisticProjectileComponent::DoDrawDebugLine(FVector StartLocation, FVector NewLocation, float inVelocity)
{
	if (bDrawDebugLine)
	{
		const UWorld* MyWorld = GetWorld();
		if (!MyWorld)
		{
			return;
		}
		FColor TraceColor = DebugLineAuxiliaryColor;
		if (bDebugLineColorFromVelocity)
		{
			//Calculate color from velocity
			float fraction = inVelocity / InitialSpeed;
			int32 i_col = (int32)(fraction * 255 * 4);
			uint8 r = 255, g = 0, b = 0;

			if (i_col >= 765 && i_col <= 1020)
			{
				g = 1020 - i_col;
			}
			else if (i_col >= 510 && i_col <= 765)
			{
				g = 255;
				r = 765 - i_col;
			}
			else if (i_col >= 255 && i_col <= 510)
			{
				g = i_col - 510;
				r = 0;
				b = 510 - i_col;
			}
			else if (i_col >= 0)
			{
				b = 255;
				r = 255 - i_col;
				g = r;
			}
			TraceColor = FColor(r, g, b);
		}

		DrawDebugLine(MyWorld,
			StartLocation,
			NewLocation,
			TraceColor,
			true,
			DebugLineDuration,
			(uint8)'\000',
			DebugLineThickness);
	}
}

/** Return true for bounce */
bool URealisticProjectileComponent::DecideBouncePenetrate(FHitResult Hit)
{
	if (ProjectileBehavior == EProjectileBehavior::PB_Bounce_Only) return true;
	else if (ProjectileBehavior == EProjectileBehavior::PB_Penetrate_Only) return false;
	else
	{
		// Find angle
		float dotp = FVector::DotProduct(Velocity.GetSafeNormal(), (-1.f * ToFVector(Hit.ImpactNormal)));
		float AngleR = acosf(dotp);

		// Get Velocity
		float VelSize = Velocity.Size();

		// Get Toughness
		float Toughness = 2000;
		if (IsValid(MaterialPropertiesTable))
		{
			UPhysicalMaterial* PhysMat = Hit.PhysMaterial.Get();
			if (PhysMat)
			{
				static const FString ContextString(TEXT("GENERAL"));
				FMaterialProperties* MP = MaterialPropertiesTable->FindRow<FMaterialProperties>(FName(*PhysMat->GetName()), ContextString);
				if (MP)
				{
					Toughness = MP->Toughness;
				}
			}
		}
		//UE_LOG(ProjectilePhysics, Warning, TEXT("Toughness = %f"), Toughness)

		float AngleMultiplier = FMath::GetMappedRangeValueClamped(FVector2D(0.3f, 1.27f), FVector2D(1.0f, 0.f), AngleR);
		float VelMultiplier = FMath::GetMappedRangeValueClamped(FVector2D(0.1f, 1.0f), FVector2D(0.0f, 1.f), VelSize / Toughness);

		float ChanceOfPenetration = AngleMultiplier * VelMultiplier;
		float Roll = RandStream.FRand();

		//UE_LOG(ProjectilePhysics, Warning, TEXT("Angle=%f, part1=%f, part2=%f, Chance=%f"), AngleR, AngleMultiplier, VelMultiplier, ChanceOfPenetration)

		return Roll >= ChanceOfPenetration;
	}
}

/** Set the initialconditions variable and add to history */
void URealisticProjectileComponent::SetInitialConditions(FVector v0, FVector Loc0, float GameTime, FRotator Rotation, float CurrentPenDeceleration)
{
	FTrajectoryInitialConditions TIC = FTrajectoryInitialConditions(v0, Loc0, GameTime, Rotation, CurrentPenDeceleration);
	InitialConditions = TIC;
	InitialConditionHistory.Emplace(TIC);
	return;
}

/** Utility function - Get the first hit out of two FHitResult arrays - entry and exit hits */
static FHitResult GetFirstHit(TArray<FHitResult> MoveEntryHits, TArray<FHitResult> MoveExitHits, FVector OrigLocation, bool &OutIsEntryHit)
{
	FHitResult HitToDealWith;
	bool IsEntryHit = true;
	if (MoveExitHits.Num() == 0)
	{
		HitToDealWith = MoveEntryHits[0];
		IsEntryHit = true;
	}
	else if (MoveEntryHits.Num() == 0)
	{
		HitToDealWith = MoveExitHits.Last();
		IsEntryHit = false;
	}
	else
	{
		MoveEntryHits.Sort([&](const FHitResult& A, const FHitResult& B) {
			return (ToFVector(A.Location) - OrigLocation).SizeSquared() < (ToFVector(B.Location) - OrigLocation).SizeSquared();
		});
		MoveExitHits.Sort([&](const FHitResult& A, const FHitResult& B) {
			return (ToFVector(A.Location) - OrigLocation).SizeSquared() < (ToFVector(B.Location) - OrigLocation).SizeSquared();
		});
		float entrysq = (ToFVector(MoveEntryHits[0].Location) - OrigLocation).SizeSquared();
		float exitsq = (ToFVector(MoveExitHits[0].Location) - OrigLocation).SizeSquared();
		OutIsEntryHit = entrysq < exitsq;
		return entrysq < exitsq ? MoveEntryHits[0] : MoveExitHits[0];
	}
	OutIsEntryHit = IsEntryHit;
	return HitToDealWith;
}

/** Return a new velocity vector in a somewhat different direction. Won't go into a plane defined by impactnormal */
FVector URealisticProjectileComponent::AdjustDirection(FVector inVelocity, FVector ImpactNormal)
{
	float max_anglechange = 2.f * FMath::GetMappedRangeValueClamped(FVector2D(1000.f, 99900.f), FVector2D(0.f, 10.f), Velocity.Size());
	float pitch = (RandStream.FRand() - 0.5f) * max_anglechange;
	float yaw = (RandStream.FRand() - 0.5f) * max_anglechange;
	float roll = (RandStream.FRand() - 0.5f) * max_anglechange;
	FRotator rot = FRotator(pitch, yaw, roll);
	FVector PossibleAnswer = rot.RotateVector(inVelocity);
	float dotp = FVector::DotProduct(PossibleAnswer, -ImpactNormal);
	if (dotp >= 0.f)
	{
		PossibleAnswer = PossibleAnswer.MirrorByVector(ImpactNormal);
	}
	return PossibleAnswer;
}

/** Expose Distance point to line to blueprint from FMath*/
float URealisticProjectileComponent::DistPointToLine(FVector l1, FVector l2, FVector p)
{
	return FMath::PointDistToLine(p, l2, l1);
}





