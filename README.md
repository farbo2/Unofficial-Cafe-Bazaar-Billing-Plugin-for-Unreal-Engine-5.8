# Cafe Bazaar Billing for Unreal Engine 5

A Blueprint-friendly Unreal Engine 5 plugin for integrating Cafe Bazaar in-app purchases on Android.

The plugin exposes Cafe Bazaar billing through a **Game Instance Subsystem**, allowing you to handle connection, purchases, subscriptions, consumption, and purchase queries directly from Blueprint.

## Requirements

* Unreal Engine 5.8
* Android target
* A Cafe Bazaar developer account and application
* Products configured in the Cafe Bazaar developer panel

> **Important:** This plugin is intended for Android builds distributed through Cafe Bazaar. It does not provide billing support for other platforms or stores.

---

## Installation

1. Copy the `CafeBazaarBilling` folder into your project's `Plugins` directory.

```text
YourProject/
└── Plugins/
    └── CafeBazaarBilling/
```

2. Open the project in Unreal Engine and enable **Cafe Bazaar Billing** from the Plugins window.

3. Restart the editor if Unreal asks you to do so.

4. Make sure your Android project is configured for packaging.

5. Define your in-app products and subscriptions in the Cafe Bazaar developer panel.

6. Get your application's **RSA public key** from the Cafe Bazaar developer panel. You will need it when opening the billing connection.

7. Package and test the game on a real Android device.

> **Do not test the complete purchase flow only in the Unreal Editor.** Billing is an Android feature and the real purchase flow requires a device with Cafe Bazaar available.

---

## Blueprint Setup

The main entry point is:

**Get Game Instance Subsystem → Cafe Bazaar Billing Subsystem**

A typical flow is:

```text
Game Start
    ↓
Get Cafe Bazaar Billing Subsystem
    ↓
Open Connection
    ↓
On Connection Succeeded
    ↓
Purchase / Subscribe / Query
    ↓
Handle Result
    ↓
Consume Purchase (consumables only)
```

Bind to the relevant events before starting an operation so that your Blueprint is ready to receive the result.

---

# Blueprint Nodes

## Open Connection

**Purpose:** Starts the billing connection.

### Input

* **RSA Public Key**
  Your application's RSA public key from the Cafe Bazaar developer panel.

### Events

* `On Connection Succeeded`
* `On Connection Failed`
* `On Disconnected`

### Important

Do not start purchases before the connection succeeds.

The RSA public key is not a secret credential. However, it should still be configured carefully and should not be exposed through unnecessary player-facing UI.

---

## Close Connection

**Purpose:** Closes the current billing connection.

Use this when the billing system is no longer needed.

You normally do not need to repeatedly open and close the connection around every purchase.

---

## Launch Purchase Flow

**Purpose:** Opens the Cafe Bazaar purchase screen for a one-time product.

### Input

* **Product Id**
  The SKU configured in Cafe Bazaar.

* **Payload**
  Optional application-defined data associated with the purchase.

### Result

`On Purchase Finished`

Use this for normal one-time in-app products.

---

## Launch Subscribe Flow

**Purpose:** Opens the Cafe Bazaar subscription purchase screen.

### Input

* **Product Id**
* **Payload**

### Result

`On Purchase Finished`

Subscriptions use the same purchase result event as one-time purchases.

> **Do not consume subscription purchases.** Consumption is intended for consumable products.

---

## Consume Purchase

**Purpose:** Marks a consumable purchase as consumed so that the same product can be purchased again.

### Input

* **Purchase Token**

Get the token from:

`Purchase → Purchase Token`

### Result

`On Consume Finished`

### Recommended order

```text
Purchase Succeeded
      ↓
Verify / Validate Purchase
      ↓
Grant Item
      ↓
Consume Purchase
```

Do not consume a purchase before you have safely handled the item delivery.

---

## Query Purchased Products

**Purpose:** Retrieves non-subscription purchases currently associated with the user's account.

### Result

`On Query Purchases Finished`

This is useful when:

* The game starts again after being closed.
* The application reconnects to Cafe Bazaar.
* A previous purchase flow was interrupted.
* You need to restore previously purchased items.

> **Do not assume that a successful purchase callback is the only way a purchase can be discovered.**

---

## Query Subscribed Products

**Purpose:** Retrieves active subscriptions associated with the user's account.

### Result

`On Query Purchases Finished`

Use the returned purchase information to determine which subscriptions the player currently owns.

---

## Reset Stuck Flags

**Purpose:** Manual recovery for an operation that remains marked as in progress after its result was lost.

This should **not** be part of your normal purchase flow.

Use it only when your game has detected that an operation is stuck and no result is expected.

> **Warning:** This does not cancel a purchase that may still be active on the Cafe Bazaar side. A transaction could still complete after you reset the local state. Query purchases again when recovering from an interrupted purchase flow.

---

# Events

## On Connection Succeeded

Called when the billing connection is ready.

Start purchase and query operations after this event.

---

## On Connection Failed

Returns:

* **Success:** `false`
* **Error Message:** Description of the failure

Treat connection failure as a recoverable state and avoid starting billing operations until a new connection has succeeded.

---

## On Disconnected

Called when the billing connection is lost.

Your game should treat the billing subsystem as disconnected and reconnect before starting new billing operations.

---

## On Purchase Finished

Returns:

* **Success**
* **Message**
* **Purchase**

The `Purchase` structure contains:

