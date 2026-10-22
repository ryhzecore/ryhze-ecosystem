import Foundation
import CoreFoundation
import Security
import SQLite3
import Darwin
#if os(iOS)
import Flutter
#elseif os(macOS)
import FlutterMacOS
#endif

/// Error messages are fixed strings: never return native diagnostics, paths,
/// account identities, state contents, or credentials through the channel.
struct AppleBridgeFailure: Error {
  let code: String
  let message: String

  static let invalidArguments = AppleBridgeFailure(
    code: "invalidArguments", message: "The request is not valid.")
  static let corruptState = AppleBridgeFailure(
    code: "corruptState", message: "Saved state could not be read safely.")
  static let unsupportedSchema = AppleBridgeFailure(
    code: "unsupportedSchema", message: "This saved state version is not supported.")
  static let ioFailure = AppleBridgeFailure(
    code: "ioFailure", message: "The storage operation did not complete.")
}

private enum AppleWire {
  static let maximumPayloadBytes = 4 * 1024 * 1024
  static let maximumSecretBytes = 64 * 1024

  /// The scope is the Dart-produced opaque ASCII key, not a normalized identity.
  /// Base64 canonicalization prevents alternate encodings of those same bytes.
  static func scope(_ value: String) throws {
    let bytes = Array(value.utf8)
    guard !bytes.isEmpty, bytes.count <= 8192,
      bytes.allSatisfy({ (65...90).contains($0) || (97...122).contains($0)
        || (48...57).contains($0) || $0 == 45 || $0 == 95 || $0 == 61 })
    else { throw AppleBridgeFailure.invalidArguments }
    let base64 = value.replacingOccurrences(of: "-", with: "+")
      .replacingOccurrences(of: "_", with: "/")
    guard let decoded = Data(base64Encoded: base64),
      decoded.base64EncodedString().replacingOccurrences(of: "+", with: "-")
        .replacingOccurrences(of: "/", with: "_") == value,
      String(data: decoded, encoding: .utf8) != nil,
      let parts = (try? JSONSerialization.jsonObject(with: decoded)) as? NSArray,
      parts.count == 2,
      let authority = parts[0] as? NSString,
      let account = parts[1] as? NSString
    else { throw AppleBridgeFailure.invalidArguments }
    try identifier(authority)
    try identifier(account)
    guard authority as String != "local" else { throw AppleBridgeFailure.invalidArguments }
  }

  static func dictionary(_ value: Any?) throws -> [String: Any] {
    guard let value = value as? [String: Any] else {
      throw AppleBridgeFailure.invalidArguments
    }
    return value
  }

  static func keys(_ value: [String: Any], _ expected: Set<String>) throws {
    guard Set(value.keys) == expected else { throw AppleBridgeFailure.invalidArguments }
  }

  static func string(_ value: Any?) throws -> String {
    guard let result = value as? String else { throw AppleBridgeFailure.invalidArguments }
    return result
  }

  static func integer(_ value: Any?) throws -> Int64 {
    guard let number = value as? NSNumber,
      CFGetTypeID(number) != CFBooleanGetTypeID(),
      ["c", "s", "i", "l", "q", "C", "S", "I", "L", "Q"].contains(
        String(cString: number.objCType)),
      number.compare(NSNumber(value: Int64.min)) != .orderedAscending,
      number.compare(NSNumber(value: Int64.max)) != .orderedDescending
    else { throw AppleBridgeFailure.invalidArguments }
    return number.int64Value
  }

  private static func identifier(_ value: NSString) throws {
    guard value.length > 0, value.length <= 256 else { throw AppleBridgeFailure.invalidArguments }
    var index = 0
    while index < value.length {
      let unit = value.character(at: index)
      guard unit >= 32, unit != 127 else { throw AppleBridgeFailure.invalidArguments }
      if (0xd800...0xdbff).contains(unit) {
        index += 1
        guard index < value.length, (0xdc00...0xdfff).contains(value.character(at: index)) else {
          throw AppleBridgeFailure.invalidArguments
        }
      } else if (0xdc00...0xdfff).contains(unit) {
        throw AppleBridgeFailure.invalidArguments
      }
      index += 1
    }
  }

