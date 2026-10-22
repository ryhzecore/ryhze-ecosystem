import Foundation
import Security
import SQLite3
import XCTest
@testable import Runner

final class RunnerTests: XCTestCase {
  private let slot = "session"
  private let fixture = Data("ryhze-native-keychain-fixture-v1".utf8)

  private struct RestartFixture: Codable {
    let version: Int
    let processID: Int32
    let payload: Data
  }

  private enum RestartFixtureFailure: Error {
    case missing
    case invalid
  }

  private func validatedRestartFixture(
    _ data: Data?, currentProcessID: Int32
  ) throws -> RestartFixture {
    guard let data = data else { throw RestartFixtureFailure.missing }
    let record = try JSONDecoder().decode(RestartFixture.self, from: data)
    guard record.version == 1,
          record.processID > 0,
          currentProcessID > 0,
          record.processID != currentProcessID,
          record.payload == fixture else {
      throw RestartFixtureFailure.invalid
    }
    return record
  }

  private func scope(_ account: String) throws -> String {
    let json = try JSONSerialization.data(
      withJSONObject: ["ryhze-native-verification", account],
      options: [.fragmentsAllowed, .withoutEscapingSlashes])
    return json.base64EncodedString()
      .replacingOccurrences(of: "+", with: "-")
      .replacingOccurrences(of: "/", with: "_")
  }

  private func snapshot(schema: Int = 2) -> Data {
    // Never encode this through a Swift Dictionary: Swift String equality would
    // merge the two distinct Unicode account keys. Dart owns the account schema.
    let json = #"{"schema":\#(schema),"accounts":{"[\"fixture\",\"caf\u00e9\"]":{},"[\"fixture\",\"cafe\u0301\"]":{}},"preferences":{},"futureField":{"retained":true}}"#
    return Data((" \r\n" + json + "\r\n").utf8)
  }

  private func withDatabase<T>(_ directory: URL, _ body: (OpaquePointer) throws -> T) throws -> T {
    var database: OpaquePointer?
    XCTAssertEqual(sqlite3_open(directory.appendingPathComponent("shared-state-v1.sqlite3").path,
                                &database), SQLITE_OK)
    let handle = try XCTUnwrap(database)
    defer { sqlite3_close_v2(handle) }
    return try body(handle)
  }

  private func databaseInteger(_ handle: OpaquePointer, _ sql: String) throws -> Int64 {
    var statement: OpaquePointer?
    XCTAssertEqual(sqlite3_prepare_v2(handle, sql, -1, &statement, nil), SQLITE_OK)
    let query = try XCTUnwrap(statement)
    defer { sqlite3_finalize(query) }
    XCTAssertEqual(sqlite3_step(query), SQLITE_ROW)
    return sqlite3_column_int64(query, 0)
  }

  private func seedLegacyDatabase(_ directory: URL, payload: Data) throws {
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    try withDatabase(directory) { handle in
      // Build the exact previous on-disk table. Hex encodes synthetic fixture
      // bytes; no provider data or executable SQL is accepted by this helper.
      let hex = payload.map { String(format: "%02x", $0) }.joined()
      XCTAssertEqual(sqlite3_exec(handle, """
        CREATE TABLE shared_state (
          singleton INTEGER PRIMARY KEY NOT NULL CHECK (singleton = 1),
          revision INTEGER NOT NULL CHECK (revision > 0), payload BLOB NOT NULL
        ) WITHOUT ROWID;
        INSERT INTO shared_state VALUES (1, 4, X'\(hex)');
        PRAGMA user_version=1;
        """, nil, nil, nil), SQLITE_OK)
    }
  }

  private func migrationBackups(_ directory: URL) throws -> [(Int64, Int64, Data)] {
    return try withDatabase(directory) { handle in
      var statement: OpaquePointer?
      XCTAssertEqual(sqlite3_prepare_v2(handle,
        "SELECT source_revision, source_schema, payload FROM migration_backups ORDER BY source_revision",
        -1, &statement, nil), SQLITE_OK)
      let query = try XCTUnwrap(statement)
      defer { sqlite3_finalize(query) }
      var result: [(Int64, Int64, Data)] = []
      var status = sqlite3_step(query)
      while status == SQLITE_ROW {
        let bytes = try XCTUnwrap(sqlite3_column_blob(query, 2))
        let value = Data(bytes: bytes, count: Int(sqlite3_column_bytes(query, 2)))
        result.append((sqlite3_column_int64(query, 0), sqlite3_column_int64(query, 1), value))
        status = sqlite3_step(query)
      }
      XCTAssertEqual(status, SQLITE_DONE)
      return result
    }
  }

