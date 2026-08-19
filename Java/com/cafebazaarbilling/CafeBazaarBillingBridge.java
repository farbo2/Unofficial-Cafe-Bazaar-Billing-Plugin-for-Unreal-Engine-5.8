package com.cafebazaarbilling;

import android.app.Activity;
import android.content.Intent;
import android.text.TextUtils;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;

import ir.cafebazaar.poolakey.Connection;
import ir.cafebazaar.poolakey.Payment;
import ir.cafebazaar.poolakey.callback.ConnectionCallback;
import ir.cafebazaar.poolakey.callback.ConsumeCallback;
import ir.cafebazaar.poolakey.callback.PurchaseCallback;
import ir.cafebazaar.poolakey.callback.PurchaseQueryCallback;
import ir.cafebazaar.poolakey.config.PaymentConfiguration;
import ir.cafebazaar.poolakey.config.SecurityCheck;
import ir.cafebazaar.poolakey.entity.PurchaseInfo;
import ir.cafebazaar.poolakey.request.PurchaseRequest;

import kotlin.Unit;
import kotlin.jvm.functions.Function0;
import kotlin.jvm.functions.Function1;

/**
 * Static bridge between UE5's C++/JNI layer and Cafe Bazaar's official Poolakey SDK
 * (https://github.com/cafebazaar/Poolakey), v2.2.0.
 *
 * Wired into GameActivity.java by CafeBazaarBilling_UPL.xml:
 *   - onCreate         -> setActivity(this)
 *   - onDestroy         -> onDestroy()
 *   - onActivityResult  -> handleActivityResult(requestCode, resultCode, data)
 *
 * SECURITY NOTE (read this before shipping anything with real money involved):
 * `SecurityCheck.Enable(rsaPublicKey)` in openConnection() makes Poolakey verify
 * the purchase signature locally, on-device, before it ever calls
 * purchaseSucceed(). That is a real check, but it is a CLIENT-SIDE check - it can
 * be bypassed on a rooted/modified device. For consumables, currency, or
 * anything else worth stealing, do not grant the item purely because
 * OnPurchaseFinished(bSuccess=true) fired. Send Purchase.OriginalJson and
 * Purchase.DataSignature to your own backend, re-verify there against your
 * public key, and only then grant the entitlement. This plugin exposes the
 * fields your backend needs; it cannot do backend verification for you since
 * every game's backend is different.
 *
 * Every native* callback below now carries a "generation" integer that the C++
 * layer stamped on the call that triggered it (see CafeBazaarBillingJNI.h).
 * This class treats it as an opaque value to store and echo back unchanged -
 * the Subsystem is what decides whether a generation is still current or stale.
 *
 * Production-hardening notes:
 *   - Every entry point validates its inputs and the current activity/connection
 *     state before touching Poolakey, and always reports a result back through
 *     the native* callbacks - callers never get silently ignored or left hanging.
 *   - sPurchaseFlowActive/sQueryInFlight guard against re-entrant calls (e.g. a
 *     double-tapped Buy button), independent of whatever guard C++ also applies -
 *     this class should be safe to call in isolation.
 *   - Every SDK call and every DSL callback body is wrapped in try/catch so a
 *     Poolakey-side exception can't crash the app or corrupt JNI state; the C++
 *     layer separately clears any pending JNI exception, but Java is the first
 *     line of defense.
 *   - All entry points run on the Activity's UI thread via runOnUiThread, since
 *     UE5's GameThread has no Android Looper and Poolakey's internal AIDL/
 *     broadcast connections need one.
 *
 * Known limitation: if Android kills the app process, OR destroys and
 * recreates GameActivity (not just resizes/rotates it - see the "NO STATE
 * PERSISTENCE" note in CafeBazaarActivityResultRegistry.java for when that
 * can happen), while Bazaar's payment Activity is in the foreground, the
 * in-memory Payment/Connection/ActivityResultRegistry state is lost, and the
 * original purchase's success/failure callback cannot be delivered. This
 * mirrors the behavior of a native app that doesn't persist billing state
 * across process death or Activity recreation; there is no general-purpose
 * fix at this layer for recovering that specific callback. What IS handled:
 * onDestroy() below notifies the C++ layer of the disconnection either way,
 * so the symptom is a clean "purchase/connection failed, try again" rather
 * than a silently stuck IsPurchaseInFlight with no callback ever arriving.
 * Call Query Purchased Products after reconnecting to reconcile any purchase
 * that actually went through on Bazaar's side despite the lost callback.
 */
