# Changelog

All notable changes to the Hackintosh-TouchID fingerprint authentication project are documented here.

---
## Unreleased

- UPEK/AuthenTec TouchStrip (`147e:2016`): Enroll `[1]` and Verify `[2]` now work with this sensor. They use the same swipe capture, minutiae extraction and template volume as the VFS5011, on top of the capture backend a tester confirmed on real hardware. Deploy `[3]` (the lock screen service) is not available for UPEK yet and says so. The `[U]` Test Capture item stays for now.

---
## v2.0.2 | November 11th 2026

- Version fix: the project version is now 2.0.2 everywhere it is shown (`VERSION.txt`, `vfs5011_proto.h` which the client and daemon are built with, the README and the website), so the client banner, About, Diagnose and the daemon check all agree. The banner also shows the branch the client tracks, next to the version and build.
- New menu layout: a status checklist with a dot per item (green is good, yellow needs attention) on top, then a two-column menu (Authenticate, Sensor Tools, System, Help and More). The logo stays at the top of every screen, at launch, after an action and in Settings. Settings uses the same style, and the prompts are now `Choice:` and `Settings:`. When the output is not a terminal, the dots and lines fall back to plain ASCII.
- Settings `[7]` is now Switch Branch (it replaces Toggle Beta Updates). The branch list is fetched from GitHub each time (through `git ls-remote`, then the GitHub API, then just main and active-development as a fallback), so a new branch shows up without a client update. A branch is only saved if its `VERSION.txt` can be read, and the client offers to install it right away, since the newest build on another branch is not always newer than what you have.
- `[CM]` Show Recent Commits uses the same branch picker, so you can see the last 5 commits on any branch.
- After an update installs, the client now pins `BRANCH=` in `VERSION.txt` to the branch the update came from, so a branch that ships another branch's `VERSION.txt` can no longer move the client back to main.
- README: the sensor table now lists ELAN `04f3:0c6e` (capture test on the `elan-0c6e` branch), EGIS `1c7a:05a1` (issue #15) and ELAN WBF `04f3:0c4c` (issue #16), and the credits thank @s17765152890-gif for the ELAN capture test.

---
## v2.0.0 | November 10th 2026

- New sensor, fully supported: Prometheus 97 (`138a:0097`), confirmed working on a ThinkPad X1C5. Pairing, calibration, Enroll, Verify, lock screen unlock, padlock unlock and the Settings pane all work. It shares the Metallica MIS driver, so `[P]` Pair, `--enroll-test`, `--verify-test`, `--mmis-connect` and `--mmis-calibrate` apply to it too.
- Fixed 0097 calibration failing with `average() failed for blank image`. The sensor starts streaming image lines as soon as the capture command is accepted, and the first lines were lost because the read was posted afterwards. The client now posts the read first and rebuilds the frames from the line counters, so a few missing lines no longer fail calibration. If calibration still fails, the raw capture is written to `/tmp/htid-calibrate-raw.bin` and `/tmp/htid-calibrate-info.txt` for diagnosis.
- Fixed a 30 second wait, with the sensor light staying off, when a touch's finger-down event was dropped by the sensor. The capture now continues from the capture-complete event.
- New authentication cards in the menu bar app (v1.4.1): a card at the top middle of the screen, above all apps and with no notification permission needed. States: Waiting for fingerprint (only while a prompt needs a finger), Authenticated, Not recognized, Incomplete swipe, Cancelled, and a lockout card. A failed or weak attempt returns to Waiting. New `swipe_cancelled` daemon event and `--overlay-demo` launch argument to preview every card.
- The authentication cards now also show on the lock screen. The card window is moved into a private SkyLight space above the login window, using the technique of the MIT licensed SkyLightWindow package (symbols are loaded at runtime). Banner wording is sensor-neutral instead of "swipe". `defaults write com.vfs5011.hackintosh.menubar OverlayOnLockScreen -bool NO` turns the lock screen card off, and Notification Center banners are used only if the SkyLight symbols are missing.
- Fixed the "Waiting for fingerprint" card flashing back right after Authenticated, because the daemon re-arms right after a success. The menu bar app now ignores new episodes within 4 seconds of a success or lockout.
- Fixed "Cancelled" being reported right after a correct match, because typing the password unlocks the screen before the success event was sent.
- Metallica MIS agent: sleep and wake handling. The sensor session is closed before the Mac sleeps and dropped on wake, the agent waits 2.5 seconds for the USB bus to come back, and a lock screen episode now keeps retrying to open the sensor instead of giving up after 3 attempts. This addresses the sensor not waking for lock screen authentication after a long idle period (reported on a ThinkPad X1C5, awaiting tester confirmation). New log lines: "System going to sleep", "System woke up" and "Sensor session dropped because of sleep or wake".
- Settings pane: Prometheus 97 (`138a:0097`) no longer shows as "not yet supported", and the Fingerprint authentication text no longer assumes a swipe sensor.
- Client: Settings `[CU]` Check for Updates Now opens its own page, "Checking for Updates on <branch>" with a spinner, and keeps the result (up to date, no connection, or the update prompt) on screen until Return, so it no longer looks like the menu just redrew.
- Client: the decorative boot log (the fake kernel lines and timestamps) is gone. The real startup checks still print, and after a clean boot the screen is cleared and the menu is drawn. If a check prints a warning or an error, the screen is left alone. `--q` / `--quiet` keep the plain startup.
- MacPorts support: the build scripts and the client's dependency check now also work with MacPorts (`/opt/local`), so Homebrew is no longer required. `prep_and_build.sh` offers `sudo port install` when only MacPorts is found.
- New sensor: UPEK/AuthenTec TouchStrip (`147e:2016`), experimental, capture test only for now.
- New sensor: Synaptics Metallica MIS, experimental. A native port of python-validity (pairing, secure session, calibration, enroll and verify) now works on real hardware. `[1]` Enroll now enrolls this sensor straight from the menu: it asks before replacing prints already stored on the sensor, sets up a never-paired sensor by itself, and keeps diagnostics in `/var/log/htid-metallica-enroll.log`. If the sensor was paired on Windows or Linux, the client asks once for the laptop's real product name and serial (with hints on where to find them) and remembers them. The sensor light now comes back on before every touch. `--enroll-test`, `--verify-test`, `--list-records`, `--wipe-records`, `--host-product`, `--host-serial` and `--debug` remain for troubleshooting. Fully working on `06cb:009a` (confirmed on a ThinkPad X1C6): Enroll, Verify, lock screen and padlock unlock. `138a:0097` is now supported (see above). `138a:009d` is detected but still in testing.
- Universal client: the project is now Hackintosh-TouchID, `hack-touchid` is added to your PATH, and the template store volume is now `HackTouchIDStore`.
- Sensor-aware setup: `prep_and_build.sh` detects your sensor and installs only what it needs.
- `[1]` Enroll now announces the detected sensor ("Declaring sensor type...") and stops with a clear message for sensors that are capture test only.
- Self-updater: checks at launch and while the menu is open, `[CU]` checks on demand, with `[A]` changelog / `[Y]` install / `[N]` cancel, progress bars, and the `--check-updates` and `--menu-updater` arguments. The menu bar's Update Client button now redeploys the daemon for you after installing.
- Menu bar app: client update notifications with an "Update Client" button and "Reinstall Daemon" health alerts.
- Swipe to Lock (macOS Tahoe only): one swipe locks the screen, with adjustable sensitivity (`[L]`).
- Touch to Lock (macOS Tahoe only, Metallica MIS): a touch from an enrolled finger locks the screen. Confirmed on `06cb:009a`.
- Restore Red Light on wrong finger for VFS5011: the sensor LED now flashes red on an unrecognized finger and the ready light comes back by itself. Brightness, red on/off, the red LED port and a steady ready light are set from Settings `[LED]` in the client.
- Client: new `Login Password` line on the status screen (Stored, Not stored or Unknown). An empty `password.txt` now counts as not stored, and Deploy always reports the result.
- Fixed the template volume staying unlocked but unmounted after a lock, which made the next swipes fail and the daemon go idle.
- Lockout after 4 failed swipes, plus a "swipe was too weak" notification.
- Sensor yield: Enroll, Verify and View Fingerprint work while Swipe to Lock is on.
- New menu items: `[T]` Sensor Test, `[D]` Diagnose (`--diag-pid`), `[X]` Uninstall for any sensor, and a regrouped Settings menu.
- New launch arguments: `--deploy-agent` and `--q` / `--quiet`.
- macOS 14 Sonoma is now the minimum, and `MATCH_THRESHOLD` went from 20 to 40.
- Project website (GitHub Pages) with a guide and the live changelog.

---
## v1.0.5 — August 18th, 2026
- Daemon/client version sync (shared version macro, `--version` flag, client blocks launch on mismatch).
- Added a sensor-presence gate on client startup.
- Moved daemon install path to avoid a recurring space-in-path quoting bug.
- Confirmed working end-to-end on real hardware.

---
## v1.0.4 — August 16th, 2026
- Lowered macOS floor from Sequoia 15 to Ventura 13 across the whole stack; fixed the menu bar app's actual version-floor bug (missing `swiftc -target` flag).
- Fixed a false-notification bug: the daemon would fire a swipe prompt (and automatic failure) on every lock even with no sensor attached.
- Investigated and closed pre-login (login-window) fingerprint auth — architecturally blocked on both approaches tried; decision is to not pursue it further.

---
## v1.0.3 — August 15th, 2026
- Fixed a 3-5 second delay between screen-lock and the swipe prompt appearing (template loading moved off the critical path onto a background thread).
- Added GitHub Actions CI (5 jobs, all green).
- Built the Swift/AppKit menu bar companion app.

---
## v1.0.2 — August 13th, 2026
- Confirmed Passwords.app and Keychain Access as valid auth surfaces.
- Added an OpenCore minimum-version warning gate.

---
## v1.0.1 - August 10th, 2026
- Fixed multi-finger support (daemon now reads all templates in `fingers/`, not just one).

---
## v1.0.0 — Initial release | August 5th 2026
- Built the full capture + matching pipeline from scratch (USB enumeration, sensor init handshake, swipe capture, NBIS minutiae matching).
- Enrollment/matching thresholds set; deployed as a LaunchAgent + NOPASSWD sudoers rule.
- Confirmed working end-to-end on real hardware.
- Template storage on an encrypted APFS volume ("VFSStore"), passphrase in the system keychain.
- Licensed BSD 3-Clause, with credit to NIST/NBIS and to Arseniy Lartsev + AceLan Kao for the original libfprint VFS5011 driver.
