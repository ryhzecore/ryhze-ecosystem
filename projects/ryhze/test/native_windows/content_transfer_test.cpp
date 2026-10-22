#include "content_transfer.h"
#include "content_stager.h"
#include <bcrypt.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using ryhze::ContentTransfer;
using ryhze::ContentTransferRequest;
using ryhze::ContentTransferResult;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
ryhze::secure_files::FileDigest Digest(const std::vector<uint8_t>& bytes) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  Check(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "fixture algorithm");
  ryhze::secure_files::FileDigest digest{};
  const auto status = BCryptHash(algorithm, nullptr, 0, const_cast<PUCHAR>(bytes.data()),
    static_cast<ULONG>(bytes.size()), digest.data(), static_cast<ULONG>(digest.size()));
  BCryptCloseAlgorithmProvider(algorithm, 0); Check(status >= 0, "fixture digest"); return digest;
}
struct Fixture {
  fs::path root;
  Fixture() {
    wchar_t temp[MAX_PATH + 1]{};
    Check(GetTempPathW(MAX_PATH, temp) != 0, "fixture temp");
    std::array<uint8_t, 16> random{};
    Check(BCryptGenRandom(nullptr, random.data(), 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0, "fixture random");
    std::wstring name = L"ryhze-transfer-";
    for (auto byte : random) { name += L"0123456789abcdef"[byte >> 4]; name += L"0123456789abcdef"[byte & 15]; }
    root = fs::path(temp) / name;
    Check(CreateDirectoryW(root.c_str(), nullptr) != FALSE, "exclusive fixture");
    fs::create_directory(root / "incoming"); fs::create_directory(root / "content-staging");
    std::wcout << L"Retained native transfer fixture: " << root.wstring() << L'\n';
  }
};
ContentTransferRequest Request(const std::vector<uint8_t>& bytes, uint32_t slot = 0) {
  ContentTransferRequest request; request.slot = slot; request.operation_id = std::string(64, 'a');
  request.expected_size = bytes.size(); request.expected_sha256 = Digest(bytes); return request;
}
ContentTransferRequest Observed(ContentTransferRequest request, const ContentTransferResult& result) {
  Check(result.error.empty(), "successful evidence");
  request.root_identity = result.root_identity; request.directory_identity = result.directory_identity;
  request.source_identity = result.source_identity; request.observed_bytes = result.observed_bytes;
  request.observed_sha256 = result.observed_sha256; return request;
}
void RoundTripAndResume() {
  Fixture fixture;
  std::vector<uint8_t> bytes(ContentTransfer::kChunkBytes + 7);
  for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>(i % 251);
  auto request = Request(bytes);
  ContentTransfer transfer(fixture.root.wstring());
  Check(transfer.Inspect(request).state == "missing" &&
    fs::is_empty(fixture.root / "incoming"), "initial observation performs no filesystem writes");
  auto begin = transfer.Begin(request);
  Check(begin.error.empty() && begin.state == "writing" && begin.source_identity.size() == 49 &&
    begin.directory_identity.size() == 49 && begin.observed_bytes == 0, "exclusive inert writer begin");
  request = Observed(request, begin);
  std::vector<uint8_t> first(bytes.begin(), bytes.begin() + ContentTransfer::kChunkBytes);
  auto appended = transfer.Append(request, first);
  Check(appended.error.empty() && appended.observed_bytes == first.size(), "bounded append actual bytes");
  Check(transfer.Append(request, first).error == "conflict", "duplicate offset cannot duplicate bytes");
  request = Observed(request, appended);
  ContentTransfer contender(fixture.root.wstring());
  Check(contender.Inspect(request).error == "conflict", "writer owns cross-process slot lease");
  transfer.Cancel(); transfer.ResetCancellation();
  Check(transfer.Append(request, {9}).error == "cancelled", "idle cancellation cannot reset live writer");
  transfer.Drain(); transfer.ResetCancellation();
  auto checkpoint = transfer.Inspect(request);
  Check(checkpoint.error.empty() && checkpoint.state == "checkpoint" &&
    checkpoint.observed_bytes == first.size() && checkpoint.source_identity.size() == 49 &&
    checkpoint.observed_sha256.size() == 64, "partial bytes have fresh identity and SHA evidence");
  const auto old = Observed(request, checkpoint);
  auto next = old; next.attempt = 1; next.source_identity.clear(); next.observed_bytes = 0; next.observed_sha256.clear();
  auto resumed = transfer.Begin(next, old);
  Check(resumed.error.empty() && resumed.observed_bytes == first.size() && resumed.source_identity.size() == 49,
    "fresh writer replays pinned validated prefix once");
  next = Observed(next, resumed);
  auto final_append = transfer.Append(next, std::vector<uint8_t>(bytes.begin() + ContentTransfer::kChunkBytes, bytes.end()));
  next = Observed(next, final_append);
  const auto complete = transfer.Finish(next);
  Check(complete.error.empty() && complete.state == "complete" && complete.observed_bytes == bytes.size() &&
    complete.source_identity != old.source_identity, "same-handle complete hash and new file identity");
  next = Observed(next, complete);
  Check(transfer.Inspect(next).state == "complete", "restart revalidation");
  ryhze::ContentStageRequest stage;
  stage.operation_id = Digest({1}); stage.manifest_sha256 = Digest({2});
  stage.expected_sha256 = next.expected_sha256; stage.expected_size = next.expected_size;
  stage.expected_root_identity = next.root_identity; stage.expected_source_identity = next.source_identity;
  stage.source_relative.assign(complete.source_relative.begin(), complete.source_relative.end());
  std::replace(stage.source_relative.begin(), stage.source_relative.end(), '/', '\\');
  ryhze::ContentStager stager(fixture.root.wstring());
  Check(stager.Stage(stage).state == ryhze::ContentStageState::staged, "completed transfer source integrates existing staging");
  Check(transfer.Discard(old).state == "discarded", "explicit old prefix discard");
  const auto repeated = transfer.Discard(old);
  Check(repeated.state == "discarded" && repeated.source_identity == old.source_identity &&
    repeated.observed_bytes == old.observed_bytes && repeated.observed_sha256 == old.observed_sha256,
    "owned discard interruption reconciles exact prior removal intent");
  Check(transfer.Inspect(next).state == "complete", "discard leaves the complete attempt untouched");
  auto wrong = next; wrong.observed_sha256[0] = wrong.observed_sha256[0] == '0' ? '1' : '0';
  Check(transfer.Discard(wrong).error == "recovery", "changed prefix evidence forbids removal");
  Check(transfer.Discard(next).state == "discarded", "exact complete incoming file removal");
  Check(transfer.Inspect(next).state == "missing", "empty retained slot observed missing");
  auto subsequent = Request(bytes); subsequent.operation_id = std::string(64, 'b');
  const auto empty = transfer.Inspect(subsequent);
  Check(empty.state == "missing" && empty.directory_identity == next.directory_identity,
    "new operation observes retained empty directory identity");
  subsequent.root_identity = empty.root_identity; subsequent.directory_identity = empty.directory_identity;
  Check(transfer.Begin(subsequent).state == "writing", "new operation safely reuses empty retained slot");
  transfer.Drain();
}
void HostileAndInvalid() {
  Fixture fixture;
  const std::vector<uint8_t> bytes{1, 2, 3};
  ContentTransfer transfer(fixture.root.wstring());
  auto request = Request(bytes);
  auto bad = request; bad.operation_id = "../outside";
  Check(transfer.Begin(bad).error == "invalidData", "no caller-selected path");
  bad = request; bad.slot = 8;
  Check(transfer.Begin(bad).error == "invalidData", "slot quota");
  bad = request; bad.attempt = 2;
  Check(transfer.Begin(bad).error == "invalidData", "attempt quota");
  bad = request; bad.expected_size = ContentTransfer::kMaximumBytes + 1;
  Check(transfer.Begin(bad).error == "invalidData", "byte quota");
  auto begun = transfer.Begin(request); request = Observed(request, begun);
  Check(transfer.Append(request, std::vector<uint8_t>(ContentTransfer::kChunkBytes + 1)).error == "invalidData", "bounded chunk");
  auto progress = transfer.Append(request, {3, 2, 1}); request = Observed(request, progress);
  Check(transfer.Finish(request).error == "recovery", "wrong content never complete");
  const auto wrong_bytes = transfer.Inspect(request);
  Check(wrong_bytes.state == "checkpoint" && wrong_bytes.observed_bytes == 3, "wrong hash retained for recovery");
  auto observed = Observed(request, wrong_bytes);
  auto orphan = Request(bytes);
  orphan.root_identity = observed.root_identity;
  orphan.directory_identity = observed.directory_identity;
  Check(transfer.Inspect(orphan).error == "recovery", "unacknowledged creation cannot adopt a present filename");
  Check(transfer.Discard(orphan).error == "recovery", "unacknowledged creation cannot remove a present filename");
  auto unobserved = observed; unobserved.source_identity.clear();
  Check(transfer.Discard(unobserved).error == "invalidData", "blind deletion forbidden");
  const auto slot = fixture.root / "incoming" / "transfer-0";
  const auto payload = slot / (std::string(64, 'a') + "-0.bin");
  { std::ofstream unknown(slot / "unowned.txt"); unknown << "preserve me"; }
  Check(transfer.Discard(observed).error == "recovery" && fs::exists(payload), "unknown slot contents preserved");
  Check(fs::exists(slot / "unowned.txt"), "unknown file intact");
  auto different = Request(bytes, 1); different.root_identity = observed.root_identity;
  different.root_identity[0] = different.root_identity[0] == '0' ? '1' : '0';
  Check(transfer.Begin(different).error == "recovery", "device replacement blocks write");
  auto absent = Request(bytes, 2);
  absent.root_identity = observed.root_identity;
  const auto discarded = transfer.Discard(absent);
  Check(discarded.state == "discarded" && discarded.source_identity.empty() &&
    discarded.observed_bytes == 0 && discarded.observed_sha256.empty(), "uncreated reservation reconciles actual absent directory only");
}
void UnhashedCreationAbsence() {
  Fixture fixture;
  ContentTransfer transfer(fixture.root.wstring());
  auto request = Observed(Request({1, 2, 3}), transfer.Begin(Request({1, 2, 3})));
  Check(request.source_identity.size() == 49 && request.observed_sha256.empty(),
    "begin persists identity without claiming a prefix hash");
  transfer.Drain();
  const auto slot = fixture.root / "incoming" / "transfer-0";
  const auto payload = slot / (std::string(64, 'a') + "-0.bin");
  Check(transfer.Discard(request).error == "recovery" && fs::exists(payload),
    "known creation identity alone cannot remove a present file");
  const auto retained_payload = fixture.root / "retained-unhashed.bin";
  Check(MoveFileW(payload.c_str(), retained_payload.c_str()) != FALSE,
    "retain fixture payload while simulating disappearance");
  const auto missing = transfer.Inspect(request);
  Check(missing.state == "missing" && missing.directory_identity == request.directory_identity &&
    missing.source_identity.empty(), "missing known file returns absence without adopting a replacement");
  const auto discarded = transfer.Discard(request);
  Check(discarded.state == "discarded" && discarded.source_identity == request.source_identity &&
    discarded.directory_identity == request.directory_identity && discarded.observed_bytes == 0 &&
    discarded.observed_sha256.empty(), "unhashed creation plan reconciles actual file absence");
  const auto retained_slot = fixture.root / "retained-empty-slot";
  Check(MoveFileW(slot.c_str(), retained_slot.c_str()) != FALSE,
    "retain fixture directory while simulating disappearance");
  const auto missing_directory = transfer.Inspect(request);
  Check(missing_directory.state == "missing" && missing_directory.directory_identity.empty() &&
    missing_directory.source_identity.empty(), "missing directory returns explicit absence proof");
  const auto discarded_directory = transfer.Discard(request);
  Check(discarded_directory.state == "discarded" &&
    discarded_directory.directory_identity == request.directory_identity &&
    discarded_directory.source_identity == request.source_identity &&
    discarded_directory.observed_bytes == 0 && discarded_directory.observed_sha256.empty(),
    "absent directory acknowledgement retains original unhashed removal plan");
  Check(fs::exists(retained_payload) && fs::exists(retained_slot), "retained fixture evidence preserved");
}
void PrefixReplacement() {
  Fixture fixture;
  auto request = Request({1, 2, 3, 4});
  ContentTransfer transfer(fixture.root.wstring());
  request = Observed(request, transfer.Begin(request));
  request = Observed(request, transfer.Append(request, {1, 2}));
  transfer.Drain();
  const auto old = Observed(request, transfer.Inspect(request));
  auto next = old; next.attempt = 1; next.source_identity.clear(); next.observed_bytes = 0; next.observed_sha256.clear();
  const auto path = fixture.root / "incoming" / "transfer-0" / (std::string(64, 'a') + "-0.bin");
  { std::fstream source(path, std::ios::in | std::ios::out | std::ios::binary); source.put(9); }
  Check(transfer.Begin(next, old).error == "recovery", "resume rereads prefix hash before target effects");
  Check(!fs::exists(path.parent_path() / (std::string(64, 'a') + "-1.bin")), "bad prefix does not create next attempt");
  Check(transfer.Discard(old).error == "recovery" && fs::exists(path), "mutated observed prefix not deleted");
}
}  // namespace
int main() {
  try { RoundTripAndResume(); HostileAndInvalid(); UnhashedCreationAbsence(); PrefixReplacement();
    std::cout << "content transfer passed\n"; return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