| Field            | Description                            |
| ---------------- | -------------------------------------- |
| `Product Id`     | Purchased product SKU                  |
| `Order Id`       | Cafe Bazaar order identifier           |
| `Purchase Token` | Token used for consumption             |
| `Payload`        | Payload associated with the purchase   |
| `Package Name`   | Application package name               |
| `Purchase State` | Purchase status                        |
| `Purchase Time`  | Purchase timestamp                     |
| `Original Json`  | Original purchase data                 |
| `Data Signature` | Signature associated with the purchase |

> **Do not grant valuable items solely because the client reports a successful purchase.** For currencies, premium items, subscriptions, or other valuable content, validate the purchase on your backend whenever possible.

---

## On Consume Finished

Returns:

* **Success**
* **Message**
* **Purchase Token**

Use the result to determine whether the consumption request completed successfully.

---

## On Query Purchases Finished

Returns:

* **Success**
* **Message**
* **Purchases**

The `Purchases` array contains the purchases returned by the query.

---

# Connection State

The subsystem exposes:

### Get Connection State

Returns one of:

* `Disconnected`
* `Connecting`
* `Connected`

### Is Connected

Returns `true` when the billing connection is ready.

### Is Platform Supported

Returns whether the current platform supports this billing integration.

### Is Purchase In Flight

Returns `true` while a purchase or subscription flow is being processed.

Use this to prevent accidental double-taps and duplicate purchase requests.

### Is Query In Flight

Returns `true` while a purchase query is being processed.

---

# Recommended Purchase Architecture

For a consumable item:

```text
Start Game
    ↓
Open Connection
    ↓
On Connection Succeeded
    ↓
Query Purchased Products
    ↓
Restore / Process Pending Purchases
    ↓
Player Requests Purchase
    ↓
Launch Purchase Flow
    ↓
On Purchase Finished
    ↓
Validate Purchase
    ↓
Grant Item
    ↓
Consume Purchase
    ↓
On Consume Finished
```

For valuable purchases, the server should be the final authority for entitlement.

The client can initiate a purchase and receive the purchase information, but your backend should verify the purchase data before permanently granting valuable content.

---

# Important Notes

### 1. Use Real Product IDs

The `Product Id` passed to the plugin must exactly match the product configured in your Cafe Bazaar application.

### 2. Test on a Real Device

The complete billing flow depends on Android and Cafe Bazaar. Editor testing cannot replace device testing.

### 3. Test Failure Cases

Do not test only the successful purchase flow.

At minimum, test:

* Successful connection
* Failed connection
* Successful purchase
* Cancelled purchase
* Failed purchase
* Consumable purchase
* Consumption
* Purchase restoration
* Subscription restoration
* Closing and reopening the game
* Losing the network connection
* Leaving the payment screen unexpectedly

### 4. Restore Purchases After Interruptions

A payment flow can be interrupted by the operating system, the application being terminated, or another lifecycle event.

After reconnecting, use the appropriate query node to check the user's current purchases.

Do not assume that a missing callback means the user was not charged.

### 5. Do Not Consume Non-Consumable Products

Consumption is for products that should become purchasable again, such as coins or other consumable items.

Do not consume permanent purchases or subscriptions unless the Cafe Bazaar product type explicitly requires it.

### 6. Validate Valuable Purchases on a Server

The plugin exposes:

* `Original Json`
* `Data Signature`

These values can be sent to your backend for server-side verification.

Do not put server secret credentials inside the Android client.

### 7. Prevent Duplicate Delivery

Your backend or local entitlement system should be designed so that the same purchase cannot grant the same valuable item multiple times.

### 8. Keep Billing State Separate From Gameplay State

Do not make your entire inventory system depend directly on a billing callback.

A better architecture is:

```text
Billing
   ↓
Validation
   ↓
Entitlement / Inventory
   ↓
Gameplay
```

This makes purchase restoration and recovery easier.

---

# Troubleshooting

## The Purchase Nodes Do Nothing

Check:

1. The game is running on Android.
2. The billing connection succeeded.
3. The product ID exactly matches the Cafe Bazaar product.
4. Cafe Bazaar is installed and available on the test device.
5. The Android package/application configuration matches the application registered in Cafe Bazaar.

## A Purchase Succeeds but the Item Is Missing After Restarting

Do not rely only on `On Purchase Finished`.

On startup:

1. Connect to Cafe Bazaar.
2. Query the user's purchases.
3. Validate the returned purchases.
4. Restore the appropriate entitlements.
5. Consume consumable purchases when appropriate.

## The Purchase Button Becomes Stuck

First make sure your game is not launching multiple billing operations simultaneously.

If the result was genuinely lost because of an interrupted Android lifecycle, `Reset Stuck Flags` can be used as a recovery mechanism.

After recovery, query purchases again. Do not assume that resetting the local state means the transaction never happened.

---

# Shipping Checklist

* [ ] Use the correct Cafe Bazaar application package name.
* [ ] Use the correct RSA public key.
* [ ] Verify every Product ID.
* [ ] Test one-time purchases on a real device.
* [ ] Test subscriptions if your game uses them.
* [ ] Test consumption for consumable products.
* [ ] Test purchase restoration after restarting the game.
* [ ] Test cancelled and failed purchases.
* [ ] Test interrupted payment flows.
* [ ] Make sure valuable purchases are verified before permanent delivery.
* [ ] Make sure duplicate purchase delivery is prevented.
* [ ] Test the final packaged build, not only the Unreal Editor.

---

## Support

This plugin is designed to keep the Unreal-facing API simple: connect, purchase, query, consume, and handle the results.

For Cafe Bazaar account, product configuration, or merchant-side issues, refer to the Cafe Bazaar developer documentation and dashboard.
