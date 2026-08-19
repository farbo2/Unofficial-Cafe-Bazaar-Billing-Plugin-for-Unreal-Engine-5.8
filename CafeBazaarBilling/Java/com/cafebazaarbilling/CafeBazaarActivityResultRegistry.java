package com.cafebazaarbilling;

import android.app.Activity;
import android.content.Intent;
import android.content.IntentSender;
import android.util.Log;

import androidx.activity.result.ActivityResultRegistry;
import androidx.activity.result.IntentSenderRequest;
import androidx.activity.result.contract.ActivityResultContract;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.core.app.ActivityOptionsCompat;

/**
 * Minimal re-implementation of what AndroidX ComponentActivity provides for free.
 *
 * Poolakey's purchaseProduct()/subscribeProduct() need an ActivityResultRegistry to
 * launch Bazaar's billing UI and get the result back. UE5's GameActivity extends
 * android.app.NativeActivity, not androidx.activity.ComponentActivity, so there is
 * no registry available out of the box - we provide our own.
 *
 * IMPORTANT - version coupling: this class was written and tested against
 * Poolakey 2.2.0's specific internal usage of ActivityResultRegistry (which
 * contract types it registers, whether it goes through a plain Intent or an
 * IntentSender). It is NOT a general-purpose, SDK-agnostic reimplementation of
 * ComponentActivity's registry - it only handles the two contract shapes
 * Poolakey 2.2.0 is known to use. If you upgrade the `poolakey` dependency in
 * CafeBazaarBilling_UPL.xml, re-verify this class still matches (check
 * Poolakey's PaymentLauncher/PaymentLauncher.Builder for any new contract
 * types) before shipping.
 *
 * onLaunch() mirrors ComponentActivity's internal logic: IntentSender-based contracts
 * go through startIntentSenderForResult, everything else through startActivityForResult.
 * dispatchResult() (inherited from ActivityResultRegistry) is called from
 * handleActivityResult(), which the UPL file wires to GameActivity.onActivityResult.
 *
 * NO STATE PERSISTENCE ACROSS ACTIVITY RECREATION - read before assuming a
 * purchase result can never go missing. AndroidX's real ActivityResultRegistry
 * (the one ComponentActivity gives you for free) saves its pending-launch
 * bookkeeping into onSaveInstanceState/onRestoreInstanceState so a launch
 * survives Activity recreation. This class does not do that - it is a plain
 * in-memory object, rebuilt from scratch by setActivity() every time
 * GameActivity.onCreate() runs. If GameActivity is destroyed and recreated
 * (not just resized/rotated - UE5 normally handles those via android:configChanges
 * without recreating the Activity, but "Don't keep activities", memory-pressure
 * trims while backgrounded, and some OEM multi-window behaviors can still force
 * a real recreation) WHILE Bazaar's payment UI is open, the new registry instance
 * has no memory of the pending launch, so the purchase result cannot be routed
 * back to the original PurchaseCallback closure - that closure no longer exists
 * either, since CafeBazaarBillingBridge.onDestroy() (called for the old Activity)
 * clears sPayment/sConnection.
 *
 * This is not silently ignored, though: onDestroy() now calls
 * nativeOnDisconnected() with the connection's generation whenever a live
 * connection existed, specifically so this scenario resolves as a clean
 * "disconnected -> purchase/query in flight invalidated" on the C++ side
 * (see UCafeBazaarBillingSubsystem::HandleDisconnected) instead of leaving
 * IsPurchaseInFlight stuck true forever with no callback ever arriving. The
 * original transaction's outcome (did Bazaar actually charge the user?) is
 * NOT recovered by this - reconcile it the same way as the process-death case
 * documented in the README: call Query Purchased Products after reconnecting.
 *
 * Hardening notes: every entry point checks the Activity is still alive
 * (isFinishing/isDestroyed) before touching it. Unlike an earlier version of
 * this class, onLaunch() below does NOT swallow failures - it rethrows them
 * (wrapped in a RuntimeException where needed) so they propagate back up
 * through Payment.purchaseProduct()/subscribeProduct(), which calls
 * registry.launch() -> onLaunch() synchronously and in the same call stack as
 * CafeBazaarBillingBridge.launchFlow()'s own try/catch. That catch block is
 * what actually resets sPurchaseFlowActive and reports failure back to C++ -
 * this class deliberately does not try to do that itself, so there is exactly
 * one place responsible for "a purchase attempt is over."
 *
 * This does NOT cover the case where dispatchResult() (called later, from
 * onActivityResult, asynchronously - no longer inside launchFlow's call
 * stack) itself throws after the launch already succeeded and Poolakey is
 * waiting for a result. There is no call frame left to catch that failure in,
 * so in that specific case sPurchaseFlowActive can still get stuck true.
 * UCafeBazaarBillingSubsystem::ResetStuckFlags() exists as a manual,
 * Blueprint-callable escape hatch for exactly this residual scenario.
 */
