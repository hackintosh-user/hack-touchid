//
//  AppDelegate.swift
//  Hackintosh Touch-ID Menu Bar
//
//  Menu bar companion app for the Hackintosh Touch-ID daemon (multi-
//  sensor as of v1.1 -- was VFS5011-only when this app was first
//  built, hence the internal symbol/IPC-prefix names below still
//  saying "VFS5011"; renamed the user-facing branding only, see the
//  comment at the enum below for why the wire-protocol strings and
//  Swift symbol name were deliberately left alone).
//  Listens for distributed notifications posted by the daemon and
//  surfaces them as user notifications + a status item, since the
//  sensor's LED is sometimes too dim to notice on its own.
//
//  IPC DESIGN:
//  Uses CFNotificationCenterGetDistributedCenter() -- the same
//  mechanism vfs5011_daemon.c already uses for
//  com.apple.screenIsLocked/Unlocked. That pairing is proven in the
//  daemon's own code to cross the root-daemon / user-session boundary
//  correctly, so this reuses it instead of introducing an unverified
//  IPC path. IMPORTANT: distributed notifications don't reliably carry
//  userInfo payloads across that root/user boundary -- every event and
//  request below is its own distinct notification name rather than one
//  generic event with a payload dictionary.
//

import Cocoa
import UserNotifications
import ServiceManagement
import Darwin
// WidgetKit's ControlCenter reload API only exists in the macOS 26 SDK
// (Swift 6.2 toolchain). Older toolchains skip it and still build.
#if canImport(WidgetKit) && compiler(>=6.2)
import WidgetKit
#endif

// MARK: - Daemon health-check constants
//
// Must match hack-touchid-agent-install.sh's own LABEL exactly
// ("com.hackintosh.vfs5011agent", per-user domain as of the v2
// plist-location change -- see that script's header). Not derived
// from anywhere else at runtime since this app doesn't share a
// header with the shell installer; if that LABEL ever changes,
// this constant needs updating too.
private let daemonLaunchdLabel = "com.hackintosh.vfs5011agent"
private let daemonReinstallActionID = "com.vfs5011.hackintosh.reinstall_daemon"
private let daemonMissingCategoryID = "com.vfs5011.hackintosh.daemon_missing"

// MARK: - Client update check constants
//
// The client (hack-touchid) is updated by its own updater. This app only
// notices that a newer build exists and offers to run
// `hack-touchid --menu-updater` in a terminal. The branch to compare
// against is read from the client's own VERSION.txt, the same rule the
// client uses, so someone on active-development is only told about newer
// active-development builds.
private let clientUpdateActionID = "com.vfs5011.hackintosh.update_client"
private let clientUpdateCategoryID = "com.vfs5011.hackintosh.client_update"
private let clientSymlinkPath = "/usr/local/bin/hack-touchid"
private let clientRawVersionBaseURL = "https://raw.githubusercontent.com/hackintosh-user/hack-touchid"
private let clientUpdateCheckInterval: TimeInterval = 60 * 60
private let clientUpdateFirstCheckDelay: TimeInterval = 15
// UserDefaults key: the last "version|build" we already notified about,
// so one published build produces one notification, not one per hour.
private let clientUpdateNotifiedKey = "lastNotifiedClientUpdate"

private struct ClientVersionInfo {
    var version = ""
    var build = ""
    var branch = ""
    var critical = false

    // Parses VERSION.txt's KEY=value lines. Returns nil without VERSION=.
    init?(text: String) {
        for rawLine in text.split(whereSeparator: \.isNewline) {
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            if line.hasPrefix("VERSION=") { version = String(line.dropFirst(8)) }
            else if line.hasPrefix("BUILD=") { build = String(line.dropFirst(6)) }
            else if line.hasPrefix("BRANCH=") { branch = String(line.dropFirst(7)) }
            else if line.hasPrefix("CRITICAL=") { critical = line.dropFirst(9).hasPrefix("true") }
        }
        if version.isEmpty { return nil }
    }

    // Same comparison as parse_version_code() in hack_touchid_client.c.
    var versionCode: Int? {
        let trimmed = version.hasPrefix("v") || version.hasPrefix("V") ? String(version.dropFirst()) : version
        let parts = trimmed.split(separator: ".").map { Int($0) }
        guard let major = parts.first ?? nil else { return nil }
        let minor = parts.count > 1 ? (parts[1] ?? 0) : 0
        let patch = parts.count > 2 ? (parts[2] ?? 0) : 0
        return major * 10000 + minor * 100 + patch
    }

    // Same comparison as parse_build_code(): "26B254" = year, letter, number.
    var buildCode: Int? {
        let chars = Array(build)
        guard chars.count >= 4,
              let y1 = chars[0].wholeNumberValue, let y2 = chars[1].wholeNumberValue,
              chars[2].isASCII, chars[2].isUppercase,
              let number = Int(String(chars[3...])),
              let letter = chars[2].asciiValue else { return nil }
        return (y1 * 10 + y2) * 1_000_000 + Int(letter - 65) * 10_000 + number
    }

    func isNewer(than local: ClientVersionInfo) -> Bool {
        guard let remoteCode = versionCode, let localCode = local.versionCode else { return false }
        if remoteCode != localCode { return remoteCode > localCode }
        guard let remoteBuild = buildCode, let localBuild = local.buildCode else { return false }
        return remoteBuild > localBuild
    }
}

// MARK: - Notification names (must match hack-touchid-menubar-ipc.h exactly)
// NOTE: kept as "VFS5011Notification"/"com.vfs5011.hackintosh" even
// after the user-facing rename to "Hackintosh Touch-ID" -- this is a
// wire-protocol identifier shared with hack-touchid-menubar-ipc.c/.h (two
// copies: repo root + Hackintosh-TouchID-Menubar/daemon-patch/). Renaming it
// would mean updating three files in lockstep for zero user-visible
// benefit (nobody sees this string), so it was left alone. Only
// user-facing text (window titles, menu items, notification banners)
// was renamed.
enum VFS5011Notification {
    static let prefix = "com.vfs5011.hackintosh"

    // Daemon -> menu bar app (events)
    static let swipeRequested   = "\(prefix).swipe_requested"
    static let swipeSuccess     = "\(prefix).swipe_success"
    static let swipeFailed      = "\(prefix).swipe_failed"
    static let swipeLockout     = "\(prefix).swipe_lockout"
    static let swipeWeak        = "\(prefix).swipe_weak"
    static let swipeCancelled   = "\(prefix).swipe_cancelled"

    // Daemon -> menu bar app (state confirmation, so the UI is correct
    // even if the app launches after the daemon, or the daemon restarts)
    static let scanningEnabled  = "\(prefix).scanning_enabled"
    static let scanningDisabled = "\(prefix).scanning_disabled"

