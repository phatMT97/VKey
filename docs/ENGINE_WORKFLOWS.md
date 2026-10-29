# Engine Workflows

## Short version

```powershell
.\vkey.cmd local              # Release + engine synced from local VKey-rs
.\vkey.cmd local -Engine Released
                              # Release + published, signed engine
.\vkey.cmd local -DebugBuild  # ... Debug, which serves the Sciter UI from files
```

Use `.\vkey.cmd`, not `.\vkey.ps1`. PowerShell does not run commands from the
current directory, so the `.\` is required, and naming the `.cmd` avoids it
picking the `.ps1` sitting beside it. PowerShell refuses unsigned scripts, and this
repository usually sits on a mapped WSL drive, which Windows treats as remote —
so even `RemoteSigned` blocks it. `vkey.cmd` is a batch file, which the execution
policy does not apply to, and it starts PowerShell with the policy bypassed for
that one process. Nothing about the machine changes.

`-DebugBuild` builds Debug, where CMake copies the Sciter UI beside the exe
instead of embedding it with packfolder. Editing HTML or CSS then needs the app
restarted, not rebuilt. (`-Debug` is a PowerShell common parameter, hence the
name.)

Only the published-engine workflow needs a token. Set it once, then open a new
terminal:

```powershell
setx VKEY_ENGINE_TOKEN "<token>"
```

That is the whole day-to-day. Everything below explains what those modes do and how to
drive the pieces yourself when something needs unpicking.

---

The prebuilt engine is no longer committed — `extern/vkey_engine/lib/` is
gitignored — so every path below either fetches it or does not need it.

## One-time setup

The engine release lives in the private `phatMT97/VKey-rs`, so fetching needs a
token. Create a fine-grained PAT scoped to that one repository with
**Contents: Read-only**, then:

```powershell
setx VKEY_ENGINE_TOKEN "<token>"     # persists; open a new terminal after
```

```bash
export VKEY_ENGINE_TOKEN=<token>     # add to your shell profile
```

Fine-grained tokens expire. When yours does, the fetch fails during a build and
looks like a build problem — the message names the token, so read it.

---

## 1. Build and run locally

`local` means the engine from the adjacent `VKey-rs` source checkout. Sync it
first in WSL:

```bash
cd ~/code/VKey-rs
bash tools/sync_nexuskey_engine.sh
```

Then build the optimized app from PowerShell:

```powershell
cd Z:\home\phatmt\code\NexusKey
.\vkey.cmd local
```

This is still a Release build. Because a locally built engine has no release
signature, this command explicitly enables exact-hash development trust: the DLL
must match `build-engine/engine.lock`. That opt-in is off by default in CMake and
is never used by official builds. The command does not call the fetcher and will
not replace the local DLL.

To build locally against the published engine instead:

```powershell
.\vkey.cmd local -Engine Released
```

That mode fetches the signed engine once and needs `VKEY_ENGINE_TOKEN`.

### Driving CMake directly

Driving CMake yourself is two extra pieces: fetch the engine once, and say you
want it. **A bare `cmake -B build` now builds without the Rust engine** — the
option defaults off so a fork does not need the noncommercial artifact — so a
command that used to include the engine silently stops doing so.

```powershell
.\tools\fetch-engine.ps1                          # once; no-op afterwards
cmake -B build -G "Visual Studio 18 2026" -A x64 -DVKEY_USE_RUST_ENGINE=ON -DVKEY_ENGINE_ROOT=build-engine
cmake --build build --config Release
```

For an unsigned local engine in an optimized build, make the trust opt-in
explicit:

```powershell
cmake -B build -G "Visual Studio 18 2026" -A x64 `
  -DVKEY_USE_RUST_ENGINE=ON `
  -DVKEY_ENGINE_ROOT=build-engine `
  -DVKEY_ALLOW_UNSIGNED_LOCAL_ENGINE=ON
cmake --build build --config Release
```

Only the configure line needs the two flags; rebuilds are unchanged because the
cache remembers them. You only revisit this after deleting `CMakeCache.txt`.

When testing behavior added to a locally built engine before its release is
pinned, configure with `-DVKEY_TEST_UNRELEASED_ENGINE=ON` as well. This enables
adapter tests for those new contracts, including commit-time restoration in
issue #272. Leave it off when testing the published engine pin; the engine's
own regression and C ABI tests cover the new behavior in its source repository.

Working on something unrelated to the engine? Skip it entirely and skip the token
with it:

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64      # engine off by default
```

CMake warns when it configures this way, because `RustInputEngineTest` and half
of `EngineFactoryTest` are inside `#ifdef VKEY_USE_RUST_ENGINE` — a green `ctest`
from that build does not cover the Rust engine.

## 2. Build for users to test

**Actions → Build → Run workflow.** Nothing to install, no token on your machine.

