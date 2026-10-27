import AppKit
import Darwin

// The standalone app's launcher: the same VM as launch.py (accelerated guest,
// stereo CoreAudio), using QEMU and guest files from inside the app bundle.
// Writable state lives in ~/Library/Application Support/RX3Prototype.
final class AppLauncher: NSObject, NSApplicationDelegate {
    private let bundle = Bundle.main
    private let support = FileManager.default.homeDirectoryForCurrentUser
        .appendingPathComponent("Library/Application Support/RX3Prototype")
    private let logs = FileManager.default.homeDirectoryForCurrentUser
        .appendingPathComponent("Library/Logs/RX3 Prototype")
    private var qemu: Process?
    private var bridge: Process?
    private var lock: Int32 = -1

    private var runtime: URL { bundle.resourceURL!.appendingPathComponent("runtime") }
    private var helpers: URL { bundle.bundleURL.appendingPathComponent("Contents/Helpers") }
    private var share: URL { support.appendingPathComponent("share") }
    private var firmware: URL { share.appendingPathComponent("rbpi4b-pi4.tgz") }

    func applicationDidFinishLaunching(_ notification: Notification) {
        guard ProcessInfo.processInfo.machineHardwareName == "arm64" else {
            fail("RX3 Prototype needs a Mac with Apple silicon (M1 or later).")
            return
        }
        do {
            try FileManager.default.createDirectory(at: share, withIntermediateDirectories: true)
            try FileManager.default.createDirectory(at: logs, withIntermediateDirectories: true)
            try installShare()
        } catch {
            fail("Could not prepare \(support.path): \(error.localizedDescription)")
            return
        }
        guard FileManager.default.fileExists(atPath: firmware.path) else {
            offerSetup()
            return
        }
        start()
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows: Bool) -> Bool {
        if let pid = qemu?.processIdentifier {
            NSRunningApplication(processIdentifier: pid)?.activate(options: [.activateAllWindows])
        }
        return false
    }

    func applicationWillTerminate(_ notification: Notification) {
        if let qemu, qemu.isRunning { qemu.terminate() }
        stopBridge()
    }

    // The share is read by the guest over 9p and gets a per-launch USB label,
    // so it must be writable; the bundle's copy stays sealed and unchanged.
    private func installShare() throws {
        let source = runtime.appendingPathComponent("share")
        for name in try FileManager.default.contentsOfDirectory(atPath: source.path) {
            let from = source.appendingPathComponent(name), to = share.appendingPathComponent(name)
            if let a = try? FileManager.default.attributesOfItem(atPath: from.path)[.size] as? Int,
               let b = try? FileManager.default.attributesOfItem(atPath: to.path)[.size] as? Int, a == b {
                continue
            }
            try? FileManager.default.removeItem(at: to)
            try FileManager.default.copyItem(at: from, to: to)
        }
    }