    // Swipe-to-lock (macOS Tahoe only). Daemon confirms its state with
    // these two, same pattern as scanning.
    static let lockswipeEnabled  = "\(prefix).lockswipe_enabled"
    static let lockswipeDisabled = "\(prefix).lockswipe_disabled"

    // Menu bar app -> daemon (requests; daemon confirms back via the
    // two state notifications above rather than the app assuming the
    // toggle succeeded)
    static let requestEnable    = "\(prefix).request_enable"
    static let requestDisable   = "\(prefix).request_disable"
    static let requestRestart   = "\(prefix).request_restart"
    static let requestState     = "\(prefix).request_state_announce"
    static let requestLockswipeEnable  = "\(prefix).request_lockswipe_enable"
    static let requestLockswipeDisable = "\(prefix).request_lockswipe_disable"
}

// MARK: - macOS version helpers

enum MacOSInfo {
    private static func sysctlString(_ name: String) -> String? {
        var size = 0
        guard sysctlbyname(name, nil, &size, nil, 0) == 0, size > 0 else { return nil }
        var buf = [CChar](repeating: 0, count: size)
        guard sysctlbyname(name, &buf, &size, nil, 0) == 0 else { return nil }
        return String(cString: buf)
    }

    /// Darwin 25 == macOS 26 Tahoe. Uses the kernel's own report, so a
    /// spoofed product version does not fool the feature gate.
    /// The Metallica agent creates this file while it runs on a macOS version where it offers
    /// Touch to Lock (the touch sensor's version of Swipe to Lock). Beta: Sequoia and newer for now.
    static var touchLockAvailable: Bool {
        FileManager.default.fileExists(atPath: "/Library/Application Support/VFS5011/touch_lock_available")
    }

    static var lockFeatureName: String { touchLockAvailable ? "Touch to Lock" : "Swipe to Lock" }

    static var isTahoeOrNewer: Bool {
        guard let rel = sysctlString("kern.osrelease"),
              let major = Int(rel.split(separator: ".").first ?? "") else { return false }
        return major >= 25
    }

    /// e.g. "Reported macOS Version: 26.7.1 Tahoe (Darwin: 25.6.0)".
    /// "Reported" because both values come from the kernel and can be
    /// spoofed or compat-mode.
    static var reportedVersionLine: String {
        let product = sysctlString("kern.osproductversion")
        let darwin = sysctlString("kern.osrelease")
        let major = Int((product ?? "").split(separator: ".").first ?? "") ?? 0
        let names = [26: "Tahoe", 15: "Sequoia", 14: "Sonoma", 13: "Ventura", 12: "Monterey", 11: "Big Sur"]
        var parts = product ?? "unknown"
        if let name = names[major] { parts += " \(name)" }
        if let darwin = darwin { parts += " (Darwin: \(darwin))" }
        return "Reported macOS Version: \(parts)"
    }
}

@main
class AppDelegate: NSObject, NSApplicationDelegate, UNUserNotificationCenterDelegate {

    // Explicit entry point. NOT relying on any implicit @main wiring --
    // that magic is normally supplied by Xcode's app target build
    // settings and does not reliably kick in for a plain `swiftc`
    // command-line build. This is exactly what a classic main.swift
    // would do by hand.
    static func main() {
        let app = NSApplication.shared
        let delegate = AppDelegate()
        app.delegate = delegate
        app.run()
    }

    private var statusItem: NSStatusItem!
    // The on-screen auth card (top middle, above every app). Replaces the old
    // Notification Center banners for swipe/touch prompts and results.
    private let authOverlay = AuthOverlay()
    // True from "swipe requested" until a final result (success, lockout, cancelled), so a
    // failed or weak attempt can drop back to the Waiting card.
    private var authEpisodeOpen = false
    // True if that episode began on the lock screen. macOS draws the lock screen above every app
    // window, so there the app falls back to a Notification Center banner, as before.
    private var authEpisodeStartedLocked = false
    // "Cancelled" waits a moment so a success that follows an unlock can cancel it.
    private var pendingCancel: DispatchWorkItem?
    private var clientUpdateTimer: Timer?
    private var aboutWindow: NSWindow?
    private var scanningEnabled: Bool = true {
        didSet { updateMenuForCurrentState() }
    }
    // Swipe-to-lock: mirrors what the daemon confirmed, never a guess.
    private var lockSwipeEnabled: Bool = false {
        didSet {
            updateMenuForCurrentState()
            reloadControlCenterControl()
        }
    }

    // MARK: - Lifecycle

    func applicationDidFinishLaunching(_ notification: Notification) {
        // Used by the client's [X] Uninstall: remove the login item, then
        // exit without showing anything.
        if CommandLine.arguments.contains("--unregister-login-item") {
            try? SMAppService.mainApp.unregister()
            NSLog("VFS5011MenuBar: login item unregistered by --unregister-login-item")
            exit(0)
        }

        // This is a menu bar-only utility -- no Dock icon, no main window.
        NSApp.setActivationPolicy(.accessory)

        setupStatusItem()
        registerNotificationCategories()
        requestNotificationPermission()
        registerForDaemonNotifications()
        registerAsLoginItemIfNeeded()

        // The lock screen covers any window, so drop the card as soon as the screen locks.
        DistributedNotificationCenter.default().addObserver(
            forName: Notification.Name("com.apple.screenIsLocked"), object: nil, queue: .main
        ) { [weak self] _ in
            if !AuthOverlay.lockScreenExperiment { self?.authOverlay.hide() }
        }

        // Preview for design checks: `HackintoshTouchID --overlay-demo` cycles every card.
        if CommandLine.arguments.contains("--overlay-demo") {
            authOverlay.runDemo()
        }

        // Known trigger for a missing/broken daemon (per project notes):
        // a macOS point update wipes it, similar to OCLP root patches
        // getting wiped on update -- the fix is just re-running deploy.
        // Checked on every launch (covers "logged back in after an
        // update installed at restart") and again after wake from
        // sleep (covers "update installed overnight, machine slept
        // through it"), since neither is guaranteed to relaunch this
        // app on its own.
        checkDaemonHealth()
        scheduleClientUpdateChecks()
        NSWorkspace.shared.notificationCenter.addObserver(
            self,
            selector: #selector(handleWake),
            name: NSWorkspace.didWakeNotification,
            object: nil
        )
    }

    @objc private func handleWake() {
        checkDaemonHealth()
        checkForClientUpdate()
    }

    // MARK: - Login item

