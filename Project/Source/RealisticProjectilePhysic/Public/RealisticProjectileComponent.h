// Copyright 2017 Mipmap Games
#pragma once
#define DEFAULT_TOUGHNESS 5000.0f
#include "Engine/DataTable.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "GameFramework/MovementComponent.h"
#include "RealisticProjectileComponent.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(ProjectilePhysics, Log, All);

/** A Data structure for extra information we want to ascribe to physical materials.
* At the moment just toughness. */
USTRUCT(BlueprintType)
struct FMaterialProperties : public FTableRowBase
{
	GENERATED_USTRUCT_BODY()

public:

	FMaterialProperties()
		: Toughness(1.f)
	{}

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = MaterialProperty)
		float Toughness;
};

UENUM(BlueprintType)
enum class EProjectileBehavior : uint8
{
	PB_Bounce_Only 			UMETA(DisplayName = "Bounce Only"),
	PB_Penetrate_Only	 	UMETA(DisplayName = "Penetrate Only"),
	PB_Bounce_And_Penetrate	UMETA(DisplayName = "Bounce and Penetrate")
};

/** Each tick the component calculates a new location based on the initial conditions,
* NOT based on the location or velocity last tick. Grouped into a struct for clarity */
USTRUCT()
struct FTrajectoryInitialConditions
{
	GENERATED_USTRUCT_BODY()

	/** Velocity of actor on spawn, or after bounce, penetration or exit*/
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Trajectory)
	FVector InitialVelocity;

	/** World location of the actor on spawn, or after bounce, penetration, or exit*/
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Trajectory)
	FVector InitialWorldLocation;

	/** Orientation */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Trajectory)
	FRotator InitialRotation;

	/** Game time the current trajectory began */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Trajectory)
	float t0;

	UPROPERTY()
	FVector v0direction;

	UPROPERTY()
	float v0length;

	/** Used when object is penetrating a solid object
	* to test if we should stop simulating. In air
	* the projectile will essentially never stop.
	* The time at which the object will come to a stop*/
	UPROPERTY()
	float PenRestDeltaTime;

	UPROPERTY()
	FVector PenRestLocation;

	/** If inside another object, should be -ve. Otherwise +ve */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Trajectory)
	float PenDeceleration;


	FTrajectoryInitialConditions()
	{}

	FTrajectoryInitialConditions(FVector v0, FVector Loc0, float GameTime, FRotator Rotation,
			float PenetrationDeceleration = 1.f)
	{
		InitialVelocity = v0;
		InitialWorldLocation = Loc0;
		InitialRotation = Rotation;
		t0 = GameTime;
		v0.ToDirectionAndLength(v0direction, v0length);
		PenDeceleration = PenetrationDeceleration;
		PenRestDeltaTime = (0.01f * v0length) / -PenetrationDeceleration;
		FVector xt = (((0.5f * (PenetrationDeceleration * PenRestDeltaTime)) * PenRestDeltaTime) * v0direction) +
			((InitialVelocity / 100.f) * PenRestDeltaTime);
		PenRestLocation = InitialWorldLocation + xt;
	}
};

/* This class moves an actor component (->RootComponent by default) and supports penetration,
* ricochet, drag and embedding. The projectile moves in a deterministic way,
* so on different machines, the path of this object will remain exactly the same.
* When used for network play the projectile should use blocking collision
* with static objects but overlap with dynamic objects (players, physobj). */
UCLASS(ClassGroup = Movement, meta = (BlueprintSpawnableComponent), ShowCategories = (Velocity))
class REALISTICPROJECTILEPHYSIC_API URealisticProjectileComponent : public UMovementComponent
{
	GENERATED_UCLASS_BODY()

	DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnProjectileAnyHitDelegate, const FHitResult&, HitDetails, const FVector&, ImpactVelocity);
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnProjectileBounceDelegate, const FHitResult&, ImpactResult, const FVector&, ImpactVelocity);
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnProjectilePenetrateDelegate, const FHitResult&, ImpactResult, const FVector&, ImpactVelocity);
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnPenetrationExitDelegate, const FHitResult&, ExitHit, const FVector&, ExitVelocity);
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnProjectileStopDelegate, const FHitResult&, ImpactResult);
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnProjectileEmbedDelegate, const FHitResult&, HitResult, float, ImpactVelocity);
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FOnComponentBeginOverlapDelegate, const FHitResult&, HitResult, int32, TrajectoryNum, float, StartTime, float, EndTime);

