# MLBB ESP Pure

External ESP for Mobile Legends — **100% native C++** (no Kotlin UI).

- Finds `com.mobile.legends:UnityKillsMe` automatically.
- Reads game memory with `process_vm_readv` (root / same privileges as before).
- Resolves `BattleManager` + entity dicts using the same field offsets as LibTool / conRent.
- ImGui mod menu + radar ESP. **No resolve log spam.**

## Build

```bash
./gradlew assembleDebug
```

APK: `app/build/outputs/apk/debug/app-debug.apk`

## Usage

1. Start MLBB (GSpace / normal), enter a match.
2. Open **MLBB ESP**, grant root if your setup requires it for cross-process read.
3. **Auto find** attaches to UnityKillsMe; tap **Resolve** once if status says so.
4. Radar on the right; toggles in the menu.