public final class CafeBazaarBillingBridge
{
	private static final String TAG = "CafeBazaarBilling";

	private static volatile Activity sActivity;
	private static volatile Payment sPayment;
	private static volatile Connection sConnection;
	private static volatile CafeBazaarActivityResultRegistry sRegistry;

	// The generation (see CafeBazaarBillingJNI.h) of whatever connection attempt
	// is currently represented by sConnection/sPayment above, if any. Used by
	// onDestroy() to tell C++ "the connection you think is still open is gone"
	// with the correct generation, regardless of whether onDestroy() fired
	// because of an explicit CloseConnection or because Android tore down and
	// recreated GameActivity out from under us.
	private static volatile int sActiveConnectionGeneration = -1;

	private static final AtomicBoolean sConnecting = new AtomicBoolean(false);
	private static final AtomicBoolean sPurchaseFlowActive = new AtomicBoolean(false);
	private static final AtomicBoolean sQueryInFlight = new AtomicBoolean(false);

	private CafeBazaarBillingBridge() {}

	// ------------------------------------------------------------------
	// Lifecycle hooks (called from GameActivity via UPL)
	// ------------------------------------------------------------------

	public static void setActivity(Activity activity)
	{
		sActivity = activity;
		sRegistry = (activity != null) ? new CafeBazaarActivityResultRegistry(activity) : null;
	}

	/**
	 * Called both for an explicit CloseConnection (Blueprint asked for it - in
	 * which case C++ has already proactively updated its own state and this
	 * call is a no-op from C++'s point of view) AND whenever GameActivity is
	 * torn down, including recreation (rotation without configChanges covering
	 * it, "don't keep activities", memory-pressure recreation, etc.) - in which
	 * case C++ otherwise has no way to find out its Connected/Connecting state
	 * is now stale, since nothing called CloseConnection on its behalf.
	 *
	 * To close that gap, if there was a live connection/payment when this runs,
	 * we synthesize a native disconnected notification carrying the generation
	 * that connection was created under. If C++'s generation counter already
	 * moved on its own (the ordinary explicit-close path), this notification
	 * arrives stale and is silently dropped - harmless. If C++ never got the
	 * memo (the recreation path), this is what tells it: ConnectionState flips
	 * to Disconnected, and any purchase/query that was in flight under this
	 * connection is invalidated too, instead of hanging forever.
	 */
	public static void onDestroy()
	{
		final Connection connection = sConnection;
		final int generationToNotify = sActiveConnectionGeneration;
		final boolean hadConnection = (connection != null || sPayment != null);

		sConnection = null;
		sPayment = null;
		sActiveConnectionGeneration = -1;
		sConnecting.set(false);
		sPurchaseFlowActive.set(false);
		sQueryInFlight.set(false);

		if (connection != null)
		{
			try { connection.disconnect(); } catch (Throwable t) { Log.e(TAG, "disconnect failed", t); }
		}

		if (hadConnection)
		{
			nativeOnDisconnected(generationToNotify);
		}
	}

	public static void handleActivityResult(int requestCode, int resultCode, Intent data)
	{
		CafeBazaarActivityResultRegistry registry = sRegistry;
		if (registry != null)
		{
			try { registry.handleActivityResult(requestCode, resultCode, data); }
			catch (Throwable t) { Log.e(TAG, "handleActivityResult failed", t); }
		}
	}

	// ------------------------------------------------------------------
	// Internal helpers
	// ------------------------------------------------------------------

	/** True if we have a usable Activity to launch UI from or bind a connection to. */
	private static boolean isActivityUsable()
	{
		Activity a = sActivity;
		if (a == null || a.isFinishing()) return false;
		return !a.isDestroyed();
	}

