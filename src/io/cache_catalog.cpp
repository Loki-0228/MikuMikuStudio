#include "mikumikustudio/cache_catalog.hpp"
#include "cache_catalog_codec.hpp"
#include "cache_catalog_intent.hpp"
#include "cache_files_crypto.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <utility>

namespace mikumikustudio::cache_catalog {
struct WritePlanState {
    WriteKind kind = WriteKind::Initialize;
    WriteFootprint footprint{};
    Snapshot expected;
    std::vector<std::uint8_t> bytes, checkpointBytes, operationBytes;
    cache_files::Owner owner{};
    cache_files::Directory root;
    cache_files::DirectoryIdentity identity{};
    cache_files::BlobInfo intentFile, decisionFile, recoveryHead;
    RecoveryAction recoveryAction = RecoveryAction::None;
    Snapshot recoveryTarget;
    cache_files::ReadLease recoveryTargetLease;
    PublicationApproval recoveryApproval;
    bool requiresApproval = false, hasRecoveryApproval = false;
    bool existingLocks = false, recoveryCheckpoint = false, recoveryInitialization = false;
    std::uint64_t recoveryRevision = 0;
    bool hasIntent = false, hasDecision = false, pendingOperation = false;
    bool valid = false, consumed = false, attempted = false, operating = false;
};
WritePlan::WritePlan() noexcept = default;
WritePlan::~WritePlan() = default;
WritePlan::WritePlan(WritePlan&& other) noexcept {
    if(!other.state_||!other.state_->operating)state_=std::move(other.state_);
}
WritePlan& WritePlan::operator=(WritePlan&& other) noexcept {
    if(this!=&other&&(!state_||!state_->operating)&&(!other.state_||!other.state_->operating))
        state_=std::move(other.state_);
    return *this;
}
bool WritePlan::IsValid() const noexcept { return state_ && state_->valid; }
WriteKind WritePlan::Kind() const noexcept { return state_ ? state_->kind : WriteKind::Initialize; }
const WriteFootprint& WritePlan::Footprint() const noexcept { static const WriteFootprint empty{}; return state_ ? state_->footprint : empty; }
RecoveryAction WritePlan::Recovery() const noexcept {
    return IsValid()&&state_->kind==WriteKind::Recovery?state_->recoveryAction:RecoveryAction::None;
}
const Snapshot& WritePlan::RecoveryTarget() const noexcept {
    static const Snapshot empty;
    return IsValid()&&state_->kind==WriteKind::Recovery?state_->recoveryTarget:empty;
}
const cache_files::DirectoryIdentity& WritePlan::RootIdentity() const noexcept {
    static const cache_files::DirectoryIdentity empty{};
    return IsValid()?state_->identity:empty;
}
bool WritePlan::IsConsumed() const noexcept { return state_ && state_->consumed; }
bool WritePlan::RequiresApproval() const noexcept { return IsValid()&&state_->requiresApproval; }
const PublicationApproval* WritePlan::RecoveryApproval() const noexcept {
    return IsValid()&&state_->kind==WriteKind::Recovery&&state_->hasRecoveryApproval?&state_->recoveryApproval:nullptr;
}
bool WritePlan::MayHaveWritten() const noexcept { return state_ && state_->attempted; }
bool WritePlan::RecoveryIsCheckpoint() const noexcept {
    return IsValid()&&state_->kind==WriteKind::Recovery&&state_->recoveryCheckpoint;
}
bool WritePlan::RecoveryIsInitialization() const noexcept {
    return IsValid()&&state_->kind==WriteKind::Recovery&&state_->recoveryInitialization;
}
Result ApprovedCheckpointFootprint(std::size_t count,WriteFootprint& output) noexcept {
    WriteFootprint value;const auto result=CheckpointFootprint(count,value);if(result!=Result::Ok)return result;
    const auto bytes=cache_files::HeaderBytes+intent::ApprovedCheckpointRecordSize(count);
    value.files[value.count++]={WriteRole::Intent,bytes};
    value.files[value.count++]={WriteRole::RollbackDecision,bytes};output=value;return Result::Ok;
}
Result MaximumApprovedCheckpointFootprint(WriteFootprint& output) noexcept {
    return ApprovedCheckpointFootprint(codec::MaximumRetired,output);
}
Result PublicationFootprint(std::size_t bytes, WriteFootprint& output) noexcept {
    if(bytes>MaximumSnapshotBytes)return Result::Limit;
    WriteFootprint value;
    value.files[0]={WriteRole::Snapshot,cache_files::HeaderBytes+bytes};
    value.files[1]={WriteRole::Commit,cache_files::HeaderBytes+codec::RecordBytes};
    value.files[2]={WriteRole::Intent,cache_files::HeaderBytes+intent::MaximumBytes};
    value.files[3]={WriteRole::RollbackDecision,cache_files::HeaderBytes+intent::MaximumBytes};
    value.count=4;output=value;return Result::Ok;
}
Result ApprovedPublicationFootprint(std::size_t bytes,WriteFootprint& output) noexcept {
    WriteFootprint value;const auto result=PublicationFootprint(bytes,value);if(result!=Result::Ok)return result;
    value.files[2].maximumFileBytes=cache_files::HeaderBytes+intent::ApprovedRecordBytes;
    value.files[3].maximumFileBytes=cache_files::HeaderBytes+intent::ApprovedRecordBytes;
    output=value;return Result::Ok;
}
Result CheckpointFootprint(std::size_t count, WriteFootprint& output) noexcept {
    if(!count)return Result::Invalid;
    if(count>codec::MaximumRetired)return Result::Limit;
    WriteFootprint value;
    value.files[0]={WriteRole::Checkpoint,cache_files::HeaderBytes+codec::CheckpointBaseBytes+160*count};
    value.count=1;output=value;return Result::Ok;
}
Result MaximumCheckpointFootprint(WriteFootprint& output) noexcept {
    return CheckpointFootprint(codec::MaximumRetired,output);
}
namespace {
namespace files = cache_files;
bool Equal(const files::BlobInfo& a, const files::BlobInfo& b) noexcept {
    return a.name == b.name && a.spec.owner == b.spec.owner && a.spec.kind == b.spec.kind &&
        a.spec.key == b.spec.key && a.payloadBytes == b.payloadBytes && a.payloadDigest == b.payloadDigest &&
        a.identity.volume == b.identity.volume && a.identity.fileIndex == b.identity.fileIndex &&
        a.identity.fileBytes == b.identity.fileBytes;
}
bool Equal(const files::DirectoryIdentity& a, const files::DirectoryIdentity& b) noexcept {
    return a.volume == b.volume && a.fileIndex == b.fileIndex;
}
std::uint64_t LogicalBytes(const WriteFootprint& value) noexcept {
    std::uint64_t result=0;
    for(std::uint32_t i=0;i<value.count;++i){
        if(value.files[i].maximumFileBytes>UINT64_MAX-result)return UINT64_MAX;
        result+=value.files[i].maximumFileBytes;
    }
    return result;
}
bool BindFootprint(WritePlanState& plan,const WriteFootprint& value,bool planning) noexcept {
    if(planning){plan.footprint=value;return true;}
    if(plan.footprint.count!=value.count)return false;
    for(std::uint32_t i=0;i<value.count;++i)
        if(plan.footprint.files[i].role!=value.files[i].role||
           plan.footprint.files[i].maximumFileBytes!=value.files[i].maximumFileBytes)return false;
    return true;
}
Result BindRoot(WritePlanState& plan,const files::Directory& root,
                const files::DirectoryIdentity& identity,bool planning) {
    if(!planning)return Equal(plan.identity,identity)?Result::Ok:Result::Conflict;
    const auto shared=root.Share(plan.root);
    if(shared!=files::Result::Ok)return Result::Io;
    plan.identity=identity;return Result::Ok;
}
void Attempt(WritePlanState& plan) noexcept {plan.attempted=true;plan.consumed=true;}
Result Convert(files::Result value) noexcept {
    switch (value) {
    case files::Result::Ok: return Result::Ok;
    case files::Result::Missing: return Result::Missing;
    case files::Result::Invalid: return Result::Invalid;
    case files::Result::Busy: return Result::Busy;
    case files::Result::Limit: return Result::Limit;
    case files::Result::Io: return Result::Io;
    default: return Result::RecoveryRequired;
    }
}
Result StoredError(files::Result value) noexcept {
    if (value == files::Result::Missing || value == files::Result::Invalid || value == files::Result::Limit)
        return Result::RecoveryRequired;
    return Convert(value);
}
// Opening the metadata root is not a stored-record failure. An invalid path or
// an invalid/moved-from directory binding is always Invalid; every other
// failure keeps the classification of the operation that opened the root.
Result RootOpenError(files::Result value) noexcept {
    return value == files::Result::Invalid ? Result::Invalid : Convert(value);
}
Result RootRecoveryError(files::Result value) noexcept {
    return value == files::Result::Invalid ? Result::Invalid : StoredError(value);
}
bool IsSnapshot(const files::BlobInfo& info) noexcept { return info.spec.kind == files::Kind::CatalogSnapshot; }
bool SameLocation(const files::BlobInfo& a, const files::BlobInfo& b) noexcept {
    return IsSnapshot(a) == IsSnapshot(b) && a.name == b.name;
}
struct StoredFile {
    files::BlobInfo info;
    files::ReadLease lease;
    bool retired = false;
};
struct Loaded {
    bool allSnapshotsReferenced = true;
    std::uint64_t revision = 0, metadataBytes = 0;
    std::size_t snapshots = 0, commits = 0;
    files::Owner owner{};
    files::BlobInfo head;
    files::BlobInfo currentSnapshot;
    std::vector<std::uint8_t> bytes;
    std::vector<StoredFile> files;
};
struct Node {
    codec::Record record;
    files::BlobInfo head;
    std::vector<files::BlobInfo> retired;
    bool checkpoint = false;
};

// A checkpoint retires exact identities, never a filename prefix or a range of
// revisions. Every physically present file is still verified before use/removal.
Result Load(const files::Directory& snapshots, const files::Directory& commits, Loaded& output,
    const intent::Record* excluded = nullptr, const files::WritePlan* excludedCheckpoint = nullptr) {
    std::vector<std::wstring> commitNames, snapshotNames;
    auto io = commits.EnumerateAll(MaximumCommits + 1, commitNames);
    if (io != files::Result::Ok) return Convert(io);
    io = snapshots.EnumerateAll(MaximumSnapshots, snapshotNames);
    if (io != files::Result::Ok) return Convert(io);
    // Only recovery supplies exclusions, after pinning the precise identities
    // from a verified intent. Ordinary reads never skip damaged metadata.
    if (excluded) {
        commitNames.erase(std::remove(commitNames.begin(), commitNames.end(), excluded->commit.name), commitNames.end());
        snapshotNames.erase(std::remove(snapshotNames.begin(), snapshotNames.end(), excluded->snapshot.name), snapshotNames.end());
    }
    if(excludedCheckpoint)commitNames.erase(std::remove(commitNames.begin(),commitNames.end(),excludedCheckpoint->name),commitNames.end());
    if (commitNames.empty()) return snapshotNames.empty() ? Result::Missing : Result::RecoveryRequired;
    Loaded candidate; candidate.commits = commitNames.size(); candidate.snapshots = snapshotNames.size();
    candidate.files.reserve(commitNames.size() + snapshotNames.size());
    std::vector<Node> nodes; nodes.reserve(commitNames.size());
    for (const auto& name : commitNames) {
        files::ReadLease lease;
        io = commits.Inspect(name, codec::NamespaceOwner, codec::MaximumCheckpointBytes, lease);
        if (io != files::Result::Ok) return StoredError(io);
        const auto& info = lease.Info();
        const bool checkpoint = info.spec.kind == files::Kind::CatalogCheckpoint;
        if (checkpoint ? info.spec.key != codec::CheckpointKey :
            (info.spec.kind != files::Kind::CatalogCommit || info.spec.key != codec::CommitKey))
            return Result::RecoveryRequired;
        std::vector<std::uint8_t> bytes;
        io = lease.ReadAll(bytes);
        if (io != files::Result::Ok) return StoredError(io);
        Node node;
        node.checkpoint = checkpoint;
        codec::Result decoded;
        if (checkpoint) {
            codec::CheckpointRecord record;
            decoded = codec::DecodeCheckpoint(bytes.data(), bytes.size(), record);
            node.record = std::move(record.state);
            node.retired = std::move(record.retired);
        } else decoded = codec::Decode(bytes.data(), bytes.size(), node.record);
        if (decoded != codec::Result::Ok)
            return decoded == codec::Result::Io ? Result::Io : Result::RecoveryRequired;
        node.head = lease.Info();
        candidate.metadataBytes += node.head.identity.fileBytes;
        nodes.push_back(std::move(node));
        candidate.files.push_back({lease.Info(), std::move(lease), false});
    }
    std::sort(nodes.begin(), nodes.end(), [](const auto& a, const auto& b) {
        return a.record.revision < b.record.revision;
    });
    const Node* checkpoint = nullptr;
    candidate.owner = nodes.front().record.owner;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].record.owner != candidate.owner ||
            (i && nodes[i].record.revision == nodes[i-1].record.revision)) return Result::RecoveryRequired;
        if (nodes[i].checkpoint) checkpoint = &nodes[i];
    }
    using Location = std::pair<bool, std::wstring>;
    std::map<Location, const files::BlobInfo*> retired;
    if (checkpoint) for (const auto& info : checkpoint->retired)
        retired.emplace(Location{IsSnapshot(info), info.name}, &info);
    std::vector<const Node*> active;
    active.reserve(nodes.size());
    for (const auto& node : nodes) {
        const auto found = retired.find(Location{false, node.head.name});
        if (found != retired.end()) {
            if (!checkpoint || &node == checkpoint || node.record.revision >= checkpoint->record.revision ||
                !Equal(*found->second, node.head)) return Result::RecoveryRequired;
            if (Equal(node.head, checkpoint->record.previous) &&
                (node.record.revision == (std::numeric_limits<std::uint64_t>::max)() ||
                 node.record.revision + 1 != checkpoint->record.revision ||
                 !Equal(node.record.snapshot, checkpoint->record.snapshot))) return Result::RecoveryRequired;
        } else {
            if (node.checkpoint && &node != checkpoint) return Result::RecoveryRequired;
            active.push_back(&node);
        }
    }
    if (active.empty() || (checkpoint ? active.front() != checkpoint : active.front()->record.revision != 1))
        return Result::RecoveryRequired;
    for (std::size_t i = 1; i < active.size(); ++i)
        if (active[i-1]->record.revision == (std::numeric_limits<std::uint64_t>::max)() ||
            active[i]->record.revision != active[i-1]->record.revision + 1 ||
            !Equal(active[i]->record.previous, active[i-1]->head)) return Result::RecoveryRequired;
    if (!checkpoint && nodes.size() > MaximumCommits) return Result::Limit;
    const auto allowance = MaximumMetadataBytes + (checkpoint ? MaintenanceReserveBytes : 0);
    if (candidate.metadataBytes > allowance) return Result::Limit;
    std::map<std::wstring, files::BlobInfo> knownSnapshots;
    for (const auto& name : snapshotNames) {
        files::ReadLease lease;
        io = snapshots.Inspect(name, candidate.owner, MaximumSnapshotBytes, lease);
        if (io != files::Result::Ok) return StoredError(io);
        const auto& info = lease.Info();
        if (info.spec.kind != files::Kind::CatalogSnapshot || info.spec.key != codec::SnapshotKey)
            return Result::RecoveryRequired;
        if (info.identity.fileBytes > allowance - candidate.metadataBytes) return Result::Limit;
        candidate.metadataBytes += info.identity.fileBytes;
        knownSnapshots.emplace(name, info);
        candidate.files.push_back({info, std::move(lease), false});
    }
    // Only the checkpoint operation may consume the reserved record/bytes.
    // An ordinary tail cannot be accepted merely because an older base exists.
    if ((candidate.commits > MaximumCommits || candidate.metadataBytes > MaximumMetadataBytes) &&
        (!checkpoint || active.back() != checkpoint)) return Result::RecoveryRequired;
    for (auto& file : candidate.files) {
        const auto found = retired.find(Location{IsSnapshot(file.info), file.info.name});
        if (found != retired.end()) {
            if (!Equal(*found->second, file.info)) return Result::RecoveryRequired;
            file.retired = true;
        }
    }
    for (const auto* node : active) {
        const auto found = knownSnapshots.find(node->record.snapshot.name);
        if (found == knownSnapshots.end() || !Equal(found->second, node->record.snapshot) ||
            retired.count(Location{true, node->record.snapshot.name}))
            return Result::RecoveryRequired;
    }
    // Validation of an envelope is not ownership. Only exact references in
    // the verified chain or an existing checkpoint retirement list establish
    // authority for approved maintenance to retire a snapshot.
    for(const auto& file:candidate.files)if(IsSnapshot(file.info)&&!file.retired){
        const bool referenced=std::any_of(active.begin(),active.end(),[&](const auto* node){
            return Equal(node->record.snapshot,file.info);
        });
        if(!referenced)candidate.allSnapshotsReferenced=false;
    }
    files::ReadLease latest;
    io = snapshots.Acquire(active.back()->record.snapshot, latest);
    if (io != files::Result::Ok) return StoredError(io);
    io = latest.ReadAll(candidate.bytes);
    if (io != files::Result::Ok) return StoredError(io);
    candidate.revision = active.back()->record.revision;
    candidate.head = active.back()->head;
    candidate.currentSnapshot = active.back()->record.snapshot;
    output = std::move(candidate);
    return Result::Ok;
}