  /// Shared Dart validateStoreSnapshot owns account, preference, and job rules.
  /// This is only an envelope check; the parsed objects are never returned or
  /// re-encoded. Swift dictionary equality must not merge raw account-map keys.
  @discardableResult
  static func stateBytes(_ data: Data, stored: Bool = false) throws -> Int64 {
    let malformed = stored ? AppleBridgeFailure.corruptState : .invalidArguments
    guard !data.isEmpty, data.count <= maximumPayloadBytes,
      String(data: data, encoding: .utf8) != nil,
      let object = (try? JSONSerialization.jsonObject(with: data)) as? NSDictionary,
      let schema = try? integer(object["schema"])
    else { throw malformed }
    guard (0...2).contains(schema) else { throw AppleBridgeFailure.unsupportedSchema }
    guard object["accounts"] is NSDictionary, object["preferences"] is NSDictionary else {
      throw malformed
    }
    return schema
  }
}

private enum AppleStatePaths {
  @discardableResult
  static func verify(_ url: URL, directory: Bool, mayBeMissing: Bool) throws -> Bool {
    var attributes = stat()
    let status = url.path.withCString { lstat($0, &attributes) }
    if status != 0 {
      if errno == ENOENT && mayBeMissing { return false }
      throw AppleBridgeFailure.ioFailure
    }
    let expected = directory ? mode_t(S_IFDIR) : mode_t(S_IFREG)
    guard attributes.st_mode & mode_t(S_IFMT) == expected else {
      throw AppleBridgeFailure.ioFailure
    }
    return true
  }

  /// Parent URLs are supplied by the app from its trusted platform directory.
  /// Never create an arbitrary chain of intermediate directories or follow an
  /// existing link at an app-owned directory, even a dangling symbolic link.
  static func prepareDirectory(_ url: URL) throws {
    let exists = try verify(url, directory: true, mayBeMissing: true)
    do {
      if !exists {
        do {
          try FileManager.default.createDirectory(at: url,
            withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
        } catch {
          // A second app engine may have created the same private directory.
          try verify(url, directory: true, mayBeMissing: false)
        }
      }
      try verify(url, directory: true, mayBeMissing: false)
      try FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: url.path)
    } catch { throw AppleBridgeFailure.ioFailure }
  }

  static func verifyDatabaseFiles(_ url: URL) throws {
    for suffix in ["", "-wal", "-shm", "-journal"] {
      try verify(URL(fileURLWithPath: url.path + suffix), directory: false, mayBeMissing: true)
    }
  }
}

/// A single nonsecret document and revision commit together. The lock also makes
/// direct host-test calls safe; the production bridge owns this on its queue.
final class AppleStateStore {
  private var database: OpaquePointer?
  private let lock = NSLock()
  private static let transient = unsafeBitCast(-1, to: sqlite3_destructor_type.self)
  private var migrationBeforeCommit: (() throws -> Void)?
  private static let backupTriggers: [(name: String, sql: String)] = [
    ("migration_backups_no_update", """
      CREATE TRIGGER migration_backups_no_update BEFORE UPDATE ON migration_backups
      BEGIN SELECT RAISE(ABORT, 'immutable migration backup'); END
      """),
    ("migration_backups_no_delete", """
      CREATE TRIGGER migration_backups_no_delete BEFORE DELETE ON migration_backups
      BEGIN SELECT RAISE(ABORT, 'immutable migration backup'); END
      """),
    ("migration_backups_no_replace", """
      CREATE TRIGGER migration_backups_no_replace BEFORE INSERT ON migration_backups
      WHEN EXISTS (SELECT 1 FROM migration_backups WHERE source_revision = NEW.source_revision)
      BEGIN SELECT RAISE(ABORT, 'immutable migration backup'); END
      """),
  ]

  #if DEBUG
  /// Test-only failure point after both migration writes, before the commit.
  /// This initializer and its hook are not exposed through the method channel.
  convenience init(directoryURL: URL, migrationBeforeCommitForTesting: @escaping () throws -> Void) throws {
    try self.init(directoryURL: directoryURL)
    migrationBeforeCommit = migrationBeforeCommitForTesting
  }
  #endif

