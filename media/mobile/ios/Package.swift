// swift-tools-version:5.7
import PackageDescription

// TurboMedia iOS mobile staging metadata.
//
// The imported iOS sources are kept here for API adaptation work. They are not
// yet a buildable Swift package in this repository because the Swift wrapper and
// hardware codec layer still reference the older TurboWebRTC media APIs.

let package = Package(
    name: "TurboMediaIOSStaging",
    platforms: [
        .iOS(.v14),
        .macOS(.v12)
    ],
    products: [],
    targets: []
)
