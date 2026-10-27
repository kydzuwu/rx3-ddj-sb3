import AppKit

// A small launcher, rather than another copy of the VM or its firmware.
final class Launcher: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        launchOrFocus()
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows: Bool) -> Bool {
        launchOrFocus()
        return false
    }

    private func launchOrFocus() {
        guard let resource = Bundle.main.url(forResource: "Prototype", withExtension: "plist"),
              let data = try? Data(contentsOf: resource),
              let settings = try? PropertyListSerialization.propertyList(from: data, format: nil) as? [String: String],
              let root = settings["RepositoryPath"] else {
            fail("The launcher is missing its prototype location.")
            return
        }
        let command = URL(fileURLWithPath: root).appendingPathComponent("macos/Launch RX3 Prototype.command")
        guard FileManager.default.fileExists(atPath: command.path) else {
            fail("The prototype folder has moved. Reinstall the Dock launcher from its macos folder.")
            return
        }

        // Match the VM's repository as well as its name so another QEMU VM is
        // never selected. NSRunningApplication activation needs no UI scripting.
        let process = Process()
        let output = Pipe()
        process.executableURL = URL(fileURLWithPath: "/bin/ps")
        process.arguments = ["-axo", "pid=,args="]
        process.standardOutput = output
        process.standardError = FileHandle.nullDevice
        do {
            try process.run()
            let bytes = output.fileHandleForReading.readDataToEndOfFile()
            process.waitUntilExit()
            let lines = String(decoding: bytes, as: UTF8.self).split(separator: "\n")
            for line in lines where line.contains("qemu-system-") &&
                line.contains("-name XDJ-RX3 Mac Prototype") &&
                line.contains(root + "/work/macos/vmlinuz") {
                if let pidText = line.split(whereSeparator: { $0.isWhitespace }).first,
                   let pid = Int32(pidText),
                   let app = NSRunningApplication(processIdentifier: pid),
                   app.activate(options: [.activateAllWindows]) {
                    NSApp.terminate(nil)
                    return
                }
            }
        } catch {
            // The existing launcher's instance lock still prevents two VMs.
        }

        // Terminal retains the guest console and diagnostics, just like the
        // existing double-click launcher. QEMU opens fullscreen during boot.
        if NSWorkspace.shared.open(command) {
            NSApp.terminate(nil)
        } else {
            fail("macOS could not open the prototype's Terminal launcher.")
        }
    }

    private func fail(_ message: String) {
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = "RX3 Prototype"
        alert.informativeText = message
        alert.alertStyle = .warning
        alert.runModal()
        NSApp.terminate(nil)
    }
}

let app = NSApplication.shared
let delegate = Launcher()
app.delegate = delegate
app.setActivationPolicy(.regular)
app.run()