	/**
	 * Attempts to run r on the UI thread. Returns false if that could not even be
	 * attempted (no/dead Activity, or runOnUiThread itself threw synchronously).
	 * Callers MUST treat a false return as an immediate terminal failure of
	 * whatever operation they were starting - see the class-level contract note.
	 */
	private static boolean tryRunOnUi(final Runnable r)
	{
		Activity a = sActivity;
		if (a == null || a.isFinishing() || a.isDestroyed())
		{
			Log.e(TAG, "tryRunOnUi: no usable Activity.");
			return false;
		}
		try
		{
			a.runOnUiThread(r);
			return true;
		}
		catch (Throwable t)
		{
			Log.e(TAG, "runOnUiThread dispatch failed", t);
			return false;
		}
	}

	// ------------------------------------------------------------------
	// API called from C++ (CafeBazaarBillingJNI.cpp). Each takes a
	// "generation" int that is opaque here - just store it in the closure
	// and echo it back unchanged on the corresponding native* callback.
	// ------------------------------------------------------------------

	public static void openConnection(final String rsaPublicKey, final int generation)
	{
		if (TextUtils.isEmpty(rsaPublicKey))
		{
			nativeOnConnectionFailed("RSA public key is empty.", generation);
			return;
		}
		if (!isActivityUsable())
		{
			nativeOnConnectionFailed("Activity not ready yet.", generation);
			return;
		}
		// No "already connecting -> silently ignore" guard here anymore: C++
		// always relays every OpenConnection call and expects a definitive
		// terminal callback for the generation it sent (see the comment in
		// UCafeBazaarBillingSubsystem::OpenConnection for why silently
		// dropping duplicates was unsafe). runOnUiThread serializes calls onto
		// a single queue, and the block below already tears down whatever
		// connection/payment existed before building a new one, so concurrent
		// calls are handled correctly without needing a separate guard here.
		sConnecting.set(true);

		if (!tryRunOnUi(new Runnable()
		{
			@Override
			public void run()
			{
				try
				{
					// Tear down any previous connection before creating a new one,
					// so re-connecting after a manual CloseConnection can't leak.
					Connection previous = sConnection;
					sConnection = null;
					sPayment = null;
					if (previous != null)
					{
						try { previous.disconnect(); } catch (Throwable ignored) {}
					}

					Activity activity = sActivity;
					if (activity == null || activity.isFinishing() || activity.isDestroyed())
					{
						sConnecting.set(false);
						nativeOnConnectionFailed("Activity became unavailable while connecting.", generation);
						return;
					}

					SecurityCheck securityCheck = new SecurityCheck.Enable(rsaPublicKey);
					PaymentConfiguration config = new PaymentConfiguration(securityCheck, true);
					Payment payment = new Payment(activity, config);
					sPayment = payment;
					sActiveConnectionGeneration = generation;

					sConnection = payment.connect(new Function1<ConnectionCallback, Unit>()
					{
						@Override
						public Unit invoke(ConnectionCallback callback)
						{
							callback.connectionSucceed(new Function0<Unit>()
							{
								@Override
								public Unit invoke()
								{
									sConnecting.set(false);
									nativeOnConnectionSucceeded(generation);
									return Unit.INSTANCE;
								}
							});
							callback.connectionFailed(new Function1<Throwable, Unit>()
							{
								@Override
								public Unit invoke(Throwable throwable)
								{
									sConnecting.set(false);
									// This attempt is dead - clear it so a later onDestroy() (e.g.
									// Activity teardown) doesn't think there's still a live
									// connection to synthesize a redundant disconnect for.
									if (sActiveConnectionGeneration == generation)
									{
										sConnection = null;
										sPayment = null;
										sActiveConnectionGeneration = -1;
									}
									nativeOnConnectionFailed(describeThrowable(throwable), generation);
									return Unit.INSTANCE;
								}
							});
							callback.disconnected(new Function0<Unit>()
							{
								@Override
								public Unit invoke()
								{
									sConnecting.set(false);
									sPurchaseFlowActive.set(false);
									sQueryInFlight.set(false);
									if (sActiveConnectionGeneration == generation)
									{
										sConnection = null;
										sPayment = null;
										sActiveConnectionGeneration = -1;
									}
									nativeOnDisconnected(generation);
									return Unit.INSTANCE;
								}
							});
							return Unit.INSTANCE;
						}
					});
				}
				catch (Throwable t)
				{
					Log.e(TAG, "openConnection failed", t);
					sConnecting.set(false);
					nativeOnConnectionFailed("Exception while connecting: " + describeThrowable(t), generation);
				}
			}
		}))
		{
			sConnecting.set(false);
			nativeOnConnectionFailed("Could not dispatch to the UI thread (activity unavailable).", generation);
		}
	}

