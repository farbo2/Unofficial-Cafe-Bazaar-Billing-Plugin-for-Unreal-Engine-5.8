#include "CafeBazaarBillingSubsystem.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if PLATFORM_ANDROID
#include "CafeBazaarBillingJNI.h"
#endif

TWeakObjectPtr<UCafeBazaarBillingSubsystem> UCafeBazaarBillingSubsystem::Instance;

UCafeBazaarBillingSubsystem* UCafeBazaarBillingSubsystem::Get()
{
	return Instance.Get();
}

void UCafeBazaarBillingSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Instance = this;
	ConnectionState = ECafeBazaarConnectionState::Disconnected;
	bIsPurchaseInFlight = false;
	bIsQueryInFlight = false;
	ConnectionGeneration = 0;
	PurchaseGeneration = 0;
	QueryGeneration = 0;

#if PLATFORM_ANDROID
	CafeBazaarJNI::RegisterNatives();
#endif
}

void UCafeBazaarBillingSubsystem::Deinitialize()
{
	// Make sure we don't leak a live native Payment/Connection object if the
	// GameInstance is torn down (PIE stop, returning to main menu, etc.)
	// while still connected.
	if (ConnectionState != ECafeBazaarConnectionState::Disconnected)
	{
		++ConnectionGeneration;
		++PurchaseGeneration;
		++QueryGeneration;
#if PLATFORM_ANDROID
		CafeBazaarJNI::CloseConnection();
#endif
	}

	if (Instance.Get() == this)
	{
		Instance = nullptr;
	}
	Super::Deinitialize();
}

bool UCafeBazaarBillingSubsystem::IsPlatformSupported() const
{
#if PLATFORM_ANDROID
	return true;
#else
	return false;
#endif
}

static const FString GNotAndroidMessage = TEXT("Cafe Bazaar billing is only available on Android.");

void UCafeBazaarBillingSubsystem::OpenConnection(const FString& RsaPublicKey)
{
	if (RsaPublicKey.IsEmpty())
	{
		AsyncTask(ENamedThreads::GameThread, [this]() { OnConnectionFailed.Broadcast(TEXT("RSA public key is empty.")); });
		return;
	}

	// Deliberately NOT short-circuiting here based on ConnectionState, even
	// though it might look like "already Connected/Connecting -> no-op" would
	// be a safe optimization. It isn't: ConnectionState is only updated
	// asynchronously (via HandleConnectionSucceeded/Failed/Disconnected,
	// dispatched through AsyncTask from a JNI callback), while Java's real
	// Activity/Payment/Connection state changes synchronously with Android's
	// Activity lifecycle. If GameActivity is destroyed and recreated, Java's
	// onDestroy() -> setActivity() sequence happens immediately and
	// synchronously, but the corresponding HandleDisconnected() on the C++
	// side is only queued, not yet processed. A Blueprint call to
	// OpenConnection landing in that window would see a stale
	// ConnectionState == Connected and could short-circuit to a false
	// OnConnectionSucceeded without ever contacting Java - meaning C++ thinks
	// it's connected while Java has nothing behind it. So every call here is
	// always relayed to Java, which tears down whatever it has and starts
	// fresh (see CafeBazaarBillingBridge.openConnection) and is the only
	// thing allowed to declare success or failure for a given generation.
	ConnectionState = ECafeBazaarConnectionState::Connecting;
	const int32 ThisGeneration = ++ConnectionGeneration;

#if PLATFORM_ANDROID
	if (!CafeBazaarJNI::OpenConnection(RsaPublicKey, ThisGeneration))
	{
		if (ThisGeneration == ConnectionGeneration)
		{
			ConnectionState = ECafeBazaarConnectionState::Disconnected;
		}
		AsyncTask(ENamedThreads::GameThread, [this]() { OnConnectionFailed.Broadcast(TEXT("Failed to dispatch to the Java layer (JNI unavailable).")); });
	}
#else
	ConnectionState = ECafeBazaarConnectionState::Disconnected;
	AsyncTask(ENamedThreads::GameThread, [this]() { OnConnectionFailed.Broadcast(GNotAndroidMessage); });
#endif
}

void UCafeBazaarBillingSubsystem::CloseConnection()
{
	ConnectionState = ECafeBazaarConnectionState::Disconnected;
	bIsPurchaseInFlight = false;
	bIsQueryInFlight = false;
	// Advancing every counter here is what makes point (7)/(8) from the review
	// safe: any connection/purchase/query callback still in flight from before
	// this CloseConnection will carry an old generation and get dropped by the
	// Handle* methods below instead of being applied to a connection that, from
	// the Subsystem's point of view, no longer exists.
	++ConnectionGeneration;
	++PurchaseGeneration;
	++QueryGeneration;

#if PLATFORM_ANDROID
	CafeBazaarJNI::CloseConnection();
#endif
}