The defaults build the Sciter edition with tests and the external engine. Sciter
stays unsigned in v4.3. Select `include_classic` to run an independent Rust-OFF
Classic build. Every output from this mixed workflow is deliberately unsigned;
it never submits a SignPath request. Manual runs only upload test artifacts;
tag runs publish both Sciter and C++-only Classic.

For a manual maintainer test, `classic_rust` opts Classic into Rust and labels the
uploaded artifact `DEV`. It requires `include_classic`, is never signed, and tag
builds cannot enable it. `include_engine` controls whether the external engine is
copied into the final manual Sciter artifact; the Sciter compile itself always
uses the pinned engine and therefore still requires the repository secret.

A one-edition manual artifact keeps its flat layout. When both editions are
selected, the artifact uses separate `sciter/` and `classic/` directories so each
edition keeps its own complete TSF, watchdog, browser host, and notices. With
`classic_rust`, the `classic/` directory also carries the pinned engine, detached
signature, and engine license for DEV testing. Official Classic never carries
those Rust files.

For local builds the product choices are explicit:

```powershell
.\vkey.cmd local                         # Sciter + local Rust engine
.\vkey.cmd local -Lite                   # official-style Classic, C++ only
.\vkey.cmd local -Lite -ClassicRust      # non-release Classic + Rust dev variant
.\vkey.cmd local -Lite -ClassicRust -Engine Released
```

The Rust-enabled Classic variant is maintainer test material, not the official
Classic product and must never be submitted to SignPath or redistributed as
`VKeyClassic.zip`. Local outputs are separated as `build-lite-cpp/` and
`build-lite-rust/` so switching variants cannot leave a stale engine DLL beside
the C++-only binary.

## 3. Publish an official release

**Current v4.3 release: Stage A publishes both Sciter and Classic unsigned,
without submitting any package to SignPath.** SignPath declined signing the
standard edition because Sciter is closed-source. Classic-only signing is
awaiting renewed approval; Classic is released unsigned while that review is
pending. Stage B below describes the future procedure after approval, not a
step for this release.
Keep the Classic signing and release gates disabled and do not dispatch either
`review` or `release` in the Foundation workflow for this release.

Both unsigned editions may be warned about, blocked, or quarantined by
Windows SmartScreen or antivirus software. Older clients (v4.2/v4.3.0) require
one manual upgrade to v4.3.1 because they pin SignPath Authenticode. From v4.3.1,
automatic updates verify project-signed ZIP packages instead. Sigstore attestations
verify build provenance but do not replace Authenticode or remove SmartScreen warnings.

The workflows separate the unsigned release from the proposed Classic signing
path so proprietary Sciter/Rust inputs never enter the SignPath Foundation boundary.

### Stage A: unsigned Sciter and Classic release

Before the first production run, create protected environment
`vkey-standard-release`, require reviewers, allow only canonical `v*` tags, and
store a dedicated `VKEY_STANDARD_WINGET_TOKEN` secret there for both WinGet
packages. Leave repository variable
`VKEY_STANDARD_RELEASE_ENABLED` unset until those controls are active, then set
it to exactly `true`.

Push a canonical `vMAJOR.MINOR.PATCH` tag whose version exactly matches
`CMakeLists.txt` and whose commit is the exact current `Main` HEAD. The `Build`
workflow validates the public tree and assets, builds Sciter with the pinned
engine and Classic in a separate Rust-OFF tree, runs each edition's tests and
the Sciter tampered-engine rejection check, then publishes:

- `VKey-x64.zip`, with SHA-256 sidecar and GitHub attestation;
- `VKeyWatchdog.exe` and `VKeyBrowserHost.exe` as separate assets, each with a
  SHA-256 sidecar;
- `VKeyClassic-x64.zip`, with SHA-256 sidecar and GitHub attestation;
  each ZIP contains only `VKeyClassic.exe`, its own `VKeyTSF.dll`,
  and `THIRD_PARTY_NOTICES.txt`;
- `VKeyClassicWatchdog.exe` and `VKeyClassicBrowserHost.exe` as separate assets,
  each with a SHA-256 sidecar;
- `vkey_engine.dll`, its detached signature, both SHA-256 sidecars, and its
  separate license notice.

The engine remains outside all application ZIPs. All four companion executables
also receive GitHub attestations. The workflow creates one GitHub Release for
both unsigned editions and submits both WinGet packages when the dedicated
environment token is configured. Without that token, the WinGet jobs report a
skipped submission; GitHub Release publication still completes. Build/package jobs have
read-only repository permission; only a protected downstream job can attest and
publish their exact artifact ID. It rechecks the tag and `Main` after approval,
creates a draft first, publishes only after every asset is present, and refuses
to overwrite a different existing asset. Publishing requires both edition builds
and packaging to succeed; a failed Classic build cannot leave a Sciter-only
release. It never invokes the Foundation signing workflow. Depending on the
environment rules, GitHub may ask again before the separate least-privilege
WinGet jobs.

### Stage B: protected Classic release (pending renewed approval)