public:
	/** Table of material properties eg. Toughness in kJ/m^3
	* This is assigned by path in the initializer */
	const UDataTable* MaterialPropertiesTable;

	/** The path to the datatable asset which stores extra information about
	* physical materials. At the moment this is just Toughness which affects
	* the amount of penetration through the material. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Physical Materials")
	FString DataTablePath = TEXT("/Game/ProjectilePhysics/DataTables/TBL_MaterialPropertiesTable");

	/** The initial trajectory inputs - t0, v0, Location at t0 */
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "Location Calculation")
	FTrajectoryInitialConditions InitialConditions;

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "AntiCheat")
	TArray<FTrajectoryInitialConditions> InitialConditionHistory;

	/** Each time new initial conditions are set - increase by 1. */
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "Location Calculation")
	int32 TrajectoryNumber = 0;

	/** The deceleration of the object in a solid medium in m/s/s, related to CSA and material toughness.
	* Set every time an object is penetrated. Should be -ve.*/
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "Location Calculation")
		float CurrentPenetrationDeceleration = DEFAULT_TOUGHNESS * -1.f;

	/** Angular velocity in degrees/second */
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "Location Calculation")
	FRotator AngularVelocity;

	/** Every time a blocking hit is encountered, the HitResult is added to this array
	* when object is exited, the matching entry hit is found and removed from the array */
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "Location Calculation")
	TArray<struct FHitResult> ObjectsPenetrated;

	/** Ignore these actors - useful for projectiles with multiple collision primitives */
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "Location Calculation")
	TArray<AActor*> IgnoreActors;

	/** Terminal velocity of the projectile when falling. This essentially decides the drag characteristics
	* of the projectile */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile Behavior")
	float TerminalVelocity = 9000.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile Behavior")
	float CurrentGravity = -981.f;

	/** Distance in cm this object must penetrate before becoming lodged in an object. Useful for
	* making sure arrows don't stick in stone for example */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile Behavior")
	float EmbedDepth = -1.f;

	/**  Multiplies the amount of momentum applied to hit physics objects */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile Behavior")
	float ImpulseScale = 1.f;

	/** Initial speed of projectile. If greater than zero, this will override the initial Velocity value and instead treat Velocity as a direction. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile Behavior")
	float InitialSpeed;

	/** Limit on speed of projectile (0 means no limit). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile Behavior")
	float MaxSpeed;

	/** Effects the deceleration experienced when inside other objects. Larger value = penetrates further*/
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile Behavior", meta = (ClampMin = "0.00000001", UIMin = "0.00000001"))
	float PenetrationModifier = 1.f;

	/** If true, this projectile will have its rotation updated each frame to match the direction of its velocity. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile Behavior")
	uint32 bRotationFollowsVelocity : 1;

	/** Initial Angular velocity in degrees/second, a random value between min and max will be chosen */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Projectile Behavior")
	FRotator AngularVelocityMin;

	/** Initial Angular velocity in degrees/second, a random value between min and max will be chosen */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Projectile Behavior")
	FRotator AngularVelocityMax;

	/** Can be bounces_only, penetrates_only or penetrates_and_bounces */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile Behavior")
	EProjectileBehavior ProjectileBehavior;

	/**
	* If true, the initial Velocity is interpreted as being in local space upon startup.
	* @see SetVelocityInLocalSpace()
	*/
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projectile Behavior")
	uint32 bInitialVelocityInLocalSpace : 1;

	/**
	* If true, forces sub-stepping to break up movement into discrete smaller steps to improve accuracy of the trajectory.
	* Objects that move in a straight line typically do *not* need to set this, as movement always uses continuous collision detection (sweeps) so collision is not missed.
	* Sub-stepping is automatically enabled when under the effects of gravity or when homing towards a target.
	* @see MaxSimulationTimeStep, MaxSimulationIterations
	*/
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = ProjectileSimulation)
	uint32 bForceSubStepping : 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = ProjectileSimulation)
	uint32 bDrawDebugLine : 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = ProjectileSimulation)
	uint32 bDebugLineColorFromVelocity : 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = ProjectileSimulation)
	float DebugLineThickness = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = ProjectileSimulation)
	float DebugLineDuration = 10.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = ProjectileSimulation)
	FColor DebugLineAuxiliaryColor = FColor(255, 255, 0);

	/**
	* Percentage of velocity maintained after the bounce in the direction of the normal of impact (coefficient of restitution).
	* 1.0 = no velocity lost, 0.0 = no bounce. Ignored if bShouldBounce is false.
	*/
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = ProjectileBounce, meta = (ClampMin = "0", UIMin = "0"))
	float Bounciness = 0.6f;

	/**
	* Coefficient of friction, affecting the resistance to sliding along a surface.
	* Normal range is [0,1] : 0.0 = no friction, 1.0+ = very high friction.
	* Also affects the percentage of velocity maintained after the bounce in the direction tangent to the normal of impact.
	* Ignored if bShouldBounce is false.
	* @see bBounceAngleAffectsFriction
	*/
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = ProjectileBounce, meta = (ClampMin = "0", UIMin = "0"))
	float Friction;

	/** Called on any hit. Other events like bounce, penetrate, embed will be called after*/
	UPROPERTY(BlueprintAssignable)
	FOnProjectileAnyHitDelegate OnProjectileAnyHit;

	/** Called when projectile impacts something and bounces are enabled. */
	UPROPERTY(BlueprintAssignable)
	FOnProjectileBounceDelegate OnProjectileBounce;

	/** Called when projectile penetrates an object. */
	UPROPERTY(BlueprintAssignable)
	FOnProjectilePenetrateDelegate OnProjectilePenetrate;

	/** Called when projectile exits an object. */
	UPROPERTY(BlueprintAssignable)
	FOnPenetrationExitDelegate OnPenetrationExit;

	/** Called when projectile has come to a stop (velocity is below simulation threshold, bounces are disabled, or it is forcibly stopped). */
	UPROPERTY(BlueprintAssignable)
	FOnProjectileStopDelegate OnProjectileStop;

	/** Called when projectile becomes embedded in an object. Returns false if an arrow 'glances' off object*/
	UPROPERTY(BlueprintAssignable)
	FOnProjectileEmbedDelegate OnProjectileEmbed;

	/** Called when projectile overlaps something - generally a player pawn*/
	UPROPERTY(BlueprintAssignable)
	FOnComponentBeginOverlapDelegate OnComponentBeginOverlap;

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "AntiCheat")
	FRandomStream RandStream;

	/**
	* Given the game time since trajectory launch, compute a new location.
	*
	* @param  DeltaTime - Game time (affected by pause/dilate) since launch of projectile.
	* @return World Location at DeltaTime.
	*/
	UFUNCTION(BlueprintCallable, Category = "Game|Components|ProjectileMovement")
	virtual FVector ComputeNewLocation(FTrajectoryInitialConditions TIC, float DeltaTime) const;

	/**
	* Given the game time since launch, compute a new velocity.
	*
	* @param  DeltaTime - Game time (affected by pause/dilate) since launch of projectile.
	* @return Velocity at DeltaTime.
	*/
	virtual FVector ComputeVelocity(float DeltaTime);

	/** Used when a hit is encountered during a tick
	* Needed to ensure the InitialConditions are deterministic on different machines with different tick rates
	* See Carpentier paper for explanation of equation. The function will subtract the current wind offset
	* from the given parameter. */
	virtual float ComputeTimeOfFlight(FVector HitLocation) const;

	/** Sweep the updated component geometry, returning all hitresults in the parameter
	* Does not move the updated component */
	virtual bool DoSweep(FVector StartLocation,
		FVector EndLocation,
		FRotator Rot,
		TArray<struct FHitResult> &OutHits,
		const UWorld* World,
		bool KeepOverlaps = false);

	/** Utility Function to move an actor */
	virtual void ActorMove(FVector NewLocation, FRotator NewRotation);

	/** If conditions are met, turn collision off and attach to hit component,
	* otherwise start simulating regular physics*/
	virtual void TryEmbed(FHitResult LastHit, FVector RestLocation);

	/** When we are in an object - update the deceleration experienced due to that object's toughness */
	virtual float GetNewPenetrationDeceleration(FHitResult Hit);

	/** Clears the reference to UpdatedComponent, fires stop event (OnProjectileStop), and stops ticking (if bAutoUpdateTickRegistration is true). */
	UFUNCTION(BlueprintCallable, Category = "Game|Components|ProjectileMovement")
	virtual void StopSimulating(const FHitResult& HitResult);

	UFUNCTION(BlueprintCallable, Category = Math)
	static float DistPointToLine(FVector l1, FVector l2, FVector p);

	bool HasStoppedSimulation() { return UpdatedComponent == NULL; }

	/** Sets the velocity to the new value, rotated into Actor space. */
	UFUNCTION(BlueprintCallable, Category = "Game|Components|ProjectileMovement")
	virtual void SetVelocityInLocalSpace(FVector NewVelocity);

	//Begin UActorComponent Interface
	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction *ThisTickFunction) override;
	virtual void PostLoad() override;
	//End UActorComponent Interface

	//Begin UMovementComponent Interface
	virtual float GetMaxSpeed() const override { return MaxSpeed; }
	virtual void InitializeComponent() override;
	//End UMovementComponent Interface

	/** This will check to see if the projectile is still in the world.  It will check things like
	* the KillZ, outside world bounds, etc. and handle the situation. */
	virtual bool CheckStillInWorld();

	/** Compute remaining time step given remaining time and current iterations. */
	float GetSimulationTimeStep(float RemainingTime, int32 Iterations) const;

	/**
	* Determine whether or not to use substepping in the projectile motion update.
	* If true, GetSimulationTimeStep() will be used to time-slice the update. If false, all remaining time will be used during the tick.
	* @return Whether or not to use substepping in the projectile motion update.
	* @see GetSimulationTimeStep()
	*/
	virtual bool ShouldUseSubStepping() const;

	/**
	* Max time delta for each discrete simulation step.
	* Lowering this value can address issues with fast-moving objects or complex collision scenarios, at the cost of performance.
	*
	* WARNING: if (MaxSimulationTimeStep * MaxSimulationIterations) is too low for the min framerate, the last simulation step may exceed MaxSimulationTimeStep to complete the simulation.
	* @see MaxSimulationIterations, bForceSubStepping
	*/
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0166", ClampMax = "0.50", UIMin = "0.0166", UIMax = "0.50"), Category = ProjectileSimulation)
	float MaxSimulationTimeStep;

