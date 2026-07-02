# dlms losss

**Digital Loudspeaker Management System — Android App**
Developed by **mas ari**

---

## What It Does

Real-time Android DSP engine for 3-way loudspeaker management:

| Feature | Detail |
|---|---|
| Crossover | Linkwitz-Riley 24 dB/oct, 3-way (Low / Mid / High) |
| Parametric EQ | 8 bands per output channel (Peaking, Lo-Shelf, Hi-Shelf) |
| Output channels | 6 (Low L/R, Mid L/R, High L/R) → USB audio interface |
| Alignment delay | 0–20 ms per channel via ring buffer |
| Look-ahead limiter | Per-channel brickwall with adjustable threshold/attack/release |
| Preset import | dbx `.dwp` binary preset files via Android file picker |
| Audio engine | Google Oboe (low-latency, exclusive, 48 kHz) |

---

## Project Structure

```
dlms-losss/
├── app/
│   └── src/main/
│       ├── cpp/
│       │   ├── CMakeLists.txt      ← Oboe fetch + library config
│       │   ├── BiquadFilter.h/cpp  ← Audio EQ Cookbook biquad implementation
│       │   ├── DlmsEngine.h/cpp    ← Main DSP chain
│       │   ├── AudioEngine.h/cpp   ← Oboe full-duplex stream
│       │   ├── DwpParser.h/cpp     ← dbx .dwp binary preset parser
│       │   └── native-lib.cpp      ← JNI bridge
│       └── java/com/masari/dlmslosss/
│           ├── DlmsNativeInterface.kt  ← JNI declarations
│           ├── MainViewModel.kt        ← Business logic / LiveData
│           └── MainActivity.kt         ← Dark-themed UI
├── .github/workflows/android_build.yml ← CI/CD
└── README.md
```

---

## Requirements

| Tool | Minimum version |
|---|---|
| Android Studio | Hedgehog (2023.1+) |
| Android NDK | r26 (26.3.x) |
| CMake | 3.22.1 |
| minSdk | 26 (Android 8.0) |
| compileSdk | 34 |
| JDK | 17 |

> Oboe is fetched automatically by CMake (`FetchContent`) — no manual download needed.

---

## Build in Android Studio

1. Open `dlms-losss/` as an Android Studio project.
2. Android Studio detects the NDK requirement and prompts to install it. Click **Install NDK**.
3. Wait for Gradle sync to complete (Oboe is fetched from GitHub during CMake configure).
4. Run on a physical device with a USB OTG audio interface for multi-channel output.

---

## Connect Replit → GitHub & Push Code

Run these commands in the Replit shell from the `dlms-losss/` directory:

```bash
cd dlms-losss

# Initialise git (if not yet done)
git init

# Set your identity
git config user.email "you@example.com"
git config user.name "mas ari"

# Add your GitHub repo as remote
git remote add origin https://github.com/boncabr/horeg-fix.git

# Stage and commit all files
git add .
git commit -m "feat: initial dlms losss project structure (mas ari)"

# Push to GitHub (uses GITHUB_PERSONAL_ACCESS_TOKEN from Replit Secrets)
git push -u origin main
```

GitHub Actions will automatically trigger on push and produce a downloadable **debug APK** from the Actions tab of your repository.

---

## CI/CD — GitHub Actions

Workflow file: `.github/workflows/android_build.yml`

Triggers on every push to `main`/`master`:
1. Installs JDK 17, Android SDK 34, NDK r26, CMake 3.22.1
2. Runs `./gradlew assembleDebug`
3. Uploads `app-debug.apk` as a downloadable artifact (retained 30 days)

To enable **release signing**, add these four GitHub repository secrets and uncomment the release block in the workflow:

| Secret key | Value |
|---|---|
| `KEYSTORE_FILE` | Base64-encoded `.jks` keystore |
| `KEY_ALIAS` | Signing key alias |
| `KEY_PASSWORD` | Key password |
| `STORE_PASSWORD` | Keystore password |

---

## Hardware Setup

```
Android phone (USB OTG)
       │
   USB-C OTG cable
       │
   USB Audio Interface (6+ output channels)
       │
  Low L ──► Woofer (Left)
  Low R ──► Woofer (Right)
  Mid L ──► Midrange (Left)
  Mid R ──► Midrange (Right)
  High L ──► Tweeter (Left)
  High R ──► Tweeter (Right)
```

---

## Credits

```
Application : dlms losss
Developer   : mas ari
Engine      : Google Oboe (Apache 2.0)
DSP math    : Audio EQ Cookbook — Robert Bristow-Johnson
```