This future path must use a new release tag: signed binaries cannot overwrite
the unsigned Classic assets from v4.3. Before enabling this future path, update
Stage A to leave Classic publication to the Foundation workflow for that new
tag; do not publish unsigned Classic first and then try to replace those assets
with signed files.

Create GitHub environment
`vkeyclassic-foundation-release`, require reviewers, restrict deployment to
`Main`, and store dedicated `VKEYCLASSIC_SIGNPATH_API_TOKEN` and
`VKEYCLASSIC_WINGET_TOKEN` secrets there. Do not reuse the repo-level secret names:
the dedicated names prevent fallback to an unprotected repository secret. Set
repository variable `VKEYCLASSIC_SIGNING_ENABLED=true` only after that environment
is protected; both review and production signing then enter the environment.

When a new review submission is authorized after this release, use
**Actions → VKeyClassic Foundation signing → Run workflow → `review`**.
This operation also sends a package to SignPath; do not run it for the current
release. It builds exactly `VKeyClassic.exe`, `VKeyTSF.dll`, and
`VKeyWatchdog.exe`, submits only that immutable artifact to `test-signing`, and
uploads a 30-day review artifact. The review job has no permission to modify a
GitHub Release and still requires the protected-environment approval.

After approval and after Stage A has completed, dispatch the same workflow from
the `Main` branch with operation `release` and the exact tag. The tag must still
point to that exact `Main` HEAD so GitHub attestation provenance, compiled source,
and release tag all name the same commit. Leave `VKEYCLASSIC_RELEASE_ENABLED`
unset until SignPath approves the exact `vkeyclassic-foundation` configuration
and `release-signing` policy, then set it to exactly `true`. Production then:

1. requires the enable variable and protected environment approval;
2. rebinds the tag, compiled version, commit, and existing published Release;
3. submits only the three-file Foundation artifact to `release-signing`;
4. requires valid `CN=SignPath Foundation` signatures, timestamps, an exact file
   inventory, and no Sciter/Rust imports;
5. keeps `THIRD_PARTY_NOTICES.txt` in the three-file Classic ZIP, while staging
   the signed watchdog and separately built unsigned browser host as independent
   `VKeyClassicWatchdog.exe` and `VKeyClassicBrowserHost.exe` release assets;
6. enforces the three-file ZIP allowlist, then archives, checksums, and attests
   both Classic ZIP names and both companion assets;
7. attaches assets only to the existing tag Release and refuses to overwrite a
   different existing asset, after rechecking the tag and `Main` following final
   approval, then updates the Classic WinGet package.

If any gate is absent, production fails before signing or publishing. A review
artifact can never take the production branch merely by changing its filename.
The signing job has read-only GitHub permission; a later protected job receives
release-write permission only after downloading and rechecking immutable artifact
IDs. Depending on the environment rules, GitHub may ask for separate approvals
for production signing, final attachment, and the least-privilege WinGet job.

## 4. Change the engine itself

The engine source is in VKey-rs. To try a local build of it here, without cutting
a release:

```bash
cd ~/code/VKey-rs
bash tools/sync_nexuskey_engine.sh          # builds mingw into NexusKey/build-engine/
```

Then build against it:

```powershell
.\vkey.cmd local
```

It writes to the gitignored `build-engine/`, including that build's own
`engine.lock`, so nothing tracked changes and there is no committed lock to
restore. The lock in `extern/vkey_engine/` keeps describing the published
release, which is what CI fetches and verifies.

Back to the released engine:

```bash
rm -rf build-engine        # the next build fetches it again
```

Your local build is mingw and the released one is MSVC, so the two have different
hashes. That is expected, and it is exactly why the local build gets its own lock
in its own directory rather than overwriting the committed one.

To switch back without deleting anything, use `-Engine Released`. To make the
default local mode work again afterwards, rerun the sync command.

---

## When something fails

| Message | Cause |
| --- | --- |
| `VKEY_ENGINE_TOKEN is unset or empty` | no token, or it expired |
| `HTTP 404 — wrong tag, or the token cannot read that repository` | tag typo, or the PAT is not scoped to VKey-rs |
| `sha256 mismatch` / `byte length mismatch` | `engine.lock` and `engine.release` disagree; do not edit either file independently |
| `No prebuilt engine at ...` | nothing fetched yet; §1, or build with `-DVKEY_USE_RUST_ENGINE=OFF` |
| `Production Classic release is locked` | keep using `review`; enable production only after the SignPath policy and protected GitHub environment are ready |
| `VKeyClassic SignPath review is locked` | configure the protected environment and its dedicated token, then set `VKEYCLASSIC_SIGNING_ENABLED=true` |
| `Standard production release is locked` | protect `vkey-standard-release` first, then set `VKEY_STANDARD_RELEASE_ENABLED=true` |
| `Classic may attach only to the existing published release` | finish the standard tag release first, then dispatch Classic with that exact tag |

Related: [`../extern/vkey_engine/LICENSE`](../extern/vkey_engine/LICENSE) for why
the engine is noncommercial-only.