    // Registers this app to launch automatically at login, using the
    // modern SMAppService API (macOS 13+ -- matches our
    // LSMinimumSystemVersion). Checked every launch against the real
    // SMAppService status (not a UserDefaults flag) -- a flag would
    // survive even after the person removes the login item in System
    // Settings, wrongly skipping re-registration forever after.
    private func registerAsLoginItemIfNeeded() {
        guard SMAppService.mainApp.status != .enabled else {
            writeSetupStatus()
            return
        }

        do {
            try SMAppService.mainApp.register()
            NSLog("VFS5011MenuBar: registered as a login item")
        } catch {
            NSLog("VFS5011MenuBar: failed to register as a login item: \(error.localizedDescription)")
        }
        writeSetupStatus()
    }

    // MARK: - Setup status file

    // The client's Deploy reads this after launching the app, to tell the
    // person whether notifications and the login item actually got set up.
    // Location: ~/Library/Application Support/Hackintosh Touch-ID/setup.status
    // Format: key=value lines (notifications=, loginitem=, version=).
    // Rewritten after the login item registration and after the
    // notification permission answer, so the last write is the full picture.
    private func writeSetupStatus() {
        UNUserNotificationCenter.current().getNotificationSettings { (settings: UNNotificationSettings) -> Void in
            // Kept as small, fully typed steps on purpose: long interpolated
            // strings and untyped multi-statement closures type-check very
            // slowly on older machines (swiftc looked hung on the DV6).
            var notifications: String = "unknown"
            switch settings.authorizationStatus {
            case .authorized:    notifications = "authorized"
            case .provisional:   notifications = "provisional"
            case .denied:        notifications = "denied"
            case .notDetermined: notifications = "notDetermined"
            default:             notifications = "unknown"
            }

            var loginItem: String = "unknown"
            switch SMAppService.mainApp.status {
            case .enabled:          loginItem = "enabled"
            case .requiresApproval: loginItem = "requiresApproval"
            case .notRegistered:    loginItem = "notRegistered"
            case .notFound:         loginItem = "notFound"
            default:                loginItem = "unknown"
            }

            var version: String = "unknown"
            if let v = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String {
                version = v
            }

            var text: String = ""
            text += "notifications=" + notifications + "\n"
            text += "loginitem=" + loginItem + "\n"
            text += "version=" + version + "\n"

            let fm: FileManager = FileManager.default
            let bases: [URL] = fm.urls(for: .applicationSupportDirectory, in: .userDomainMask)
            guard let base: URL = bases.first else { return }
            let dir: URL = base.appendingPathComponent("Hackintosh Touch-ID", isDirectory: true)
            let file: URL = dir.appendingPathComponent("setup.status")
            do {
                try fm.createDirectory(at: dir, withIntermediateDirectories: true, attributes: nil)
                try text.write(to: file, atomically: true, encoding: String.Encoding.utf8)
            } catch {
                NSLog("VFS5011MenuBar: could not write setup.status: " + error.localizedDescription)
            }
        }
    }

    func applicationWillTerminate(_ notification: Notification) {
        unregisterForDaemonNotifications()
    }

    // MARK: - Status item / menu

    private func setupStatusItem() {
        // variableLength (not squareLength) lets the item size itself to
        // whatever content actually renders -- squareLength combined
        // with a nil image is exactly how a status item goes invisible.
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        if let button = statusItem.button {
            if let customIcon = NSImage(named: "MenuBarIcon") {
                // Template mode: macOS recolors this automatically for
                // light/dark menu bars and Control Center, same as any
                // system status item. The PNG itself is pure black on
                // transparent -- isTemplate is what makes that adaptive.
                customIcon.isTemplate = true
                button.image = customIcon
            } else if let symbolImage = NSImage(
                systemSymbolName: "touchid",
                accessibilityDescription: "Hackintosh Touch-ID Daemon"
            ) {
                // Fallback if the bundled icon didn't load for any
                // reason (e.g. Resources weren't copied into the .app).
                button.image = symbolImage
            } else {
                // Last-resort fallback -- guarantees the item is never
                // an invisible, zero-width sliver in the menu bar.
                button.title = "🫆"
            }
        }
        statusItem.menu = buildMenu()
        statusItem.isVisible = true
    }

    private func buildMenu() -> NSMenu {
        let menu = NSMenu()

        let toggleItem = NSMenuItem(
            title: scanningEnabled ? "Disable Fingerprint Authentication" : "Enable Fingerprint Authentication",
            action: #selector(toggleScanningTapped),
            keyEquivalent: ""
        )
        toggleItem.target = self
        toggleItem.tag = 100 // used to find + relabel this item later
        menu.addItem(toggleItem)

        // Swipe-to-lock is a macOS Tahoe feature, so the item only exists there, except that the
        // Metallica agent (Touch to Lock, beta) also offers it on Sequoia while it is running.
        if MacOSInfo.isTahoeOrNewer || MacOSInfo.touchLockAvailable {
            let lockItem = NSMenuItem(
                title: lockSwipeEnabled ? "Disable \(MacOSInfo.lockFeatureName)" : "Enable \(MacOSInfo.lockFeatureName)",
                action: #selector(toggleLockSwipeTapped),
                keyEquivalent: ""
            )
            lockItem.target = self
            lockItem.tag = 101
            menu.addItem(lockItem)
        }

        menu.addItem(NSMenuItem.separator())

        let versionItem = NSMenuItem(title: MacOSInfo.reportedVersionLine, action: nil, keyEquivalent: "")
        versionItem.isEnabled = false
        versionItem.tag = 102
        menu.addItem(versionItem)

        menu.addItem(NSMenuItem.separator())

        let aboutItem = NSMenuItem(
            title: "About Hackintosh Touch-ID",
            action: #selector(aboutTapped),
            keyEquivalent: ""
        )
        aboutItem.target = self
        menu.addItem(aboutItem)

        menu.addItem(NSMenuItem.separator())

        let quitItem = NSMenuItem(
            title: "Quit Hackintosh Touch-ID",
            action: #selector(quitTapped),
            keyEquivalent: "q"
        )
        quitItem.target = self
        menu.addItem(quitItem)

        return menu
    }

    private func updateMenuForCurrentState() {
        guard let menu = statusItem.menu,
              let toggleItem = menu.item(withTag: 100) else { return }

        toggleItem.title = scanningEnabled
            ? "Disable Fingerprint Authentication"
            : "Enable Fingerprint Authentication"

        if let lockItem = menu.item(withTag: 101) {
            lockItem.title = lockSwipeEnabled ? "Disable \(MacOSInfo.lockFeatureName)" : "Enable \(MacOSInfo.lockFeatureName)"
            lockItem.state = lockSwipeEnabled ? .on : .off
        }

        // Dim the icon while paused so the state is visible without
        // opening the menu.
        statusItem.button?.appearsDisabled = !scanningEnabled
    }

