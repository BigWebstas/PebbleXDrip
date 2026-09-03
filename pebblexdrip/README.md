# xDrip CGM (Pebble quick-launch app)

A glucose viewer you open from a quick-launch button. It is **not** a
watchface, so you can run any watchface and still get a CGM glance on demand.

## How data flows

```
xDrip+  --local web server-->  PebbleKit JS (phone)  --AppMessage-->  this app
        http://127.0.0.1:17580/sgv.json?count=24
```

The phone JS polls xDrip+'s Nightscout-style `/sgv.json` endpoint (newest
first). Delta is the gap to the previous reading. The watch asks for a fresh
pull on launch, on SELECT press, and once a minute while open.

## One-time xDrip+ setup

1. xDrip+ > Settings > Inter-app settings > enable **xDrip Web Service**.
2. Leave it on the default port `17580` (localhost only).

That is also all you need for the passive half: xDrip+'s glucose
notification is mirrored to the watch by the Pebble app regardless of
watchface. Turn on Settings > Notifications > "Notification shows BG" plus
trend/delta for a always-there status line.

## Build and install

```
pebble build
pebble install --phone <phone-ip>      # or --emulator basalt
```

## Assign the quick-launch button

Pebble phone app > your watch > Quick Launch > pick a button > **xDrip CGM**.
Long-press that button from any watchface to open it.

## Display

- Big number: mg/dL, green in range, red low, yellow high, grey when stale (>12 min).
- Arrow: xDrip trend, 1..7. `?` when unknown.
- Line: signed delta and minutes since the reading.
- Graph: last ~2 h, target band 70-180 shaded.

Targets: basalt, chalk, diorite. For mmol/L, change the `snprintf` of
`s_sgv` in `src/c/pebblexdrip.c` and the band bounds in `draw_graph`.