private:

	/** k = 0.5g / ||Vterminal|| as per Carpentier paper
	* where g is gravity but positive */
	float k;

	/** Vinf = Terminal Velocity + Wind Velocity in Carpentier paper.*/
	FVector Vinf = FVector(0.f, 0.f, -TerminalVelocity);

protected:

	/** Apply impulse after hit*/
	UFUNCTION(BlueprintCallable, Category = "Game|Components|ProjectileMovement")
	void ApplyImpulse(FHitResult& Hit, FVector ImpactVelocity) const;

	/** Don't allow velocity magnitude to exceed MaxSpeed, if MaxSpeed is non-zero. */
	UFUNCTION(BlueprintCallable, Category = "Game|Components|ProjectileMovement")
	FVector LimitVelocity(FVector NewVelocity) const;

	/** Compute gravity effect given current physics volume, projectile gravity scale, etc. */
	virtual float GetGravityZ() const override;

	void DoDrawDebugLine(FVector StartLocation, FVector NewLocation, float Velocity);

	/** Return true for bounce */
	bool DecideBouncePenetrate(FHitResult Hit);

	/** Set the initialconditions variable and add to history */
	void SetInitialConditions(FVector v0, FVector Loc0, float GameTime, FRotator Rotation, float CurrentPenDeceleration);

	/** Minimum delta time considered when ticking. Delta times below this are not considered. This is a very small non-zero positive value to avoid potential divide-by-zero in simulation code. */
	static const float MIN_TICK_TIME;



	FVector AdjustDirection(FVector Velocity, FVector ImpactNormal);

	TArray<UPrimitiveComponent*> OverlappedComponents;

	/** Variables which are used each tick - save overhead by allocating memory here */
	TArray<struct FHitResult> MoveEntryHits;
	TArray<struct FHitResult> MoveExitHits;
};