    // Tells Control Center to re-read the swipe-to-lock control's state
    // (the control itself lives in the optional Tahoe extension, see
    // ControlCenterExtension/README.md). No-op without the macOS 26 SDK.
    private func reloadControlCenterControl() {
        #if canImport(WidgetKit) && compiler(>=6.2)
        if #available(macOS 26.0, *) {
            ControlCenter.shared.reloadControls(ofKind: "com.vfs5011.hackintosh.lockswipe")
        }
        #endif
    }

    // MARK: - Menu actions

    @objc private func aboutTapped() {
        if aboutWindow == nil {
            aboutWindow = makeAboutWindow()
        }
        NSApp.activate(ignoringOtherApps: true)
        aboutWindow?.center()
        aboutWindow?.makeKeyAndOrderFront(nil)
    }

    private func makeAboutWindow() -> NSWindow {
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 360, height: 340),
            styleMask: [.titled, .closable],
            backing: .buffered,
            defer: false
        )
        window.title = "About Hackintosh Touch-ID"
        window.isReleasedWhenClosed = false

        let content = NSView(frame: NSRect(x: 0, y: 0, width: 360, height: 340))

        let icon = NSImageView(frame: NSRect(x: 140, y: 240, width: 80, height: 80))
        icon.image = NSImage(named: "MenuBarIcon") ?? NSImage(systemSymbolName: "touchid", accessibilityDescription: nil)
        icon.imageScaling = .scaleProportionallyUpOrDown
        content.addSubview(icon)

        let title = NSTextField(labelWithString: "Hackintosh Touch-ID")
        title.font = NSFont.boldSystemFont(ofSize: 18)
        title.alignment = .center
        title.frame = NSRect(x: 0, y: 205, width: 360, height: 24)
        content.addSubview(title)

        let subtitle = NSTextField(labelWithString: "Fingerprint Authentication for Hackintosh")
        subtitle.font = NSFont.systemFont(ofSize: 12)
        subtitle.textColor = .secondaryLabelColor
        subtitle.alignment = .center
        subtitle.frame = NSRect(x: 0, y: 185, width: 360, height: 18)
        content.addSubview(subtitle)

        let body = NSTextField(wrappingLabelWithString:
            "Brings supported fingerprint sensors (Validity VFS5011, with more " +
            "in progress) to Hackintosh macOS as a real authentication " +
            "method -- lock screen, System Settings, Finder, installers, " +
            "and more. This menu bar app surfaces swipe prompts and " +
            "results as notifications, since the sensor's LED can be " +
            "hard to notice on its own.")
        body.font = NSFont.systemFont(ofSize: 12)
        body.alignment = .center
        body.frame = NSRect(x: 24, y: 70, width: 312, height: 110)
        content.addSubview(body)

        let link = NSTextField(labelWithString: "github.com/hackintosh-user/hack-touchid")
        link.font = NSFont.systemFont(ofSize: 11)
        link.textColor = .linkColor
        link.alignment = .center
        link.frame = NSRect(x: 0, y: 35, width: 360, height: 18)
        let linkClick = NSClickGestureRecognizer(target: self, action: #selector(openProjectPage))
        link.addGestureRecognizer(linkClick)
        content.addSubview(link)

        window.contentView = content
        return window
    }

    @objc private func openProjectPage() {
        if let url = URL(string: "https://github.com/hackintosh-user/hack-touchid") {
            NSWorkspace.shared.open(url)
        }
    }

    @objc private func toggleScanningTapped() {
        if scanningEnabled {
            postDistributedNotification(VFS5011Notification.requestDisable)
        } else {
            postDistributedNotification(VFS5011Notification.requestEnable)
        }
        // Deliberately NOT flipping `scanningEnabled` here. The menu
        // should reflect what the daemon confirms, not what we hope
        // happened -- avoids the UI lying if a request is ever dropped.
    }

    @objc private func toggleLockSwipeTapped() {
        if lockSwipeEnabled {
            postDistributedNotification(VFS5011Notification.requestLockswipeDisable)
        } else {
            postDistributedNotification(VFS5011Notification.requestLockswipeEnable)
        }
        // Same rule as scanning: the menu follows the daemon's confirmation.
    }

    @objc private func quitTapped() {
        NSApp.terminate(nil)
    }

    // MARK: - Notification permission

    // Registers the "Reinstall Daemon" actionable notification category.
    // Must happen before requestNotificationPermission()/any notification
    // using this category is posted, so it's called first in
    // applicationDidFinishLaunching().
    private func registerNotificationCategories() {
        let reinstallAction = UNNotificationAction(
            identifier: daemonReinstallActionID,
            title: "Reinstall Daemon",
            options: [.foreground]
        )
        let category = UNNotificationCategory(
            identifier: daemonMissingCategoryID,
            actions: [reinstallAction],
            intentIdentifiers: [],
            options: []
        )
        let updateAction = UNNotificationAction(
            identifier: clientUpdateActionID,
            title: "Update Client",
            options: [.foreground]
        )
        let updateCategory = UNNotificationCategory(
            identifier: clientUpdateCategoryID,
            actions: [updateAction],
            intentIdentifiers: [],
            options: []
        )
        UNUserNotificationCenter.current().setNotificationCategories([category, updateCategory])
        UNUserNotificationCenter.current().delegate = self
    }

    private func requestNotificationPermission() {
        let center = UNUserNotificationCenter.current()
        center.requestAuthorization(options: [.alert, .sound]) { [weak self] granted, error in
            if let error = error {
                NSLog("VFS5011MenuBar: notification permission error: \(error.localizedDescription)")
            } else if !granted {
                NSLog("VFS5011MenuBar: notification permission denied by user")
            }
            self?.writeSetupStatus()
        }
    }

    private func fireLocalNotification(title: String, body: String) {
        let content = UNMutableNotificationContent()
        content.title = title
        content.body = body
        content.sound = .default

        let request = UNNotificationRequest(
            identifier: UUID().uuidString,
            content: content,
            trigger: nil // deliver immediately
        )
        UNUserNotificationCenter.current().add(request) { error in
            if let error = error {
                NSLog("VFS5011MenuBar: failed to post notification: \(error.localizedDescription)")
            }
        }
    }

    // Removes this app's swipe notifications from Notification Center so
    // they don't pile up. The daemon-missing alert (it has a category and
    // an action button) is left alone.
    private func clearSwipeNotifications(after delay: TimeInterval, then completion: (() -> Void)? = nil) {
        DispatchQueue.main.asyncAfter(deadline: .now() + delay) {
            let center = UNUserNotificationCenter.current()
            center.getDeliveredNotifications { delivered in
                let ids = delivered
                    .filter { $0.request.content.categoryIdentifier != daemonMissingCategoryID
                           && $0.request.content.categoryIdentifier != clientUpdateCategoryID }
                    .map { $0.request.identifier }
                if !ids.isEmpty {
                    center.removeDeliveredNotifications(withIdentifiers: ids)
                }
                if let completion = completion {
                    DispatchQueue.main.async { completion() }
                }
            }
        }
    }

    // MARK: - Daemon health check

    // Checked with `launchctl print gui/<uid>/<label>` rather than just
    // testing for the plist file's existence -- the plist can be present
    // but the service still not bootstrapped (or crash-looping), and
    // `launchctl print` is the same command the install script itself
    // already uses to confirm success (see hack-touchid-agent-install.sh),
    // so this mirrors what "installed and running" actually means there.
    // A nonzero exit (launchctl's own convention for "no such service")
    // is treated as "missing" -- good enough to trigger the notification;
    // this deliberately doesn't try to distinguish "never installed" from
    // "wiped by an update" from "crashed", since the fix is the same
    // --deploy-agent re-run either way.
    private func checkDaemonHealth() {
        let uid = getuid()
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/bin/launchctl")
        process.arguments = ["print", "gui/\(uid)/\(daemonLaunchdLabel)"]
        process.standardOutput = FileHandle.nullDevice
        process.standardError = FileHandle.nullDevice

        do {
            try process.run()
            process.waitUntilExit()
            if process.terminationStatus != 0 {
                fireDaemonMissingNotification()
            }
        } catch {
            // Couldn't even launch launchctl -- don't nag the user over
            // something on our end; log and move on rather than firing
            // a possibly-false alarm.
            NSLog("VFS5011MenuBar: daemon health check failed to run launchctl: \(error.localizedDescription)")
        }
    }

    private func fireDaemonMissingNotification() {
        let content = UNMutableNotificationContent()
        content.title = "Hackintosh Touch-ID"
        content.body = "The daemon isn't installed or running. Tap Reinstall Daemon to fix it."
        content.sound = .default
        content.categoryIdentifier = daemonMissingCategoryID

        let request = UNNotificationRequest(
            identifier: "daemon-missing-\(UUID().uuidString)",
            content: content,
            trigger: nil // deliver immediately
        )
        UNUserNotificationCenter.current().add(request) { error in
            if let error = error {
                NSLog("VFS5011MenuBar: failed to post daemon-missing notification: \(error.localizedDescription)")
            }
        }
    }

    // Launches the user's default Terminal app running the actual
    // deploy command, via osascript rather than Process directly --
    // `sudo hack-touchid --deploy-agent` needs an interactive password
    // prompt and a visible window, which only a real Terminal.app
    // session (not a background Process) can give it. Relies on
    // ensure_path_symlink() (hack_touchid_client.c) having already put
    // `hack-touchid` on PATH via /usr/local/bin.
    private func launchDeployAgentInTerminal() {
        let script = """
        tell application "Terminal"
            activate
            do script "sudo hack-touchid --deploy-agent"
        end tell
        """
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/osascript")
        process.arguments = ["-e", script]
        do {
            try process.run()
        } catch {
            NSLog("VFS5011MenuBar: failed to launch Terminal for --deploy-agent: \(error.localizedDescription)")
        }
    }

    // MARK: - Client update check

    // First check shortly after launch (so it does not compete with the
    // daemon health check and the login-time rush), then hourly. Wake from
    // sleep also checks, see handleWake().
    private func scheduleClientUpdateChecks() {
        DispatchQueue.main.asyncAfter(deadline: .now() + clientUpdateFirstCheckDelay) { [weak self] in
            self?.checkForClientUpdate()
        }
        clientUpdateTimer = Timer.scheduledTimer(withTimeInterval: clientUpdateCheckInterval, repeats: true) { [weak self] _ in
            self?.checkForClientUpdate()
        }
    }

    // The client's VERSION.txt sits next to the real binary, which
    // /usr/local/bin/hack-touchid points at. Missing or unreadable just
    // means "can't check", never an error: the client may not have been
    // run yet.
    private func readLocalClientVersion() -> ClientVersionInfo? {
        let link = URL(fileURLWithPath: clientSymlinkPath).resolvingSymlinksInPath()
        let versionFile = link.deletingLastPathComponent().appendingPathComponent("VERSION.txt")
        guard let text = try? String(contentsOf: versionFile, encoding: .utf8) else { return nil }
        return ClientVersionInfo(text: text)
    }

    private func checkForClientUpdate() {
        guard let local = readLocalClientVersion(), !local.branch.isEmpty,
              let url = URL(string: "\(clientRawVersionBaseURL)/\(local.branch)/VERSION.txt") else { return }

        var request = URLRequest(url: url)
        request.timeoutInterval = 10
        request.cachePolicy = .reloadIgnoringLocalCacheData
        URLSession.shared.dataTask(with: request) { [weak self] data, response, _ in
            // Offline, GitHub down, bad branch: stay quiet, try again next time.
            guard let self = self,
                  let http = response as? HTTPURLResponse, http.statusCode == 200,
                  let data = data, let text = String(data: data, encoding: .utf8),
                  let remote = ClientVersionInfo(text: text),
                  remote.isNewer(than: local) else { return }

            let marker = "\(remote.version)|\(remote.build)"
            DispatchQueue.main.async {
                if UserDefaults.standard.string(forKey: clientUpdateNotifiedKey) == marker { return }
                UserDefaults.standard.set(marker, forKey: clientUpdateNotifiedKey)
                self.fireClientUpdateNotification(local: local, remote: remote)
            }
        }.resume()
    }

    private func fireClientUpdateNotification(local: ClientVersionInfo, remote: ClientVersionInfo) {
        let content = UNMutableNotificationContent()
        content.title = remote.critical ? "Hackintosh Touch-ID: critical update" : "Hackintosh Touch-ID update"
        content.body = "Client v\(remote.version) (\(remote.build)) is available. You are on v\(local.version) (\(local.build))."
        content.sound = .default
        content.categoryIdentifier = clientUpdateCategoryID

        let request = UNNotificationRequest(
            identifier: "client-update-\(UUID().uuidString)",
            content: content,
            trigger: nil // deliver immediately
        )
        UNUserNotificationCenter.current().add(request) { error in
            if let error = error {
                NSLog("VFS5011MenuBar: failed to post client-update notification: \(error.localizedDescription)")
            }
        }
    }

    // Opens a terminal running `hack-touchid --menu-updater`. A small
    // .command file is opened with the system default handler, so it
    // lands in whatever terminal app the person has set as default
    // (Terminal.app unless they changed it). The client asks for sudo
    // itself, so the password prompt appears in that window.
    private func launchClientUpdaterInTerminal() {
        let script = """
        #!/bin/zsh
        export PATH="/usr/local/bin:$PATH"
        hack-touchid --menu-updater
        echo
        echo "Press Return to close this window."
        read
        # Terminal.app only: close this window and quit Terminal if no other window is left.
        # Runs detached so it fires after this shell has exited. Other terminal apps are left alone.
        if [[ "$TERM_PROGRAM" == "Apple_Terminal" ]]; then
            nohup /usr/bin/osascript -e 'delay 0.7' -e 'tell application "Terminal"' -e 'try' -e 'close (every window whose name contains "HTID-Update-Client")' -e 'end try' -e 'delay 0.3' -e 'if (count of windows) is 0 then quit' -e 'end tell' >/dev/null 2>&1 &!
        fi
        exit
        """
        let fileURL = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("HTID-Update-Client.command")
        do {
            try script.write(to: fileURL, atomically: true, encoding: .utf8)
            try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: fileURL.path)
            NSWorkspace.shared.open(fileURL)
        } catch {
            NSLog("VFS5011MenuBar: failed to launch the client updater: \(error.localizedDescription)")
        }
    }

    // MARK: - UNUserNotificationCenterDelegate

    // Handles the "Reinstall Daemon" action button tap. Also shows
    // notifications while the app is frontmost (.banner/.sound) --
    // without this, UNUserNotificationCenter silently drops
    // notifications whose posting app is currently active, which would
    // otherwise make the daemon-missing alert invisible if this app
    // happens to be in the foreground (e.g. right after launch).
    func userNotificationCenter(
        _ center: UNUserNotificationCenter,
        willPresent notification: UNNotification,
        withCompletionHandler completionHandler: @escaping (UNNotificationPresentationOptions) -> Void
    ) {
        completionHandler([.banner, .sound])
    }

    func userNotificationCenter(
        _ center: UNUserNotificationCenter,
        didReceive response: UNNotificationResponse,
        withCompletionHandler completionHandler: @escaping () -> Void
    ) {
        if response.notification.request.content.categoryIdentifier == daemonMissingCategoryID,
           response.actionIdentifier == daemonReinstallActionID {
            launchDeployAgentInTerminal()
        }
        if response.notification.request.content.categoryIdentifier == clientUpdateCategoryID,
           response.actionIdentifier == clientUpdateActionID {
            launchClientUpdaterInTerminal()
        }
        completionHandler()
    }

    // MARK: - Distributed notifications (daemon IPC)
    //
    // Uses CFNotificationCenterGetDistributedCenter(), NOT the
    // lower-level Darwin notify center -- see the file header comment.

    private func registerForDaemonNotifications() {
        let center = CFNotificationCenterGetDistributedCenter()
        let observer = Unmanaged.passUnretained(self).toOpaque()

        let names = [
            VFS5011Notification.swipeRequested,
            VFS5011Notification.swipeSuccess,
            VFS5011Notification.swipeFailed,
            VFS5011Notification.swipeLockout,
            VFS5011Notification.swipeWeak,
            VFS5011Notification.swipeCancelled,
            VFS5011Notification.scanningEnabled,
            VFS5011Notification.scanningDisabled,
            VFS5011Notification.lockswipeEnabled,
            VFS5011Notification.lockswipeDisabled,
        ]

        for name in names {
            CFNotificationCenterAddObserver(
                center,
                observer,
                { (_, observerPtr, name, _, _) in
                    guard let observerPtr = observerPtr, let name = name else { return }
                    let mySelf = Unmanaged<AppDelegate>.fromOpaque(observerPtr).takeUnretainedValue()
                    mySelf.handleDaemonNotification(name.rawValue as String)
                },
                name as CFString,
                nil,
                .deliverImmediately
            )
        }

        // On launch, ask the daemon to (re-)announce its current
        // scanning state, since this app may have launched after the
        // daemon and otherwise wouldn't know if scanning is paused.
        postDistributedNotification(VFS5011Notification.requestState)
        clearSwipeNotifications(after: 1.0) // leftovers from before this launch

        // After a cold boot this app can launch before the daemon is up,
        // so the first request goes unanswered. Ask again a few times;
        // the daemon's answer is idempotent.
        for delay in [2.0, 5.0, 10.0, 20.0] {
            DispatchQueue.main.asyncAfter(deadline: .now() + delay) { [weak self] in
                self?.postDistributedNotification(VFS5011Notification.requestState)
            }
        }
    }

    private func unregisterForDaemonNotifications() {
        let center = CFNotificationCenterGetDistributedCenter()
        let observer = Unmanaged.passUnretained(self).toOpaque()
        CFNotificationCenterRemoveEveryObserver(center, observer)
    }

    private func postDistributedNotification(_ name: String) {
        let center = CFNotificationCenterGetDistributedCenter()
        CFNotificationCenterPostNotification(
            center,
            CFNotificationName(name as CFString),
            nil,
            nil,
            true
        )
    }

    // The same flag the daemon reads: present (and true) only while the screen is locked.
    private func isScreenLocked() -> Bool {
        guard let session = CGSessionCopyCurrentDictionary() as? [String: Any] else { return false }
        return (session["CGSSessionScreenIsLocked"] as? Bool) ?? false
    }

    // Unlocked: the overlay card. Locked: a Notification Center banner, because no app window can
    // be drawn above the lock screen.
    private func showAuth(_ kind: AuthOverlayKind, thenWaiting: Bool = false) {
        let locked = isScreenLocked()
        if !locked {
            authOverlay.show(kind, thenWaiting: thenWaiting)
            return
        }
        if AuthOverlay.lockScreenExperiment {
            // Experiment: also try the card over the lock screen. The banner below stays as the safety net.
            authOverlay.show(kind, thenWaiting: thenWaiting)
        } else {
            authOverlay.hide()
        }
        let body: String
        switch kind {
        case .waiting:       body = "Waiting for fingerprint 🔻"
        case .success:       body = "Authenticated ✅"
        case .notRecognized: body = "Not recognized, please try again ❌"
        case .incomplete:    body = "Incomplete swipe, please try again 🔻"
        case .lockout:       body = "Too many attempts, use your password ❌"
        case .cancelled:     return
        }
        clearSwipeNotifications(after: 0) { [weak self] in
            self?.fireLocalNotification(title: "Hackintosh Touch-ID", body: body)
            if kind != .waiting { self?.clearSwipeNotifications(after: 10.0) }
        }
    }

    private func handleDaemonNotification(_ name: String) {
        // Distributed notifications can arrive on a background thread.
        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }

            switch name {
            case VFS5011Notification.swipeRequested:
                self.pendingCancel?.cancel()
                self.authEpisodeOpen = true
                self.authEpisodeStartedLocked = self.isScreenLocked()
                self.showAuth(.waiting)

            case VFS5011Notification.swipeSuccess:
                self.pendingCancel?.cancel()
                self.authEpisodeOpen = false
                self.showAuth(.success)

            case VFS5011Notification.swipeFailed:
                self.showAuth(.notRecognized, thenWaiting: self.authEpisodeOpen)

            case VFS5011Notification.swipeWeak:
                self.showAuth(.incomplete, thenWaiting: self.authEpisodeOpen)

            case VFS5011Notification.swipeLockout:
                self.pendingCancel?.cancel()
                self.authEpisodeOpen = false
                self.showAuth(.lockout)

            case VFS5011Notification.swipeCancelled:
                self.pendingCancel?.cancel()
                let startedLocked = self.authEpisodeStartedLocked
                self.authEpisodeOpen = false
                if startedLocked {
                    // Unlocked with the password: nothing to show, just clear the leftover banner.
                    self.authOverlay.hide()
                    self.clearSwipeNotifications(after: 0)
                } else {
                    let work = DispatchWorkItem { [weak self] in self?.authOverlay.show(.cancelled) }
                    self.pendingCancel = work
                    DispatchQueue.main.asyncAfter(deadline: .now() + 0.8, execute: work)
                }

            case VFS5011Notification.scanningEnabled:
                self.scanningEnabled = true

            case VFS5011Notification.scanningDisabled:
                self.scanningEnabled = false
                self.authEpisodeOpen = false
                self.authOverlay.hide()

            case VFS5011Notification.lockswipeEnabled:
                self.lockSwipeEnabled = true

            case VFS5011Notification.lockswipeDisabled:
                self.lockSwipeEnabled = false

            default:
                break
            }
        }
    }
}


