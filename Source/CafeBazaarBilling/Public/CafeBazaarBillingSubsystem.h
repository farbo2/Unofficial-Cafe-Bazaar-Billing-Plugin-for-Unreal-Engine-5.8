#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "CafeBazaarBillingTypes.h"
#include "CafeBazaarBillingSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FCafeBazaarOnConnectionSucceeded);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FCafeBazaarOnConnectionFailed, const FString&, ErrorMessage);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FCafeBazaarOnDisconnected);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FCafeBazaarOnPurchaseFinished,
	bool, bSuccess,
	const FString&, Message,
	const FCafeBazaarPurchase&, Purchase);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FCafeBazaarOnConsumeFinished,
	bool, bSuccess,
	const FString&, Message,
	const FString&, PurchaseToken);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FCafeBazaarOnQueryPurchasesFinished,
	bool, bSuccess,
	const FString&, Message,
	const TArray<FCafeBazaarPurchase>&, Purchases);

/**
 * Blueprint entry point for Cafe Bazaar (Poolakey) in-app billing.
 *
 * Usage from Blueprint:
 *   1. Get Game Instance Subsystem (Cafe Bazaar Billing Subsystem)
 *   2. Bind to OnConnectionSucceeded / OnConnectionFailed
 *   3. Call Open Connection with your RSA public key from the Cafe Bazaar developer panel
 *   4. Once connected: Launch Purchase Flow / Subscribe Flow, then Consume Purchase for consumables
 *
 * On non-Android platforms every call immediately fails through the corresponding
 * delegate with an explanatory message, so Blueprint graphs remain testable in the editor.
 */
