/*
 * coex — Wi-Fi/ESP-NOW ↔ BT Classic coexistence tuning.
 *
 * Single 2.4 GHz radio shared via Espressif's PTA arbiter. Default
 * coex preference is "balanced"; we raise BT priority during an
 * active phone call (SCO is unforgiving) and lower it during pure
 * mesh sessions so ESP-NOW gets more airtime.
 *
 * See Espressif issue #5567 for the canonical write-up of the
 * documented A2DP-vs-ESP-NOW dropout problem we are mitigating.
 */

#pragma once

void coex_init(void);

/* Bias the coex arbiter when a call/SCO is active. */
void coex_prefer_bt_call(void);

/* Symmetric default. */
void coex_prefer_balanced(void);

/* Favor ESP-NOW (use when no BT audio is active). */
void coex_prefer_wifi(void);