// MARK: - Auth overlay (top middle, above every app)

// The five cards from the design: Waiting (white), Authenticated (green), Not recognized (red),
// Incomplete swipe (yellow) and Cancelled (cyan, with a close button). Lockout reuses the red style.
enum AuthOverlayKind {
    case waiting, success, notRecognized, incomplete, lockout, cancelled

    var title: String {
        switch self {
        case .waiting:       return "Waiting for fingerprint"
        case .success:       return "Authenticated"
        case .notRecognized: return "Not recognized"
        case .incomplete:    return "Incomplete swipe"
        case .lockout:       return "Too many attempts, use your password"
        case .cancelled:     return "Cancelled"
        }
    }
    var subtitle: String? { self == .cancelled ? "Authentication cancelled." : nil }
    var symbol: String {
        switch self {
        case .waiting, .cancelled: return "touchid"
        case .success:             return "checkmark.circle"
        case .notRecognized, .lockout: return "xmark.circle"
        case .incomplete:          return "exclamationmark.circle"
        }
    }
    var accent: NSColor {
        switch self {
        case .waiting:       return NSColor(calibratedWhite: 0.94, alpha: 1)
        case .success:       return NSColor(calibratedRed: 0.20, green: 0.80, blue: 0.45, alpha: 1)
        case .notRecognized, .lockout: return NSColor(calibratedRed: 0.93, green: 0.30, blue: 0.32, alpha: 1)
        case .incomplete:    return NSColor(calibratedRed: 0.96, green: 0.78, blue: 0.20, alpha: 1)
        case .cancelled:     return NSColor(calibratedRed: 0.20, green: 0.78, blue: 0.90, alpha: 1)
        }
    }
    // Seconds on screen. nil = stays until the next event (Waiting).
    var lifetime: TimeInterval? {
        switch self {
        case .waiting:       return nil
        case .success:       return 1.6
        case .notRecognized, .incomplete: return 2.2
        case .lockout:       return 4.0
        case .cancelled:     return 4.0
        }
    }
}