void UCafeBazaarBillingSubsystem::ResetStuckFlags()
{
	// Only advance the generation for whichever operation was actually stuck -
	// bumping both unconditionally would also silently drop the real result of
	// a legitimate, still-in-flight operation of the *other* kind (e.g. calling
	// this to recover a stuck purchase while a genuine query is still running).
	if (bIsPurchaseInFlight)
	{
		bIsPurchaseInFlight = false;
		++PurchaseGeneration;
	}
	if (bIsQueryInFlight)
	{
		bIsQueryInFlight = false;
		++QueryGeneration;
	}
}

void UCafeBazaarBillingSubsystem::LaunchPurchaseFlow(const FString& ProductId, const FString& Payload)
{
	if (ProductId.IsEmpty())
	{
		AsyncTask(ENamedThreads::GameThread, [this]() { OnPurchaseFinished.Broadcast(false, TEXT("Product ID is empty."), FCafeBazaarPurchase()); });
		return;
	}
	if (ConnectionState != ECafeBazaarConnectionState::Connected)
	{
		AsyncTask(ENamedThreads::GameThread, [this]() { OnPurchaseFinished.Broadcast(false, TEXT("Not connected. Call Open Connection first and wait for On Connection Succeeded."), FCafeBazaarPurchase()); });
		return;
	}
	if (bIsPurchaseInFlight)
	{
		AsyncTask(ENamedThreads::GameThread, [this]() { OnPurchaseFinished.Broadcast(false, TEXT("A purchase is already in progress."), FCafeBazaarPurchase()); });
		return;
	}

	bIsPurchaseInFlight = true;
	const int32 ThisGeneration = ++PurchaseGeneration;

#if PLATFORM_ANDROID
	if (!CafeBazaarJNI::LaunchPurchaseFlow(ProductId, Payload, ThisGeneration))
	{
		if (ThisGeneration == PurchaseGeneration) bIsPurchaseInFlight = false;
		AsyncTask(ENamedThreads::GameThread, [this]() { OnPurchaseFinished.Broadcast(false, TEXT("Failed to dispatch to the Java layer (JNI unavailable)."), FCafeBazaarPurchase()); });
	}
#else
	bIsPurchaseInFlight = false;
	AsyncTask(ENamedThreads::GameThread, [this]() { OnPurchaseFinished.Broadcast(false, GNotAndroidMessage, FCafeBazaarPurchase()); });
#endif
}

void UCafeBazaarBillingSubsystem::LaunchSubscribeFlow(const FString& ProductId, const FString& Payload)
{
	if (ProductId.IsEmpty())
	{
		AsyncTask(ENamedThreads::GameThread, [this]() { OnPurchaseFinished.Broadcast(false, TEXT("Product ID is empty."), FCafeBazaarPurchase()); });
		return;
	}
	if (ConnectionState != ECafeBazaarConnectionState::Connected)
	{
		AsyncTask(ENamedThreads::GameThread, [this]() { OnPurchaseFinished.Broadcast(false, TEXT("Not connected. Call Open Connection first and wait for On Connection Succeeded."), FCafeBazaarPurchase()); });
		return;
	}
	if (bIsPurchaseInFlight)
	{
		AsyncTask(ENamedThreads::GameThread, [this]() { OnPurchaseFinished.Broadcast(false, TEXT("A purchase is already in progress."), FCafeBazaarPurchase()); });
		return;
	}

	bIsPurchaseInFlight = true;
	const int32 ThisGeneration = ++PurchaseGeneration;

#if PLATFORM_ANDROID
	if (!CafeBazaarJNI::LaunchSubscribeFlow(ProductId, Payload, ThisGeneration))
	{
		if (ThisGeneration == PurchaseGeneration) bIsPurchaseInFlight = false;
		AsyncTask(ENamedThreads::GameThread, [this]() { OnPurchaseFinished.Broadcast(false, TEXT("Failed to dispatch to the Java layer (JNI unavailable)."), FCafeBazaarPurchase()); });
	}
#else
	bIsPurchaseInFlight = false;
	AsyncTask(ENamedThreads::GameThread, [this]() { OnPurchaseFinished.Broadcast(false, GNotAndroidMessage, FCafeBazaarPurchase()); });
#endif
}