    private func offerSetup() {
        guard let script = bundle.url(forResource: "first-run-setup", withExtension: "sh") else {
            fail("This copy of RX3 Prototype is missing its player files. Download it again.")
            return
        }
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = "First-time setup"
        alert.informativeText = """
            RX3 Prototype builds its player from AlphaTheta's official XDJ-RX3 v1.20 update, \
            on this Mac. This needs Docker Desktop running, an internet connection, about 15 GB \
            of free space, and takes roughly 20–40 minutes. It only happens once.

            Terminal will open to show progress. When it finishes, RX3 Prototype opens again.
            """
        alert.addButton(withTitle: "Start Setup")
        alert.addButton(withTitle: "Cancel")
        guard alert.runModal() == .alertFirstButtonReturn else { NSApp.terminate(nil); return }
        let command = support.appendingPathComponent("Run Setup.command")
        let text = "#!/bin/bash\nexec /bin/bash \(quote(script.path)) \(quote(bundle.bundlePath))\n"
        do {
            try text.write(to: command, atomically: true, encoding: .utf8)
            try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: command.path)
        } catch {
            fail("Could not write the setup script: \(error.localizedDescription)")
            return
        }
        if !NSWorkspace.shared.open(command) {
            fail("macOS could not open Terminal for setup.")
            return
        }
        NSApp.terminate(nil)
    }

    private func start() {
        lock = open(support.appendingPathComponent("launch.lock").path, O_CREAT | O_RDWR, 0o644)
        guard lock >= 0, flock(lock, LOCK_EX | LOCK_NB) == 0 else {
            fail("RX3 Prototype is already running.")
            return
        }
        // fsdev option strings treat commas specially; reject rather than misparse.
        guard !support.path.contains(",") else {
            fail("Your home folder path contains a comma, which QEMU cannot use.")
            return
        }
        var usb: URL?
        switch detectUSB() {
        case .none: usb = nil
        case .one(let volume): usb = volume
        case .many: usb = chooseUSB()
        }
        if let usb, usb.path.contains(",") {
            fail("The USB path cannot contain commas: \(usb.path)")
            return
        }
        try? (usb?.lastPathComponent ?? "RX3 Mac USB")
            .write(to: share.appendingPathComponent("usb-label"), atomically: true, encoding: .utf8)

        let midiLog = logs.appendingPathComponent("midi-host.log")
        let consoleLog = logs.appendingPathComponent("console.log")
        FileManager.default.createFile(atPath: midiLog.path, contents: nil)
        FileManager.default.createFile(atPath: consoleLog.path, contents: nil)
        let midiSocket = support.appendingPathComponent("midi.sock")
        let bridge = Process()
        bridge.executableURL = helpers.appendingPathComponent("midi-host")
        bridge.arguments = [midiSocket.path]
        bridge.standardOutput = FileHandle(forWritingAtPath: midiLog.path)
        bridge.standardError = bridge.standardOutput
        do { try bridge.run() } catch {
            fail("Could not start the MIDI bridge: \(error.localizedDescription)")
            return
        }
        self.bridge = bridge

        let process = Process()
        process.executableURL = helpers.appendingPathComponent("qemu-system-aarch64")
        process.arguments = arguments(usb: usb, midiSocket: midiSocket)
        var environment = ProcessInfo.processInfo.environment
        environment["DYLD_INSERT_LIBRARIES"] = bundle.bundleURL
            .appendingPathComponent("Contents/Frameworks/coreaudio-adapter.dylib").path
        process.environment = environment
        let console = FileHandle(forWritingAtPath: consoleLog.path)
        process.standardOutput = console
        process.standardError = console
        process.standardInput = FileHandle.nullDevice
        process.terminationHandler = { [weak self] finished in
            DispatchQueue.main.async {
                self?.stopBridge()
                if finished.terminationStatus != 0 && finished.terminationReason == .exit {
                    self?.fail("The player stopped unexpectedly (status \(finished.terminationStatus)). Details are in \(consoleLog.path).")
                } else {
                    NSApp.terminate(nil)
                }
            }
        }
        do { try process.run() } catch {
            stopBridge()
            fail("Could not start QEMU: \(error.localizedDescription)")
            return
        }
        qemu = process
        // QEMU shows its own fullscreen window and Dock icon; stay out of the way.
        NSApp.setActivationPolicy(.accessory)
    }

    // Mirrors launch.py's command() for --native-guest --audio coreaudio --latency responsive.
    private func arguments(usb: URL?, midiSocket: URL) -> [String] {
        let backend = "coreaudio,id=prototype,out.frequency=48000,out.channels=2,out.format=s16," +
            "timer-period=1000,out.buffer-length=5000,out.buffer-count=2"
        var options = ["-name", "XDJ-RX3 Mac Prototype", "-machine", "virt,highmem=off", "-cpu", "host",
            "-accel", "hvf", "-smp", "4", "-m", "2048",
            "-kernel", runtime.appendingPathComponent("vmlinuz-native").path,
            "-initrd", runtime.appendingPathComponent("initrd-native.gz").path,
            "-append", "console=ttyAMA0 loglevel=4 video=Virtual-1:1280x800-32@60 rx3_latency=responsive",
            "-device", "virtio-gpu-pci", "-device", "qemu-xhci,id=usb,p2=8,p3=8",
            "-device", "usb-tablet", "-device", "usb-kbd",
            "-device", "virtio-serial-pci,id=control",
            "-chardev", "socket,id=midi,path=\(midiSocket.path),server=on,wait=off",
            "-device", "virtserialport,bus=control.0,nr=1,chardev=midi,name=flx4",
            "-audiodev", backend, "-device", "usb-audio,audiodev=prototype,multi=off,buffer=4096",
            "-fsdev", "local,id=source,path=\(share.path),security_model=none,readonly=on",
            "-device", "virtio-9p-pci,fsdev=source,mount_tag=source",
            "-display", "cocoa,zoom-to-fit=on,zoom-interpolation=on,full-screen=on",
            "-qmp", "unix:\(support.appendingPathComponent("qmp.sock").path),server=on,wait=off",
            "-serial", "stdio", "-monitor", "none", "-no-reboot", "-nic", "none"]
        if let usb {
            options += ["-fsdev", "local,id=usbfiles,path=\(usb.path),security_model=none,readonly=on",
                "-device", "virtio-9p-pci,fsdev=usbfiles,mount_tag=usb1",
                "-drive", "if=none,id=media,file=\(runtime.appendingPathComponent("media-id.img").path),format=raw,readonly=on",
                "-device", "usb-storage,bus=usb.0,drive=media"]
        }
        return options
    }

    private enum USB { case none, one(URL), many }

    private func detectUSB() -> USB {
        let volumes = (try? FileManager.default.contentsOfDirectory(
            at: URL(fileURLWithPath: "/Volumes"), includingPropertiesForKeys: nil)) ?? []
        let found = volumes.filter { hasExport($0) }
        return found.count > 1 ? .many : found.first.map { .one($0) } ?? .none
    }

    private func hasExport(_ root: URL) -> Bool {
        FileManager.default.fileExists(atPath: root.appendingPathComponent("PIONEER/rekordbox/export.pdb").path)
    }

    private func chooseUSB() -> URL? {
        NSApp.activate(ignoringOtherApps: true)
        let panel = NSOpenPanel()
        panel.message = "More than one rekordbox USB is connected. Choose the one to load."
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.directoryURL = URL(fileURLWithPath: "/Volumes")
        while panel.runModal() == .OK, let url = panel.url {
            if hasExport(url) { return url }
            let alert = NSAlert()
            alert.messageText = "Not a rekordbox USB"
            alert.informativeText = "Choose the top of a USB exported by rekordbox (it contains PIONEER/rekordbox/export.pdb)."
            alert.runModal()
        }
        return nil
    }

    private func stopBridge() {
        if let bridge, bridge.isRunning {
            bridge.terminate()
            bridge.waitUntilExit()
        }
        bridge = nil
    }

    private func quote(_ text: String) -> String {
        "'" + text.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }

    private func fail(_ message: String) {
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = "RX3 Prototype"
        alert.informativeText = message
        alert.alertStyle = .warning
        alert.runModal()
        NSApp.terminate(nil)
    }
}

extension ProcessInfo {
    var machineHardwareName: String {
        var info = utsname()
        uname(&info)
        return withUnsafeBytes(of: &info.machine) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
    }
}

let app = NSApplication.shared
let delegate = AppLauncher()
app.delegate = delegate
app.setActivationPolicy(.regular)
app.run()