// Draws one card: dark rounded rectangle, 1.5 pt accent border, icon on the left, text on the right.
final class AuthCardView: NSView {
    var onClose: (() -> Void)?
    private let kind: AuthOverlayKind

    init(kind: AuthOverlayKind) {
        self.kind = kind
        super.init(frame: .zero)
        wantsLayer = true
        layer?.backgroundColor = NSColor(calibratedWhite: 0.10, alpha: 0.97).cgColor
        layer?.borderColor = kind.accent.cgColor
        layer?.borderWidth = 1.5
        layer?.cornerRadius = kind.subtitle == nil ? 16 : 18

        let icon = NSImageView()
        icon.translatesAutoresizingMaskIntoConstraints = false
        if let img = NSImage(systemSymbolName: kind.symbol, accessibilityDescription: kind.title) {
            let cfg = NSImage.SymbolConfiguration(pointSize: kind.subtitle == nil ? 17 : 26, weight: .regular)
            icon.image = img.withSymbolConfiguration(cfg)
        }
        icon.contentTintColor = kind.accent
        addSubview(icon)

        let title = NSTextField(labelWithString: kind.title)
        title.translatesAutoresizingMaskIntoConstraints = false
        title.textColor = NSColor(calibratedWhite: 0.97, alpha: 1)
        title.font = NSFont.systemFont(ofSize: kind.subtitle == nil ? 14 : 16, weight: kind.subtitle == nil ? .medium : .semibold)
        title.lineBreakMode = .byTruncatingTail

        let stack = NSStackView(views: [title])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 2
        stack.translatesAutoresizingMaskIntoConstraints = false
        if let sub = kind.subtitle {
            let subtitle = NSTextField(labelWithString: sub)
            subtitle.textColor = NSColor(calibratedWhite: 0.70, alpha: 1)
            subtitle.font = NSFont.systemFont(ofSize: 12.5, weight: .regular)
            stack.addArrangedSubview(subtitle)
        }
        addSubview(stack)

        NSLayoutConstraint.activate([
            icon.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 16),
            icon.centerYAnchor.constraint(equalTo: centerYAnchor),
            stack.leadingAnchor.constraint(equalTo: icon.trailingAnchor, constant: 12),
            stack.centerYAnchor.constraint(equalTo: centerYAnchor),
        ])