Result OpenChildren(const files::Directory& root, bool create,
    files::Directory& snapshots, files::Directory& commits,
    files::Directory* operationDirectory = nullptr, bool allowPending = false) {
    std::vector<std::wstring> rootNames;
    auto rootResult = root.EnumerateAll(5, rootNames);
    if (rootResult == files::Result::Limit) return Result::RecoveryRequired;
    if (rootResult != files::Result::Ok) return Convert(rootResult);
    for (const auto& name : rootNames)
        if (name != L".lock" && name != L"snapshots" && name != L"commits" && name != L"operations") return Result::RecoveryRequired;
    files::Directory operations;
    const auto opened = root.Child(L"operations", create, operations);
    if (opened != files::Result::Ok && opened != files::Result::Missing) return StoredError(opened);
    if (opened == files::Result::Ok) {
        std::vector<std::wstring> names;
        const auto listed = operations.EnumerateAll(2, names);
        if (listed != files::Result::Ok) return StoredError(listed);
        if (!allowPending && !names.empty()) return Result::RecoveryRequired;
        if (operationDirectory) *operationDirectory = std::move(operations);
    }
    const auto a = root.Child(L"snapshots", create, snapshots);
    const auto b = root.Child(L"commits", create, commits);
    if (a == files::Result::Ok && b == files::Result::Ok) return Result::Ok;
    if (a != files::Result::Ok && a != files::Result::Missing) return StoredError(a);
    if (b != files::Result::Ok && b != files::Result::Missing) return StoredError(b);
    // An absent half is only empty if the other half contains no generations.
    if (a == files::Result::Ok || b == files::Result::Ok) {
        std::vector<std::wstring> names;
        const auto result = (a == files::Result::Ok ? snapshots : commits).EnumerateAll(1, names);
        if (result == files::Result::Limit) return Result::RecoveryRequired;
        if (result != files::Result::Ok) return Convert(result);
        if (!names.empty()) return Result::RecoveryRequired;
    }
    return Result::Missing;
}

