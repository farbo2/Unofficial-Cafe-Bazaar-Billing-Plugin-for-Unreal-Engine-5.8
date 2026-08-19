#include "CafeBazaarBillingJNI.h"

#if PLATFORM_ANDROID

#include "CafeBazaarBillingSubsystem.h"
#include "Async/Async.h"
#include "Android/AndroidApplication.h"
#include "Android/AndroidJNI.h"

// Must match the package declared at the top of CafeBazaarBillingBridge.java
// and the destination path used for it in CafeBazaarBilling_UPL.xml's <copyDir>.
#define CAFEBAZAAR_BRIDGE_CLASS "com/cafebazaarbilling/CafeBazaarBillingBridge"

namespace
{
	jclass GBridgeClass = nullptr;
	bool bNativesRegistered = false;

	jclass GetBridgeClass(JNIEnv* Env)
	{
		if (!GBridgeClass)
		{
			jclass LocalClass = FAndroidApplication::FindJavaClass(CAFEBAZAAR_BRIDGE_CLASS);
			if (LocalClass)
			{
				GBridgeClass = (jclass)Env->NewGlobalRef(LocalClass);
				Env->DeleteLocalRef(LocalClass);
			}
		}
		return GBridgeClass;
	}

	/** Clears and logs any pending Java exception so it can't corrupt the next JNI call. */
	bool CheckAndClearException(JNIEnv* Env, const TCHAR* Where)
	{
		if (Env->ExceptionCheck())
		{
			Env->ExceptionDescribe();
			Env->ExceptionClear();
			UE_LOG(LogTemp, Error, TEXT("[CafeBazaarBilling] Java exception during %s - see logcat above for details."), Where);
			return true;
		}
		return false;
	}

	/** GetStaticMethodID with a friendly log on failure (e.g. after a Java-side signature change). */
	jmethodID FindStaticMethodOrLog(JNIEnv* Env, jclass Clazz, const char* Name, const char* Sig)
	{
		jmethodID Method = Env->GetStaticMethodID(Clazz, Name, Sig);
		if (!Method || CheckAndClearException(Env, *FString::Printf(TEXT("GetStaticMethodID(%hs)"), Name)))
		{
			UE_LOG(LogTemp, Error, TEXT("[CafeBazaarBilling] Could not resolve Java method %hs%hs - Java/C++ are out of sync."), Name, Sig);
			return nullptr;
		}
		return Method;
	}

	FString JStringToFString(JNIEnv* Env, jstring JStr)
	{
		if (!JStr) return FString();
		const char* Chars = Env->GetStringUTFChars(JStr, nullptr);
		if (!Chars) return FString();
		FString Result = FString(UTF8_TO_TCHAR(Chars));
		Env->ReleaseStringUTFChars(JStr, Chars);
		return Result;
	}

	jstring FStringToJString(JNIEnv* Env, const FString& Str)
	{
		return Env->NewStringUTF(TCHAR_TO_UTF8(*Str));
	}

	// ---------------------------------------------------------------------
	// Java -> C++ callbacks, bound via RegisterNatives. Every callback now
	// carries the generation it was issued for (see CafeBazaarBillingJNI.h);
	// the Subsystem's Handle* methods are responsible for comparing it
	// against their current generation counter and dropping stale calls.
	// ---------------------------------------------------------------------

	void JNICALL Native_OnConnectionSucceeded(JNIEnv* Env, jclass Clazz, jint Generation)
	{
		AsyncTask(ENamedThreads::GameThread, [Generation]()
		{
			if (UCafeBazaarBillingSubsystem* S = UCafeBazaarBillingSubsystem::Get()) S->HandleConnectionSucceeded(Generation);
		});
	}

	void JNICALL Native_OnConnectionFailed(JNIEnv* Env, jclass Clazz, jstring Message, jint Generation)
	{
		FString MessageStr = JStringToFString(Env, Message);
		AsyncTask(ENamedThreads::GameThread, [MessageStr, Generation]()
		{
			if (UCafeBazaarBillingSubsystem* S = UCafeBazaarBillingSubsystem::Get()) S->HandleConnectionFailed(MessageStr, Generation);
		});
	}

	void JNICALL Native_OnDisconnected(JNIEnv* Env, jclass Clazz, jint Generation)
	{
		AsyncTask(ENamedThreads::GameThread, [Generation]()
		{
			if (UCafeBazaarBillingSubsystem* S = UCafeBazaarBillingSubsystem::Get()) S->HandleDisconnected(Generation);
		});
	}