	public static void closeConnection()
	{
		if (!tryRunOnUi(new Runnable() { @Override public void run() { onDestroy(); } }))
		{
			// Dispatch failed - fall back to synchronous cleanup right here so
			// state doesn't get left dangling just because the Activity is gone.
			onDestroy();
		}
	}

	public static void launchPurchaseFlow(final String productId, final String payload, final int generation)
	{
		launchFlow(productId, payload, false, generation);
	}

	public static void launchSubscribeFlow(final String productId, final String payload, final int generation)
	{
		launchFlow(productId, payload, true, generation);
	}

	private static void launchFlow(final String productId, final String payload, final boolean isSubscription, final int generation)
	{
		if (TextUtils.isEmpty(productId))
		{
			nativeOnPurchaseFinished(false, "Product ID is empty.", "{}", generation);
			return;
		}
		final Payment payment = sPayment;
		final CafeBazaarActivityResultRegistry registry = sRegistry;
		if (payment == null || registry == null)
		{
			nativeOnPurchaseFinished(false, "Not connected. Call OpenConnection first.", "{}", generation);
			return;
		}
		if (!isActivityUsable())
		{
			nativeOnPurchaseFinished(false, "Activity not available.", "{}", generation);
			return;
		}
		if (!sPurchaseFlowActive.compareAndSet(false, true))
		{
			nativeOnPurchaseFinished(false, "A purchase is already in progress.", "{}", generation);
			return;
		}

		if (!tryRunOnUi(new Runnable()
		{
			@Override
			public void run()
			{
				try
				{
					if (!isActivityUsable())
					{
						sPurchaseFlowActive.set(false);
						nativeOnPurchaseFinished(false, "Activity became unavailable.", "{}", generation);
						return;
					}

					PurchaseRequest request = new PurchaseRequest(productId, payload != null ? payload : "", null);

					Function1<PurchaseCallback, Unit> callbackBuilder = new Function1<PurchaseCallback, Unit>()
					{
						@Override
						public Unit invoke(PurchaseCallback callback)
						{
							callback.purchaseFlowBegan(new Function0<Unit>()
							{
								@Override public Unit invoke() { Log.d(TAG, "Purchase flow began for " + productId); return Unit.INSTANCE; }
							});
							callback.failedToBeginFlow(new Function1<Throwable, Unit>()
							{
								@Override
								public Unit invoke(Throwable t)
								{
									sPurchaseFlowActive.set(false);
									nativeOnPurchaseFinished(false, "Failed to open billing UI: " + describeThrowable(t), "{}", generation);
									return Unit.INSTANCE;
								}
							});
							callback.purchaseSucceed(new Function1<PurchaseInfo, Unit>()
							{
								@Override
								public Unit invoke(PurchaseInfo info)
								{
									sPurchaseFlowActive.set(false);
									nativeOnPurchaseFinished(true, "OK", purchaseInfoToJson(info).toString(), generation);
									return Unit.INSTANCE;
								}
							});
							callback.purchaseCanceled(new Function0<Unit>()
							{
								@Override
								public Unit invoke()
								{
									sPurchaseFlowActive.set(false);
									nativeOnPurchaseFinished(false, "Canceled by user", "{}", generation);
									return Unit.INSTANCE;
								}
							});
							callback.purchaseFailed(new Function1<Throwable, Unit>()
							{
								@Override
								public Unit invoke(Throwable t)
								{
									sPurchaseFlowActive.set(false);
									nativeOnPurchaseFinished(false, describeThrowable(t), "{}", generation);
									return Unit.INSTANCE;
								}
							});
							return Unit.INSTANCE;
						}
					};

					if (isSubscription)
					{
						payment.subscribeProduct(registry, request, callbackBuilder);
					}
					else
					{
						payment.purchaseProduct(registry, request, callbackBuilder);
					}
				}
				catch (Throwable t)
				{
					Log.e(TAG, "launchFlow failed", t);
					sPurchaseFlowActive.set(false);
					nativeOnPurchaseFinished(false, "Exception launching purchase flow: " + describeThrowable(t), "{}", generation);
				}
			}
		}))
		{
			sPurchaseFlowActive.set(false);
			nativeOnPurchaseFinished(false, "Could not dispatch to the UI thread (activity unavailable).", "{}", generation);
		}
	}