  init(directoryURL: URL) throws {
    guard directoryURL.isFileURL else { throw AppleBridgeFailure.invalidArguments }
    try AppleStatePaths.prepareDirectory(directoryURL)
    let url = directoryURL.appendingPathComponent("shared-state-v1.sqlite3", isDirectory: false)
    try AppleStatePaths.verifyDatabaseFiles(url)
    let status = sqlite3_open_v2(url.path, &database,
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW, nil)
    guard status == SQLITE_OK else {
      if let handle = database { sqlite3_close_v2(handle) }
      database = nil
      throw Self.failure(status)
    }
    do {
      try AppleStatePaths.verifyDatabaseFiles(url)
      try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: url.path)
      try check(sqlite3_busy_timeout(database, 5000))
      try execute("PRAGMA trusted_schema=OFF")
      guard try scalarText("PRAGMA journal_mode=WAL") == "wal" else {
        throw AppleBridgeFailure.ioFailure
      }
      try execute("PRAGMA synchronous=FULL")
      guard try scalarInteger("PRAGMA synchronous") == 2 else {
        throw AppleBridgeFailure.ioFailure
      }
      // FULL provides WAL durability; these flags request the stronger Apple
      // flush primitive for both commits and checkpoints when available.
      try execute("PRAGMA fullfsync=ON")
      try execute("PRAGMA checkpoint_fullfsync=ON")
      guard try scalarInteger("PRAGMA fullfsync") == 1,
        try scalarInteger("PRAGMA checkpoint_fullfsync") == 1 else {
        throw AppleBridgeFailure.ioFailure
      }
      try execute("BEGIN IMMEDIATE")
      do {
        let version = try scalarInteger("PRAGMA user_version")
        guard (0...2).contains(version) else {
          throw AppleBridgeFailure.unsupportedSchema
        }
        if version == 0 {
          try execute("""
            CREATE TABLE shared_state (
              singleton INTEGER PRIMARY KEY NOT NULL CHECK (singleton = 1),
              revision INTEGER NOT NULL CHECK (revision > 0),
              payload BLOB NOT NULL
            ) WITHOUT ROWID
            """)
        }
        try validateSharedSchema()
        // Validate a legacy envelope before exposing it to Dart or committing a
        // structural upgrade. This does not rewrite the document or its schema.
        _ = try readLocked()
        if version < 2 {
          try execute("""
            CREATE TABLE migration_backups (
              source_revision INTEGER PRIMARY KEY NOT NULL CHECK (source_revision > 0),
              source_schema INTEGER NOT NULL CHECK (source_schema IN (0, 1)),
              payload BLOB NOT NULL
            ) WITHOUT ROWID
            """)
          for trigger in Self.backupTriggers { try execute(trigger.sql) }
          try execute("PRAGMA user_version=2")
        }
        try validateBackupSchema()
        try execute("COMMIT")
      } catch {
        try? execute("ROLLBACK")
        throw error
      }
    } catch {
      if let handle = database { sqlite3_close_v2(handle) }
      database = nil
      if let failure = error as? AppleBridgeFailure { throw failure }
      throw AppleBridgeFailure.ioFailure
    }
  }

  deinit {
    if let handle = database { sqlite3_close_v2(handle) }
  }

  func read() throws -> [String: Any] {
    lock.lock()
    defer { lock.unlock() }
    return try readLocked()
  }

  func compareAndSet(expectedRevision: Int64, value: Data) throws
    -> [String: Any]
  {
    guard expectedRevision >= 0, expectedRevision < Int64.max else {
      throw AppleBridgeFailure.invalidArguments
    }
    guard try AppleWire.stateBytes(value) == 2 else { throw AppleBridgeFailure.unsupportedSchema }
    lock.lock()
    defer { lock.unlock() }
    try execute("BEGIN IMMEDIATE")
    do {
      let current = try readLocked()
      guard try AppleWire.integer(current["revision"]) == expectedRevision else {
        throw AppleBridgeFailure(code: "conflict", message: "Saved state changed. Read it again.")
      }
      if let previous = current["value"] as? Data,
        try AppleWire.stateBytes(previous, stored: true) != 2 {
        throw AppleBridgeFailure.unsupportedSchema
      }
      let revision = expectedRevision + 1
      let statement = try prepare("""
        INSERT INTO shared_state(singleton, revision, payload) VALUES (1, ?, ?)
        ON CONFLICT(singleton) DO UPDATE SET revision=excluded.revision, payload=excluded.payload
        """)
      defer { sqlite3_finalize(statement) }
      try check(sqlite3_bind_int64(statement, 1, revision))
      try bindBlob(value, at: 2, to: statement)
      try check(sqlite3_step(statement), expecting: SQLITE_DONE)
      try execute("COMMIT")
      return ["revision": NSNumber(value: revision), "value": value]
    } catch {
      try? execute("ROLLBACK")
      throw error
    }
  }

  func migrate(expectedRevision: Int64, expectedValue: Data, value: Data) throws -> [String: Any] {
    guard expectedRevision > 0, expectedRevision < Int64.max else {
      throw AppleBridgeFailure.invalidArguments
    }
    let sourceSchema = try AppleWire.stateBytes(expectedValue)
    guard sourceSchema == 0 || sourceSchema == 1,
      try AppleWire.stateBytes(value) == 2 else { throw AppleBridgeFailure.unsupportedSchema }
    lock.lock()
    defer { lock.unlock() }
    try execute("BEGIN IMMEDIATE")
    do {
      let current = try readLocked()
      guard try AppleWire.integer(current["revision"]) == expectedRevision,
        let original = current["value"] as? Data, original == expectedValue else {
        throw AppleBridgeFailure(code: "conflict", message: "Saved state changed. Read it again.")
      }
      // The original bytes and schema belong to the snapshot checked inside this
      // transaction. A plain INSERT cannot overwrite an earlier backup.
      let backup = try prepare("""
        INSERT INTO migration_backups(source_revision, source_schema, payload) VALUES (?, ?, ?)
        """)
      defer { sqlite3_finalize(backup) }
      try check(sqlite3_bind_int64(backup, 1, expectedRevision))
      try check(sqlite3_bind_int64(backup, 2, sourceSchema))
      try bindBlob(original, at: 3, to: backup)
      try check(sqlite3_step(backup), expecting: SQLITE_DONE)
      let revision = expectedRevision + 1
      let update = try prepare("UPDATE shared_state SET revision = ?, payload = ? WHERE singleton = 1")
      defer { sqlite3_finalize(update) }
      try check(sqlite3_bind_int64(update, 1, revision))
      try bindBlob(value, at: 2, to: update)
      try check(sqlite3_step(update), expecting: SQLITE_DONE)
      guard sqlite3_changes(database) == 1 else { throw AppleBridgeFailure.corruptState }
      try migrationBeforeCommit?()
      try execute("COMMIT")
      return ["revision": NSNumber(value: revision), "value": value]
    } catch {
      try? execute("ROLLBACK")
      throw error
    }
  }

  private func readLocked() throws -> [String: Any] {
    let statement = try prepare("SELECT revision, payload FROM shared_state WHERE singleton = 1")
    defer { sqlite3_finalize(statement) }
    let status = sqlite3_step(statement)
    if status == SQLITE_DONE { return ["revision": NSNumber(value: Int64(0)), "value": NSNull()] }
    try check(status, expecting: SQLITE_ROW)
    guard sqlite3_column_type(statement, 0) == SQLITE_INTEGER,
      sqlite3_column_type(statement, 1) == SQLITE_BLOB else {
      throw AppleBridgeFailure.corruptState
    }
    let revision = sqlite3_column_int64(statement, 0)
    let bytes = sqlite3_column_blob(statement, 1)
    let length = Int(sqlite3_column_bytes(statement, 1))
    guard revision > 0, length > 0, length <= AppleWire.maximumPayloadBytes,
      let bytes = bytes else {
      throw AppleBridgeFailure.corruptState
    }
    let payload = Data(bytes: bytes, count: length)
    try AppleWire.stateBytes(payload, stored: true)
    return ["revision": NSNumber(value: revision), "value": payload]
  }

  private func validateSharedSchema() throws {
    // user_version alone is insufficient: a missing/replaced table must fail
    // bootstrap. This database contains one exact shared StoreData byte string.
    try validateTable("shared_state", names: ["singleton", "revision", "payload"],
      types: ["INTEGER", "INTEGER", "BLOB"])
    guard try scalarInteger("SELECT count(*) FROM sqlite_master WHERE type='trigger' AND tbl_name='shared_state'") == 0,
      try scalarInteger("SELECT count(*) FROM shared_state WHERE singleton != 1") == 0,
      try scalarText("PRAGMA quick_check(1)") == "ok"
    else { throw AppleBridgeFailure.corruptState }
  }

  private func validateBackupSchema() throws {
    try validateTable("migration_backups", names: ["source_revision", "source_schema", "payload"],
      types: ["INTEGER", "INTEGER", "BLOB"])
    guard try scalarInteger("SELECT count(*) FROM sqlite_master WHERE type='trigger' AND tbl_name='migration_backups'") == Int64(Self.backupTriggers.count) else {
      throw AppleBridgeFailure.corruptState
    }
    for trigger in Self.backupTriggers {
      let storedSQL = try scalarText("SELECT sql FROM sqlite_master WHERE type='trigger' AND name='\(trigger.name)'")
      guard storedSQL == trigger.sql else { throw AppleBridgeFailure.corruptState }
    }
  }

  private func validateTable(_ table: String, names: [String], types: [String]) throws {
    // Table names originate only from the fixed schema above, never the channel.
    let columns = try prepare("PRAGMA table_info(\(table))")
    defer { sqlite3_finalize(columns) }
    for index in 0..<names.count {
      guard sqlite3_step(columns) == SQLITE_ROW,
        sqlite3_column_int(columns, 0) == Int32(index),
        try columnText(columns, index: 1) == names[index],
        try columnText(columns, index: 2) == types[index],
        sqlite3_column_int(columns, 3) == 1,
        sqlite3_column_type(columns, 4) == SQLITE_NULL,
        sqlite3_column_int(columns, 5) == (index == 0 ? 1 : 0)
      else { throw AppleBridgeFailure.corruptState }
    }
    guard sqlite3_step(columns) == SQLITE_DONE else { throw AppleBridgeFailure.corruptState }
    let primaryKey = try prepare("PRAGMA index_xinfo(sqlite_autoindex_\(table)_1)")
    defer { sqlite3_finalize(primaryKey) }
    for index in 0..<names.count {
      guard sqlite3_step(primaryKey) == SQLITE_ROW,
        sqlite3_column_int(primaryKey, 1) == Int32(index),
        try columnText(primaryKey, index: 2) == names[index],
        sqlite3_column_int(primaryKey, 3) == 0,
        try columnText(primaryKey, index: 4) == "BINARY",
        sqlite3_column_int(primaryKey, 5) == (index == 0 ? 1 : 0)
      else { throw AppleBridgeFailure.corruptState }
    }
    guard sqlite3_step(primaryKey) == SQLITE_DONE else { throw AppleBridgeFailure.corruptState }
  }

  private func columnText(_ statement: OpaquePointer, index: Int32) throws -> String {
    guard sqlite3_column_type(statement, index) == SQLITE_TEXT,
      let bytes = sqlite3_column_text(statement, index) else {
      throw AppleBridgeFailure.corruptState
    }
    let data = Data(bytes: bytes, count: Int(sqlite3_column_bytes(statement, index)))
    guard let result = String(data: data, encoding: .utf8) else {
      throw AppleBridgeFailure.corruptState
    }
    return result
  }

  private func prepare(_ sql: String) throws -> OpaquePointer {
    var statement: OpaquePointer?
    let status = sqlite3_prepare_v2(database, sql, -1, &statement, nil)
    guard status == SQLITE_OK, let result = statement else {
      if let statement = statement { sqlite3_finalize(statement) }
      throw Self.failure(status)
    }
    return result
  }

  private func execute(_ sql: String) throws {
    try check(sqlite3_exec(database, sql, nil, nil, nil))
  }

  private func scalarText(_ sql: String) throws -> String {
    let statement = try prepare(sql)
    defer { sqlite3_finalize(statement) }
    try check(sqlite3_step(statement), expecting: SQLITE_ROW)
    guard let result = sqlite3_column_text(statement, 0) else {
      throw AppleBridgeFailure.corruptState
    }
    return String(cString: result)
  }

  private func scalarInteger(_ sql: String) throws -> Int64 {
    let statement = try prepare(sql)
    defer { sqlite3_finalize(statement) }
    try check(sqlite3_step(statement), expecting: SQLITE_ROW)
    guard sqlite3_column_type(statement, 0) == SQLITE_INTEGER else {
      throw AppleBridgeFailure.corruptState
    }
    return sqlite3_column_int64(statement, 0)
  }

  private func bindBlob(_ bytes: Data, at index: Int32, to statement: OpaquePointer) throws {
    let status = bytes.withUnsafeBytes { (buffer: UnsafeRawBufferPointer) in
      sqlite3_bind_blob(statement, index, buffer.baseAddress, Int32(buffer.count), Self.transient)
    }
    try check(status)
  }

  private func check(_ status: Int32, expecting: Int32 = SQLITE_OK) throws {
    guard status == expecting else { throw Self.failure(status) }
  }

  private static func failure(_ status: Int32) -> AppleBridgeFailure {
    switch status & 0xff {
    case SQLITE_CORRUPT, SQLITE_NOTADB, SQLITE_SCHEMA: return .corruptState
    default: return .ioFailure
    }
  }
}

