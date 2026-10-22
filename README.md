# Ryhze ecosystem

An early source release of three projects maintained by Leo Mishra.

| Project | Purpose | Source |
| --- | --- | --- |
| Ryhze | Entertainment app and website for discovery, library, installation and launch workflows | [Flutter/Dart app](projects/ryhze/lib), [Astro/TypeScript website](projects/ryhze/Website/src) |
| RACE | C++20 game engine, Qt editor and SDL runtime, including Windows D3D12 rendering | [Applications](projects/race/apps), [engine](projects/race/src), [interfaces](projects/race/include) |
| Axeon | Rust bootstrap compiler, language tooling and numerical/runtime libraries | [Compiler](projects/axeon/src), [libraries](projects/axeon/crates), [examples](projects/axeon/examples) |

## How the ecosystem is intended to form

1. Improve each project's code structure, checks and reliability.
2. Qualify the app, engine and language independently, with Windows work first.
3. Explore explicit, tested interfaces between Axeon and RACE; integration is not complete.
4. Connect compatible, authorized creator content to Ryhze's discovery and library workflows.

These are development goals. This snapshot does not establish completed integration,
production readiness, platform acceptance or compatibility between every component.

## Published scope

Published on 22 October 2026: 1224 selected source, test, build-configuration
and licensed font files (606 Ryhze, 401 RACE, 217 Axeon),
plus this release's licence and notices. [The source manifest](source-manifest.json)
records paths and SHA-256 hashes.

This is a fresh source snapshot, not an import of private Git history. The original
working repositories remain private. Tests are included as source; this exported
layout has not been built or tested, and historical private runs are not qualification
of this public snapshot. Public adoption metrics are not yet established.

## Browse and develop

Clone this repository and open the relevant folder under `projects` in your editor.
Dependency versions are recorded in the included manifests and lockfiles.

- **Ryhze app:** inspect `pubspec.yaml`, `lib`, `test` and the platform folders.
  It requires Flutter/Dart matching the SDK constraints in `pubspec.yaml`.
- **Ryhze website:** inspect `Website/package.json`, `src`, `auth`, `hosting` and tests.
  Its Node version and dependencies are specified in that manifest and lockfile.
  `publish_to: none` and the npm `private` flag prevent package-registry publishing;
  they do not override this source licence.
- **RACE:** inspect `CMakeLists.txt`, `CMakePresets.json`, `apps`, `src`, `include`
  and `tests`. It requires CMake 3.28+, a C++20 toolchain and Python; optional desktop
  targets require separately installed Qt/SDL dependencies. Linux presets select Clang.
- **Axeon:** inspect `Cargo.toml`, `rust-toolchain.toml`, `src`, `crates`, `runtime`
  and examples. The pinned Rust toolchain is 1.85.0; native compilation also needs
  the external compiler/LLVM environment expected by the backend.

### Snapshot limitations

Brand artwork/icons, unreviewed media, golden screenshots, private operational docs,
run evidence, credentials, build outputs, installers and CI workflows are omitted.
Private owner-review/agreement components and five key-shaped website fixture files
are withheld pending a separate publication review. Windows-validation helpers tied
to the private host are omitted. Personal machine paths and owner contact/coordination
identifiers in retained files use public placeholders. These changes affect the
export only; original working repositories were not edited.

Some source/configuration/test references therefore point to omitted assets or private
validation inputs. Supply assets you have rights to and configure your own environment
before attempting those targets. The published subset is not a turnkey app package,
complete validation harness or deployment recipe. No secrets, private Git history or
private evidence are intentionally published.

## Licence

First-party source and this documentation are available under the [MIT licence](LICENSE).
Bundled fonts retain their SIL Open Font License notices; Flutter-derived scaffolding
and external dependencies retain upstream terms. See [NOTICE](NOTICE). This licence
choice does not grant rights to unbundled third-party content or trademarks.

## Maintenance

Changes use bounded tasks, one source writer and proportionate checks and review.
This is a new pre-release public snapshot; no download, user-count, performance or
program-eligibility claims are made. Issues and pull requests can discuss the published
source; do not include credentials, private logs or personal information.