constexpr files::Digest RollbackKey{5};
struct PendingOperation {
    intent::Record record;
    PublicationApproval approval;
    intent::ApprovedCheckpointRecord checkpointRecord;
    bool checkpoint=false;
    std::vector<std::uint8_t> bytes;
    files::BlobInfo intentFile, decisionFile;
    bool hasIntent = false, hasDecision = false, approved = false;
};
Result ReadOperation(const files::Directory& operations, PendingOperation& out) {
    std::vector<std::wstring> names;
    const auto listed = operations.EnumerateAll(2, names);
    if (listed != files::Result::Ok) return StoredError(listed);
    if (names.empty()) return Result::Missing;
    PendingOperation candidate;
    for (const auto& name : names) {
        files::ReadLease lease;
        auto io = operations.Inspect(name, codec::NamespaceOwner, intent::MaximumOperationBytes, lease);
        if (io != files::Result::Ok) return StoredError(io);
        const auto& info = lease.Info();
        const bool checkpoint=info.spec.key==intent::ApprovedCheckpointKey||info.spec.key==intent::ApprovedCheckpointRollbackKey;
        const bool approved=checkpoint||info.spec.key==intent::ApprovedKey||info.spec.key==intent::ApprovedRollbackKey;
        if (info.spec.kind != files::Kind::CatalogIntent ||
            (!approved&&info.spec.key != intent::Key && info.spec.key != RollbackKey)) return Result::RecoveryRequired;
        const bool decision = info.spec.key == RollbackKey||info.spec.key==intent::ApprovedRollbackKey||info.spec.key==intent::ApprovedCheckpointRollbackKey;
        if (decision ? candidate.hasDecision : candidate.hasIntent) return Result::RecoveryRequired;
        std::vector<std::uint8_t> bytes;
        io = lease.ReadAll(bytes);
        if (io != files::Result::Ok) return StoredError(io);
        intent::Record decoded;intent::ApprovedRecord authorized;intent::ApprovedCheckpointRecord maintenance;
        const auto result = checkpoint?intent::DecodeApprovedCheckpoint(bytes.data(),bytes.size(),maintenance):approved?intent::DecodeApproved(bytes.data(),bytes.size(),authorized):
            intent::Decode(bytes.data(), bytes.size(), decoded);
        if (result != intent::Result::Ok) return result == intent::Result::Io ? Result::Io : Result::RecoveryRequired;
        if(approved)decoded=std::move(authorized.operation);
        if (candidate.hasIntent || candidate.hasDecision) {
            if (checkpoint!=candidate.checkpoint||approved!=candidate.approved||bytes != candidate.bytes) return Result::RecoveryRequired;
        } else {
            candidate.record = std::move(decoded);
            candidate.bytes = std::move(bytes);
            candidate.approved=approved;candidate.checkpoint=checkpoint;
            if(checkpoint){candidate.approval=maintenance.approval;candidate.checkpointRecord=std::move(maintenance);}
            else if(approved)candidate.approval=authorized.approval;
        }
        if (decision) { candidate.hasDecision = true; candidate.decisionFile = info; }
        else { candidate.hasIntent = true; candidate.intentFile = info; }
    }
    out = std::move(candidate);
    return Result::Ok;
}
Result RemoveOperationFile(const files::Directory& directory, const files::BlobInfo& file) {
    const auto removed = directory.Remove(file);
    if (removed != files::Result::DeletePending && removed != files::Result::Missing) return StoredError(removed);
    files::ReadLease remains;
    const auto absent = directory.Inspect(file.name, file.spec.owner, file.payloadBytes, remains);
    return absent == files::Result::Missing ? Result::Ok :
        absent == files::Result::Ok ? Result::RecoveryRequired : StoredError(absent);
}
Result WriteOperation(const files::Directory& operations, const std::vector<std::uint8_t>& bytes,
    bool decision, files::BlobInfo& output,bool approved=false,bool checkpoint=false) {
    files::Spec spec;
    spec.owner = codec::NamespaceOwner; spec.kind = files::Kind::CatalogIntent;
    spec.key = checkpoint?(decision?intent::ApprovedCheckpointRollbackKey:intent::ApprovedCheckpointKey):approved?(decision?intent::ApprovedRollbackKey:intent::ApprovedKey):(decision ? RollbackKey : intent::Key);
    files::BlobInfo file;
    auto io = operations.Create(spec, bytes.data(), bytes.size(), file);
    if (io != files::Result::Ok) return Convert(io);
    files::ReadLease lease;
    io = operations.Acquire(file, lease);
    if (io != files::Result::Ok) return StoredError(io);
    std::vector<std::uint8_t> observed;
    io = lease.ReadAll(observed);
    if (io != files::Result::Ok) return StoredError(io);
    if (observed != bytes) return Result::RecoveryRequired;
    output = std::move(file);
    return Result::Ok;
}
Result FlushOperationFile(const files::Directory& directory, const files::BlobInfo& file) {
    files::WritePlan plan;
    plan.name = file.name; plan.spec = file.spec; plan.payloadDigest = file.payloadDigest;
    plan.payloadBytes = file.payloadBytes; plan.createdIdentity = file.identity;
    plan.createdIdentity.fileBytes = 0; plan.completedBytes = file.identity.fileBytes;
    files::PreparedWrite pinned;
    const auto opened = directory.ReopenPrepared(plan, pinned);
    if (opened != files::Result::Ok) return StoredError(opened);
    return StoredError(pinned.Flush());
}
std::uint64_t PublicationBytes(const intent::Record& operation,bool approved) noexcept {
    // Reserve both records before preparing any file, including the decision
    // needed if publication must later roll back. No checkpoint reserve here.
    WriteFootprint value;
    const auto result=approved?ApprovedPublicationFootprint(static_cast<std::size_t>(operation.snapshot.payloadBytes),value):
        PublicationFootprint(static_cast<std::size_t>(operation.snapshot.payloadBytes),value);
    return result==Result::Ok?
        LogicalBytes(value):UINT64_MAX;
}
Result ApprovalDirectories(const PublicationApproval& approval,const files::Directory& catalog,
    const files::Directory& snapshots,const files::Directory& commits,const files::Directory& operations) {
    const files::Directory* actual[]{&catalog,&snapshots,&commits,&operations};
    const ApprovalArea areas[]{ApprovalArea::Catalog,ApprovalArea::Snapshots,ApprovalArea::Commits,ApprovalArea::Operations};
    for(unsigned i=0;i<4;++i){
        files::DirectoryIdentity identity;const auto io=actual[i]->GetIdentity(identity);
        if(io!=files::Result::Ok)return Convert(io);
        if(!Equal(identity,approval.directories[static_cast<std::size_t>(areas[i])]))return Result::Conflict;
    }
    return Result::Ok;
}
}

namespace {
// Temporary opt-in diagnosis of the real Store::Commit call chain. Disabled
// unless MMS_CACHE_COMMIT_STAGE_TRACE is exactly "1". Store::Commit installs a
// caller-owned record for the current thread; PreparePlan/Execute/Publish only
// annotate that record. Direct plan/execute calls, Checkpoint, Recover, other
// stores and other threads stay inert when no record is installed. Nothing
// here allocates, opens files, retries or recovers, and every existing return
// value and cleanup order is preserved. One line is emitted only after the plan
// and its handles are gone, and only when the real Commit returned non-Ok.
enum class CommitTraceDomain : unsigned char {
    None, Catalog, Files, Codec, Intent, Predicate, Exception
};

struct CommitStageRecord {
    bool enabled = false;
    unsigned long long callId = 0;
    const void* store = nullptr;
    bool bound = false;
    const wchar_t* rootText = nullptr;
    std::size_t rootLength = 0;
    // Inputs are frozen before PlanCommit: expected and output may alias.
    std::uint64_t expectedRevision = 0;
    std::uint64_t payloadBytes = 0;
    bool execution = false;
    const char* stage = "commit.enter";
    bool failed = false;
    CommitTraceDomain domain = CommitTraceDomain::None;
    long long raw = 0;
    const char* rawName = "-";
    bool exception = false;
    bool commitAttempted = false;
    bool writeAttempted = false;
    Result finalResult = Result::Ok;
};

thread_local CommitStageRecord* activeCommitStage = nullptr;
// Thread-safe and monotonic for this process; advanced only when enabled.
std::atomic<unsigned long long> commitStageCallSequence{0};

// The first recorded failure freezes stage/phase/attempted. Cleanup layers and
// enclosing functions must not move the reported location or state.
void SetCommitStage(const char* value) noexcept {
    auto* record = activeCommitStage;
    if (!record || record->failed) return;
    record->stage = value;
}
void SetCommitStagePhase(bool execution) noexcept {
    auto* record = activeCommitStage;
    if (!record || record->failed) return;
    record->execution = execution;
}
void MarkCommitWriteAttempted() noexcept {
    auto* record = activeCommitStage;
    if (!record || record->failed) return;
    record->writeAttempted = true;
}
void MarkCommitAttempted() noexcept {
    auto* record = activeCommitStage;
    if (!record || record->failed) return;
    record->commitAttempted = true;
}
const char* CommitTraceDomainName(CommitTraceDomain value) noexcept {
    switch (value) {
    case CommitTraceDomain::Catalog: return "catalog";
    case CommitTraceDomain::Files: return "files";
    case CommitTraceDomain::Codec: return "codec";
    case CommitTraceDomain::Intent: return "intent";
    case CommitTraceDomain::Predicate: return "predicate";
    case CommitTraceDomain::Exception: return "exception";
    default: return "none";
    }
}
const char* CommitTraceResultName(Result value) noexcept {
    switch (value) {
    case Result::Ok: return "Ok";
    case Result::Missing: return "Missing";
    case Result::Invalid: return "Invalid";
    case Result::Busy: return "Busy";
    case Result::Io: return "Io";
    case Result::Conflict: return "Conflict";
    case Result::Limit: return "Limit";
    case Result::RecoveryRequired: return "RecoveryRequired";
    case Result::Indeterminate: return "Indeterminate";
    default: return "Unknown";
    }
}
const char* CommitTraceFilesResultName(files::Result value) noexcept {
    switch (value) {
    case files::Result::Ok: return "Ok";
    case files::Result::Missing: return "Missing";
    case files::Result::Invalid: return "Invalid";
    case files::Result::Corrupt: return "Corrupt";
    case files::Result::Busy: return "Busy";
    case files::Result::Io: return "Io";
    case files::Result::Conflict: return "Conflict";
    case files::Result::Limit: return "Limit";
    case files::Result::DeletePending: return "DeletePending";
    case files::Result::Cancelled: return "Cancelled";
    case files::Result::CleanupRequired: return "CleanupRequired";
    default: return "Unknown";
    }
}
const char* CommitTraceCodecResultName(codec::Result value) noexcept {
    switch (value) {
    case codec::Result::Ok: return "Ok";
    case codec::Result::Invalid: return "Invalid";
    case codec::Result::Io: return "Io";
    default: return "Unknown";
    }
}
// Only the first actual failure of this Commit is kept: once a precise stage
// has failed, later outer layers and cleanup cannot overwrite it.
void RecordCommitFailure(CommitTraceDomain domain, long long raw, const char* name) noexcept {
    auto* record = activeCommitStage;
    if (!record || record->failed) return;
    record->failed = true;
    record->domain = domain;
    record->raw = raw;
    record->rawName = name ? name : "-";
}
void RecordCommitCatalog(Result value) noexcept {
    RecordCommitFailure(CommitTraceDomain::Catalog, static_cast<long long>(value), CommitTraceResultName(value));
}
void RecordCommitFiles(files::Result value) noexcept {
    RecordCommitFailure(CommitTraceDomain::Files, static_cast<long long>(value), CommitTraceFilesResultName(value));
}
void RecordCommitCodec(codec::Result value) noexcept {
    RecordCommitFailure(CommitTraceDomain::Codec, static_cast<long long>(value), CommitTraceCodecResultName(value));
}
void RecordCommitIntent(codec::Result value) noexcept {
    RecordCommitFailure(CommitTraceDomain::Intent, static_cast<long long>(value), CommitTraceCodecResultName(value));
}
void RecordCommitPredicate(bool value) noexcept {
    RecordCommitFailure(CommitTraceDomain::Predicate, value ? 1 : 0, value ? "true" : "false");
}
void RecordCommitException() noexcept {
    auto* record = activeCommitStage;
    if (!record) return;
    record->exception = true;
    RecordCommitFailure(CommitTraceDomain::Exception, 0, "-");
}
bool PutCommitPathByte(char* output, std::size_t capacity, std::size_t& used, char value) noexcept {
    if (used + 1 >= capacity) return false;
    output[used++] = value;
    return true;
}
// Fixed-buffer rendering of the Store's existing root_ spelling. It never
// queries or normalizes the path and never allocates; characters outside
// printable ASCII (and quote/backslash) become \uXXXX so one record stays one
// line. Capacity exhaustion is reported through truncated.
void EncodeCommitPath(const wchar_t* text, std::size_t length, char* output,
                      std::size_t capacity, bool& truncated) noexcept {
    truncated = false;
    if (!capacity) return;
    std::size_t used = 0;
    output[0] = 0;
    static const char digits[] = "0123456789abcdef";
    for (std::size_t i = 0; i < length; ++i) {
        const std::uint32_t unit = static_cast<std::uint32_t>(static_cast<std::uint16_t>(text[i]));
        if (unit >= 0x20 && unit <= 0x7e && unit != '"' && unit != '\\') {
            if (!PutCommitPathByte(output, capacity, used, static_cast<char>(unit))) { truncated = true; break; }
        } else {
            if (used + 6 >= capacity) { truncated = true; break; }
            output[used++] = '\\';
            output[used++] = 'u';
            output[used++] = digits[(unit >> 12) & 0xf];
            output[used++] = digits[(unit >> 8) & 0xf];
            output[used++] = digits[(unit >> 4) & 0xf];
            output[used++] = digits[unit & 0xf];
        }
    }
    output[used] = 0;
}
void PrintCommitStage(const CommitStageRecord& record) noexcept {
    if (!record.enabled || record.finalResult == Result::Ok) return;
    // The trace itself must not rewrite the caller-visible GetLastError/errno
    // of the failed Commit. Read the Win32 error before the CRT-backed errno.
    const DWORD savedError = GetLastError();
    const int savedErrno = errno;
    char root[512];
    bool rootTruncated = false;
    EncodeCommitPath(record.rootText, record.rootLength, root, sizeof(root), rootTruncated);
    char line[1200];
    const int written = std::snprintf(line, sizeof(line),
        "CACHE_COMMIT_STAGE call=%llu store=%p bound=%d root=\"%s\" root_truncated=%d "
        "expected_revision=%llu bytes=%llu phase=%s stage=%s source=%s sub=%lld(%s) "
        "result=%s(%d) exception=%d commit_attempted=%d write_attempted=%d\n",
        record.callId, const_cast<void*>(record.store), record.bound ? 1 : 0, root,
        rootTruncated ? 1 : 0,
        static_cast<unsigned long long>(record.expectedRevision),
        static_cast<unsigned long long>(record.payloadBytes),
        record.execution ? "execution" : "planning", record.stage,
        CommitTraceDomainName(record.domain), record.raw, record.rawName,
        CommitTraceResultName(record.finalResult), static_cast<int>(record.finalResult),
        record.exception ? 1 : 0, record.commitAttempted ? 1 : 0, record.writeAttempted ? 1 : 0);
    if (written > 0) {
        std::size_t count = static_cast<std::size_t>(written);
        if (count >= sizeof(line)) count = sizeof(line) - 1;
        std::fwrite(line, 1, count, stderr);
        std::fflush(stderr);
    }
    errno = savedErrno;
    SetLastError(savedError);
}
struct CommitStageScope {
    CommitStageRecord& record;
    CommitStageRecord* previous = nullptr;
    explicit CommitStageScope(CommitStageRecord& value) noexcept : record(value) {
        if (!record.enabled) return;
        previous = activeCommitStage;
        activeCommitStage = &record;
    }
    ~CommitStageScope() {
        if (!record.enabled) return;
        activeCommitStage = previous;
        PrintCommitStage(record);
    }
    CommitStageScope(const CommitStageScope&) = delete;
    CommitStageScope& operator=(const CommitStageScope&) = delete;
};
bool ReadCommitStageSwitch() noexcept {
    const DWORD savedError = GetLastError();
    const int savedErrno = errno;
    const char* value = std::getenv("MMS_CACHE_COMMIT_STAGE_TRACE");
    const bool enabled = value != nullptr && value[0] == '1' && value[1] == '\0';
    errno = savedErrno;
    SetLastError(savedError);
    return enabled;
}
}