/// Secrets stay in an app-specific Keychain service. There is no plaintext
/// fallback, synchronisation, broad-delete operation, or delete-before-update.
final class AppleCredentialStore {
  private static let service = "com.ryhze.ryhze.apple.credentials.v1"

  func read(scope: String, slot: String) throws -> Data? {
    var query = try identity(scope: scope, slot: slot)
    query[kSecReturnData as String] = true
    query[kSecMatchLimit as String] = kSecMatchLimitOne
    var result: CFTypeRef?
    let status = SecItemCopyMatching(query as CFDictionary, &result)
    if status == errSecItemNotFound { return nil }
    try check(status)
    guard let data = result as? Data, !data.isEmpty,
      data.count <= AppleWire.maximumSecretBytes else {
      throw AppleBridgeFailure.corruptState
    }
    return data
  }

  func write(scope: String, slot: String, value: Data) throws {
    guard !value.isEmpty, value.count <= AppleWire.maximumSecretBytes else {
      throw AppleBridgeFailure.invalidArguments
    }
    let query = try identity(scope: scope, slot: slot)
    let attributes = [
      kSecValueData as String: value,
      kSecAttrAccessible as String: kSecAttrAccessibleWhenUnlockedThisDeviceOnly,
    ] as CFDictionary
    var status = SecItemUpdate(query as CFDictionary, attributes)
    if status == errSecItemNotFound {
      var item = try identity(scope: scope, slot: slot)
      item[kSecAttrAccessible as String] = kSecAttrAccessibleWhenUnlockedThisDeviceOnly
      item[kSecValueData as String] = value
      status = SecItemAdd(item as CFDictionary, nil)
      // Another engine/process can create the same exact item after our update.
      if status == errSecDuplicateItem {
        status = SecItemUpdate(query as CFDictionary, attributes)
      }
    }
    try check(status)
  }