  private func keychainQuery(_ account: String) -> [String: Any] {
    var query: [String: Any] = [
      kSecClass as String: kSecClassGenericPassword,
      kSecAttrService as String: "com.ryhze.ryhze.apple.credentials.v1",
      kSecAttrAccount as String: account + "." + slot
    ]
    #if os(macOS)
    query[kSecUseDataProtectionKeychain as String] = true
    #endif
    return query
  }

  func testKeychainRoundTripAndScopeIsolation() throws {
    let store = AppleCredentialStore()
    let first = try scope("roundtrip-" + UUID().uuidString)
    let second = try scope("other-" + UUID().uuidString)
    defer { try? store.delete(scope: first, slot: slot) }
    XCTAssertNil(try store.read(scope: first, slot: slot))
    try store.write(scope: first, slot: slot, value: fixture)
    XCTAssertEqual(try store.read(scope: first, slot: slot), fixture)
    let replacement = Data("updated-native-fixture".utf8)
    try store.write(scope: first, slot: slot, value: replacement)
    XCTAssertEqual(try store.read(scope: first, slot: slot), replacement)
    try store.write(scope: first, slot: slot, value: fixture)
    XCTAssertThrowsError(try store.write(scope: first, slot: slot, value: Data()))
    XCTAssertThrowsError(try store.write(scope: first, slot: slot,
                                         value: Data(repeating: 1, count: 65537)))
    let deviceScope = Data("[\"local\",\"device\"]".utf8).base64EncodedString()
    XCTAssertThrowsError(try store.write(scope: deviceScope, slot: slot, value: fixture))
    XCTAssertNil(try store.read(scope: second, slot: slot))
    try store.delete(scope: second, slot: slot)
    XCTAssertEqual(try store.read(scope: first, slot: slot), fixture)
    try store.delete(scope: first, slot: slot)
    XCTAssertNil(try store.read(scope: first, slot: slot))

    // Updating an existing entry must also tighten its accessibility policy.
    var weak = keychainQuery(first)
    weak[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
    weak[kSecValueData as String] = fixture
    XCTAssertEqual(SecItemAdd(weak as CFDictionary, nil), errSecSuccess)
    try store.write(scope: first, slot: slot, value: replacement)
    var query = keychainQuery(first)
    query[kSecReturnAttributes as String] = true
    query[kSecMatchLimit as String] = kSecMatchLimitOne
    var result: CFTypeRef?
    XCTAssertEqual(SecItemCopyMatching(query as CFDictionary, &result), errSecSuccess)
    let attributes = try XCTUnwrap(result as? [String: Any])
    XCTAssertEqual(attributes[kSecAttrAccessible as String] as? String,
                   kSecAttrAccessibleWhenUnlockedThisDeviceOnly as String)
  }

  // These two tests MUST run in separate xcodebuild invocations, seed first.
  // The persisted PID proves a different host process read the Keychain value;
  // it does not establish forced-crash, device-reboot or physical-device recovery.
  // PID reuse fails conservatively. No real member data is used.
  func testKeychainSeed() throws {
    let processID = ProcessInfo.processInfo.processIdentifier
    XCTAssertGreaterThan(processID, 0)
    let record = RestartFixture(version: 1, processID: processID, payload: fixture)
    let encoded = try JSONEncoder().encode(record)

    // Exercise rejection in this existing gate, including reuse of the same
    // process. A fresh per-test UUID would not establish a process boundary.
    XCTAssertThrowsError(try validatedRestartFixture(nil, currentProcessID: processID))
    XCTAssertThrowsError(try validatedRestartFixture(
      Data("{malformed".utf8), currentProcessID: processID))
    XCTAssertThrowsError(try validatedRestartFixture(
      Data(#"{"version":1}"#.utf8), currentProcessID: processID))
    XCTAssertThrowsError(try validatedRestartFixture(encoded, currentProcessID: processID))
    let otherProcessID: Int32 = processID == Int32.max ? processID - 1 : processID + 1
    let validOther = try JSONEncoder().encode(
      RestartFixture(version: 1, processID: otherProcessID, payload: fixture))
    XCTAssertEqual(try validatedRestartFixture(
      validOther, currentProcessID: processID).processID, otherProcessID)
    for invalid in [
      RestartFixture(version: 2, processID: otherProcessID, payload: fixture),
      RestartFixture(version: 1, processID: 0, payload: fixture),
      RestartFixture(version: 1, processID: -1, payload: fixture),
      RestartFixture(version: 1, processID: otherProcessID, payload: Data("wrong-fixture".utf8))
    ] {
      XCTAssertThrowsError(try validatedRestartFixture(
        JSONEncoder().encode(invalid), currentProcessID: processID))
    }

    let store = AppleCredentialStore()
    let account = try scope("process-restart-fixture")
    try store.delete(scope: account, slot: slot)
    // One Keychain value keeps the real process identity and fixture together.
    try store.write(scope: account, slot: slot, value: encoded)
    XCTAssertEqual(try store.read(scope: account, slot: slot), encoded)
  }

  func testKeychainRestoreAfterProcessRestart() throws {
    let store = AppleCredentialStore()
    let account = try scope("process-restart-fixture")
    // Throw before deletion if data is absent/malformed, the PID is unchanged,
    // the version is unsupported or the original fixture was not preserved.
    let restored = try validatedRestartFixture(
      store.read(scope: account, slot: slot),
      currentProcessID: ProcessInfo.processInfo.processIdentifier)
    XCTAssertEqual(restored.payload, fixture,
                   "A different native process must restore the exact Keychain fixture.")
    try store.delete(scope: account, slot: slot)
    XCTAssertNil(try store.read(scope: account, slot: slot))
  }

  func testDurableSnapshotReopensAndRejectsStaleWrite() throws {
    let directory = FileManager.default.temporaryDirectory
      .appendingPathComponent("ryhze-native-tests-" + UUID().uuidString, isDirectory: true)
    defer { try? FileManager.default.removeItem(at: directory) }
    do {
      let store = try AppleStateStore(directoryURL: directory)
      let empty = try store.read()
      XCTAssertEqual((empty["revision"] as? NSNumber)?.int64Value, 0)
      XCTAssertTrue(empty["value"] is NSNull)
      let written = try store.compareAndSet(
        expectedRevision: 0, value: snapshot())
      XCTAssertEqual((written["revision"] as? NSNumber)?.int64Value, 1)
      XCTAssertThrowsError(try store.compareAndSet(
        expectedRevision: 0, value: snapshot())) { error in
        XCTAssertEqual((error as? AppleBridgeFailure)?.code, "conflict")
      }
      let invalid = Data("{\"schema\":2,\"accounts\":[],\"preferences\":{}}".utf8)
      XCTAssertThrowsError(try store.compareAndSet(
        expectedRevision: 1, value: invalid)) { error in
        XCTAssertEqual((error as? AppleBridgeFailure)?.code, "invalidArguments")
      }
      for schema in ["true", "2.0", "\"2\""] {
        let malformed = Data("{\"schema\":\(schema),\"accounts\":{},\"preferences\":{}}".utf8)
        XCTAssertThrowsError(try store.compareAndSet(expectedRevision: 1, value: malformed))
      }
    }
    let reopened = try AppleStateStore(directoryURL: directory)
    let read = try reopened.read()
    XCTAssertEqual((read["revision"] as? NSNumber)?.int64Value, 1)
    XCTAssertEqual(try XCTUnwrap(read["value"] as? Data), snapshot(),
                   "Raw JSON must preserve every account identity byte-for-byte.")
  }

  func testUnsupportedSchemaIsPreserved() throws {
    let directory = FileManager.default.temporaryDirectory
      .appendingPathComponent("ryhze-newer-schema-" + UUID().uuidString, isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: directory) }
    let path = directory.appendingPathComponent("shared-state-v1.sqlite3").path
    var database: OpaquePointer?
    XCTAssertEqual(sqlite3_open(path, &database), SQLITE_OK)
    let handle = try XCTUnwrap(database)
    defer { sqlite3_close_v2(handle) }
    XCTAssertEqual(sqlite3_exec(handle,
      "CREATE TABLE future_data(value TEXT); INSERT INTO future_data VALUES ('preserve'); PRAGMA user_version=9;",
      nil, nil, nil), SQLITE_OK)
    XCTAssertThrowsError(try AppleStateStore(directoryURL: directory)) { error in
      XCTAssertEqual((error as? AppleBridgeFailure)?.code, "unsupportedSchema")
    }
    var statement: OpaquePointer?
    XCTAssertEqual(sqlite3_prepare_v2(handle, "SELECT value FROM future_data", -1,
                                    &statement, nil), SQLITE_OK)
    let query = try XCTUnwrap(statement)
    defer { sqlite3_finalize(query) }
    XCTAssertEqual(sqlite3_step(query), SQLITE_ROW)
    let value = try XCTUnwrap(sqlite3_column_text(query, 0))
    XCTAssertEqual(String(cString: value), "preserve")
  }

  func testAtomicMigrationPreservesBackupAndReopens() throws {
    for schema in [0, 1] {
      let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("ryhze-migration-" + UUID().uuidString, isDirectory: true)
      defer { try? FileManager.default.removeItem(at: directory) }
      let original = snapshot(schema: schema)
      try seedLegacyDatabase(directory, payload: original)
      do {
        let store = try AppleStateStore(directoryURL: directory)
        XCTAssertEqual(try XCTUnwrap(store.read()["value"] as? Data), original)
        XCTAssertTrue(try migrationBackups(directory).isEmpty)
        // Ordinary writes must not erase a legacy document without a backup.
        XCTAssertThrowsError(try store.compareAndSet(expectedRevision: 4, value: snapshot()))
        XCTAssertThrowsError(try store.migrate(
          expectedRevision: 3, expectedValue: original, value: snapshot()))
        XCTAssertThrowsError(try store.migrate(
          expectedRevision: 4, expectedValue: snapshot(schema: 1 - schema), value: snapshot()))
        XCTAssertEqual(try XCTUnwrap(store.read()["value"] as? Data), original)
        XCTAssertTrue(try migrationBackups(directory).isEmpty)
        let migrated = try store.migrate(
          expectedRevision: 4, expectedValue: original, value: snapshot())
        XCTAssertEqual((migrated["revision"] as? NSNumber)?.int64Value, 5)
        XCTAssertEqual(try XCTUnwrap(migrated["value"] as? Data), snapshot())
        XCTAssertThrowsError(try store.migrate(
          expectedRevision: 4, expectedValue: original, value: snapshot()))
        // A later schema-2 commit must retain the original recovery record.
        _ = try store.compareAndSet(expectedRevision: 5, value: snapshot())
      }
      let reopened = try AppleStateStore(directoryURL: directory)
      let restored = try reopened.read()
      XCTAssertEqual((restored["revision"] as? NSNumber)?.int64Value, 6)
      XCTAssertEqual(try XCTUnwrap(restored["value"] as? Data), snapshot())
      let backup = try XCTUnwrap(migrationBackups(directory).first)
      XCTAssertEqual(try migrationBackups(directory).count, 1)
      XCTAssertEqual(backup.0, 4)
      XCTAssertEqual(backup.1, Int64(schema))
      XCTAssertEqual(backup.2, original,
                     "The recovery record must preserve original identities and unknown fields byte-for-byte.")
      try withDatabase(directory) { handle in
        XCTAssertEqual(sqlite3_exec(handle, "DELETE FROM migration_backups", nil, nil, nil),
                       SQLITE_CONSTRAINT)
        XCTAssertEqual(sqlite3_exec(handle, "UPDATE migration_backups SET payload=X'00'", nil, nil, nil),
                       SQLITE_CONSTRAINT)
        XCTAssertEqual(sqlite3_exec(handle,
          "INSERT OR REPLACE INTO migration_backups VALUES(4, 0, X'00')", nil, nil, nil),
          SQLITE_CONSTRAINT)
      }
      XCTAssertEqual(try XCTUnwrap(migrationBackups(directory).first).2, original)
    }
  }

  func testInterruptedMigrationRollsBackBothWrites() throws {
    #if DEBUG
    enum FixtureInterruption: Error { case beforeCommit }
    for schema in [0, 1] {
      let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("ryhze-interrupted-migration-" + UUID().uuidString, isDirectory: true)
      defer { try? FileManager.default.removeItem(at: directory) }
      let original = snapshot(schema: schema)
      try seedLegacyDatabase(directory, payload: original)
      var reachedCommitBoundary = false
      do {
        let store = try AppleStateStore(directoryURL: directory, migrationBeforeCommitForTesting: {
          reachedCommitBoundary = true
          throw FixtureInterruption.beforeCommit
        })
        XCTAssertThrowsError(try store.migrate(
          expectedRevision: 4, expectedValue: original, value: snapshot())) { error in
          XCTAssertTrue(error is FixtureInterruption)
        }
      }
      XCTAssertTrue(reachedCommitBoundary,
                    "Inject failure after both writes and before committing the transaction.")
      let reopened = try AppleStateStore(directoryURL: directory)
      let restored = try reopened.read()
      XCTAssertEqual((restored["revision"] as? NSNumber)?.int64Value, 4)
      XCTAssertEqual(try XCTUnwrap(restored["value"] as? Data), original)
      XCTAssertTrue(try migrationBackups(directory).isEmpty)
      let committed = try reopened.migrate(
        expectedRevision: 4, expectedValue: original, value: snapshot())
      XCTAssertEqual((committed["revision"] as? NSNumber)?.int64Value, 5)
      XCTAssertEqual(try migrationBackups(directory).count, 1)
      XCTAssertEqual(try XCTUnwrap(migrationBackups(directory).first).2, original)
    }
    #else
    throw XCTSkip("Migration fault injection is available only in the Debug native verification build.")
    #endif
  }

  func testFutureDocumentSchemaIsPreserved() throws {
    let directory = FileManager.default.temporaryDirectory
      .appendingPathComponent("ryhze-future-document-" + UUID().uuidString, isDirectory: true)
    defer { try? FileManager.default.removeItem(at: directory) }
    let original = snapshot(schema: 3)
    try seedLegacyDatabase(directory, payload: original)
    XCTAssertThrowsError(try AppleStateStore(directoryURL: directory).read()) { error in
      XCTAssertEqual((error as? AppleBridgeFailure)?.code, "unsupportedSchema")
    }
    try withDatabase(directory) { handle in
      XCTAssertEqual(try databaseInteger(handle, "PRAGMA user_version"), 1)
      XCTAssertEqual(try databaseInteger(handle,
        "SELECT count(*) FROM sqlite_master WHERE name='migration_backups'"), 0)
      var statement: OpaquePointer?
      XCTAssertEqual(sqlite3_prepare_v2(handle,
        "SELECT revision, payload FROM shared_state WHERE singleton=1", -1, &statement, nil), SQLITE_OK)
      let query = try XCTUnwrap(statement)
      defer { sqlite3_finalize(query) }
      XCTAssertEqual(sqlite3_step(query), SQLITE_ROW)
      XCTAssertEqual(sqlite3_column_int64(query, 0), 4)
      let bytes = try XCTUnwrap(sqlite3_column_blob(query, 1))
      XCTAssertEqual(Data(bytes: bytes, count: Int(sqlite3_column_bytes(query, 1))), original)
    }
  }

  func testStateSymlinksAreRejected() throws {
    let parent = FileManager.default.temporaryDirectory
      .appendingPathComponent("ryhze-symlink-tests-" + UUID().uuidString, isDirectory: true)
    try FileManager.default.createDirectory(at: parent, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: parent) }
    let target = parent.appendingPathComponent("outside")
    let bytes = Data("preserve outside target".utf8)
    try bytes.write(to: target)
    let directory = parent.appendingPathComponent("store", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false)
    let databaseLink = directory.appendingPathComponent("shared-state-v1.sqlite3")
    try FileManager.default.createSymbolicLink(at: databaseLink, withDestinationURL: target)
    XCTAssertThrowsError(try AppleStateStore(directoryURL: directory)) { error in
      XCTAssertEqual((error as? AppleBridgeFailure)?.code, "ioFailure")
    }
    XCTAssertEqual(try Data(contentsOf: target), bytes)
    let linkedDirectory = parent.appendingPathComponent("linked-store", isDirectory: true)
    try FileManager.default.createSymbolicLink(at: linkedDirectory, withDestinationURL: directory)
    XCTAssertThrowsError(try AppleStateStore(directoryURL: linkedDirectory)) { error in
      XCTAssertEqual((error as? AppleBridgeFailure)?.code, "ioFailure")
    }
    XCTAssertEqual(try Data(contentsOf: target), bytes)
  }
}