void UCafeBazaarBillingSubsystem::ConsumePurchase(const FString& PurchaseToken)
{
	if (PurchaseToken.IsEmpty())
	{
		AsyncTask(ENamedThreads::GameThread, [this]() { OnConsumeFinished.Broadcast(false, TEXT("Purchase token is empty."), FString()); });
		return;
	}
	if (ConnectionState != ECafeBazaarConnectionState::Connected)
	{
		AsyncTask(ENamedThreads::GameThread, [this, PurchaseToken]() { OnConsumeFinished.Broadcast(false, TEXT("Not connected. Call Open Connection first."), PurchaseToken); });
		return;
	}

	// Tag Consume with the current connection generation (not a dedicated
	// counter of its own) - see review point 4/7: a consume result belonging
	// to a connection that has since been closed and replaced should not be
	// reported as if it happened on the current connection.
	const int32 ThisGeneration = ConnectionGeneration;

#if PLATFORM_ANDROID
	if (!CafeBazaarJNI::ConsumePurchase(PurchaseToken, ThisGeneration))
	{
		AsyncTask(ENamedThreads::GameThread, [this, PurchaseToken]() { OnConsumeFinished.Broadcast(false, TEXT("Failed to dispatch to the Java layer (JNI unavailable)."), PurchaseToken); });
	}
#else
	AsyncTask(ENamedThreads::GameThread, [this, PurchaseToken]() { OnConsumeFinished.Broadcast(false, GNotAndroidMessage, PurchaseToken); });
#endif
}

void UCafeBazaarBillingSubsystem::QueryPurchasedProducts()
{
	if (ConnectionState != ECafeBazaarConnectionState::Connected)
	{
		AsyncTask(ENamedThreads::GameThread, [this]() { OnQueryPurchasesFinished.Broadcast(false, TEXT("Not connected. Call Open Connection first."), TArray<FCafeBazaarPurchase>()); });
		return;
	}
	if (bIsQueryInFlight)
	{
		AsyncTask(ENamedThreads::GameThread, [this]() { OnQueryPurchasesFinished.Broadcast(false, TEXT("A query is already in progress."), TArray<FCafeBazaarPurchase>()); });
		return;
	}

	bIsQueryInFlight = true;
	const int32 ThisGeneration = ++QueryGeneration;

#if PLATFORM_ANDROID
	if (!CafeBazaarJNI::QueryPurchasedProducts(ThisGeneration))
	{
		if (ThisGeneration == QueryGeneration) bIsQueryInFlight = false;
		AsyncTask(ENamedThreads::GameThread, [this]() { OnQueryPurchasesFinished.Broadcast(false, TEXT("Failed to dispatch to the Java layer (JNI unavailable)."), TArray<FCafeBazaarPurchase>()); });
	}
#else
	bIsQueryInFlight = false;
	AsyncTask(ENamedThreads::GameThread, [this]() { OnQueryPurchasesFinished.Broadcast(false, GNotAndroidMessage, TArray<FCafeBazaarPurchase>()); });
#endif
}

void UCafeBazaarBillingSubsystem::QuerySubscribedProducts()
{
	if (ConnectionState != ECafeBazaarConnectionState::Connected)
	{
		AsyncTask(ENamedThreads::GameThread, [this]() { OnQueryPurchasesFinished.Broadcast(false, TEXT("Not connected. Call Open Connection first."), TArray<FCafeBazaarPurchase>()); });
		return;
	}
	if (bIsQueryInFlight)
	{
		AsyncTask(ENamedThreads::GameThread, [this]() { OnQueryPurchasesFinished.Broadcast(false, TEXT("A query is already in progress."), TArray<FCafeBazaarPurchase>()); });
		return;
	}

	bIsQueryInFlight = true;
	const int32 ThisGeneration = ++QueryGeneration;

#if PLATFORM_ANDROID
	if (!CafeBazaarJNI::QuerySubscribedProducts(ThisGeneration))
	{
		if (ThisGeneration == QueryGeneration) bIsQueryInFlight = false;
		AsyncTask(ENamedThreads::GameThread, [this]() { OnQueryPurchasesFinished.Broadcast(false, TEXT("Failed to dispatch to the Java layer (JNI unavailable)."), TArray<FCafeBazaarPurchase>()); });
	}
#else
	bIsQueryInFlight = false;
	AsyncTask(ENamedThreads::GameThread, [this]() { OnQueryPurchasesFinished.Broadcast(false, GNotAndroidMessage, TArray<FCafeBazaarPurchase>()); });
#endif
}