public class CafeBazaarActivityResultRegistry extends ActivityResultRegistry
{
	private static final String TAG = "CafeBazaarBilling";
	private final Activity activity;

	public CafeBazaarActivityResultRegistry(Activity activity)
	{
		this.activity = activity;
	}

	private boolean isActivityUsable()
	{
		return activity != null && !activity.isFinishing() && !activity.isDestroyed();
	}

	@Override
	public <I, O> void onLaunch(final int requestCode, final ActivityResultContract<I, O> contract, final I input, ActivityOptionsCompat options)
	{
		if (!isActivityUsable())
		{
			throw new IllegalStateException("CafeBazaarActivityResultRegistry.onLaunch: Activity is not usable (requestCode " + requestCode + ").");
		}

		if (contract instanceof ActivityResultContracts.StartIntentSenderForResult)
		{
			if (!(input instanceof IntentSenderRequest))
			{
				throw new IllegalStateException("onLaunch: expected IntentSenderRequest input, got " + (input != null ? input.getClass() : "null"));
			}
			IntentSenderRequest request = (IntentSenderRequest) input;
			try
			{
				activity.startIntentSenderForResult(
					request.getIntentSender(),
					requestCode,
					request.getFillInIntent(),
					request.getFlagsMask(),
					request.getFlagsValues(),
					0,
					options != null ? options.toBundle() : null
				);
			}
			catch (IntentSender.SendIntentException e)
			{
				// Rethrow (wrapped) rather than swallow: launchFlow's catch(Throwable)
				// is what actually resets state and reports the failure back to C++.
				throw new RuntimeException("startIntentSenderForResult failed", e);
			}
		}
		else
		{
			Intent intent = contract.createIntent(activity, input);
			if (intent == null)
			{
				throw new IllegalStateException("onLaunch: contract.createIntent returned null for requestCode " + requestCode);
			}
			// Deliberately NOT wrapped in try/catch: ActivityNotFoundException etc.
			// should propagate to launchFlow's catch(Throwable), same reasoning as above.
			activity.startActivityForResult(intent, requestCode, options != null ? options.toBundle() : null);
		}
	}

	/**
	 * Call from GameActivity.onActivityResult (wired via the UPL file).
	 * This runs asynchronously, outside of launchFlow's call stack, so a
	 * failure here can't be caught by it - see the class-level doc comment.
	 */
	public void handleActivityResult(int requestCode, int resultCode, Intent data)
	{
		try
		{
			if (!dispatchResult(requestCode, resultCode, data))
			{
				Log.w(TAG, "dispatchResult: no pending launch for requestCode " + requestCode);
			}
		}
		catch (Throwable t)
		{
			Log.e(TAG, "dispatchResult threw - the in-flight purchase/subscribe flag may now be stuck. "
				+ "Blueprint can call ResetStuckFlags() on the subsystem to recover.", t);
		}
	}
}