	public static void consumePurchase(final String purchaseToken, final int generation)
	{
		if (TextUtils.isEmpty(purchaseToken))
		{
			nativeOnConsumeFinished(false, "Purchase token is empty.", "", generation);
			return;
		}
		final Payment payment = sPayment;
		if (payment == null)
		{
			nativeOnConsumeFinished(false, "Not connected. Call OpenConnection first.", purchaseToken, generation);
			return;
		}

		if (!tryRunOnUi(new Runnable()
		{
			@Override
			public void run()
			{
				try
				{
					payment.consumeProduct(purchaseToken, new Function1<ConsumeCallback, Unit>()
					{
						@Override
						public Unit invoke(ConsumeCallback callback)
						{
							callback.consumeSucceed(new Function0<Unit>()
							{
								@Override public Unit invoke() { nativeOnConsumeFinished(true, "OK", purchaseToken, generation); return Unit.INSTANCE; }
							});
							callback.consumeFailed(new Function1<Throwable, Unit>()
							{
								@Override
								public Unit invoke(Throwable t)
								{
									nativeOnConsumeFinished(false, describeThrowable(t), purchaseToken, generation);
									return Unit.INSTANCE;
								}
							});
							return Unit.INSTANCE;
						}
					});
				}
				catch (Throwable t)
				{
					Log.e(TAG, "consumePurchase failed", t);
					nativeOnConsumeFinished(false, "Exception consuming purchase: " + describeThrowable(t), purchaseToken, generation);
				}
			}
		}))
		{
			nativeOnConsumeFinished(false, "Could not dispatch to the UI thread (activity unavailable).", purchaseToken, generation);
		}
	}

	public static void queryPurchasedProducts(final int generation)
	{
		queryProducts(false, generation);
	}

	public static void querySubscribedProducts(final int generation)
	{
		queryProducts(true, generation);
	}

	private static void queryProducts(final boolean isSubscription, final int generation)
	{
		final Payment payment = sPayment;
		if (payment == null)
		{
			nativeOnQueryPurchasesFinished(false, "Not connected. Call OpenConnection first.", "[]", generation);
			return;
		}
		if (!sQueryInFlight.compareAndSet(false, true))
		{
			nativeOnQueryPurchasesFinished(false, "A query is already in progress.", "[]", generation);
			return;
		}

		if (!tryRunOnUi(new Runnable()
		{
			@Override
			public void run()
			{
				try
				{
					Function1<PurchaseQueryCallback, Unit> callbackBuilder = new Function1<PurchaseQueryCallback, Unit>()
					{
						@Override
						public Unit invoke(PurchaseQueryCallback callback)
						{
							callback.querySucceed(new Function1<List<PurchaseInfo>, Unit>()
							{
								@Override
								public Unit invoke(List<PurchaseInfo> purchasedProducts)
								{
									sQueryInFlight.set(false);
									nativeOnQueryPurchasesFinished(true, "OK", purchasesToJson(purchasedProducts).toString(), generation);
									return Unit.INSTANCE;
								}
							});
							callback.queryFailed(new Function1<Throwable, Unit>()
							{
								@Override
								public Unit invoke(Throwable t)
								{
									sQueryInFlight.set(false);
									nativeOnQueryPurchasesFinished(false, describeThrowable(t), "[]", generation);
									return Unit.INSTANCE;
								}
							});
							return Unit.INSTANCE;
						}
					};

					if (isSubscription)
					{
						payment.getSubscribedProducts(callbackBuilder);
					}
					else
					{
						payment.getPurchasedProducts(callbackBuilder);
					}
				}
				catch (Throwable t)
				{
					Log.e(TAG, "queryProducts failed", t);
					sQueryInFlight.set(false);
					nativeOnQueryPurchasesFinished(false, "Exception querying products: " + describeThrowable(t), "[]", generation);
				}
			}
		}))
		{
			sQueryInFlight.set(false);
			nativeOnQueryPurchasesFinished(false, "Could not dispatch to the UI thread (activity unavailable).", "[]", generation);
		}
	}