// ---------------------------------------------------------------------------
// Callbacks from the platform glue layer (already on the game thread).
// Every one of these first checks its Generation against the current
// counter and drops the callback (log + return, no state mutation, no
// delegate broadcast) if it's stale. This is what stops a late-arriving
// result from a superseded connection/purchase/query attempt from
// corrupting whatever the player is doing now (external review points 3/4/5/7/8).
// ---------------------------------------------------------------------------

void UCafeBazaarBillingSubsystem::HandleConnectionSucceeded(int32 Generation)
{
	if (Generation != ConnectionGeneration)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CafeBazaarBilling] Dropping stale HandleConnectionSucceeded (gen %d, current %d)."), Generation, ConnectionGeneration);
		return;
	}
	ConnectionState = ECafeBazaarConnectionState::Connected;
	OnConnectionSucceeded.Broadcast();
}

void UCafeBazaarBillingSubsystem::HandleConnectionFailed(const FString& Message, int32 Generation)
{
	if (Generation != ConnectionGeneration)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CafeBazaarBilling] Dropping stale HandleConnectionFailed (gen %d, current %d)."), Generation, ConnectionGeneration);
		return;
	}
	ConnectionState = ECafeBazaarConnectionState::Disconnected;
	bIsPurchaseInFlight = false;
	bIsQueryInFlight = false;
	// A purchase/query/consume that started under this connection is now
	// meaningless - bump ConnectionGeneration too (Consume is tagged with it),
	// on top of Purchase/QueryGeneration, so any late callback for any of them
	// gets dropped as stale instead of being applied after the connection it
	// belonged to is gone.
	++ConnectionGeneration;
	++PurchaseGeneration;
	++QueryGeneration;
	OnConnectionFailed.Broadcast(Message);
}

void UCafeBazaarBillingSubsystem::HandleDisconnected(int32 Generation)
{
	if (Generation != ConnectionGeneration)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CafeBazaarBilling] Dropping stale HandleDisconnected (gen %d, current %d)."), Generation, ConnectionGeneration);
		return;
	}
	ConnectionState = ECafeBazaarConnectionState::Disconnected;
	bIsPurchaseInFlight = false;
	bIsQueryInFlight = false;
	// Same reasoning as HandleConnectionFailed above.
	++ConnectionGeneration;
	++PurchaseGeneration;
	++QueryGeneration;
	OnDisconnected.Broadcast();
}

void UCafeBazaarBillingSubsystem::HandlePurchaseFinished(bool bSuccess, const FString& Message, const FString& PurchaseJson, int32 Generation)
{
	if (Generation != PurchaseGeneration)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CafeBazaarBilling] Dropping stale HandlePurchaseFinished (gen %d, current %d)."), Generation, PurchaseGeneration);
		return;
	}
	bIsPurchaseInFlight = false;

	FCafeBazaarPurchase Purchase;
	bool bValid = bSuccess;
	if (bSuccess)
	{
		Purchase = ParsePurchaseJson(PurchaseJson);
		// External review point 11: don't trust bSuccess blindly - a "successful"
		// purchase with an empty ProductId/PurchaseToken, or a state other than
		// Purchased, is internally inconsistent and almost certainly a parsing
		// problem or a malformed payload. Downgrade it to a failure rather than
		// handing the game code something that looks successful but isn't safe
		// to act on (e.g. granting an item for it).
		if (Purchase.ProductId.IsEmpty() || Purchase.PurchaseToken.IsEmpty() || Purchase.PurchaseState != ECafeBazaarPurchaseState::Purchased)
		{
			UE_LOG(LogTemp, Error, TEXT("[CafeBazaarBilling] Purchase reported success but failed invariant checks (empty ProductId/PurchaseToken or PurchaseState != Purchased) - reporting as failure instead."));
			bValid = false;
			Purchase = FCafeBazaarPurchase();
		}
	}

	OnPurchaseFinished.Broadcast(bValid, bValid ? Message : TEXT("Purchase data failed internal validation."), Purchase);
}

void UCafeBazaarBillingSubsystem::HandleConsumeFinished(bool bSuccess, const FString& Message, const FString& PurchaseToken, int32 Generation)
{
	if (Generation != ConnectionGeneration)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CafeBazaarBilling] Dropping stale HandleConsumeFinished (gen %d, current %d)."), Generation, ConnectionGeneration);
		return;
	}
	OnConsumeFinished.Broadcast(bSuccess, Message, PurchaseToken);
}

