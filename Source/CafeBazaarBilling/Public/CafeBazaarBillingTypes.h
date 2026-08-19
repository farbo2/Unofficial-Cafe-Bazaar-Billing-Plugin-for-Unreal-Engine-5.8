#pragma once

#include "CoreMinimal.h"
#include "CafeBazaarBillingTypes.generated.h"

UENUM(BlueprintType)
enum class ECafeBazaarPurchaseState : uint8
{
	Purchased UMETA(DisplayName = "Purchased"),
	Refunded  UMETA(DisplayName = "Refunded"),
	Unknown   UMETA(DisplayName = "Unknown")
};

/** Connection lifecycle state, exposed to Blueprint so UI can disable buttons appropriately. */
UENUM(BlueprintType)
enum class ECafeBazaarConnectionState : uint8
{
	Disconnected UMETA(DisplayName = "Disconnected"),
	Connecting   UMETA(DisplayName = "Connecting"),
	Connected    UMETA(DisplayName = "Connected")
};

/** Mirrors Poolakey's PurchaseInfo entity. */
USTRUCT(BlueprintType)
struct CAFEBAZAARBILLING_API FCafeBazaarPurchase
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "CafeBazaar")
	FString ProductId;

	UPROPERTY(BlueprintReadOnly, Category = "CafeBazaar")
	FString OrderId;

	/** Pass this back into ConsumePurchase. */
	UPROPERTY(BlueprintReadOnly, Category = "CafeBazaar")
	FString PurchaseToken;

	UPROPERTY(BlueprintReadOnly, Category = "CafeBazaar")
	FString Payload;

	UPROPERTY(BlueprintReadOnly, Category = "CafeBazaar")
	FString PackageName;

	UPROPERTY(BlueprintReadOnly, Category = "CafeBazaar")
	ECafeBazaarPurchaseState PurchaseState = ECafeBazaarPurchaseState::Unknown;

	UPROPERTY(BlueprintReadOnly, Category = "CafeBazaar")
	int64 PurchaseTime = 0;

	/** Raw JSON + signature - send both to your backend if you verify server-side. */
	UPROPERTY(BlueprintReadOnly, Category = "CafeBazaar")
	FString OriginalJson;

	UPROPERTY(BlueprintReadOnly, Category = "CafeBazaar")
	FString DataSignature;
};