  func delete(scope: String, slot: String) throws {
    let status = SecItemDelete(try identity(scope: scope, slot: slot) as CFDictionary)
    if status == errSecItemNotFound { return }
    try check(status)
  }

  private func identity(scope: String, slot: String) throws -> [String: Any] {
    try AppleWire.scope(scope)
    guard slot == "session" else { throw AppleBridgeFailure.invalidArguments }
    var query: [String: Any] = [
      kSecClass as String: kSecClassGenericPassword,
      kSecAttrService as String: Self.service,
      kSecAttrAccount as String: scope + "." + slot,
      kSecAttrSynchronizable as String: false,
    ]
    #if os(macOS)
    if #available(macOS 10.15, *) {
      query[kSecUseDataProtectionKeychain as String] = true
    } else {
      throw AppleBridgeFailure(code: "unavailable", message: "Secure storage is not available.")
    }
    #endif
    return query
  }

  private func check(_ status: OSStatus) throws {
    switch status {
    case errSecSuccess: return
    case errSecInteractionNotAllowed:
      throw AppleBridgeFailure(code: "locked", message: "Secure storage is locked.")
    case errSecNotAvailable:
      throw AppleBridgeFailure(code: "unavailable", message: "Secure storage is not available.")
    case errSecAuthFailed, errSecUserCanceled, errSecMissingEntitlement:
      throw AppleBridgeFailure(code: "denied", message: "Secure storage access was denied.")
    default: throw AppleBridgeFailure.ioFailure
    }
  }
}

