import Flutter
import UIKit
import CoreFoundation
#if DEBUG && targetEnvironment(simulator)
import Darwin
#endif

@main
@objc class AppDelegate: FlutterAppDelegate, FlutterImplicitEngineDelegate {
  #if DEBUG && targetEnvironment(simulator)
  private let firstFrameProbe = AppleFirstFrameProbe()
  #endif
  override func application(
    _ application: UIApplication,
    didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?
  ) -> Bool {
    return super.application(application, didFinishLaunchingWithOptions: launchOptions)
  }

  func didInitializeImplicitFlutterEngine(_ engineBridge: FlutterImplicitEngineBridge) {
    GeneratedPluginRegistrant.register(with: engineBridge.pluginRegistry)
    ApplePlatformBridge.register(messenger: engineBridge.applicationRegistrar.messenger())
    #if DEBUG && targetEnvironment(simulator)
    firstFrameProbe.register(messenger: engineBridge.applicationRegistrar.messenger())
    #endif
  }
}

#if DEBUG && targetEnvironment(simulator)
/// Explicit local simulator evidence only. Never compiled into a signed device
/// release, never reads credentials and never accepts an output path from Dart.
private final class AppleFirstFrameProbe {
  private let nonce: String?
  private let processID = ProcessInfo.processInfo.processIdentifier
  private var marker: URL?
  private var state = "starting"
  private var bootstrap = "pending"
  private var shellLayout = false
  private var firstFrameRasterized = false
  private var failureCode: String?
  private static let failures: Set<String> = [
    "bootstrap-degraded", "bootstrap-failed", "flutter-error", "async-error",
    "shell-layout", "raster-timeout", "probe-failed"
  ]

  init() {
    let supplied = ProcessInfo.processInfo.environment["RYHZE_FIRST_FRAME_NONCE"]
    if let supplied = supplied, supplied.utf8.count == 32,
       supplied.utf8.allSatisfy({ (48...57).contains($0) || (97...102).contains($0) }) {
      nonce = supplied
    } else {
      nonce = nil
    }
  }

  func register(messenger: FlutterBinaryMessenger) {
    guard let nonce = nonce else { return }
    let channel = FlutterMethodChannel(
      name: "com.ryhze.ryhze/first-frame-probe.v1", binaryMessenger: messenger)
    do {
      let directory = try FileManager.default.url(
        for: .cachesDirectory, in: .userDomainMask, appropriateFor: nil, create: true
      ).resolvingSymlinksInPath()
      marker = directory.appendingPathComponent("ryhze-first-frame-v1.json", isDirectory: false)
      try persist()
    } catch {
      state = "failed"
      failureCode = "probe-failed"
    }
    channel.setMethodCallHandler { [weak self] call, result in
      guard let self = self else {
        result(FlutterError(code: "probe-failed", message: "Probe unavailable.", details: nil))
        return
      }
      do {
        switch call.method {
        case "configuration":
          guard call.arguments == nil || call.arguments is NSNull,
                self.failureCode == nil else { throw ProbeFailure.invalid }
          result(["schemaVersion": 1, "nonce": nonce, "processID": self.processID])
        case "update":
          try self.update(call.arguments)
          result(nil)
        default:
          result(FlutterMethodNotImplemented)
        }
      } catch {
        self.fail("probe-failed")
        try? self.persist()
        result(FlutterError(code: "probe-failed", message: "Probe unavailable.", details: nil))
      }
    }
  }

  private enum ProbeFailure: Error { case invalid }

  private func update(_ value: Any?) throws {
    guard let args = value as? [String: Any],
          Set(args.keys) == ["nonce", "processID", "event", "failureCode"],
          let suppliedNonce = args["nonce"] as? String, suppliedNonce == nonce,
          let suppliedPID = args["processID"] as? NSNumber,
          CFGetTypeID(suppliedPID) != CFBooleanGetTypeID(),
          suppliedPID.compare(NSNumber(value: processID)) == .orderedSame,
          let event = args["event"] as? String else { throw ProbeFailure.invalid }
    let code = args["failureCode"] as? String
    guard code != nil || args["failureCode"] is NSNull else { throw ProbeFailure.invalid }
    if let code = code, !Self.failures.contains(code) { throw ProbeFailure.invalid }
    // A framework/asynchronous failure remains terminal even after a previous
    // ready write. The host rechecks the same nonce/PID after taking its capture.
    if state == "failed" { return }
    switch event {
    case "bootstrap-healthy":
      guard bootstrap == "pending", code == nil else { throw ProbeFailure.invalid }
      bootstrap = "healthy"
    case "bootstrap-degraded":
      guard bootstrap == "pending", code == "bootstrap-degraded" else {
        throw ProbeFailure.invalid
      }
      bootstrap = "degraded"
      fail("bootstrap-degraded")
    case "shell-layout":
      guard bootstrap == "healthy", state == "starting", code == nil else {
        throw ProbeFailure.invalid
      }
      shellLayout = true
    case "first-frame-rasterized":
      guard bootstrap == "healthy", shellLayout, state == "starting", code == nil else {
        throw ProbeFailure.invalid
      }
      firstFrameRasterized = true
      state = "ready"
    case "failed":
      guard let code = code else { throw ProbeFailure.invalid }
      fail(code)
    default:
      throw ProbeFailure.invalid
    }
    try persist()
  }

  private func fail(_ code: String) {
    guard state != "failed" else { return }
    state = "failed"
    failureCode = code
  }

  private func persist() throws {
    guard let marker = marker, let nonce = nonce else { throw ProbeFailure.invalid }
    var attributes = stat()
    if marker.path.withCString({ lstat($0, &attributes) }) == 0 {
      guard attributes.st_mode & mode_t(S_IFMT) == mode_t(S_IFREG) else {
        throw ProbeFailure.invalid
      }
    } else if errno != ENOENT {
      throw ProbeFailure.invalid
    }
    let record: [String: Any] = [
      "schemaVersion": 1, "nonce": nonce, "processID": processID, "state": state,
      "bootstrap": bootstrap, "shellLayout": shellLayout,
      "firstFrameRasterized": firstFrameRasterized,
      "failureCode": failureCode as Any? ?? NSNull()
    ]
    let bytes = try JSONSerialization.data(withJSONObject: record, options: [.sortedKeys])
    guard bytes.count <= 4096 else { throw ProbeFailure.invalid }
    try bytes.write(to: marker, options: [.atomic])
    try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: marker.path)
  }
}
#endif