        if kind == .cancelled {
            let close = NSButton(title: "", target: self, action: #selector(closeTapped))
            close.translatesAutoresizingMaskIntoConstraints = false
            close.isBordered = false
            close.image = NSImage(systemSymbolName: "xmark", accessibilityDescription: "Close")?
                .withSymbolConfiguration(NSImage.SymbolConfiguration(pointSize: 12, weight: .semibold))
            close.contentTintColor = kind.accent
            addSubview(close)
            NSLayoutConstraint.activate([
                close.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -14),
                close.topAnchor.constraint(equalTo: topAnchor, constant: 14),
                close.widthAnchor.constraint(equalToConstant: 20),
                close.heightAnchor.constraint(equalToConstant: 20),
                stack.trailingAnchor.constraint(lessThanOrEqualTo: close.leadingAnchor, constant: -8),
            ])
        } else {
            stack.trailingAnchor.constraint(lessThanOrEqualTo: trailingAnchor, constant: -18).isActive = true
        }

        setAccessibilityElement(true)
        setAccessibilityRole(.staticText)
        setAccessibilityLabel(kind.subtitle.map { "\(kind.title). \($0)" } ?? kind.title)
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    @objc private func closeTapped() { onClose?() }

    // Card size, measured from the text so it never depends on layout timing. Floors keep the
    // short ones looking like the design.
    var preferredSize: NSSize {
        let hasSub = kind.subtitle != nil
        let titleFont = NSFont.systemFont(ofSize: hasSub ? 16 : 14, weight: hasSub ? .semibold : .medium)
        var textW = (kind.title as NSString).size(withAttributes: [.font: titleFont]).width
        if let sub = kind.subtitle {
            let subW = (sub as NSString).size(withAttributes: [.font: NSFont.systemFont(ofSize: 12.5)]).width
            textW = max(textW, subW)
        }
        let iconW: CGFloat = hasSub ? 30 : 20
        let closeW: CGFloat = hasSub ? 34 : 0
        let width = ceil(16 + iconW + 12 + textW + 18 + closeW)
        return hasSub ? NSSize(width: max(272, width), height: 80) : NSSize(width: max(250, width), height: 50)
    }
}