	// ------------------------------------------------------------------
	// JSON helpers - keep in sync with UCafeBazaarBillingSubsystem's parsers.
	// Deliberately never log the JSONObject/JSONArray contents themselves -
	// they carry purchaseToken/orderId/dataSignature/originalJson, which are
	// billing-sensitive and shouldn't end up in logcat, crash reports, or any
	// telemetry pipeline that scrapes device logs.
	// ------------------------------------------------------------------

	private static String describeThrowable(Throwable t)
	{
		if (t == null) return "Unknown error";
		String msg = t.getMessage();
		return TextUtils.isEmpty(msg) ? t.getClass().getSimpleName() : msg;
	}

	private static JSONObject purchaseInfoToJson(PurchaseInfo info)
	{
		JSONObject obj = new JSONObject();
		if (info == null) return obj;
		try
		{
			obj.put("productId", nullToEmpty(info.getProductId()));
			obj.put("purchaseToken", nullToEmpty(info.getPurchaseToken()));
			obj.put("payload", nullToEmpty(info.getPayload()));
			obj.put("packageName", nullToEmpty(info.getPackageName()));
			obj.put("purchaseState", info.getPurchaseState() != null ? info.getPurchaseState().name() : "");
			obj.put("purchaseTime", info.getPurchaseTime());
			obj.put("orderId", nullToEmpty(info.getOrderId()));
			obj.put("originalJson", nullToEmpty(info.getOriginalJson()));
			obj.put("dataSignature", nullToEmpty(info.getDataSignature()));
		}
		catch (JSONException e)
		{
			// Not logging e's message or the JSONObject itself - could contain
			// fragments of the billing fields above. The exception type/stack
			// alone is enough to diagnose a JSONException here.
			Log.e(TAG, "purchaseInfoToJson failed: " + e.getClass().getSimpleName());
		}
		return obj;
	}

	private static String nullToEmpty(String s)
	{
		return s != null ? s : "";
	}

	private static JSONArray purchasesToJson(List<PurchaseInfo> purchases)
	{
		JSONArray array = new JSONArray();
		if (purchases != null)
		{
			for (PurchaseInfo info : purchases)
			{
				if (info != null)
				{
					array.put(purchaseInfoToJson(info));
				}
			}
		}
		return array;
	}

	// ------------------------------------------------------------------
	// Native callbacks, implemented in C++ and bound via RegisterNatives.
	// The trailing `generation` int on each is the value that was passed
	// into the corresponding call above - echoed back unchanged.
	// ------------------------------------------------------------------

	private static native void nativeOnConnectionSucceeded(int generation);
	private static native void nativeOnConnectionFailed(String message, int generation);
	private static native void nativeOnDisconnected(int generation);
	private static native void nativeOnPurchaseFinished(boolean success, String message, String purchaseJson, int generation);
	private static native void nativeOnConsumeFinished(boolean success, String message, String purchaseToken, int generation);
	private static native void nativeOnQueryPurchasesFinished(boolean success, String message, String purchasesJson, int generation);
}