void UCafeBazaarBillingSubsystem::HandleQueryPurchasesFinished(bool bSuccess, const FString& Message, const FString& PurchasesJson, int32 Generation)
{
	if (Generation != QueryGeneration)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CafeBazaarBilling] Dropping stale HandleQueryPurchasesFinished (gen %d, current %d)."), Generation, QueryGeneration);
		return;
	}
	bIsQueryInFlight = false;
	TArray<FCafeBazaarPurchase> Purchases = bSuccess ? ParsePurchasesJson(PurchasesJson) : TArray<FCafeBazaarPurchase>();
	OnQueryPurchasesFinished.Broadcast(bSuccess, Message, Purchases);
}

// ---------------------------------------------------------------------------
// JSON parsing. CafeBazaarBillingBridge.java's purchaseInfoToJson() must
// produce exactly these field names. All fields are read defensively -
// a missing/malformed field just leaves the default value rather than crashing.
//
// Deliberately does NOT log the raw Json string on failure (external review
// point 9): it can contain purchaseToken/orderId/dataSignature/originalJson,
// which are billing-sensitive and shouldn't end up in device logs, crash
// reports, or telemetry pipelines.
// ---------------------------------------------------------------------------

FCafeBazaarPurchase UCafeBazaarBillingSubsystem::ParsePurchaseJson(const FString& Json)
{
	FCafeBazaarPurchase Purchase;
	if (Json.IsEmpty()) return Purchase;

	TSharedPtr<FJsonObject> Obj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("[CafeBazaarBilling] Failed to parse purchase JSON (%d chars) - malformed or unexpected shape."), Json.Len());
		return Purchase;
	}

	Obj->TryGetStringField(TEXT("productId"), Purchase.ProductId);
	Obj->TryGetStringField(TEXT("orderId"), Purchase.OrderId);
	Obj->TryGetStringField(TEXT("purchaseToken"), Purchase.PurchaseToken);
	Obj->TryGetStringField(TEXT("payload"), Purchase.Payload);
	Obj->TryGetStringField(TEXT("packageName"), Purchase.PackageName);
	Obj->TryGetStringField(TEXT("originalJson"), Purchase.OriginalJson);
	Obj->TryGetStringField(TEXT("dataSignature"), Purchase.DataSignature);

	FString StateStr;
	if (Obj->TryGetStringField(TEXT("purchaseState"), StateStr))
	{
		if (StateStr == TEXT("PURCHASED")) Purchase.PurchaseState = ECafeBazaarPurchaseState::Purchased;
		else if (StateStr == TEXT("REFUNDED")) Purchase.PurchaseState = ECafeBazaarPurchaseState::Refunded;
		else Purchase.PurchaseState = ECafeBazaarPurchaseState::Unknown;
	}

	double PurchaseTimeRaw = 0.0;
	if (Obj->TryGetNumberField(TEXT("purchaseTime"), PurchaseTimeRaw))
	{
		// External review point 10: JSON numbers only ever arrive as double via
		// Unreal's Json module - that's not something this parser can change.
		// What we CAN do is reject obviously-malformed values (NaN/Inf, negative,
		// or absurdly far in the future) instead of silently truncating them into
		// a garbage int64.
		const double MaxSaneUnixMs = 4102444800000.0; // year 2100, milliseconds
		if (FMath::IsFinite(PurchaseTimeRaw) && PurchaseTimeRaw >= 0.0 && PurchaseTimeRaw <= MaxSaneUnixMs)
		{
			Purchase.PurchaseTime = (int64)PurchaseTimeRaw;
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[CafeBazaarBilling] purchaseTime out of sane range, leaving as 0."));
		}
	}

	return Purchase;
}

TArray<FCafeBazaarPurchase> UCafeBazaarBillingSubsystem::ParsePurchasesJson(const FString& Json)
{
	TArray<FCafeBazaarPurchase> Result;
	if (Json.IsEmpty()) return Result;

	TArray<TSharedPtr<FJsonValue>> JsonArray;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, JsonArray))
	{
		UE_LOG(LogTemp, Warning, TEXT("[CafeBazaarBilling] Failed to parse purchases array JSON (%d chars)."), Json.Len());
		return Result;
	}

	for (const TSharedPtr<FJsonValue>& Value : JsonArray)
	{
		if (!Value.IsValid()) continue;

		const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
		if (!Value->TryGetObject(ObjPtr) || !ObjPtr || !ObjPtr->IsValid()) continue;

		FString ObjJson;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ObjJson);
		FJsonSerializer::Serialize((*ObjPtr).ToSharedRef(), Writer);
		Result.Add(ParsePurchaseJson(ObjJson));
	}
	return Result;
}