	void JNICALL Native_OnPurchaseFinished(JNIEnv* Env, jclass Clazz, jboolean bSuccess, jstring Message, jstring PurchaseJson, jint Generation)
	{
		FString MessageStr = JStringToFString(Env, Message);
		FString PurchaseJsonStr = JStringToFString(Env, PurchaseJson);
		bool bOk = (bSuccess != JNI_FALSE);
		AsyncTask(ENamedThreads::GameThread, [bOk, MessageStr, PurchaseJsonStr, Generation]()
		{
			if (UCafeBazaarBillingSubsystem* S = UCafeBazaarBillingSubsystem::Get()) S->HandlePurchaseFinished(bOk, MessageStr, PurchaseJsonStr, Generation);
		});
	}

	void JNICALL Native_OnConsumeFinished(JNIEnv* Env, jclass Clazz, jboolean bSuccess, jstring Message, jstring PurchaseToken, jint Generation)
	{
		FString MessageStr = JStringToFString(Env, Message);
		FString TokenStr = JStringToFString(Env, PurchaseToken);
		bool bOk = (bSuccess != JNI_FALSE);
		AsyncTask(ENamedThreads::GameThread, [bOk, MessageStr, TokenStr, Generation]()
		{
			if (UCafeBazaarBillingSubsystem* S = UCafeBazaarBillingSubsystem::Get()) S->HandleConsumeFinished(bOk, MessageStr, TokenStr, Generation);
		});
	}

	void JNICALL Native_OnQueryPurchasesFinished(JNIEnv* Env, jclass Clazz, jboolean bSuccess, jstring Message, jstring PurchasesJson, jint Generation)
	{
		FString MessageStr = JStringToFString(Env, Message);
		FString PurchasesJsonStr = JStringToFString(Env, PurchasesJson);
		bool bOk = (bSuccess != JNI_FALSE);
		AsyncTask(ENamedThreads::GameThread, [bOk, MessageStr, PurchasesJsonStr, Generation]()
		{
			if (UCafeBazaarBillingSubsystem* S = UCafeBazaarBillingSubsystem::Get()) S->HandleQueryPurchasesFinished(bOk, MessageStr, PurchasesJsonStr, Generation);
		});
	}
}

namespace CafeBazaarJNI
{
	void RegisterNatives()
	{
		if (bNativesRegistered) return;

		JNIEnv* Env = FAndroidApplication::GetJavaEnv();
		if (!Env)
		{
			UE_LOG(LogTemp, Error, TEXT("[CafeBazaarBilling] No JNIEnv available - cannot register natives."));
			return;
		}

		jclass BridgeClass = GetBridgeClass(Env);
		if (!BridgeClass)
		{
			UE_LOG(LogTemp, Error, TEXT("[CafeBazaarBilling] Could not find Java class %s - is the UPL wired up correctly?"), TEXT(CAFEBAZAAR_BRIDGE_CLASS));
			return;
		}

		static const JNINativeMethod Methods[] =
		{
			{ "nativeOnConnectionSucceeded",    "(I)V",                                        (void*)&Native_OnConnectionSucceeded },
			{ "nativeOnConnectionFailed",       "(Ljava/lang/String;I)V",                      (void*)&Native_OnConnectionFailed },
			{ "nativeOnDisconnected",           "(I)V",                                        (void*)&Native_OnDisconnected },
			{ "nativeOnPurchaseFinished",       "(ZLjava/lang/String;Ljava/lang/String;I)V",   (void*)&Native_OnPurchaseFinished },
			{ "nativeOnConsumeFinished",        "(ZLjava/lang/String;Ljava/lang/String;I)V",   (void*)&Native_OnConsumeFinished },
			{ "nativeOnQueryPurchasesFinished", "(ZLjava/lang/String;Ljava/lang/String;I)V",   (void*)&Native_OnQueryPurchasesFinished },
		};

		if (Env->RegisterNatives(BridgeClass, Methods, UE_ARRAY_COUNT(Methods)) == 0)
		{
			bNativesRegistered = true;
		}
		else
		{
			CheckAndClearException(Env, TEXT("RegisterNatives"));
			UE_LOG(LogTemp, Error, TEXT("[CafeBazaarBilling] RegisterNatives failed."));
		}
	}

	bool OpenConnection(const FString& RsaPublicKey, int32 Generation)
	{
		JNIEnv* Env = FAndroidApplication::GetJavaEnv();
		jclass BridgeClass = Env ? GetBridgeClass(Env) : nullptr;
		if (!Env || !BridgeClass) return false;

		jmethodID Method = FindStaticMethodOrLog(Env, BridgeClass, "openConnection", "(Ljava/lang/String;I)V");
		if (!Method) return false;

		jstring JKey = FStringToJString(Env, RsaPublicKey);
		Env->CallStaticVoidMethod(BridgeClass, Method, JKey, (jint)Generation);
		bool bThrew = CheckAndClearException(Env, TEXT("openConnection"));
		Env->DeleteLocalRef(JKey);
		return !bThrew;
	}

