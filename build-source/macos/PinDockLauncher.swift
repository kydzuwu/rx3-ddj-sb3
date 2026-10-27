import Foundation
import CoreFoundation

guard CommandLine.arguments.count == 2 else {
    fputs("Usage: pin-dock-launcher APP_PATH\n", stderr)
    exit(2)
}
let appURL = URL(fileURLWithPath: CommandLine.arguments[1]).standardizedFileURL
guard FileManager.default.fileExists(atPath: appURL.appendingPathComponent("Contents/MacOS/RX3Launcher").path) else {
    fputs("The RX3 launcher is missing.\n", stderr)
    exit(1)
}
let domain = "com.apple.dock" as CFString
let key = "persistent-apps" as CFString
let bundleID = "local.rbp.rx3-prototype.launcher"
let oldItems = CFPreferencesCopyAppValue(key, domain) as? [[String: Any]] ?? []
let kept = oldItems.filter { item in
    guard let tile = item["tile-data"] as? [String: Any] else { return true }
    if tile["bundle-identifier"] as? String == bundleID { return false }
    guard let file = tile["file-data"] as? [String: Any],
          let location = file["_CFURLString"] as? String else { return true }
    let url = location.hasPrefix("file:") ? URL(string: location) : URL(fileURLWithPath: location)
    return url?.standardizedFileURL.path != appURL.path
}
// CoreFoundation retains native integer types. OpenStep plist strings passed to
// defaults -array-add had turned these numbers into strings, breaking the URL.
var tileData: [String: Any] = [
        "bundle-identifier": bundleID,
        "file-label": "RX3 Prototype",
        "file-type": 41,
        "file-data": [
            "_CFURLString": appURL.absoluteString,
            "_CFURLStringType": 15,
        ],
]
do {
    let bookmark = try appURL.bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil)
    var stale = false
    let resolved = try URL(resolvingBookmarkData: bookmark, options: [.withoutUI], relativeTo: nil, bookmarkDataIsStale: &stale)
    guard !stale && resolved.standardizedFileURL.path == appURL.path else {
        fputs("The app's Dock bookmark could not be verified.\n", stderr)
        exit(1)
    }
    tileData["book"] = bookmark
} catch {
    fputs("Could not create the app's Dock bookmark: \(error)\n", stderr)
    exit(1)
}
let tile: [String: Any] = [
    "GUID": UInt32.random(in: 1...UInt32.max),
    "tile-type": "file-tile",
    "tile-data": tileData,
]
CFPreferencesSetAppValue(key, (kept + [tile]) as CFArray, domain)
guard CFPreferencesAppSynchronize(domain) else {
    fputs("Could not save the Dock shortcut.\n", stderr)
    exit(1)
}
print("Preserved \(kept.count) Dock apps; replaced the RX3 shortcut with native metadata.")