UCLASS(BlueprintType)
class CAFEBAZAARBILLING_API UCafeBazaarBillingSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Must be called once, before any other call. RsaPublicKey comes from the Cafe Bazaar developer panel. */
	UFUNCTION(BlueprintCallable, Category = "CafeBazaar|Billing")
	void OpenConnection(const FString& RsaPublicKey);

	UFUNCTION(BlueprintCallable, Category = "CafeBazaar|Billing")
	void CloseConnection();

	/** Opens Bazaar's purchase UI for a one-time (in-app) product. */
	UFUNCTION(BlueprintCallable, Category = "CafeBazaar|Billing")
	void LaunchPurchaseFlow(const FString& ProductId, const FString& Payload);

	/** Opens Bazaar's purchase UI for a subscription product. */
	UFUNCTION(BlueprintCallable, Category = "CafeBazaar|Billing")
	void LaunchSubscribeFlow(const FString& ProductId, const FString& Payload);

	/** Consumable products only: call after delivering the item so it can be bought again. */
	UFUNCTION(BlueprintCallable, Category = "CafeBazaar|Billing")
	void ConsumePurchase(const FString& PurchaseToken);

	/** Non-subscription purchases currently owned by the user. */
	UFUNCTION(BlueprintCallable, Category = "CafeBazaar|Billing")
	void QueryPurchasedProducts();

	/** Active subscriptions owned by the user. */
	UFUNCTION(BlueprintCallable, Category = "CafeBazaar|Billing")
	void QuerySubscribedProducts();

	UFUNCTION(BlueprintPure, Category = "CafeBazaar|Billing")
	bool IsPlatformSupported() const;

	/** Current connection lifecycle state. Blueprint should disable buttons appropriately based on this. */
	UFUNCTION(BlueprintPure, Category = "CafeBazaar|Billing")
	ECafeBazaarConnectionState GetConnectionState() const { return ConnectionState; }

	UFUNCTION(BlueprintPure, Category = "CafeBazaar|Billing")
	bool IsConnected() const { return ConnectionState == ECafeBazaarConnectionState::Connected; }

	/** True while a purchase/subscribe flow is in progress. Disable purchase buttons while true. */
	UFUNCTION(BlueprintPure, Category = "CafeBazaar|Billing")
	bool IsPurchaseInFlight() const { return bIsPurchaseInFlight; }

	/** True while a purchased/subscribed-products query is in progress. */
	UFUNCTION(BlueprintPure, Category = "CafeBazaar|Billing")
	bool IsQueryInFlight() const { return bIsQueryInFlight; }

	/**
	 * Manual recovery: force-clears the purchase/query in-flight flags without
	 * touching the connection. Use this if a purchase button is stuck disabled
	 * (IsPurchaseInFlight staying true) with no OnPurchaseFinished ever arriving -
	 * this can happen if the Bazaar payment UI's result is lost after the OS
	 * reclaims the app process mid-flow.
	 *
	 * Safety note: this also advances the internal purchase generation counter,
	 * so if the native layer's callback for that abandoned attempt does
	 * eventually arrive, it will be recognized as stale and silently dropped
	 * instead of corrupting the state of whatever purchase you start next.
	 * It still does not cancel an in-progress native purchase - Bazaar's UI may
	 * still be showing and may still complete the transaction on the user's
	 * account; this only unblocks the Blueprint-side flag.
	 */
	UFUNCTION(BlueprintCallable, Category = "CafeBazaar|Billing")
	void ResetStuckFlags();

	UPROPERTY(BlueprintAssignable, Category = "CafeBazaar|Billing|Events")
	FCafeBazaarOnConnectionSucceeded OnConnectionSucceeded;

	UPROPERTY(BlueprintAssignable, Category = "CafeBazaar|Billing|Events")
	FCafeBazaarOnConnectionFailed OnConnectionFailed;

	UPROPERTY(BlueprintAssignable, Category = "CafeBazaar|Billing|Events")
	FCafeBazaarOnDisconnected OnDisconnected;

	UPROPERTY(BlueprintAssignable, Category = "CafeBazaar|Billing|Events")
	FCafeBazaarOnPurchaseFinished OnPurchaseFinished;

	UPROPERTY(BlueprintAssignable, Category = "CafeBazaar|Billing|Events")
	FCafeBazaarOnConsumeFinished OnConsumeFinished;

	UPROPERTY(BlueprintAssignable, Category = "CafeBazaar|Billing|Events")
	FCafeBazaarOnQueryPurchasesFinished OnQueryPurchasesFinished;

	static UCafeBazaarBillingSubsystem* Get();

	// --- Called on the game thread by the platform glue layer. Not for Blueprint use.
	// Each takes the "generation" it was issued for; if it no longer matches the
	// subsystem's current generation counter (a newer OpenConnection/Launch*/Close
	// happened since), the callback is stale and is dropped without touching state
	// or broadcasting - this is what stops a late-arriving result from a previous
	// connection/purchase attempt from corrupting whatever the player is doing now. ---
	void HandleConnectionSucceeded(int32 Generation);
	void HandleConnectionFailed(const FString& Message, int32 Generation);
	void HandleDisconnected(int32 Generation);
	void HandlePurchaseFinished(bool bSuccess, const FString& Message, const FString& PurchaseJson, int32 Generation);
	void HandleConsumeFinished(bool bSuccess, const FString& Message, const FString& PurchaseToken, int32 Generation);
	void HandleQueryPurchasesFinished(bool bSuccess, const FString& Message, const FString& PurchasesJson, int32 Generation);

private:
	static TWeakObjectPtr<UCafeBazaarBillingSubsystem> Instance;

	ECafeBazaarConnectionState ConnectionState = ECafeBazaarConnectionState::Disconnected;
	bool bIsPurchaseInFlight = false;
	bool bIsQueryInFlight = false;

	// Bumped on every OpenConnection/CloseConnection - invalidates in-flight
	// connect attempts and, transitively, anything gated on being connected.
	int32 ConnectionGeneration = 0;
	// Bumped on every LaunchPurchaseFlow/LaunchSubscribeFlow/ResetStuckFlags/
	// CloseConnection - invalidates a specific purchase attempt.
	int32 PurchaseGeneration = 0;
	// Bumped on every QueryPurchasedProducts/QuerySubscribedProducts/CloseConnection.
	int32 QueryGeneration = 0;

	static FCafeBazaarPurchase ParsePurchaseJson(const FString& Json);
	static TArray<FCafeBazaarPurchase> ParsePurchasesJson(const FString& Json);
};