final class ApplePlatformBridge {
  private let queue = DispatchQueue(label: "com.ryhze.ryhze.apple.v1", qos: .utility)
  private let credentials = AppleCredentialStore()
  private var state: AppleStateStore?

  static func register(messenger: FlutterBinaryMessenger) {
    let bridge = ApplePlatformBridge()
    let channel = FlutterMethodChannel(name: "com.ryhze.ryhze/apple.v1", binaryMessenger: messenger)
    channel.setMethodCallHandler { call, result in
      bridge.queue.async {
        do {
          let value = try bridge.handle(call)
          DispatchQueue.main.async { result(value) }
        } catch let failure as AppleBridgeFailure {
          DispatchQueue.main.async {
            result(FlutterError(code: failure.code, message: failure.message, details: nil))
          }
        } catch {
          DispatchQueue.main.async {
            result(FlutterError(code: "ioFailure", message: "The operation did not complete.", details: nil))
          }
        }
      }
    }
  }

  private func handle(_ call: FlutterMethodCall) throws -> Any? {
    switch call.method {
    case "bootstrap":
      guard call.arguments == nil || call.arguments is NSNull else {
        throw AppleBridgeFailure.invalidArguments
      }
      _ = try stateStore()
      #if os(iOS)
      let platform = "ios"
      #else
      let platform = "macos"
      #endif
      return ["schemaVersion": 1, "platform": platform,
        "secureCredentials": true, "durableState": true]
    case "state.read":
      let arguments = try AppleWire.dictionary(call.arguments)
      try AppleWire.keys(arguments, [])
      return try stateReply(stateStore().read())
    case "state.compareAndSet":
      let arguments = try AppleWire.dictionary(call.arguments)
      try AppleWire.keys(arguments, ["expectedRevision", "value"])
      guard let value = arguments["value"] as? FlutterStandardTypedData,
        value.type == FlutterStandardTypedData(bytes: Data()).type else {
          throw AppleBridgeFailure.invalidArguments
        }
      return try stateReply(stateStore().compareAndSet(
        expectedRevision: AppleWire.integer(arguments["expectedRevision"]), value: value.data))
    case "state.migrate":
      let arguments = try AppleWire.dictionary(call.arguments)
      try AppleWire.keys(arguments, ["expectedRevision", "expectedValue", "value"])
      guard let original = arguments["expectedValue"] as? FlutterStandardTypedData,
        let value = arguments["value"] as? FlutterStandardTypedData,
        original.type == FlutterStandardTypedData(bytes: Data()).type,
        value.type == FlutterStandardTypedData(bytes: Data()).type else {
          throw AppleBridgeFailure.invalidArguments
        }
      return try stateReply(stateStore().migrate(
        expectedRevision: AppleWire.integer(arguments["expectedRevision"]),
        expectedValue: original.data, value: value.data))
    case "credentials.read":
      let arguments = try AppleWire.dictionary(call.arguments)
      try AppleWire.keys(arguments, ["scope", "slot"])
      guard let value = try credentials.read(scope: AppleWire.string(arguments["scope"]),
        slot: AppleWire.string(arguments["slot"])) else { return nil }
      return FlutterStandardTypedData(bytes: value)
    case "credentials.write":
      let arguments = try AppleWire.dictionary(call.arguments)
      try AppleWire.keys(arguments, ["scope", "slot", "value"])
      guard let value = arguments["value"] as? FlutterStandardTypedData,
        value.type == FlutterStandardTypedData(bytes: Data()).type else {
          throw AppleBridgeFailure.invalidArguments
        }
      try credentials.write(scope: AppleWire.string(arguments["scope"]),
        slot: AppleWire.string(arguments["slot"]), value: value.data)
      return nil
    case "credentials.delete":
      let arguments = try AppleWire.dictionary(call.arguments)
      try AppleWire.keys(arguments, ["scope", "slot"])
      try credentials.delete(scope: AppleWire.string(arguments["scope"]),
        slot: AppleWire.string(arguments["slot"]))
      return nil
    default: return FlutterMethodNotImplemented
    }
  }

  private func stateReply(_ snapshot: [String: Any]) -> [String: Any] {
    var reply = snapshot
    if let bytes = snapshot["value"] as? Data {
      reply["value"] = FlutterStandardTypedData(bytes: bytes)
    }
    return reply
  }

  private func stateStore() throws -> AppleStateStore {
    if let state = state { return state }
    do {
      let applicationSupport = try FileManager.default.url(for: .applicationSupportDirectory,
        in: .userDomainMask, appropriateFor: nil, create: true).resolvingSymlinksInPath()
      // Resolve only the trusted OS-provided base (for example /var ->
      // /private/var). Reject links at each app-owned component below it.
      let root = applicationSupport.appendingPathComponent("com.ryhze.ryhze", isDirectory: true)
      try AppleStatePaths.prepareDirectory(root)
      let directory = root.appendingPathComponent("apple-v1", isDirectory: true)
      let store = try AppleStateStore(directoryURL: directory)
      state = store
      return store
    } catch let failure as AppleBridgeFailure { throw failure }
    catch { throw AppleBridgeFailure.ioFailure }
  }
}