	bool CloseConnection()
	{
		JNIEnv* Env = FAndroidApplication::GetJavaEnv();
		jclass BridgeClass = Env ? GetBridgeClass(Env) : nullptr;
		if (!Env || !BridgeClass) return false;

		jmethodID Method = FindStaticMethodOrLog(Env, BridgeClass, "closeConnection", "()V");
		if (!Method) return false;

		Env->CallStaticVoidMethod(BridgeClass, Method);
		return !CheckAndClearException(Env, TEXT("closeConnection"));
	}

	bool LaunchPurchaseFlow(const FString& ProductId, const FString& Payload, int32 Generation)
	{
		JNIEnv* Env = FAndroidApplication::GetJavaEnv();
		jclass BridgeClass = Env ? GetBridgeClass(Env) : nullptr;
		if (!Env || !BridgeClass) return false;

		jmethodID Method = FindStaticMethodOrLog(Env, BridgeClass, "launchPurchaseFlow", "(Ljava/lang/String;Ljava/lang/String;I)V");
		if (!Method) return false;

		jstring JProductId = FStringToJString(Env, ProductId);
		jstring JPayload = FStringToJString(Env, Payload);
		Env->CallStaticVoidMethod(BridgeClass, Method, JProductId, JPayload, (jint)Generation);
		bool bThrew = CheckAndClearException(Env, TEXT("launchPurchaseFlow"));
		Env->DeleteLocalRef(JProductId);
		Env->DeleteLocalRef(JPayload);
		return !bThrew;
	}

	bool LaunchSubscribeFlow(const FString& ProductId, const FString& Payload, int32 Generation)
	{
		JNIEnv* Env = FAndroidApplication::GetJavaEnv();
		jclass BridgeClass = Env ? GetBridgeClass(Env) : nullptr;
		if (!Env || !BridgeClass) return false;

		jmethodID Method = FindStaticMethodOrLog(Env, BridgeClass, "launchSubscribeFlow", "(Ljava/lang/String;Ljava/lang/String;I)V");
		if (!Method) return false;

		jstring JProductId = FStringToJString(Env, ProductId);
		jstring JPayload = FStringToJString(Env, Payload);
		Env->CallStaticVoidMethod(BridgeClass, Method, JProductId, JPayload, (jint)Generation);
		bool bThrew = CheckAndClearException(Env, TEXT("launchSubscribeFlow"));
		Env->DeleteLocalRef(JProductId);
		Env->DeleteLocalRef(JPayload);
		return !bThrew;
	}

	bool ConsumePurchase(const FString& PurchaseToken, int32 Generation)
	{
		JNIEnv* Env = FAndroidApplication::GetJavaEnv();
		jclass BridgeClass = Env ? GetBridgeClass(Env) : nullptr;
		if (!Env || !BridgeClass) return false;

		jmethodID Method = FindStaticMethodOrLog(Env, BridgeClass, "consumePurchase", "(Ljava/lang/String;I)V");
		if (!Method) return false;

		jstring JToken = FStringToJString(Env, PurchaseToken);
		Env->CallStaticVoidMethod(BridgeClass, Method, JToken, (jint)Generation);
		bool bThrew = CheckAndClearException(Env, TEXT("consumePurchase"));
		Env->DeleteLocalRef(JToken);
		return !bThrew;
	}

	bool QueryPurchasedProducts(int32 Generation)
	{
		JNIEnv* Env = FAndroidApplication::GetJavaEnv();
		jclass BridgeClass = Env ? GetBridgeClass(Env) : nullptr;
		if (!Env || !BridgeClass) return false;

		jmethodID Method = FindStaticMethodOrLog(Env, BridgeClass, "queryPurchasedProducts", "(I)V");
		if (!Method) return false;

		Env->CallStaticVoidMethod(BridgeClass, Method, (jint)Generation);
		return !CheckAndClearException(Env, TEXT("queryPurchasedProducts"));
	}

	bool QuerySubscribedProducts(int32 Generation)
	{
		JNIEnv* Env = FAndroidApplication::GetJavaEnv();
		jclass BridgeClass = Env ? GetBridgeClass(Env) : nullptr;
		if (!Env || !BridgeClass) return false;

		jmethodID Method = FindStaticMethodOrLog(Env, BridgeClass, "querySubscribedProducts", "(I)V");
		if (!Method) return false;

		Env->CallStaticVoidMethod(BridgeClass, Method, (jint)Generation);
		return !CheckAndClearException(Env, TEXT("querySubscribedProducts"));
	}
}

#endif // PLATFORM_ANDROID