Store::Store(std::filesystem::path absoluteMetadataRoot) : root_(std::move(absoluteMetadataRoot)) {}
Store::Store(cache_files::Directory&& directory)
    : boundMode_(true), bound_(std::make_shared<cache_files::Directory>(std::move(directory))) {}
files::Result Store::OpenRoot(bool create, files::Directory& output) const noexcept {
    if (boundMode_) return bound_ ? bound_->Share(output) : files::Result::Invalid;
    return files::Directory::Open(root_, create, output);
}
bool Store::AcceptOrigin(const Snapshot& expected) const noexcept {
    return expected.valid_ && (boundMode_ || expected.boundRoot_ || expected.root_ == root_);
}
void Snapshot::Swap(Snapshot& other) noexcept {
    std::swap(valid_, other.valid_); root_.swap(other.root_); std::swap(revision_, other.revision_);
    std::swap(rootIdentity_, other.rootIdentity_); std::swap(boundRoot_, other.boundRoot_);
    owner_.swap(other.owner_); std::swap(head_, other.head_); bytes_.swap(other.bytes_);
}
Result Store::Read(Snapshot& output) const noexcept {
    return ReadImpl(output, nullptr);
}
bool Snapshot::SameVersion(const Snapshot& other) const noexcept {
    return valid_&&other.valid_&&!head_.name.empty()&&!other.head_.name.empty()&&
        Equal(rootIdentity_,other.rootIdentity_)&&revision_==other.revision_&&
        owner_==other.owner_&&Equal(head_,other.head_);
}
Result Store::ReadPinned(Snapshot& output, files::ReadLease& lease) const noexcept {
    return ReadImpl(output, &lease);
}
Result Store::ReadPinnedExistingLocks(Snapshot& output, files::ReadLease& lease) const noexcept {
    return ReadImpl(output, &lease, true);
}
Result Store::ReadImpl(Snapshot& output, files::ReadLease* lease, bool requireExistingLocks) const noexcept {
    try {
        files::Directory directory;
        auto io = OpenRoot(false, directory);
        if (io != files::Result::Ok) return RootOpenError(io);
        files::FileLock lock;
        io = requireExistingLocks ? directory.LockExisting(0, lock) : directory.Lock(0, lock);
        if (io != files::Result::Ok) return Convert(io);
        files::DirectoryIdentity identity;
        io = directory.GetIdentity(identity);
        if (io != files::Result::Ok) return Convert(io);
        files::Directory snapshots, commits;
        auto result = OpenChildren(directory, false, snapshots, commits);
        if (result != Result::Ok) return result;
        Loaded loaded;
        result = Load(snapshots, commits, loaded);
        if (result != Result::Ok) return result;
        // Load already inspected and digest-verified every present snapshot. The
        // pinned result reuses that same lease for the current snapshot instead
        // of reading the file again; it is the only lease moved out.
        files::ReadLease pinned;
        if (lease) {
            const auto found = std::find_if(loaded.files.begin(), loaded.files.end(), [&](const auto& file) {
                return !file.retired && Equal(file.info, loaded.currentSnapshot);
            });
            if (found == loaded.files.end() || !found->lease.IsValid()) return Result::RecoveryRequired;
            pinned = std::move(found->lease);
        }
        Snapshot candidate; candidate.root_ = root_; candidate.owner_ = loaded.owner;
        candidate.rootIdentity_ = identity; candidate.boundRoot_ = boundMode_;
        candidate.revision_ = loaded.revision; candidate.head_ = std::move(loaded.head);
        candidate.bytes_ = std::move(loaded.bytes); candidate.valid_ = true;
        candidate.Swap(output);
        if (lease) *lease = std::move(pinned);
        return Result::Ok;
    } catch (...) { return Result::Io; }
}
Result Store::Bootstrap() const noexcept {
    try {
        files::Directory directory;auto io=OpenRoot(true,directory);if(io!=files::Result::Ok)return RootOpenError(io);
        files::FileLock lock;io=directory.Lock(0,lock);if(io!=files::Result::Ok)return Convert(io);
        files::Directory snapshots,commits,operations;
        return OpenChildren(directory,true,snapshots,commits,&operations);
    }catch(...){return Result::Io;}
}
Result Store::PreparePlan(WriteKind kind,const Snapshot* expected,const std::vector<std::uint8_t>& bytes,
                         const files::Owner* owner,WritePlan& output,bool approved,bool existingLocks) const noexcept {
    try {
        SetCommitStage("preparePlan.guard");
        if(output.state_&&output.state_->operating){RecordCommitPredicate(true);return Result::Busy;}
        if(bytes.size()>MaximumSnapshotBytes){RecordCommitPredicate(true);return Result::Limit;}
        if(owner&&std::all_of(owner->begin(),owner->end(),[](auto value){return value==0;})){RecordCommitPredicate(true);return Result::Invalid;}
        SetCommitStage("preparePlan.allocate");
        auto candidate=std::make_unique<WritePlanState>();
        SetCommitStage("preparePlan.copy");
        candidate->requiresApproval=approved;
        candidate->existingLocks=existingLocks;
        candidate->kind=kind;candidate->bytes=bytes;if(expected)candidate->expected=*expected;
        if(owner)candidate->owner=*owner;
        Snapshot ignored;RecoveryReport recovery;Result result=Result::Invalid;
        if(kind==WriteKind::Initialize||kind==WriteKind::Commit)
            result=Publish(expected,candidate->bytes,ignored,owner,*candidate,true);
        else if(kind==WriteKind::Checkpoint)result=CheckpointImpl(*expected,ignored,*candidate,true);
        else result=RecoverImpl(recovery,*candidate,true);
        if(result!=Result::Ok)return result;
        SetCommitStage("preparePlan.commit");
        candidate->valid=true;output.state_.swap(candidate);return Result::Ok;
    }catch(...){RecordCommitException();return Result::Io;}
}
Result Store::PlanInitialize(const std::vector<std::uint8_t>& bytes,WritePlan& out) const noexcept {
    return PreparePlan(WriteKind::Initialize,nullptr,bytes,nullptr,out);
}
Result Store::PlanInitializeOwned(const files::Owner& owner,const std::vector<std::uint8_t>& bytes,WritePlan& out) const noexcept {
    return PreparePlan(WriteKind::Initialize,nullptr,bytes,&owner,out);
}
Result Store::PlanCommit(const Snapshot& expected,const std::vector<std::uint8_t>& bytes,WritePlan& out) const noexcept {
    return PreparePlan(WriteKind::Commit,&expected,bytes,nullptr,out);
}
Result Store::PlanApprovedInitializeOwned(const files::Owner& owner,const std::vector<std::uint8_t>& bytes,WritePlan& out) const noexcept {
    return PreparePlan(WriteKind::Initialize,nullptr,bytes,&owner,out,true,true);
}
Result Store::PlanApprovedCommit(const Snapshot& expected,const std::vector<std::uint8_t>& bytes,WritePlan& out) const noexcept {
    return PreparePlan(WriteKind::Commit,&expected,bytes,nullptr,out,true);
}
Result Store::PlanCheckpoint(const Snapshot& expected,WritePlan& out) const noexcept {
    return PreparePlan(WriteKind::Checkpoint,&expected,{},nullptr,out);
}
Result Store::PlanApprovedCheckpoint(const Snapshot& expected,WritePlan& out) const noexcept {
    return PreparePlan(WriteKind::Checkpoint,&expected,{},nullptr,out,true);
}
Result Store::PlanRecovery(WritePlan& out) const noexcept {
    return PreparePlan(WriteKind::Recovery,nullptr,{},nullptr,out);
}
Result Store::PlanRecoveryExistingLocks(WritePlan& out) const noexcept {
    return PreparePlan(WriteKind::Recovery,nullptr,{},nullptr,out,false,true);
}
Result Store::Execute(WritePlan& plan,Snapshot& output) const noexcept {
    SetCommitStage("execute.guard");
    if(!plan.IsValid()||plan.state_->kind==WriteKind::Recovery||plan.state_->requiresApproval){RecordCommitPredicate(true);return Result::Invalid;}
    auto& state=*plan.state_;
    SetCommitStage("execute.operating");
    if(state.operating){RecordCommitPredicate(true);return Result::Busy;}
    SetCommitStage("execute.consumed");
    if(state.consumed){RecordCommitPredicate(true);return Result::Conflict;}
    struct Scope{bool& value;~Scope(){value=false;}} scope{state.operating};state.operating=true;
    SetCommitStage("execute.publish");
    const auto result=state.kind==WriteKind::Checkpoint?CheckpointImpl(state.expected,output,state,false):
        Publish(state.kind==WriteKind::Commit?&state.expected:nullptr,state.bytes,output,
                state.kind==WriteKind::Initialize?&state.owner:nullptr,state,false);
    if(result==Result::Ok)state.consumed=true;
    return result;
}
Result Store::ExecuteApproved(WritePlan& plan,const PublicationApproval& approval,Snapshot& output) const noexcept {
    if(!plan.IsValid()||(plan.state_->kind!=WriteKind::Commit&&plan.state_->kind!=WriteKind::Checkpoint&&plan.state_->kind!=WriteKind::Initialize)||!plan.state_->requiresApproval)return Result::Invalid;
    auto& state=*plan.state_;if(state.operating)return Result::Busy;if(state.consumed)return Result::Conflict;
    struct Scope{bool& value;~Scope(){value=false;}} scope{state.operating};state.operating=true;
    // Own the immutable approval for this invocation even if a caller aliases
    // mutable external storage. Validate before candidate creation in Publish.
    const auto approved=approval;
    const auto result=state.kind==WriteKind::Checkpoint?CheckpointImpl(state.expected,output,state,false,&approved):
        Publish(state.kind==WriteKind::Initialize?nullptr:&state.expected,state.bytes,output,
                state.kind==WriteKind::Initialize?&state.owner:nullptr,state,false,&approved);
    if(result==Result::Ok)state.consumed=true;
    return result;
}
Result Store::ExecuteRecovery(WritePlan& plan,RecoveryReport& output) const noexcept {
    if(!plan.IsValid()||plan.state_->kind!=WriteKind::Recovery)return Result::Invalid;
    auto& state=*plan.state_;if(state.operating)return Result::Busy;if(state.consumed)return Result::Conflict;
    struct Scope{bool& value;~Scope(){value=false;}} scope{state.operating};state.operating=true;
    const auto result=RecoverImpl(output,state,false);if(result==Result::Ok)state.consumed=true;return result;
}
Result Store::Initialize(const std::vector<std::uint8_t>& bytes, Snapshot& output) const noexcept {
    if(bytes.size()>MaximumSnapshotBytes)return Result::Limit;
    const auto initialized=Bootstrap();if(initialized!=Result::Ok)return initialized;
    WritePlan plan;const auto result=PlanInitialize(bytes,plan);
    return result==Result::Ok?Execute(plan,output):result;
}
Result Store::InitializeOwned(const cache_files::Owner& owner,
    const std::vector<std::uint8_t>& bytes, Snapshot& output) const noexcept {
    if(std::all_of(owner.begin(),owner.end(),[](auto value){return value==0;}))return Result::Invalid;
    if(bytes.size()>MaximumSnapshotBytes)return Result::Limit;
    const auto initialized=Bootstrap();if(initialized!=Result::Ok)return initialized;
    WritePlan plan;const auto result=PlanInitializeOwned(owner,bytes,plan);
    return result==Result::Ok?Execute(plan,output):result;
}
Result Store::Commit(const Snapshot& expected, const std::vector<std::uint8_t>& bytes, Snapshot& output) const noexcept {
    // The observation record lives on this stack frame. It is installed before
    // the plan so the single failure line is emitted after the plan and every
    // preparation/lease it owns has been destroyed. Inputs are frozen before
    // planning because expected and output may alias.
    CommitStageRecord stageRecord;
    stageRecord.enabled = ReadCommitStageSwitch();
    if (stageRecord.enabled) {
        stageRecord.callId = commitStageCallSequence.fetch_add(1, std::memory_order_relaxed) + 1;
        stageRecord.store = this;
        stageRecord.bound = boundMode_;
        const auto& rootText = root_.native();
        stageRecord.rootText = rootText.data();
        stageRecord.rootLength = rootText.size();
        stageRecord.expectedRevision = expected.revision_;
        stageRecord.payloadBytes = static_cast<std::uint64_t>(bytes.size());
    }
    CommitStageScope stageScope(stageRecord);
    WritePlan plan;
    SetCommitStagePhase(false);
    SetCommitStage("commit.plan");
    const auto planned = PlanCommit(expected, bytes, plan);
    Result result = planned;
    if (planned == Result::Ok) {
        SetCommitStagePhase(true);
        SetCommitStage("commit.execute");
        result = Execute(plan, output);
    }
    stageRecord.finalResult = result;
    return result;
}
Result Store::Checkpoint(const Snapshot& expected, Snapshot& output) const noexcept {
    WritePlan plan;const auto result=PlanCheckpoint(expected,plan);
    return result==Result::Ok?Execute(plan,output):result;
}
Result Store::CheckpointImpl(const Snapshot& expected, Snapshot& output,WritePlanState& plan,bool planning,const PublicationApproval* approval) const noexcept {
    bool attempted = false;
    try {
        if (!AcceptOrigin(expected)) return Result::Conflict;
        files::Directory directory;
        auto io = OpenRoot(false, directory);
        if (io != files::Result::Ok) return RootRecoveryError(io);
        files::DirectoryIdentity identity;
        io = directory.GetIdentity(identity);
        if (io != files::Result::Ok) return Convert(io);
        if (!Equal(expected.rootIdentity_, identity)) return Result::Conflict;
        const auto bound=BindRoot(plan,directory,identity,planning);if(bound!=Result::Ok)return bound;
        files::FileLock lock;
        io = planning&&!plan.requiresApproval ? directory.Lock(0, lock) : directory.LockExisting(0, lock);
        if (io != files::Result::Ok) return Convert(io);
        files::Directory snapshots, commits, operations;
        auto result = OpenChildren(directory, false, snapshots, commits, &operations);
        if (result != Result::Ok) return result == Result::Missing ? Result::RecoveryRequired : result;
        if(plan.requiresApproval&&!operations.IsValid())return Result::Missing;
        Loaded current;
        result = Load(snapshots, commits, current);
        if (result != Result::Ok) return result == Result::Missing ? Result::RecoveryRequired : result;
        if(plan.requiresApproval&&!current.allSnapshotsReferenced)return Result::RecoveryRequired;
        if (expected.revision_ != current.revision || expected.owner_ != current.owner ||
            !Equal(expected.head_, current.head)) return Result::Conflict;
        if (current.revision == (std::numeric_limits<std::uint64_t>::max)() ||
            current.commits > MaximumCommits || current.metadataBytes > MaximumMetadataBytes)
            return Result::Limit;
        Snapshot candidate;
        candidate.root_ = root_;
        candidate.rootIdentity_ = identity; candidate.boundRoot_ = boundMode_;
        candidate.owner_ = current.owner;
        candidate.revision_ = current.revision + 1;
        candidate.bytes_ = current.bytes;
        codec::CheckpointRecord record;
        record.state.revision = candidate.revision_;
        record.state.owner = current.owner;
        record.state.snapshot = current.currentSnapshot;
        record.state.previous = current.head;
        record.retired.reserve(current.files.size());
        for (const auto& file : current.files)
            if (!SameLocation(file.info, current.currentSnapshot)) record.retired.push_back(file.info);
        std::sort(record.retired.begin(), record.retired.end(), [](const auto& a, const auto& b) {
            if (IsSnapshot(a) != IsSnapshot(b)) return !IsSnapshot(a);
            return a.name < b.name;
        });
        if (record.retired.size() > codec::MaximumRetired) return Result::Limit;
        std::vector<std::uint8_t> encoded;
        const auto coded = codec::EncodeCheckpoint(record, encoded);
        if (coded != codec::Result::Ok) return coded == codec::Result::Io ? Result::Io : Result::Invalid;
        WriteFootprint footprint;
        const auto sized=plan.requiresApproval?ApprovedCheckpointFootprint(record.retired.size(),footprint):CheckpointFootprint(record.retired.size(),footprint);
        if(sized!=Result::Ok)return sized;
        if(!BindFootprint(plan,footprint,planning))return Result::Conflict;
        if(planning)plan.checkpointBytes=encoded;
        else if(plan.checkpointBytes!=encoded)return Result::Conflict;
        if (LogicalBytes(footprint) > MaximumMetadataBytes + (plan.requiresApproval?ApprovedMaintenanceReserveBytes:MaintenanceReserveBytes) - current.metadataBytes)
            return Result::Limit;
        if(planning)return Result::Ok;
        files::Spec spec;
        spec.owner = codec::NamespaceOwner;
        spec.kind = files::Kind::CatalogCheckpoint;
        spec.key = codec::CheckpointKey;
        if(plan.requiresApproval){
            if(!approval||intent::ValidateCheckpointApproval(record.retired.size(),*approval)!=intent::Result::Ok)return Result::Invalid;
            result=ApprovalDirectories(*approval,directory,snapshots,commits,operations);if(result!=Result::Ok)return result;
            intent::ApprovedCheckpointRecord operation;operation.publication=record;operation.approval=*approval;
            if(!files::crypto::Random(operation.operation)||std::all_of(operation.operation.begin(),operation.operation.end(),[](auto x){return x==0;}))return Result::Io;
            files::PreparedWrite prepared;
            bool intentAttempted=false;
            struct Cleanup{files::PreparedWrite& file;bool& attempted;~Cleanup(){if(!attempted&&file.IsValid())file.Abort();}} cleanup{prepared,intentAttempted};
            Attempt(plan);io=commits.Prepare(spec,encoded.data(),encoded.size(),prepared);if(io!=files::Result::Ok)return Convert(io);
            operation.checkpoint=prepared.Plan();std::vector<std::uint8_t> bytes;
            if(intent::EncodeApprovedCheckpoint(operation,bytes)!=intent::Result::Ok)return Result::Invalid;
            files::BlobInfo log;intentAttempted=true;
            result=WriteOperation(operations,bytes,false,log,true,true);if(result!=Result::Ok)return result;
            attempted=true;io=prepared.Complete(encoded.data(),encoded.size(),candidate.head_);
            if(io!=files::Result::Ok)return Result::Indeterminate;
            result=RemoveOperationFile(operations,log);if(result!=Result::Ok)return Result::Indeterminate;
            candidate.valid_=true;candidate.Swap(output);return Result::Ok;
        }
        attempted = true;
        Attempt(plan);
        io = commits.Create(spec, encoded.data(), encoded.size(), candidate.head_);
        if (io != files::Result::Ok) return Result::Indeterminate;
        candidate.valid_ = true;
        candidate.Swap(output);
        return Result::Ok;
    } catch (...) { return attempted ? Result::Indeterminate : Result::Io; }
}
Result Store::CollectRetired(CollectionReport& report) const noexcept {
    return CollectImpl(nullptr,report);
}
Result Store::CollectRetired(const Snapshot& expected,CollectionReport& report) const noexcept {
    return CollectImpl(&expected,report);
}
Result Store::CollectImpl(const Snapshot* expected,CollectionReport& report) const noexcept {
    try {
        if(expected&&!AcceptOrigin(*expected))return Result::Conflict;
        files::Directory directory;
        auto io = OpenRoot(false, directory);
        if (io != files::Result::Ok) return Convert(io);
        if(expected){
            files::DirectoryIdentity identity;io=directory.GetIdentity(identity);
            if(io!=files::Result::Ok)return Convert(io);
            if(!Equal(expected->rootIdentity_,identity))return Result::Conflict;
        }
        files::FileLock lock;
        io = expected ? directory.LockExisting(0, lock) : directory.Lock(0, lock);
        if (io != files::Result::Ok) return Convert(io);
        files::Directory snapshots, commits;
        auto result = OpenChildren(directory, false, snapshots, commits);
        if (result != Result::Ok) return result;
        Loaded current;
        result = Load(snapshots, commits, current);
        if (result != Result::Ok) return result;
        if(expected&&(expected->revision_!=current.revision||expected->owner_!=current.owner||
                      !Equal(expected->head_,current.head)))return Result::Conflict;
        std::vector<std::size_t> targets;
        targets.reserve(current.files.size());
        for (std::size_t i = 0; i < current.files.size(); ++i)
            if (current.files[i].retired) targets.push_back(i);
        CollectionReport collected;
        // All planning allocations precede the first deletion. Release only our
        // own retired-file lease; active metadata remains pinned throughout.
        for (const auto index : targets) {
            auto& file = current.files[index];
            file.lease = {};
            const auto& target = IsSnapshot(file.info) ? snapshots : commits;
            ++collected.attempted;
            io = target.Remove(file.info);
            if (io == files::Result::Busy) ++collected.busy;
            else if (io == files::Result::DeletePending || io == files::Result::Missing) {
                files::ReadLease confirmation;
                const auto check = target.Inspect(file.info.name, file.info.spec.owner, file.info.payloadBytes, confirmation);
                if (check == files::Result::Missing) {
                    ++collected.absent;
                    collected.absentBytes += file.info.identity.fileBytes;
                } else ++collected.pending;
            } else ++collected.failed;
        }
        report = collected;
        return Result::Ok;
    } catch (...) { return Result::Io; }
}
Result Store::RecoverOwned(RecoveryReport& report) const noexcept {
    WritePlan plan;const auto result=PlanRecovery(plan);
    return result==Result::Ok?ExecuteRecovery(plan,report):result;
}
Result Store::RecoverImpl(RecoveryReport& report,WritePlanState& plan,bool planning) const noexcept {
    try {
        files::Directory directory;
        auto io = OpenRoot(false, directory);
        if (io != files::Result::Ok) return Convert(io);
        files::DirectoryIdentity rootIdentity;
        io=directory.GetIdentity(rootIdentity);if(io!=files::Result::Ok)return Convert(io);
        const auto bound=BindRoot(plan,directory,rootIdentity,planning);if(bound!=Result::Ok)return bound;
        files::FileLock lock;
        io = planning && !plan.existingLocks ? directory.Lock(0, lock) : directory.LockExisting(0, lock);
        if (io != files::Result::Ok) return Convert(io);
        files::Directory snapshots, commits, operations;
        auto result = OpenChildren(directory, false, snapshots, commits, &operations, true);
        if (result != Result::Ok) return result;
        // Only this verified recovery path may construct the endpoint. Reuse
        // Load's already-verified payload lease rather than reopen by filename.
        const auto bindTarget = [&](RecoveryAction action,Loaded* loaded)->Result {
            if(!planning){
                if(plan.recoveryAction!=action)return Result::Conflict;
                const auto& target=plan.recoveryTarget;
                if(!loaded)return !target.IsValid()?Result::Ok:Result::Conflict;
                return target.valid_&&Equal(target.rootIdentity_,rootIdentity)&&
                    target.revision_==loaded->revision&&target.owner_==loaded->owner&&
                    Equal(target.head_,loaded->head)?Result::Ok:Result::Conflict;
            }
            if(!loaded){plan.recoveryAction=action;return Result::Ok;}
            const auto pinned=std::find_if(loaded->files.begin(),loaded->files.end(),[&](const auto& file){
                return !file.retired&&Equal(file.info,loaded->currentSnapshot);
            });
            if(pinned==loaded->files.end()||!pinned->lease.IsValid())return Result::RecoveryRequired;
            Snapshot target;target.root_=root_;target.rootIdentity_=rootIdentity;target.boundRoot_=boundMode_;
            target.owner_=loaded->owner;target.revision_=loaded->revision;target.head_=std::move(loaded->head);
            target.bytes_=std::move(loaded->bytes);target.valid_=true;
            target.Swap(plan.recoveryTarget);plan.recoveryTargetLease=std::move(pinned->lease);
            plan.recoveryAction=action;return Result::Ok;
        };
        PendingOperation pending;
        result = operations.IsValid() ? ReadOperation(operations, pending) : Result::Missing;
        if (result == Result::Missing) {
            if(!planning&&plan.pendingOperation)return Result::Conflict;
            Loaded current;
            result = Load(snapshots, commits, current);
            if (result != Result::Ok) return result;
            if(planning){plan.pendingOperation=false;plan.recoveryHead=current.head;plan.recoveryRevision=current.revision;}
            else if(plan.recoveryRevision!=current.revision||!Equal(plan.recoveryHead,current.head))return Result::Conflict;
            if(!BindFootprint(plan,WriteFootprint{},planning))return Result::Conflict;
            result=bindTarget(RecoveryAction::Noop,&current);if(result!=Result::Ok)return result;
            if(planning)return Result::Ok;
            RecoveryReport done; done.revision = current.revision;
            report = done; return Result::Ok;
        }
        if (result != Result::Ok) return result;
        if(pending.approved){
            result=ApprovalDirectories(pending.approval,directory,snapshots,commits,operations);
            if(result!=Result::Ok)return result;
        }
        if(planning){
            plan.recoveryCheckpoint=pending.checkpoint;
            plan.recoveryInitialization=!pending.checkpoint&&pending.record.publication.revision==1;
            plan.pendingOperation=true;plan.hasIntent=pending.hasIntent;plan.hasDecision=pending.hasDecision;
            plan.intentFile=pending.intentFile;plan.decisionFile=pending.decisionFile;plan.operationBytes=pending.bytes;
            plan.hasRecoveryApproval=pending.approved;if(pending.approved)plan.recoveryApproval=pending.approval;
        }else if(plan.recoveryInitialization!=(!pending.checkpoint&&pending.record.publication.revision==1)||plan.recoveryCheckpoint!=pending.checkpoint||!plan.pendingOperation||plan.hasIntent!=pending.hasIntent||plan.hasDecision!=pending.hasDecision||
            plan.hasRecoveryApproval!=pending.approved||plan.operationBytes!=pending.bytes||(pending.hasIntent&&!Equal(plan.intentFile,pending.intentFile))||
            (pending.hasDecision&&!Equal(plan.decisionFile,pending.decisionFile)))return Result::Conflict;
        if(pending.checkpoint){
            const auto& operation=pending.checkpointRecord;
            const auto& publication=operation.publication.state;
            // The active snapshot is shared with the old head. Only the new
            // checkpoint is a candidate; retirement never authorizes rollback
            // to delete any historic generation.
            files::PreparedWrite checkpoint;
            io=commits.ReopenPrepared(operation.checkpoint,checkpoint);
            if(io!=files::Result::Ok&&!(io==files::Result::Missing&&pending.hasDecision))return StoredError(io);
            bool complete=false;
            if(checkpoint.IsValid()){io=checkpoint.Inspect(complete);if(io!=files::Result::Ok)return StoredError(io);}
            WriteFootprint required;
            result=ApprovedCheckpointFootprint(operation.publication.retired.size(),required);if(result!=Result::Ok)return result;
            Loaded previous;
            result=Load(snapshots,commits,previous,nullptr,&operation.checkpoint);if(result!=Result::Ok)return result;
            if(!previous.allSnapshotsReferenced||previous.owner!=publication.owner||previous.revision!=publication.revision-1||
               !Equal(previous.head,publication.previous)||!Equal(previous.currentSnapshot,publication.snapshot))return Result::RecoveryRequired;
            // Verify the complete intended retirement set, not merely its hash.
            std::vector<files::BlobInfo> retired;
            for(const auto& file:previous.files)if(!SameLocation(file.info,previous.currentSnapshot))retired.push_back(file.info);
            std::sort(retired.begin(),retired.end(),[](const auto&a,const auto&b){if(IsSnapshot(a)!=IsSnapshot(b))return !IsSnapshot(a);return a.name<b.name;});
            if(retired.size()!=operation.publication.retired.size())return Result::RecoveryRequired;
            for(std::size_t i=0;i<retired.size();++i)if(!Equal(retired[i],operation.publication.retired[i]))return Result::RecoveryRequired;
            if(previous.metadataBytes>MaximumMetadataBytes||LogicalBytes(required)>MaximumMetadataBytes+ApprovedMaintenanceReserveBytes-previous.metadataBytes)return Result::Limit;
            if(complete){
                if(pending.hasDecision)return Result::RecoveryRequired;
                // Release exclusive candidate handle before normal Load verifies
                // the full checkpoint and its precise retirement references.
                checkpoint={};Loaded current;result=Load(snapshots,commits,current);if(result!=Result::Ok)return result;
                files::BlobInfo expectedHead;
                if(intent::PlannedInfo(operation.checkpoint,expectedHead)!=intent::Result::Ok||
                   !Equal(current.head,expectedHead)||current.owner!=publication.owner||current.revision!=publication.revision)return Result::RecoveryRequired;
                if(current.metadataBytes>MaximumMetadataBytes+ApprovedMaintenanceReserveBytes||
                   2*(files::HeaderBytes+pending.bytes.size())>MaximumMetadataBytes+ApprovedMaintenanceReserveBytes-current.metadataBytes)return Result::Limit;
                if(!BindFootprint(plan,WriteFootprint{},planning))return Result::Conflict;
                result=bindTarget(RecoveryAction::ConfirmPublication,&current);if(result!=Result::Ok)return result;
                if(planning)return Result::Ok;
                // Release Load's checkpoint read handle before exclusive flush.
                current={};Attempt(plan);
                io=commits.ReopenPrepared(operation.checkpoint,checkpoint);if(io!=files::Result::Ok)return StoredError(io);
                io=checkpoint.Flush();if(io!=files::Result::Ok)return StoredError(io);checkpoint={};
                result=RemoveOperationFile(operations,pending.intentFile);if(result!=Result::Ok)return result;
                RecoveryReport done;done.confirmedPublication=true;done.revision=publication.revision;report=done;return Result::Ok;
            }
            WriteFootprint recovery;
            if(!pending.hasDecision){recovery.count=1;recovery.files[0]={WriteRole::RollbackDecision,files::HeaderBytes+pending.bytes.size()};}
            if(!BindFootprint(plan,recovery,planning))return Result::Conflict;
            result=bindTarget(RecoveryAction::Rollback,&previous);if(result!=Result::Ok)return result;
            if(planning)return Result::Ok;
            Attempt(plan);
            if(!pending.hasDecision){result=WriteOperation(operations,pending.bytes,true,pending.decisionFile,true,true);if(result!=Result::Ok)return result;}
            else {result=FlushOperationFile(operations,pending.decisionFile);if(result!=Result::Ok)return result;}
            files::ReadLease decisionLease;io=operations.Acquire(pending.decisionFile,decisionLease);if(io!=files::Result::Ok)return StoredError(io);
            if(checkpoint.IsValid()){io=checkpoint.Abort();if(io!=files::Result::DeletePending)return StoredError(io);}
            files::ReadLease remains;io=commits.Inspect(operation.checkpoint.name,operation.checkpoint.spec.owner,operation.checkpoint.payloadBytes,remains);
            if(io!=files::Result::Missing)return io==files::Result::Ok?Result::RecoveryRequired:StoredError(io);
            if(pending.hasIntent){result=RemoveOperationFile(operations,pending.intentFile);if(result!=Result::Ok)return result;}
            decisionLease={};result=RemoveOperationFile(operations,pending.decisionFile);if(result!=Result::Ok)return result;
            RecoveryReport done;done.rolledBack=true;done.revision=publication.revision-1;report=done;return Result::Ok;
        }
        if(!planning&&plan.recoveryAction==RecoveryAction::ConfirmPublication){
            // A read pin and ReopenPrepared's exclusive flush handle cannot
            // coexist. Reject stale endpoints before transferring our own pin;
            // external pins still make the exclusive reopen safely fail Busy.
            {
                Loaded actual;result=Load(snapshots,commits,actual);if(result!=Result::Ok)return result;
                result=bindTarget(RecoveryAction::ConfirmPublication,&actual);
                if(result!=Result::Ok)return result;
            }
            plan.consumed=true;
            plan.recoveryTargetLease={};
        }
        files::PreparedWrite snapshot, commit;
        const auto pin = [&](const files::Directory& parent, const files::WritePlan& plan,
                             files::PreparedWrite& output) {
            const auto opened = parent.ReopenPrepared(plan, output);
            if (opened == files::Result::Missing)
                return pending.hasDecision ? Result::Ok : Result::RecoveryRequired;
            return opened == files::Result::Ok ? Result::Ok : StoredError(opened);
        };
        result = pin(snapshots, pending.record.snapshot, snapshot);
        if (result != Result::Ok) return result;
        result = pin(commits, pending.record.commit, commit);
        if (result != Result::Ok) return result;
        bool validSnapshot = false;
        if (snapshot.IsValid()) {
            io = snapshot.Inspect(validSnapshot);
            if (io != files::Result::Ok) return StoredError(io);
        }
        bool complete = false;
        if (commit.IsValid()) {
            io = commit.Inspect(complete);
            if (io != files::Result::Ok) return StoredError(io);
        }
        const auto& publication = pending.record.publication;
        if (complete) {
            // A full but corrupt commit was already rejected by Inspect. A
            // durable rollback decision must never coexist with a publication.
            if (pending.hasDecision || !snapshot.IsValid()) return Result::RecoveryRequired;
            if (!validSnapshot) return Result::RecoveryRequired;
            if(!BindFootprint(plan,WriteFootprint{},planning))return Result::Conflict;
            if(planning){
                snapshot={};commit={};
                Loaded current;result=Load(snapshots,commits,current);if(result!=Result::Ok)return result;
                files::BlobInfo expectedHead;
                if(intent::PlannedInfo(pending.record.commit,expectedHead)!=intent::Result::Ok||
                   current.revision!=publication.revision||current.owner!=publication.owner||!Equal(current.head,expectedHead))return Result::RecoveryRequired;
                if(current.metadataBytes>MaximumMetadataBytes||
                   2*(files::HeaderBytes+pending.bytes.size())>MaximumMetadataBytes-current.metadataBytes)return Result::Limit;
                return bindTarget(RecoveryAction::ConfirmPublication,&current);
            }
            Attempt(plan);
            io = snapshot.Flush();
            if (io != files::Result::Ok) return StoredError(io);
            io = commit.Flush();
            if (io != files::Result::Ok) return StoredError(io);
            snapshot = {}; commit = {};
            Loaded current;
            result = Load(snapshots, commits, current);
            if (result != Result::Ok) return result;
            files::BlobInfo expectedHead;
            if (intent::PlannedInfo(pending.record.commit, expectedHead) != intent::Result::Ok)
                return Result::RecoveryRequired;
            if (current.revision != publication.revision || current.owner != publication.owner ||
                !Equal(current.head, expectedHead)) return Result::RecoveryRequired;
            if (current.metadataBytes > MaximumMetadataBytes ||
                2 * (files::HeaderBytes + pending.bytes.size()) > MaximumMetadataBytes - current.metadataBytes)
                return Result::Limit;
            result=bindTarget(RecoveryAction::ConfirmPublication,&current);if(result!=Result::Ok)return result;
            result = RemoveOperationFile(operations, pending.intentFile);
            if (result != Result::Ok) return result;
            RecoveryReport done; done.confirmedPublication = true; done.revision = current.revision;
            report = done; return Result::Ok;
        }
        Loaded previous;
        result = Load(snapshots, commits, previous, &pending.record);
        if (publication.revision == 1) {
            if (result != Result::Missing) return result == Result::Ok ? Result::RecoveryRequired : result;
        } else {
            if (result != Result::Ok) return result == Result::Missing ? Result::RecoveryRequired : result;
            if (previous.owner != publication.owner || previous.revision != publication.revision - 1 ||
                !Equal(previous.head, publication.previous)) return Result::RecoveryRequired;
        }
        const auto required = PublicationBytes(pending.record,pending.approved);
        if (previous.metadataBytes > MaximumMetadataBytes || required > MaximumMetadataBytes - previous.metadataBytes)
            return Result::Limit;
        WriteFootprint recovery;
        if(!pending.hasDecision){recovery.count=1;recovery.files[0]={WriteRole::RollbackDecision,files::HeaderBytes+pending.bytes.size()};}
        if(!BindFootprint(plan,recovery,planning))return Result::Conflict;
        result=bindTarget(RecoveryAction::Rollback,publication.revision==1?nullptr:&previous);
        if(result!=Result::Ok)return result;
        if(planning)return Result::Ok;
        Attempt(plan);
        if (!pending.hasDecision) {
            result = WriteOperation(operations, pending.bytes, true, pending.decisionFile,pending.approved);
            if (result != Result::Ok) return result;
            pending.hasDecision = true;
        } else {
            // A complete decision may have survived a failed flush. Obtain a
            // successful same-handle flush before using it as deletion authority.
            result = FlushOperationFile(operations, pending.decisionFile);
            if (result != Result::Ok) return result;
        }
        files::ReadLease decisionLease;
        io = operations.Acquire(pending.decisionFile, decisionLease);
        if (io != files::Result::Ok) return StoredError(io);
        const auto abort = [&](files::PreparedWrite& target, const files::Directory& parent,
                               const files::WritePlan& plan) {
            if (target.IsValid()) {
                const auto removed = target.Abort();
                if (removed != files::Result::DeletePending) return StoredError(removed);
            }
            files::ReadLease remains;
            const auto absent = parent.Inspect(plan.name, plan.spec.owner, plan.payloadBytes, remains);
            return absent == files::Result::Missing ? Result::Ok :
                absent == files::Result::Ok ? Result::RecoveryRequired : StoredError(absent);
        };
        result = abort(commit, commits, pending.record.commit);
        if (result != Result::Ok) return result;
        result = abort(snapshot, snapshots, pending.record.snapshot);
        if (result != Result::Ok) return result;
        if (pending.hasIntent) {
            result = RemoveOperationFile(operations, pending.intentFile);
            if (result != Result::Ok) return result;
        }
        decisionLease = {};
        result = RemoveOperationFile(operations, pending.decisionFile);
        if (result != Result::Ok) return result;
        RecoveryReport done; done.rolledBack = true; done.revision = previous.revision;
        report = done; return Result::Ok;
    } catch (...) { return Result::Io; }
}
Result Store::Publish(const Snapshot* expected, const std::vector<std::uint8_t>& bytes,
    Snapshot& output, const cache_files::Owner* initialOwner,WritePlanState& plan,bool planning,
    const PublicationApproval* approval) const noexcept {
    bool commitAttempted = false;
    try {
        SetCommitStage("publish.input");
        if (initialOwner && (expected || std::all_of(initialOwner->begin(), initialOwner->end(),
            [](auto value) { return value == 0; }))) { RecordCommitPredicate(true); return Result::Invalid; }
        SetCommitStage("publish.size");
        if (bytes.size() > MaximumSnapshotBytes) { RecordCommitPredicate(true); return Result::Limit; }
        SetCommitStage("publish.approval");
        if(plan.requiresApproval){
            if(expected ? initialOwner!=nullptr : initialOwner==nullptr){ RecordCommitPredicate(true); return Result::Invalid; }
            if(!planning&&(!approval||intent::ValidateApproval(bytes.size(),*approval)!=intent::Result::Ok)){ RecordCommitPredicate(true); return Result::Invalid; }
        }else if(approval){ RecordCommitPredicate(true); return Result::Invalid; }
        SetCommitStage("publish.origin");
        if (expected && !AcceptOrigin(*expected)) { RecordCommitPredicate(true); return Result::Conflict; }
        // Copy caller-owned bytes before file creation; input may alias output.
        SetCommitStage("publish.candidate");
        Snapshot candidate; candidate.root_ = root_; candidate.bytes_ = bytes;
        if (initialOwner) candidate.owner_ = *initialOwner;
        files::Directory directory;
        SetCommitStage("publish.OpenRoot");
        auto io = OpenRoot(false, directory);
        if (io != files::Result::Ok) { RecordCommitFiles(io); return expected ? RootRecoveryError(io) : RootOpenError(io); }
        SetCommitStage("publish.identity");
        io = directory.GetIdentity(candidate.rootIdentity_);
        if (io != files::Result::Ok) { RecordCommitFiles(io); return Convert(io); }
        SetCommitStage("publish.BindRoot");
        const auto bound=BindRoot(plan,directory,candidate.rootIdentity_,planning);
        if(bound!=Result::Ok){ RecordCommitCatalog(bound); return bound; }
        candidate.boundRoot_ = boundMode_;
        SetCommitStage("publish.rootIdentity");
        if (expected && !Equal(expected->rootIdentity_, candidate.rootIdentity_)) { RecordCommitPredicate(true); return Result::Conflict; }
        files::FileLock lock;
        SetCommitStage("lock");
        io = planning&&!plan.requiresApproval ? directory.Lock(0, lock) : directory.LockExisting(0, lock);
        if (io != files::Result::Ok) { RecordCommitFiles(io); return Convert(io); }
        files::Directory snapshots, commits, operations;
        SetCommitStage("publish.OpenChildren");
        auto result = OpenChildren(directory, false, snapshots, commits, &operations);
        if (result != Result::Ok) { RecordCommitCatalog(result); return expected && result == Result::Missing ? Result::RecoveryRequired : result; }
        if(plan.requiresApproval){
            SetCommitStage("publish.operations");
            if(!operations.IsValid()){ RecordCommitPredicate(true); return Result::Missing; }
            if(!planning){
                SetCommitStage("publish.approvalDirectories");
                result=ApprovalDirectories(*approval,directory,snapshots,commits,operations);
                if(result!=Result::Ok){ RecordCommitCatalog(result); return result; }
            }
        }
        Loaded current;
        SetCommitStage("publish.Load");
        result = Load(snapshots, commits, current);
        if (expected) {
            if (result != Result::Ok) { RecordCommitCatalog(result); return result == Result::Missing ? Result::RecoveryRequired : result; }
            SetCommitStage("publish.expectedVersion");
            if (expected->revision_ != current.revision || expected->owner_ != current.owner ||
                !Equal(expected->head_, current.head)) { RecordCommitPredicate(true); return Result::Conflict; }
            SetCommitStage("publish.expectedLimit");
            if (current.revision == (std::numeric_limits<std::uint64_t>::max)()) { RecordCommitPredicate(true); return Result::Limit; }
            candidate.owner_ = current.owner;
            candidate.revision_ = current.revision + 1;
        } else {
            SetCommitStage("publish.initial");
            if (result == Result::Ok) { RecordCommitPredicate(true); return Result::Conflict; }
            if (result != Result::Missing) { RecordCommitCatalog(result); return result; }
            SetCommitStage("publish.owner.random");
            if (!initialOwner && (!files::crypto::Random(candidate.owner_) ||
                std::all_of(candidate.owner_.begin(), candidate.owner_.end(), [](auto v) { return v == 0; }))) { RecordCommitPredicate(true); return Result::Io; }
            candidate.revision_ = 1;
            if(planning)plan.owner=candidate.owner_;
        }
        WriteFootprint footprint;
        SetCommitStage("publish.footprint");
        const auto sized=plan.requiresApproval?ApprovedPublicationFootprint(candidate.bytes_.size(),footprint):
            PublicationFootprint(candidate.bytes_.size(),footprint);
        if(sized!=Result::Ok){ RecordCommitCatalog(sized); return sized; }
        SetCommitStage("publish.footprint.bind");
        if(!BindFootprint(plan,footprint,planning)){ RecordCommitPredicate(true); return Result::Conflict; }
        const auto additional=LogicalBytes(footprint);
        SetCommitStage("publish.footprint.limit");
        if (current.commits >= MaximumCommits || current.snapshots >= MaximumSnapshots ||
            current.metadataBytes > MaximumMetadataBytes ||
            additional > MaximumMetadataBytes - current.metadataBytes) { RecordCommitPredicate(true); return Result::Limit; }
        if(planning)return Result::Ok;
        files::Spec spec; spec.owner = candidate.owner_; spec.kind = files::Kind::CatalogSnapshot; spec.key = codec::SnapshotKey;
        codec::Record record; record.owner = candidate.owner_; record.revision = candidate.revision_;
        SetCommitStage("publish.record.copy");
        record.previous = current.head;
        SetCommitStage("publish.operations.create");
        if (!operations.IsValid()) {
            MarkCommitWriteAttempted();
            Attempt(plan);
            io = directory.Child(L"operations", true, operations);
            if (io != files::Result::Ok) { RecordCommitFiles(io); return Convert(io); }
        }
        intent::Record operation;
        SetCommitStage("publish.operation.random");
        if (!files::crypto::Random(operation.operation) ||
            std::all_of(operation.operation.begin(), operation.operation.end(), [](auto v) { return v == 0; })) { RecordCommitPredicate(true); return Result::Io; }
        files::PreparedWrite snapshot, commit;
        struct UnregisteredPreparation {
            files::PreparedWrite& snapshot;
            files::PreparedWrite& commit;
            bool intentAttempted = false;
            ~UnregisteredPreparation() {
                // These CREATE_NEW handles have not been named by any durable
                // operation yet. Clean only locally handled preparation errors.
                if (!intentAttempted) {
                    if (commit.IsValid()) commit.Abort();
                    if (snapshot.IsValid()) snapshot.Abort();
                }
            }
        } prepared{snapshot, commit};
        MarkCommitWriteAttempted();
        Attempt(plan);
        SetCommitStage("snapshots.Prepare");
        io = snapshots.Prepare(spec, candidate.bytes_.data(), candidate.bytes_.size(), snapshot);
        if (io != files::Result::Ok) { RecordCommitFiles(io); return Convert(io); }
        SetCommitStage("snapshot.plan.copy");
        operation.snapshot = snapshot.Plan();
        SetCommitStage("intent.PlannedInfo");
        const auto plannedInfo = intent::PlannedInfo(operation.snapshot, record.snapshot);
        if (plannedInfo != intent::Result::Ok) { RecordCommitIntent(plannedInfo); return Result::Io; }
        std::vector<std::uint8_t> encoded;
        SetCommitStage("codec.Encode");
        const auto coded = codec::Encode(record, encoded);
        if (coded != codec::Result::Ok) { RecordCommitCodec(coded); return coded == codec::Result::Io ? Result::Io : Result::Invalid; }
        spec.owner = codec::NamespaceOwner; spec.kind = files::Kind::CatalogCommit; spec.key = codec::CommitKey;
        SetCommitStage("commits.Prepare");
        io = commits.Prepare(spec, encoded.data(), encoded.size(), commit);
        if (io != files::Result::Ok) { RecordCommitFiles(io); return Convert(io); }
        SetCommitStage("commit.plan.copy");
        operation.commit = commit.Plan();
        SetCommitStage("publication.copy");
        operation.publication = record;
        std::vector<std::uint8_t> operationBytes;
        SetCommitStage("intent.Encode");
        const auto planned = plan.requiresApproval?
            intent::EncodeApproved(intent::ApprovedRecord{operation,*approval},operationBytes):intent::Encode(operation, operationBytes);
        if (planned != intent::Result::Ok) { RecordCommitIntent(planned); return planned == intent::Result::Io ? Result::Io : Result::Invalid; }
        files::BlobInfo operationFile;
        prepared.intentAttempted = true;
        SetCommitStage("WriteOperation");
        result = WriteOperation(operations, operationBytes, false, operationFile,plan.requiresApproval);
        if (result != Result::Ok) { RecordCommitCatalog(result); return result; }
        files::BlobInfo writtenSnapshot;
        SetCommitStage("snapshot.Complete");
        io = snapshot.Complete(candidate.bytes_.data(), candidate.bytes_.size(), writtenSnapshot);
        if (io != files::Result::Ok) { RecordCommitFiles(io); return Convert(io); }
        SetCommitStage("snapshot.info");
        if (!Equal(writtenSnapshot, record.snapshot)) { RecordCommitPredicate(true); return Result::RecoveryRequired; }
        files::ReadLease newSnapshot;
        SetCommitStage("snapshots.Acquire");
        io = snapshots.Acquire(record.snapshot, newSnapshot);
        if (io != files::Result::Ok) { RecordCommitFiles(io); return StoredError(io); }
        SetCommitStage("commit.Complete");
        MarkCommitAttempted();
        commitAttempted = true;
        io = commit.Complete(encoded.data(), encoded.size(), candidate.head_);
        if (io != files::Result::Ok) { RecordCommitFiles(io); return Result::Indeterminate; }
        SetCommitStage("RemoveOperationFile");
        result = RemoveOperationFile(operations, operationFile);
        if (result != Result::Ok) { RecordCommitCatalog(result); return Result::Indeterminate; }
        SetCommitStage("Swap");
        candidate.valid_ = true;
        candidate.Swap(output);
        return Result::Ok;
    } catch (...) { RecordCommitException(); return commitAttempted ? Result::Indeterminate : Result::Io; }
}
}
