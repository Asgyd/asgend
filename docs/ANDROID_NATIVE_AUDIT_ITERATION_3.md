# ASGEND Native Android — аудит итерация 3/3: UI, Android 17 и release

**Дата:** 2026-10-25  
**Фокус:** Material 3 Expressive, adaptive layout, accessibility, target API 37, CI/test evidence.  
**База:** Iteration 1/2 reports plus direct re-check of source files and official Android Developers docs.

## Проверки третьего прохода

- `python3 clients/asgend_android/tools/verify_native_source.py` — **PASS**.
- Android XML — **6/6 parsed successfully** via Python `xml.etree.ElementTree`.
- Standalone `CoreSettings.kt` — Kotlin compiler result **PASS**.
- `src/androidTest` — **ABSENT**.
- `gradle` — **NOT INSTALLED**; Android SDK platform `android-37` — **NOT AVAILABLE**.
- Therefore actual Compose compile, JUnit, APK build, UI/instrumentation and emulator runs were not executed.
- Official version check: Compose Material 3 `1.4.0` is listed as stable in AndroidX release notes updated 2026-10-21; AGP 9.4 requires Gradle 9.6.0 and the repository pins AGP 9.4.1/Gradle 9.6.0. API 37/Android 17 has both all-app and target-specific behavior change pages; passing `targetSdk=37` alone does not prove runtime compatibility.

## Additional findings

### ANDR-013 — expanded layout does not implement the decomposed list-detail/multi-pane workspace

**Priority: P2 · OPEN.**

`AsgendApp.kt` selects NavigationRail at `maxWidth >= 640.dp`, but `DestinationBody` remains the same single destination screen. The desktop-like TUI left task list + center timeline + right Files/Plan model does not become a responsive list-detail or multi-pane workspace on tablets/landscape; the wide UI changes navigation chrome, not workspace composition.

**Acceptance:** define Compact/Medium/Expanded window behavior using actual window measurements; compact: task list → detail navigation with predictable back; expanded: list/detail split and optional files/plan pane where width permits. Add tests for resizing/split screen and not relying on orientation alone.

### ANDR-014 — M3 Expressive and accessibility acceptance is not proven

**Priority: P2 · OPEN.**

The theme (`AsgendTheme.kt`) uses Material 3 `1.4.0`, dynamic/fallback color schemes and custom shape scale. Screens primarily use standard Scaffold/NavigationBar/NavigationRail/Card/Button/TextField components. This is a valid Material 3 baseline, but source inspection alone does not establish visual use of expressive motion/components or accessibility. No `androidTest` directory and no Compose semantics/UI tests were found. Many icons are correctly decorative (`contentDescription=null` adjacent to text), but no TalkBack traversal/announcement test is present.

**Acceptance:** device/UI run on dynamic and fallback palettes, light/dark; verify status is never color-only; TalkBack, font scale ≥ 1.3/2.0, 48dp touch target, keyboard/switch navigation, IME/rotation, reduced motion, progress/event announcements and semantics actions. Document manual acceptance results; add Compose UI tests for critical journeys.

### ANDR-015 — API 37 target behavior is not verified on Android 17

**Priority: P1 release gate · BLOCKED/NOT RUN.**

Gradle declares `compileSdk=37` and `targetSdk=37`, but the current environment has no Android SDK/emulator and no Gradle. Official Android 17 docs publish changes for all apps and for apps targeting API 37+. There is no current evidence that this binary starts and passes UI/network/IME/back/edge-to-edge tests on API 37.

**Acceptance:** CI/emulator run on API 37 for cold start, process recreation, compact/expanded layout, insets/edge-to-edge, IME during task/answer/file edit, predictive back, dynamic theme, large response handling and remote error states; record API/emulator image and result.

### ANDR-016 — user-visible strings are hard-coded in Kotlin

**Priority: P3 · OPEN.**

Main screens and view models contain literal English text rather than string resources. This weakens locale support, consistency, automated copy review and accessibility wording changes. It is not a runtime/security defect, but it increases maintenance cost and makes Russian/English parity hard to audit.

**Acceptance:** move visible labels/errors/dialog messages to `strings.xml`, use plural resources for counts, add at least English + Russian resources if both are product requirements, and test locale/font scaling.

### ANDR-017 — source verifier is a guardrail, not semantic/build verification

**Priority: P2 · OPEN as test strategy.**

`verify_native_source.py` uses text-pattern assertions against sources and test names. It is useful for catching a missing route/guard or a documentation regression, but it can pass while a Compose screen fails to compile, a branch is unreachable, or a test source is present but never executed. The current PASS must not be interpreted as Kotlin/Compose compile or API37 behavior proof.

**Acceptance:** keep the script as a fast preflight, but make real `testDebugUnitTest`, `assembleDebug`, `assembleRelease` required CI steps; add emulator/instrumentation job and upload results. Backend contract paths should trigger the same workflow (ANDR-005).

### ANDR-018 — feature files have large responsibilities, making auditability harder

**Priority: P3 · OPEN.**

The largest UI/state files are `NativeAppViewModel.kt` (~681 lines), `RuntimeScreen.kt` (~598), `ControlPlaneClient.kt` (~461), `NativeRemoteViewModel.kt` (~439), `TaskWorkspaceScreen.kt` (~362) and `AsgendApp.kt` (~367). Counts are not inherently defects, but networking, mapping, state orchestration and UI composition are coupled enough to make future compile failures and contract reviews costly.

**Acceptance:** incrementally split core/remote state models and repository operations from ViewModels; split Runtime sections; centralize typed API envelopes/response schema checks; preserve contract tests while extracting.

## Final iteration verdict

UI architecture is a reasonable single-activity Compose foundation with compact bottom navigation, expanded rail, dynamic/fallback palette and several task-specific screens. It is not yet at the acceptance level specified in the decomposition: wide-window multi-pane, actual accessibility, API37 runtime and UI automation remain unverified. A source-only audit cannot substitute for device evidence.

**Iteration 3/3 complete.** Updated consolidated audit/decomposition/TODO now separate implementation presence, static/contract evidence, Android build evidence and device acceptance. No independent external subagent assessment is claimed.


**Artifact note:** pre-existing `dist/asgend-1.0.0/asgend-android-1.0.0-unsigned.apk` is described in release notes as the Flutter Android client. It is not an output produced by the Kotlin module and must not be used as evidence for native APK build acceptance.
