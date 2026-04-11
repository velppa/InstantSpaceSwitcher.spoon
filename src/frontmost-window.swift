// Prints the title of the most recently focused window for a given application
// using AXFocusedWindow which tracks focus across Spaces.
import AppKit
import ApplicationServices

guard CommandLine.arguments.count > 1 else {
    fputs("Usage: frontmost-window <app-name>\n", stderr)
    exit(1)
}

let appName = CommandLine.arguments[1]
let apps = NSWorkspace.shared.runningApplications.filter { $0.localizedName == appName }
guard let app = apps.first else {
    fputs("\(appName) is not running\n", stderr)
    exit(1)
}

let axApp = AXUIElementCreateApplication(app.processIdentifier)
var focusedWindow: AnyObject?
let result = AXUIElementCopyAttributeValue(axApp, kAXFocusedWindowAttribute as CFString, &focusedWindow)

if result == .success, let window = focusedWindow {
    var title: AnyObject?
    AXUIElementCopyAttributeValue(window as! AXUIElement, kAXTitleAttribute as CFString, &title)
    if let t = title as? String {
        print(t, terminator: "")
    }
}