// One borderless panel, reused. Level is the assistive-technology window level, which sits above
// normal windows, full-screen apps and the menu bar, so it needs no notification permission.
final class AuthOverlay {
    private var panel: NSPanel?
    private var hideWork: DispatchWorkItem?
    private let waitingSafetySeconds: TimeInterval = 90 // never leave a stale Waiting card up

    // EXPERIMENT: try to draw the card above the lock screen. Off by default; turn on with
    //   defaults write com.vfs5011.hackintosh.menubar OverlayOnLockScreen -bool YES
    // then quit and relaunch the menu bar app. macOS may still hide it there.
    static var lockScreenExperiment: Bool {
        UserDefaults.standard.bool(forKey: "OverlayOnLockScreen")
    }

    private func makePanel() -> NSPanel {
        let p = NSPanel(contentRect: NSRect(x: 0, y: 0, width: 260, height: 50),
                        styleMask: [.borderless, .nonactivatingPanel],
                        backing: .buffered, defer: false)
        p.isOpaque = false
        p.backgroundColor = .clear
        p.hasShadow = true
        p.isFloatingPanel = true
        p.hidesOnDeactivate = false
        p.becomesKeyOnlyIfNeeded = true
        applyLevel(to: p)
        p.isReleasedWhenClosed = false
        return p
    }

    private func applyLevel(to p: NSPanel) {
        if AuthOverlay.lockScreenExperiment {
            // One step above the shielding window the lock screen and screen saver use.
            p.level = NSWindow.Level(rawValue: Int(CGShieldingWindowLevel()) + 1)
            p.collectionBehavior = [.canJoinAllSpaces, .stationary, .fullScreenAuxiliary, .fullScreenDisallowsTiling, .ignoresCycle]
        } else {
            p.level = NSWindow.Level(rawValue: Int(CGWindowLevelForKey(.assistiveTechHighWindow)))
            p.collectionBehavior = [.canJoinAllSpaces, .stationary, .fullScreenAuxiliary, .ignoresCycle]
        }
    }

    // Shows a card. thenWaiting: after a timed card (a failed or weak attempt) go back to Waiting,
    // because the sensor is still listening.
    func show(_ kind: AuthOverlayKind, thenWaiting: Bool = false) {
        DispatchQueue.main.async { [weak self] in self?.present(kind, thenWaiting: thenWaiting) }
    }

    func hide() {
        DispatchQueue.main.async { [weak self] in self?.dismiss() }
    }

    private func present(_ kind: AuthOverlayKind, thenWaiting: Bool) {
        hideWork?.cancel()
        let panel = self.panel ?? makePanel()
        self.panel = panel

        let card = AuthCardView(kind: kind)
        card.onClose = { [weak self] in self?.dismiss() }
        let size = card.preferredSize
        card.frame = NSRect(origin: .zero, size: size)
        panel.contentView = card
        panel.ignoresMouseEvents = (kind != .cancelled) // only the Cancelled card has a button
        applyLevel(to: panel) // follows the experiment flag without a relaunch of the panel

        // Top middle of the screen that has the menu bar, just under it.
        let screen = NSScreen.screens.first ?? NSScreen.main
        let area = screen?.visibleFrame ?? NSRect(x: 0, y: 0, width: 1440, height: 900)
        let x = area.midX - size.width / 2
        let y = area.maxY - size.height - 10
        let wasVisible = panel.isVisible && panel.alphaValue > 0.01
        panel.setFrame(NSRect(x: x, y: wasVisible ? y : y + 8, width: size.width, height: size.height), display: true)
        if !wasVisible { panel.alphaValue = 0 }
        panel.orderFrontRegardless()
        NSAnimationContext.runAnimationGroup { ctx in
            ctx.duration = 0.18
            panel.animator().alphaValue = 1
            panel.animator().setFrame(NSRect(x: x, y: y, width: size.width, height: size.height), display: true)
        }

        NSAccessibility.post(element: NSApp as Any, notification: .announcementRequested,
                             userInfo: [.announcement: kind.subtitle ?? kind.title])

        let life = kind.lifetime ?? waitingSafetySeconds
        let work = DispatchWorkItem { [weak self] in
            if thenWaiting { self?.present(.waiting, thenWaiting: false) } else { self?.dismiss() }
        }
        hideWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + life, execute: work)
    }

    private func dismiss() {
        hideWork?.cancel()
        hideWork = nil
        guard let panel = panel, panel.isVisible else { return }
        NSAnimationContext.runAnimationGroup({ ctx in
            ctx.duration = 0.18
            panel.animator().alphaValue = 0
        }, completionHandler: { [weak panel] in
            // A newer card may have been shown while this one faded out.
            if let panel = panel, panel.alphaValue < 0.01 { panel.orderOut(nil) }
        })
    }

    // Design preview: every card for a few seconds each.
    func runDemo() {
        let steps: [(AuthOverlayKind, TimeInterval)] = [
            (.waiting, 0), (.incomplete, 3), (.notRecognized, 6), (.success, 9), (.cancelled, 12), (.lockout, 17)
        ]
        for (kind, delay) in steps {
            DispatchQueue.main.asyncAfter(deadline: .now() + 1 + delay) { [weak self] in self?.show(kind) }
        }
    }
}
