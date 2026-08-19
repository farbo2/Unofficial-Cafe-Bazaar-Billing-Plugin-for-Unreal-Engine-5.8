#pragma once

#include "CoreMinimal.h"

#if PLATFORM_ANDROID

/**
 * Thin C++ <-> Java bridge for CafeBazaarBillingBridge.java.
 * Same RegisterNatives-based pattern as the MyketBilling plugin - see that
 * plugin's MyketBillingJNI.h for the full rationale.
 *
 * Every function returns true only if the call was actually dispatched into
 * Java. If it returns false (JNIEnv/class/method not resolvable, or a Java
 * exception escaped the dispatch itself), the caller MUST treat this as an
 * immediate failure and broadcast/reset its own state - Java will not be
 * calling back in that case, so nothing else will.
 *
 * The Generation parameters are opaque integers the Subsystem stamps on each
 * new attempt; Java echoes them back unchanged via the corresponding
 * native* callback. This lets the Subsystem recognize and drop a late
 * callback from an attempt that has since been superseded (a newer connect/
 * purchase/query call, or a manual reset) instead of letting it corrupt
 * current state.
 */
namespace CafeBazaarJNI
{
	void RegisterNatives();

	bool OpenConnection(const FString& RsaPublicKey, int32 Generation);
	bool CloseConnection();
	bool LaunchPurchaseFlow(const FString& ProductId, const FString& Payload, int32 Generation);
	bool LaunchSubscribeFlow(const FString& ProductId, const FString& Payload, int32 Generation);
	bool ConsumePurchase(const FString& PurchaseToken, int32 Generation);
	bool QueryPurchasedProducts(int32 Generation);
	bool QuerySubscribedProducts(int32 Generation);
}

#endif // PLATFORM_ANDROID
