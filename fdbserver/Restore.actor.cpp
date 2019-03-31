/*
 * Restore.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "fdbserver/RestoreInterface.h"
#include "fdbclient/NativeAPI.actor.h"
#include "fdbclient/SystemData.h"
#include "flow/actorcompiler.h"  // This must be the last #include.

// Backup agent header
#include "fdbclient/BackupAgent.h"
//#include "FileBackupAgent.h"
#include "fdbclient/ManagementAPI.h"
#include "fdbclient/MutationList.h"
#include "fdbclient/BackupContainer.h"

#include <ctime>
#include <climits>
#include "fdbrpc/IAsyncFile.h"
#include "flow/genericactors.actor.h"
#include "flow/Hash3.h"
#include <numeric>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <algorithm>

const int min_num_workers = 3; //10; // TODO: This can become a configuration param later
const int ratio_loader_to_applier = 1; // the ratio of loader over applier. The loader number = total worker * (ratio /  (ratio + 1) )

int FastRestore_Failure_Timeout = 3600; // seconds

class RestoreConfig;
struct RestoreData; // Only declare the struct exist but we cannot use its field

bool concatenateBackupMutationForLogFile(Reference<RestoreData> rd, Standalone<StringRef> val_input, Standalone<StringRef> key_input);
Future<Void> registerMutationsToApplier(Reference<RestoreData> const& rd);
Future<Void> notifyApplierToApplyMutations(Reference<RestoreData> const& rd);
Future<Void> registerMutationsToMasterApplier(Reference<RestoreData> const& rd);
Future<Void> sampleHandler(Reference<RestoreData> const& rd, RestoreCommandInterface const& interf, RestoreCommandInterface const& leaderInter);
Future<Void> receiveSampledMutations(Reference<RestoreData> const& rd, RestoreCommandInterface const& interf);
static Future<Void> finishRestore(Database const& cx, Standalone<VectorRef<RestoreRequest>> const& restoreRequests); // Forward declaration
void sanityCheckMutationOps(Reference<RestoreData> rd);
void printRestorableFileSet(Optional<RestorableFileSet> files);
void parseSerializedMutation(Reference<RestoreData> rd, bool isSampling = false);

// Helper class for reading restore data from a buffer and throwing the right errors.
struct StringRefReaderMX {
	StringRefReaderMX(StringRef s = StringRef(), Error e = Error()) : rptr(s.begin()), end(s.end()), failure_error(e), str_size(s.size()) {}

	// Return remainder of data as a StringRef
	StringRef remainder() {
		return StringRef(rptr, end - rptr);
	}

	// Return a pointer to len bytes at the current read position and advance read pos
	//Consume a little-Endian data. Since we only run on little-Endian machine, the data on storage is little Endian
	const uint8_t * consume(unsigned int len) {
		if(rptr == end && len != 0)
			throw end_of_stream();
		const uint8_t *p = rptr;
		rptr += len;
		if(rptr > end) {
			printf("[ERROR] StringRefReaderMX throw error! string length:%d\n", str_size);
			printf("!!!!!!!!!!!![ERROR]!!!!!!!!!!!!!! Worker may die due to the error. Master will stuck when a worker die\n");
			throw failure_error;
		}
		return p;
	}

	// Return a T from the current read position and advance read pos
	template<typename T> const T consume() {
		return *(const T *)consume(sizeof(T));
	}

	// Functions for consuming big endian (network byte order) integers.
	// Consumes a big endian number, swaps it to little endian, and returns it.
	const int32_t  consumeNetworkInt32()  { return (int32_t)bigEndian32((uint32_t)consume< int32_t>());}
	const uint32_t consumeNetworkUInt32() { return          bigEndian32(          consume<uint32_t>());}

	const int64_t  consumeNetworkInt64()  { return (int64_t)bigEndian64((uint32_t)consume< int64_t>());}
	const uint64_t consumeNetworkUInt64() { return          bigEndian64(          consume<uint64_t>());}

	bool eof() { return rptr == end; }

	const uint8_t *rptr, *end;
	const int str_size;
	Error failure_error;
};

bool debug_verbose = true;


////-- Restore code declaration START
//TODO: Move to RestoreData
//std::map<Version, Standalone<VectorRef<MutationRef>>> kvOps;
////std::map<Version, std::vector<MutationRef>> kvOps; //TODO: Must change to standAlone before run correctness test. otherwise, you will see the mutationref memory is corrupted
//std::map<Standalone<StringRef>, Standalone<StringRef>> mutationMap; //key is the unique identifier for a batch of mutation logs at the same version
//std::map<Standalone<StringRef>, uint32_t> mutationPartMap; //Record the most recent
// MXX: Important: Can not use std::vector because you won't have the arena and you will hold the reference to memory that will be freed.
// Use push_back_deep() to copy data to the standalone arena.
//Standalone<VectorRef<MutationRef>> mOps;
std::vector<MutationRef> mOps;


void printGlobalNodeStatus(Reference<RestoreData>);


std::vector<std::string> RestoreRoleStr = {"Invalid", "Master", "Loader", "Applier"};
int numRoles = RestoreRoleStr.size();
std::string getRoleStr(RestoreRole role) {
	if ( (int) role >= numRoles || (int) role < 0) {
		printf("[ERROR] role:%d is out of scope\n", (int) role);
		return "[Unset]";
	}
	return RestoreRoleStr[(int)role];
}


const char *RestoreCommandEnumStr[] = {"Init",
		"Set_Role", "Set_Role_Done",
		"Sample_Range_File", "Sample_Log_File", "Sample_File_Done",
		"Loader_Send_Sample_Mutation_To_Applier", "Loader_Send_Sample_Mutation_To_Applier_Done",
		"Calculate_Applier_KeyRange", "Get_Applier_KeyRange", "Get_Applier_KeyRange_Done",
		"Assign_Applier_KeyRange", "Assign_Applier_KeyRange_Done",
		"Assign_Loader_Range_File", "Assign_Loader_Log_File", "Assign_Loader_File_Done",
		"Loader_Send_Mutations_To_Applier", "Loader_Send_Mutations_To_Applier_Done",
		"Apply_Mutation_To_DB", "Apply_Mutation_To_DB_Skip",
		"Loader_Notify_Appler_To_Apply_Mutation",
		"Notify_Loader_ApplierKeyRange", "Notify_Loader_ApplierKeyRange_Done"
};


////--- Parse backup files

// For convenience
typedef FileBackupAgent::ERestoreState ERestoreState;
template<> Tuple Codec<ERestoreState>::pack(ERestoreState const &val); // { return Tuple().append(val); }
template<> ERestoreState Codec<ERestoreState>::unpack(Tuple const &val); // { return (ERestoreState)val.getInt(0); }

// RestoreConfig copied from FileBackupAgent.actor.cpp
// We copy RestoreConfig instead of using (and potentially changing) it in place to avoid conflict with the existing code
class RestoreConfig : public KeyBackedConfig, public ReferenceCounted<RestoreConfig> {
public:
	RestoreConfig(UID uid = UID()) : KeyBackedConfig(fileRestorePrefixRange.begin, uid) {}
	RestoreConfig(Reference<Task> task) : KeyBackedConfig(fileRestorePrefixRange.begin, task) {}

	KeyBackedProperty<ERestoreState> stateEnum() {
		return configSpace.pack(LiteralStringRef(__FUNCTION__));
	}
	Future<StringRef> stateText(Reference<ReadYourWritesTransaction> tr) {
		return map(stateEnum().getD(tr), [](ERestoreState s) -> StringRef { return FileBackupAgent::restoreStateText(s); });
	}
	KeyBackedProperty<Key> addPrefix() {
		return configSpace.pack(LiteralStringRef(__FUNCTION__));
	}
	KeyBackedProperty<Key> removePrefix() {
		return configSpace.pack(LiteralStringRef(__FUNCTION__));
	}
	KeyBackedProperty<KeyRange> restoreRange() {
		return configSpace.pack(LiteralStringRef(__FUNCTION__));
	}
	KeyBackedProperty<Key> batchFuture() {
		return configSpace.pack(LiteralStringRef(__FUNCTION__));
	}
	KeyBackedProperty<Version> restoreVersion() {
		return configSpace.pack(LiteralStringRef(__FUNCTION__));
	}

	KeyBackedProperty<Reference<IBackupContainer>> sourceContainer() {
		return configSpace.pack(LiteralStringRef(__FUNCTION__));
	}
	// Get the source container as a bare URL, without creating a container instance
	KeyBackedProperty<Value> sourceContainerURL() {
		return configSpace.pack(LiteralStringRef("sourceContainer"));
	}

	// Total bytes written by all log and range restore tasks.
	KeyBackedBinaryValue<int64_t> bytesWritten() {
		return configSpace.pack(LiteralStringRef(__FUNCTION__));
	}
	// File blocks that have had tasks created for them by the Dispatch task
	KeyBackedBinaryValue<int64_t> filesBlocksDispatched() {
		return configSpace.pack(LiteralStringRef(__FUNCTION__));
	}
	// File blocks whose tasks have finished
	KeyBackedBinaryValue<int64_t> fileBlocksFinished() {
		return configSpace.pack(LiteralStringRef(__FUNCTION__));
	}
	// Total number of files in the fileMap
	KeyBackedBinaryValue<int64_t> fileCount() {
		return configSpace.pack(LiteralStringRef(__FUNCTION__));
	}
	// Total number of file blocks in the fileMap
	KeyBackedBinaryValue<int64_t> fileBlockCount() {
		return configSpace.pack(LiteralStringRef(__FUNCTION__));
	}

	// Describes a file to load blocks from during restore.  Ordered by version and then fileName to enable
	// incrementally advancing through the map, saving the version and path of the next starting point.
	struct RestoreFile {
		Version version;
		std::string fileName;
		bool isRange;  // false for log file
		int64_t blockSize;
		int64_t fileSize;
		Version endVersion;  // not meaningful for range files
		Version beginVersion;  // range file's beginVersion == endVersion; log file contains mutations in version [beginVersion, endVersion)
		int64_t cursor; //The start block location to be restored. All blocks before cursor have been scheduled to load and restore

		Tuple pack() const {
			return Tuple()
					.append(version)
					.append(StringRef(fileName))
					.append(isRange)
					.append(fileSize)
					.append(blockSize)
					.append(endVersion)
					.append(beginVersion)
					.append(cursor);
		}
		static RestoreFile unpack(Tuple const &t) {
			RestoreFile r;
			int i = 0;
			r.version = t.getInt(i++);
			r.fileName = t.getString(i++).toString();
			r.isRange = t.getInt(i++) != 0;
			r.fileSize = t.getInt(i++);
			r.blockSize = t.getInt(i++);
			r.endVersion = t.getInt(i++);
			r.beginVersion = t.getInt(i++);
			r.cursor = t.getInt(i++);
			return r;
		}

		bool operator<(const RestoreFile& rhs) const { return endVersion < rhs.endVersion; }

		std::string toString() const {
//			return "UNSET4TestHardness";
			return "version:" + std::to_string(version) + " fileName:" + fileName +" isRange:" + std::to_string(isRange)
				   + " blockSize:" + std::to_string(blockSize) + " fileSize:" + std::to_string(fileSize)
				   + " endVersion:" + std::to_string(endVersion) + std::to_string(beginVersion)  + " cursor:" + std::to_string(cursor);
		}
	};

	typedef KeyBackedSet<RestoreFile> FileSetT;
	FileSetT fileSet() {
		return configSpace.pack(LiteralStringRef(__FUNCTION__));
	}

	Future<bool> isRunnable(Reference<ReadYourWritesTransaction> tr) {
		return map(stateEnum().getD(tr), [](ERestoreState s) -> bool { return   s != ERestoreState::ABORTED
																				&& s != ERestoreState::COMPLETED
																				&& s != ERestoreState::UNITIALIZED;
		});
	}

	Future<Void> logError(Database cx, Error e, std::string const &details, void *taskInstance = nullptr) {
		if(!uid.isValid()) {
			TraceEvent(SevError, "FileRestoreErrorNoUID").error(e).detail("Description", details);
			return Void();
		}
		TraceEvent t(SevWarn, "FileRestoreError");
		t.error(e).detail("RestoreUID", uid).detail("Description", details).detail("TaskInstance", (uint64_t)taskInstance);
		// These should not happen
		if(e.code() == error_code_key_not_found)
			t.backtrace();

		return updateErrorInfo(cx, e, details);
	}

	Key mutationLogPrefix() {
		return uidPrefixKey(applyLogKeys.begin, uid);
	}

	Key applyMutationsMapPrefix() {
		return uidPrefixKey(applyMutationsKeyVersionMapRange.begin, uid);
	}

	ACTOR static Future<int64_t> getApplyVersionLag_impl(Reference<ReadYourWritesTransaction> tr, UID uid) {
		// Both of these are snapshot reads
		state Future<Optional<Value>> beginVal = tr->get(uidPrefixKey(applyMutationsBeginRange.begin, uid), true);
		state Future<Optional<Value>> endVal = tr->get(uidPrefixKey(applyMutationsEndRange.begin, uid), true);
		wait(success(beginVal) && success(endVal));

		if(!beginVal.get().present() || !endVal.get().present())
			return 0;

		Version beginVersion = BinaryReader::fromStringRef<Version>(beginVal.get().get(), Unversioned());
		Version endVersion = BinaryReader::fromStringRef<Version>(endVal.get().get(), Unversioned());
		return endVersion - beginVersion;
	}

	Future<int64_t> getApplyVersionLag(Reference<ReadYourWritesTransaction> tr) {
		return getApplyVersionLag_impl(tr, uid);
	}

	void initApplyMutations(Reference<ReadYourWritesTransaction> tr, Key addPrefix, Key removePrefix) {
		// Set these because they have to match the applyMutations values.
		this->addPrefix().set(tr, addPrefix);
		this->removePrefix().set(tr, removePrefix);

		clearApplyMutationsKeys(tr);

		// Initialize add/remove prefix, range version map count and set the map's start key to InvalidVersion
		tr->set(uidPrefixKey(applyMutationsAddPrefixRange.begin, uid), addPrefix);
		tr->set(uidPrefixKey(applyMutationsRemovePrefixRange.begin, uid), removePrefix);
		int64_t startCount = 0;
		tr->set(uidPrefixKey(applyMutationsKeyVersionCountRange.begin, uid), StringRef((uint8_t*)&startCount, 8));
		Key mapStart = uidPrefixKey(applyMutationsKeyVersionMapRange.begin, uid);
		tr->set(mapStart, BinaryWriter::toValue<Version>(invalidVersion, Unversioned()));
	}

	void clearApplyMutationsKeys(Reference<ReadYourWritesTransaction> tr) {
		tr->setOption(FDBTransactionOptions::COMMIT_ON_FIRST_PROXY);

		// Clear add/remove prefix keys
		tr->clear(uidPrefixKey(applyMutationsAddPrefixRange.begin, uid));
		tr->clear(uidPrefixKey(applyMutationsRemovePrefixRange.begin, uid));

		// Clear range version map and count key
		tr->clear(uidPrefixKey(applyMutationsKeyVersionCountRange.begin, uid));
		Key mapStart = uidPrefixKey(applyMutationsKeyVersionMapRange.begin, uid);
		tr->clear(KeyRangeRef(mapStart, strinc(mapStart)));

		// Clear any loaded mutations that have not yet been applied
		Key mutationPrefix = mutationLogPrefix();
		tr->clear(KeyRangeRef(mutationPrefix, strinc(mutationPrefix)));

		// Clear end and begin versions (intentionally in this order)
		tr->clear(uidPrefixKey(applyMutationsEndRange.begin, uid));
		tr->clear(uidPrefixKey(applyMutationsBeginRange.begin, uid));
	}

	void setApplyBeginVersion(Reference<ReadYourWritesTransaction> tr, Version ver) {
		tr->set(uidPrefixKey(applyMutationsBeginRange.begin, uid), BinaryWriter::toValue(ver, Unversioned()));
	}

	void setApplyEndVersion(Reference<ReadYourWritesTransaction> tr, Version ver) {
		tr->set(uidPrefixKey(applyMutationsEndRange.begin, uid), BinaryWriter::toValue(ver, Unversioned()));
	}

	Future<Version> getApplyEndVersion(Reference<ReadYourWritesTransaction> tr) {
		return map(tr->get(uidPrefixKey(applyMutationsEndRange.begin, uid)), [=](Optional<Value> const &value) -> Version {
			return value.present() ? BinaryReader::fromStringRef<Version>(value.get(), Unversioned()) : 0;
		});
	}

	static Future<std::string> getProgress_impl(Reference<RestoreConfig> const &restore, Reference<ReadYourWritesTransaction> const &tr);
	Future<std::string> getProgress(Reference<ReadYourWritesTransaction> tr) {
		Reference<RestoreConfig> restore = Reference<RestoreConfig>(this);
		return getProgress_impl(restore, tr);
	}

	static Future<std::string> getFullStatus_impl(Reference<RestoreConfig> const &restore, Reference<ReadYourWritesTransaction> const &tr);
	Future<std::string> getFullStatus(Reference<ReadYourWritesTransaction> tr) {
		Reference<RestoreConfig> restore = Reference<RestoreConfig>(this);
		return getFullStatus_impl(restore, tr);
	}

	std::string toString() {
		std::string ret = "uid:" + uid.toString() + " prefix:" + prefix.contents().toString();
		return ret;
	}

};

typedef RestoreConfig::RestoreFile RestoreFile;

// parallelFileRestore is copied from FileBackupAgent.actor.cpp for the same reason as RestoreConfig is copied
namespace parallelFileRestore {
	// Helper class for reading restore data from a buffer and throwing the right errors.
	struct StringRefReader {
		StringRefReader(StringRef s = StringRef(), Error e = Error()) : rptr(s.begin()), end(s.end()), failure_error(e) {}

		// Return remainder of data as a StringRef
		StringRef remainder() {
			return StringRef(rptr, end - rptr);
		}

		// Return a pointer to len bytes at the current read position and advance read pos
		const uint8_t * consume(unsigned int len) {
			if(rptr == end && len != 0)
				throw end_of_stream();
			const uint8_t *p = rptr;
			rptr += len;
			if(rptr > end)
				throw failure_error;
			return p;
		}

		// Return a T from the current read position and advance read pos
		template<typename T> const T consume() {
			return *(const T *)consume(sizeof(T));
		}

		// Functions for consuming big endian (network byte order) integers.
		// Consumes a big endian number, swaps it to little endian, and returns it.
		const int32_t  consumeNetworkInt32()  { return (int32_t)bigEndian32((uint32_t)consume< int32_t>());}
		const uint32_t consumeNetworkUInt32() { return          bigEndian32(          consume<uint32_t>());}

		bool eof() { return rptr == end; }

		const uint8_t *rptr, *end;
		Error failure_error;
	};


	ACTOR Future<Standalone<VectorRef<KeyValueRef>>> decodeRangeFileBlock(Reference<IAsyncFile> file, int64_t offset, int len) {
		state Standalone<StringRef> buf = makeString(len);
		int rLen = wait(file->read(mutateString(buf), len, offset));
		if(rLen != len)
			throw restore_bad_read();

		Standalone<VectorRef<KeyValueRef>> results({}, buf.arena());
		state StringRefReader reader(buf, restore_corrupted_data());

		try {
			// Read header, currently only decoding version 1001
			if(reader.consume<int32_t>() != 1001)
				throw restore_unsupported_file_version();

			// Read begin key, if this fails then block was invalid.
			uint32_t kLen = reader.consumeNetworkUInt32();
			const uint8_t *k = reader.consume(kLen);
			results.push_back(results.arena(), KeyValueRef(KeyRef(k, kLen), ValueRef()));

			// Read kv pairs and end key
			while(1) {
				// Read a key.
				kLen = reader.consumeNetworkUInt32();
				k = reader.consume(kLen);

				// If eof reached or first value len byte is 0xFF then a valid block end was reached.
				if(reader.eof() || *reader.rptr == 0xFF) {
					results.push_back(results.arena(), KeyValueRef(KeyRef(k, kLen), ValueRef()));
					break;
				}

				// Read a value, which must exist or the block is invalid
				uint32_t vLen = reader.consumeNetworkUInt32();
				const uint8_t *v = reader.consume(vLen);
				results.push_back(results.arena(), KeyValueRef(KeyRef(k, kLen), ValueRef(v, vLen)));

				// If eof reached or first byte of next key len is 0xFF then a valid block end was reached.
				if(reader.eof() || *reader.rptr == 0xFF)
					break;
			}

			// Make sure any remaining bytes in the block are 0xFF
			for(auto b : reader.remainder())
				if(b != 0xFF)
					throw restore_corrupted_data_padding();

			return results;

		} catch(Error &e) {
			TraceEvent(SevWarn, "FileRestoreCorruptRangeFileBlock")
				.error(e)
				.detail("Filename", file->getFilename())
				.detail("BlockOffset", offset)
				.detail("BlockLen", len)
				.detail("ErrorRelativeOffset", reader.rptr - buf.begin())
				.detail("ErrorAbsoluteOffset", reader.rptr - buf.begin() + offset);
			throw;
		}
	}


	ACTOR Future<Standalone<VectorRef<KeyValueRef>>> decodeLogFileBlock(Reference<IAsyncFile> file, int64_t offset, int len) {
		state Standalone<StringRef> buf = makeString(len);
		int rLen = wait(file->read(mutateString(buf), len, offset));
		if(rLen != len)
			throw restore_bad_read();

		Standalone<VectorRef<KeyValueRef>> results({}, buf.arena());
		state StringRefReader reader(buf, restore_corrupted_data());

		try {
			// Read header, currently only decoding version 2001
			if(reader.consume<int32_t>() != 2001)
				throw restore_unsupported_file_version();

			// Read k/v pairs.  Block ends either at end of last value exactly or with 0xFF as first key len byte.
			while(1) {
				// If eof reached or first key len bytes is 0xFF then end of block was reached.
				if(reader.eof() || *reader.rptr == 0xFF)
					break;

				// Read key and value.  If anything throws then there is a problem.
				uint32_t kLen = reader.consumeNetworkUInt32();
				const uint8_t *k = reader.consume(kLen);
				uint32_t vLen = reader.consumeNetworkUInt32();
				const uint8_t *v = reader.consume(vLen);

				results.push_back(results.arena(), KeyValueRef(KeyRef(k, kLen), ValueRef(v, vLen)));
			}

			// Make sure any remaining bytes in the block are 0xFF
			for(auto b : reader.remainder())
				if(b != 0xFF)
					throw restore_corrupted_data_padding();

			return results;

		} catch(Error &e) {
			TraceEvent(SevWarn, "FileRestoreCorruptLogFileBlock")
				.error(e)
				.detail("Filename", file->getFilename())
				.detail("BlockOffset", offset)
				.detail("BlockLen", len)
				.detail("ErrorRelativeOffset", reader.rptr - buf.begin())
				.detail("ErrorAbsoluteOffset", reader.rptr - buf.begin() + offset);
			throw;
		}
	}


}

// CMDUID implementation
void CMDUID::initPhase(RestoreCommandEnum newPhase) {
	printf("CMDID, current phase:%d, new phase:%d\n", phase, newPhase);
	phase = (uint16_t) newPhase;
	cmdID = 0;
}

void CMDUID::nextPhase() {
	phase++;
	cmdID = 0;
}

void CMDUID::nextCmd() {
	cmdID++;
}

RestoreCommandEnum CMDUID::getPhase() {
	return (RestoreCommandEnum) phase;
}

void CMDUID::setPhase(RestoreCommandEnum newPhase) {
	phase = (uint16_t) newPhase;
}

void CMDUID::setBatch(int newBatchIndex) {
	batch = newBatchIndex;
}


uint64_t CMDUID::getIndex() {
	return cmdID;
}

std::string CMDUID::toString() const {
	return format("%04ld|%04ld|%016lld", batch, phase, cmdID);
}

// getPreviousCmd help provide better debug information
// getPreviousCmd will return the last command type used in the previous phase before input curCmd
// Because the cmd sender waits on all acks from the previous phase, at any phase, the cmd receiver needs to reply to the sender if it receives a cmd from its previous phase.
// However, if receiver receives a cmd that is not in the current or previous phase, it is highly possible there is an error.
// RestoreCommandEnum getPreviousCmd(RestoreCommandEnum curCmd) {
// 	RestoreCommandEnum ret = RestoreCommandEnum::Init;
// 	switch (curCmd) {
// 		case RestoreCommandEnum::Set_Role_Done:
// 			ret = RestoreCommandEnum::Set_Role_Done;
// 			break;
// 		case RestoreCommandEnum::Sample_File_Done: // On each loader
// 			ret = RestoreCommandEnum::Set_Role_Done; // or RestoreCommandEnum::Assign_Loader_File_Done or RestoreCommandEnum::Loader_Notify_Appler_To_Apply_Mutation
// 			break;
// 		case RestoreCommandEnum::Notify_Loader_ApplierKeyRange_Done: // On each loader
// 			ret = RestoreCommandEnum::Sample_File_Done;
// 			break;
// 		case RestoreCommandEnum::Assign_Loader_File_Done: // On each loader: The end command for each version batch
// 			ret = RestoreCommandEnum::Notify_Loader_ApplierKeyRange_Done;
// 			break;
		
// 		case RestoreCommandEnum::Get_Applier_KeyRange_Done: // On master applier
// 			ret = RestoreCommandEnum::Loader_Send_Sample_Mutation_To_Applier_Done;
// 			break;
// 		case RestoreCommandEnum::Assign_Applier_KeyRange_Done: // On master applier and other appliers
// 			ret = RestoreCommandEnum::Get_Applier_KeyRange_Done;
// 			break;
// 		case RestoreCommandEnum::Loader_Send_Mutations_To_Applier_Done: // On each applier
// 			ret = RestoreCommandEnum::Assign_Applier_KeyRange_Done;
// 			break;
// 		case RestoreCommandEnum::Loader_Notify_Appler_To_Apply_Mutation: // On each applier
// 			ret = RestoreCommandEnum::Loader_Send_Mutations_To_Applier_Done;
// 			break; 
// 		case RestoreCommandEnum::Loader_Send_Sample_Mutation_To_Applier_Done: // On master applier
// 			ret = RestoreCommandEnum::Set_Role_Done;
// 			break;
		
// 		default:
// 			ret = RestoreCommandEnum::Init;
// 			fprintf(stderr, "[ERROR] GetPreviousCmd Unknown curCmd:%d\n", curCmd);
// 			break;
// 	}

// 	return ret;
// }

std::string getPreviousCmdStr(RestoreCommandEnum curCmd) {
	std::string ret = RestoreCommandEnumStr[(int) RestoreCommandEnum::Init];
	switch (curCmd) {
		case RestoreCommandEnum::Set_Role_Done:
			ret = RestoreCommandEnumStr[(int)RestoreCommandEnum::Set_Role_Done];
			break;
		case RestoreCommandEnum::Sample_File_Done: // On each loader
			ret = std::string(RestoreCommandEnumStr[(int)RestoreCommandEnum::Set_Role_Done]) + "|" 
				+ RestoreCommandEnumStr[(int)RestoreCommandEnum::Assign_Loader_File_Done] + "|"
				+ RestoreCommandEnumStr[(int)RestoreCommandEnum::Loader_Notify_Appler_To_Apply_Mutation];
			break;
		case RestoreCommandEnum::Notify_Loader_ApplierKeyRange_Done: // On each loader
			ret = RestoreCommandEnumStr[(int)RestoreCommandEnum::Sample_File_Done];
			break;
		case RestoreCommandEnum::Assign_Loader_File_Done: // On each loader: The end command for each version batch
			ret = RestoreCommandEnumStr[(int)RestoreCommandEnum::Notify_Loader_ApplierKeyRange_Done];
			break;
		
		case RestoreCommandEnum::Get_Applier_KeyRange_Done: // On master applier
			ret = RestoreCommandEnumStr[(int)RestoreCommandEnum::Loader_Send_Sample_Mutation_To_Applier_Done];
			break;
		case RestoreCommandEnum::Assign_Applier_KeyRange_Done: // On master applier and other appliers
			ret = RestoreCommandEnumStr[(int)RestoreCommandEnum::Get_Applier_KeyRange_Done];
			break;
		case RestoreCommandEnum::Loader_Send_Mutations_To_Applier_Done: // On each applier
			ret = RestoreCommandEnumStr[(int)RestoreCommandEnum::Assign_Applier_KeyRange_Done];
			break;
		case RestoreCommandEnum::Loader_Notify_Appler_To_Apply_Mutation: // On each applier
			ret = RestoreCommandEnumStr[(int)RestoreCommandEnum::Loader_Send_Mutations_To_Applier_Done];
			break; 
		case RestoreCommandEnum::Loader_Send_Sample_Mutation_To_Applier_Done: // On master applier
			ret = RestoreCommandEnumStr[(int)RestoreCommandEnum::Set_Role_Done];
			break;
		
		default:
			ret = RestoreCommandEnumStr[(int)RestoreCommandEnum::Init];
			fprintf(stderr, "[ERROR] GetPreviousCmd Unknown curCmd:%d\n", curCmd);
			break;
	}

	return ret;
}

bool IsCmdInPreviousPhase(RestoreCommandEnum curCmd, RestoreCommandEnum receivedCmd) {
	bool ret = false;
	switch (curCmd) {
		case RestoreCommandEnum::Set_Role_Done:
			ret = (receivedCmd == RestoreCommandEnum::Set_Role_Done);
			break;
		case RestoreCommandEnum::Sample_File_Done: // On each loader
			ret = (receivedCmd == RestoreCommandEnum::Set_Role_Done || receivedCmd == RestoreCommandEnum::Assign_Loader_File_Done || receivedCmd == RestoreCommandEnum::Loader_Notify_Appler_To_Apply_Mutation);
			break;
		case RestoreCommandEnum::Notify_Loader_ApplierKeyRange_Done: // On each loader
			ret = (receivedCmd == RestoreCommandEnum::Sample_File_Done);
			break;
		case RestoreCommandEnum::Assign_Loader_File_Done: // On each loader: The end command for each version batch
			ret = (receivedCmd == RestoreCommandEnum::Notify_Loader_ApplierKeyRange_Done);
			break;
		
		case RestoreCommandEnum::Get_Applier_KeyRange_Done: // On master applier
			ret = (receivedCmd == RestoreCommandEnum::Loader_Send_Sample_Mutation_To_Applier_Done);
			break;
		case RestoreCommandEnum::Assign_Applier_KeyRange_Done: // On master applier and other appliers
			ret = (receivedCmd == RestoreCommandEnum::Get_Applier_KeyRange_Done || receivedCmd == RestoreCommandEnum::Set_Role_Done || receivedCmd == RestoreCommandEnum::Loader_Notify_Appler_To_Apply_Mutation);
			break;
		case RestoreCommandEnum::Loader_Send_Mutations_To_Applier_Done: // On each applier
			ret = (receivedCmd == RestoreCommandEnum::Assign_Applier_KeyRange_Done);
			break;
		case RestoreCommandEnum::Loader_Notify_Appler_To_Apply_Mutation: // On each applier
			ret = (receivedCmd == RestoreCommandEnum::Loader_Send_Mutations_To_Applier_Done);
			break; 
		case RestoreCommandEnum::Loader_Send_Sample_Mutation_To_Applier_Done: // On master applier
			ret = (receivedCmd == RestoreCommandEnum::Set_Role_Done || receivedCmd == RestoreCommandEnum::Loader_Notify_Appler_To_Apply_Mutation);
			break;
		
		default:
			fprintf(stderr, "[ERROR] GetPreviousCmd Unknown curCmd:%d\n", curCmd);
			break;
	}

	return ret;

}

// DEBUG_FAST_RESTORE is not used any more
#define DEBUG_FAST_RESTORE 1

#ifdef DEBUG_FAST_RESTORE
#define dbprintf_rs(fmt, args...)	printf(fmt, ## args);
#else
#define dbprintf_rs(fmt, args...)
#endif

// RestoreData is the context for each restore process (worker and master)
struct RestoreData : NonCopyable, public ReferenceCounted<RestoreData>  {
	//---- Declare status structure which records the progress and status of each worker in each role
	std::map<UID, RestoreCommandInterface> workers_interface; // UID is worker's node id, RestoreCommandInterface is worker's communication interface
	UID masterApplier; //TODO: Remove this variable. The first version uses 1 applier to apply the mutations

	RestoreNodeStatus localNodeStatus; //Each worker node (process) has one such variable.
	std::vector<RestoreNodeStatus> globalNodeStatus; // status of all notes, excluding master node, stored in master node // May change to map, like servers_info

	// range2Applier is in master and loader node. Loader node uses this to determine which applier a mutation should be sent
	std::map<Standalone<KeyRef>, UID> range2Applier; // KeyRef is the inclusive lower bound of the key range the applier (UID) is responsible for
	std::map<Standalone<KeyRef>, int> keyOpsCount; // The number of operations per key which is used to determine the key-range boundary for appliers
	int numSampledMutations; // The total number of mutations received from sampled data.

	struct ApplierStatus { // NOT USED //TODO: Remove this
		UID id;
		KeyRange keyRange; // the key range the applier is responsible for
		// Applier state is changed at the following event
		// Init: when applier's role is set
		// Assigned: when applier is set for a key range to be respoinsible for
		// Applying: when applier starts to apply the mutations to DB after receiving the cmd from loader
		// Done: when applier has finished applying the mutation and notify the master. It will change to Assigned after Done
		enum class ApplierState {Invalid = 0, Init = 1, Assigned, Applying, Done};
		ApplierState state;
	};
	ApplierStatus applierStatus;

	// LoadingState is a state machine, each state is set in the following event:
	// Init: when master starts to collect all files before ask loaders to load data
	// Assigned: when master sends out the loading cmd to loader to load a block of data
	// Loading: when master receives the ack. responds from the loader about the loading cmd
	// Applying: when master receives from applier that the applier starts to apply the results for the load cmd
	// Done: when master receives from applier that the applier has finished applying the results for the load cmd
	// When LoadingState becomes done, master knows the particular backup file block has been applied (restored) to DB
	enum class LoadingState {Invalid = 0, Init = 1, Assigned, Loading, Applying, Done};
	// TODO: RestoreStatus
	// Information of the backup files to be restored, and the restore progress
	struct LoadingStatus {
		RestoreFile file;
		int64_t start; // Starting point of the block in the file to load
		int64_t length;// Length of block to load
		LoadingState state; // Loading state of the particular file block
		UID node; // The loader node ID that responsible for the file block

		explicit LoadingStatus() {}
		explicit LoadingStatus(RestoreFile file, int64_t start, int64_t length, UID node): file(file), start(start), length(length), state(LoadingState::Init), node(node) {}
	};
	std::map<int64_t, LoadingStatus> loadingStatus; // first is the global index of the loading cmd, starting from 0

	 //Loader's state to handle the duplicate delivery of loading commands
	std::map<std::string, int> processedFiles; //first is filename of processed file, second is not used
	std::map<CMDUID, int> processedCmd;


	std::vector<RestoreFile> allFiles; // All backup files to be processed in all version batches
	std::vector<RestoreFile> files; // Backup files to be parsed and applied: range and log files in 1 version batch
	std::map<Version, Version> forbiddenVersions; // forbidden version range [first, second)

	// Temporary data structure for parsing range and log files into (version, <K, V, mutationType>)
	std::map<Version, Standalone<VectorRef<MutationRef>>> kvOps;
	// Must use StandAlone to save mutations, otherwise, the mutationref memory will be corrupted
	std::map<Standalone<StringRef>, Standalone<StringRef>> mutationMap; // Key is the unique identifier for a batch of mutation logs at the same version
	std::map<Standalone<StringRef>, uint32_t> mutationPartMap; // Record the most recent

	// Command id to record the progress
	CMDUID cmdID;

	bool isCmdProcessed(CMDUID const &cmdID) {
		return processedCmd.find(cmdID) != processedCmd.end();
	}

	// Describe the node information
	std::string describeNode() {
		return "[Role:" + getRoleStr(localNodeStatus.role) + "] [NodeID:" + localNodeStatus.nodeID.toString().c_str() + "] [NodeIndex:" + std::to_string(localNodeStatus.nodeIndex) + "]";
	}

	void resetPerVersionBatch() {
		printf("[INFO]Node:%s resetPerVersionBatch\n", localNodeStatus.nodeID.toString().c_str());
		range2Applier.clear();
		keyOpsCount.clear();
		numSampledMutations = 0;
		kvOps.clear();
		mutationMap.clear();
		mutationPartMap.clear();
		processedCmd.clear();
	}

	vector<UID> getBusyAppliers() {
		vector<UID> busyAppliers;
		for (auto &app : range2Applier) {
			busyAppliers.push_back(app.second);
		}
		return busyAppliers;
	}

	RestoreData() {
		cmdID.initPhase(RestoreCommandEnum::Init);
		localNodeStatus.role = RestoreRole::Invalid;
		localNodeStatus.nodeIndex = 0;
	}

	~RestoreData() {
		printf("[Exit] NodeID:%s RestoreData is deleted\n", localNodeStatus.nodeID.toString().c_str());
	}
};

typedef RestoreData::LoadingStatus LoadingStatus;
typedef RestoreData::LoadingState LoadingState;

// Log error message when the command is unexpected
// Use stdout so that correctness test won't report error.
void logUnexpectedCmd(Reference<RestoreData> rd, RestoreCommandEnum current, RestoreCommandEnum received, CMDUID cmdID) {
	fprintf(stdout, "[WARNING!] Node:%s Log Unexpected Cmd: CurrentCmd:%d(%s), Received cmd:%d(%s), Received CmdUID:%s, Expected cmd:%s\n",
			rd->describeNode().c_str(), current, RestoreCommandEnumStr[(int)current], received, RestoreCommandEnumStr[(int)received], cmdID.toString().c_str(), getPreviousCmdStr(current).c_str());
}

// Log  message when we receive a command from the old phase
void logExpectedOldCmd(Reference<RestoreData> rd, RestoreCommandEnum current, RestoreCommandEnum received, CMDUID cmdID) {
	fprintf(stdout, "[Warning] Node:%s Log Expected Old Cmd: CurrentCmd:%d(%s) Received cmd:%d(%s), Received CmdUID:%s, Expected cmd:%s\n",
			rd->describeNode().c_str(), current, RestoreCommandEnumStr[(int)current], received, RestoreCommandEnumStr[(int)received], cmdID.toString().c_str(), getPreviousCmdStr(current).c_str());
}

void printAppliersKeyRange(Reference<RestoreData> rd) {
	printf("[INFO] The mapping of KeyRange_start --> Applier ID\n");
	// applier type: std::map<Standalone<KeyRef>, UID>
	for (auto &applier : rd->range2Applier) {
		printf("\t[INFO]%s -> %s\n", getHexString(applier.first).c_str(), applier.second.toString().c_str());
	}
}

//Print out the works_interface info
void printWorkersInterface(Reference<RestoreData> rd) {
	printf("[INFO] workers_interface info: num of workers:%d\n", rd->workers_interface.size());
	int index = 0;
	for (auto &interf : rd->workers_interface) {
		printf("\t[INFO][Worker %d] NodeID:%s, Interface.id():%s\n", index,
				interf.first.toString().c_str(), interf.second.id().toString().c_str());
	}
}

// Return <num_of_loader, num_of_applier> in the system
std::pair<int, int> getNumLoaderAndApplier(Reference<RestoreData> rd){
	int numLoaders = 0;
	int numAppliers = 0;
	for (int i = 0; i < rd->globalNodeStatus.size(); ++i) {
		if (rd->globalNodeStatus[i].role == RestoreRole::Loader) {
			numLoaders++;
		} else if (rd->globalNodeStatus[i].role == RestoreRole::Applier) {
			numAppliers++;
		} else {
			printf("[ERROR] unknown role: %d\n", rd->globalNodeStatus[i].role);
		}
	}

	if ( numLoaders + numAppliers != rd->globalNodeStatus.size() ) {
		printf("[ERROR] Number of workers does not add up! numLoaders:%d, numApplier:%d, totalProcess:%d\n",
				numLoaders, numAppliers, rd->globalNodeStatus.size());
	}

	return std::make_pair(numLoaders, numAppliers);
}

std::vector<UID> getApplierIDs(Reference<RestoreData> rd) {
	std::vector<UID> applierIDs;
	for (int i = 0; i < rd->globalNodeStatus.size(); ++i) {
		if (rd->globalNodeStatus[i].role == RestoreRole::Applier) {
			applierIDs.push_back(rd->globalNodeStatus[i].nodeID);
		}
	}

	// Check if there exist duplicate applier IDs, which should never occur
	std::sort(applierIDs.begin(), applierIDs.end());
	bool unique = true;
	for (int i = 1; i < applierIDs.size(); ++i) {
		if (applierIDs[i-1] == applierIDs[i]) {
			unique = false;
			break;
		}
	}
	if (!unique) {
		fprintf(stderr, "[ERROR] Applier IDs are not unique! All worker IDs are as follows\n");
		printGlobalNodeStatus(rd);
	}

	return applierIDs;
}

std::vector<UID> getLoaderIDs(Reference<RestoreData> rd) {
	std::vector<UID> loaderIDs;
	for (int i = 0; i < rd->globalNodeStatus.size(); ++i) {
		if (rd->globalNodeStatus[i].role == RestoreRole::Loader) {
			loaderIDs.push_back(rd->globalNodeStatus[i].nodeID);
		}
	}

	// Check if there exist duplicate applier IDs, which should never occur
	std::sort(loaderIDs.begin(), loaderIDs.end());
	bool unique = true;
	for (int i = 1; i < loaderIDs.size(); ++i) {
		if (loaderIDs[i-1] == loaderIDs[i]) {
			unique = false;
			break;
		}
	}
	if (!unique) {
		printf("[ERROR] Applier IDs are not unique! All worker IDs are as follows\n");
		printGlobalNodeStatus(rd);
	}

	return loaderIDs;
}

void printGlobalNodeStatus(Reference<RestoreData> rd) {
	printf("---Print globalNodeStatus---\n");
	printf("Number of entries:%d\n", rd->globalNodeStatus.size());
	for(int i = 0; i < rd->globalNodeStatus.size(); ++i) {
		printf("[Node:%d] %s, role:%s\n", i, rd->globalNodeStatus[i].toString().c_str(),
				getRoleStr(rd->globalNodeStatus[i].role).c_str());
	}
}

void concatenateBackupMutation(Standalone<StringRef> val_input, Standalone<StringRef> key_input);
void registerBackupMutationForAll(Version empty);
bool isKVOpsSorted(Reference<RestoreData> rd);
bool allOpsAreKnown(Reference<RestoreData> rd);



void printBackupFilesInfo(Reference<RestoreData> rd) {
	printf("[INFO] The backup files for current batch to load and apply: num:%d\n", rd->files.size());
	for (int i = 0; i < rd->files.size(); ++i) {
		printf("\t[INFO][File %d] %s\n", i, rd->files[i].toString().c_str());
	}
}


void printAllBackupFilesInfo(Reference<RestoreData> rd) {
	printf("[INFO] All backup files: num:%d\n", rd->allFiles.size());
	for (int i = 0; i < rd->allFiles.size(); ++i) {
		printf("\t[INFO][File %d] %s\n", i, rd->allFiles[i].toString().c_str());
	}
}

void buildForbiddenVersionRange(Reference<RestoreData> rd) {

	printf("[INFO] Build forbidden version ranges for all backup files: num:%d\n", rd->allFiles.size());
	for (int i = 0; i < rd->allFiles.size(); ++i) {
		if (!rd->allFiles[i].isRange) {
			rd->forbiddenVersions.insert(std::make_pair(rd->allFiles[i].beginVersion, rd->allFiles[i].endVersion));
		}
	}
}

bool isForbiddenVersionRangeOverlapped(Reference<RestoreData> rd) {
	printf("[INFO] Check if forbidden version ranges is overlapped: num of ranges:%d\n", rd->forbiddenVersions.size());
	if (rd->forbiddenVersions.empty()) {
		return false;
	}

	std::map<Version, Version>::iterator prevRange = rd->forbiddenVersions.begin();
	std::map<Version, Version>::iterator curRange = rd->forbiddenVersions.begin();
	curRange++; // Assume rd->forbiddenVersions has at least one element!

	while ( curRange != rd->forbiddenVersions.end() ) {
		if ( curRange->first < prevRange->second ) {
			return true; // overlapped
		}
		curRange++;
	}

	return false; //not overlapped
}

// endVersion:
bool isVersionInForbiddenRange(Reference<RestoreData> rd, Version endVersion, bool isRange) {
//	std::map<Version, Version>::iterator iter = rd->forbiddenVersions.upper_bound(ver); // The iterator that is > ver
//	if ( iter == rd->forbiddenVersions.end() ) {
//		return false;
//	}
	bool isForbidden = false;
	for (auto &range : rd->forbiddenVersions) {
		if ( isRange ) { //the range file includes mutations at the endVersion
			if (endVersion >= range.first && endVersion < range.second) {
				isForbidden = true;
				break;
			}
		} else { // the log file does NOT include mutations at the endVersion
			continue; // Log file's endVersion is always a valid version batch boundary as long as the forbidden version ranges do not overlap
		}
	}

	return isForbidden;
}

void printForbiddenVersionRange(Reference<RestoreData> rd) {
	printf("[INFO] Number of forbidden version ranges:%d\n", rd->forbiddenVersions.size());
	int i = 0;
	for (auto &range : rd->forbiddenVersions) {
		printf("\t[INFO][Range%d] [%ld, %ld)\n", i, range.first, range.second);
		++i;
	}
}

void constructFilesWithVersionRange(Reference<RestoreData> rd) {
	printf("[INFO] constructFilesWithVersionRange for num_files:%d\n", rd->files.size());
	rd->allFiles.clear();
	for (int i = 0; i < rd->files.size(); i++) {
		printf("\t[File:%d] %s\n", i, rd->files[i].toString().c_str());
		Version beginVersion = 0;
		Version endVersion = 0;
		if (rd->files[i].isRange) {
			// No need to parse range filename to get endVersion
			beginVersion = rd->files[i].version;
			endVersion = beginVersion;
		} else { // Log file
			//Refer to pathToLogFile() in BackupContainer.actor.cpp
			long blockSize, len;
			int pos = rd->files[i].fileName.find_last_of("/");
			std::string fileName = rd->files[i].fileName.substr(pos);
			printf("\t[File:%d] Log filename:%s, pos:%d\n", i, fileName.c_str(), pos);
			sscanf(fileName.c_str(), "/log,%lld,%lld,%*[^,],%u%n", &beginVersion, &endVersion, &blockSize, &len);
			printf("\t[File:%d] Log filename:%s produces beginVersion:%lld endVersion:%lld\n",i, fileName.c_str(), beginVersion, endVersion);
		}
		ASSERT(beginVersion <= endVersion);
		rd->allFiles.push_back(rd->files[i]);
		rd->allFiles.back().beginVersion = beginVersion;
		rd->allFiles.back().endVersion = endVersion;
	}
}


//// --- Some common functions

ACTOR static Future<Void> prepareRestoreFilesV2(Reference<RestoreData> rd, Database cx, Reference<ReadYourWritesTransaction> tr, Key tagName, Key backupURL,
		Version restoreVersion, Key addPrefix, Key removePrefix, KeyRange restoreRange, bool lockDB, UID uid,
		Reference<RestoreConfig> restore_input) {
 	ASSERT(restoreRange.contains(removePrefix) || removePrefix.size() == 0);

 	printf("[INFO] prepareRestore: the current db lock status is as below\n");
	wait(checkDatabaseLock(tr, uid));

 	tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
 	tr->setOption(FDBTransactionOptions::LOCK_AWARE);

 	printf("[INFO] Prepare restore for the tag:%s\n", tagName.toString().c_str());
 	// Get old restore config for this tag
 	state KeyBackedTag tag = makeRestoreTag(tagName.toString());
 	state Optional<UidAndAbortedFlagT> oldUidAndAborted = wait(tag.get(tr));
 	TraceEvent("PrepareRestoreMX").detail("OldUidAndAbortedPresent", oldUidAndAborted.present());
 	if(oldUidAndAborted.present()) {
 		if (oldUidAndAborted.get().first == uid) {
 			if (oldUidAndAborted.get().second) {
 				throw restore_duplicate_uid();
 			}
 			else {
 				return Void();
 			}
 		}

 		state Reference<RestoreConfig> oldRestore = Reference<RestoreConfig>(new RestoreConfig(oldUidAndAborted.get().first));

 		// Make sure old restore for this tag is not runnable
 		bool runnable = wait(oldRestore->isRunnable(tr));

 		if (runnable) {
 			throw restore_duplicate_tag();
 		}

 		// Clear the old restore config
 		oldRestore->clear(tr);
 	}

 	KeyRange restoreIntoRange = KeyRangeRef(restoreRange.begin, restoreRange.end).removePrefix(removePrefix).withPrefix(addPrefix);
 	Standalone<RangeResultRef> existingRows = wait(tr->getRange(restoreIntoRange, 1));
 	if (existingRows.size() > 0) {
 		throw restore_destination_not_empty();
 	}

 	// Make new restore config
 	state Reference<RestoreConfig> restore = Reference<RestoreConfig>(new RestoreConfig(uid));

 	// Point the tag to the new uid
	printf("[INFO] Point the tag:%s to the new uid:%s\n", tagName.toString().c_str(), uid.toString().c_str());
 	tag.set(tr, {uid, false});

 	printf("[INFO] Open container for backup url:%s\n", backupURL.toString().c_str());
 	Reference<IBackupContainer> bc = IBackupContainer::openContainer(backupURL.toString());

 	// Configure the new restore
 	restore->tag().set(tr, tagName.toString());
 	restore->sourceContainer().set(tr, bc);
 	restore->stateEnum().set(tr, ERestoreState::QUEUED);
 	restore->restoreVersion().set(tr, restoreVersion);
 	restore->restoreRange().set(tr, restoreRange);
 	// this also sets restore.add/removePrefix.
 	restore->initApplyMutations(tr, addPrefix, removePrefix);
	printf("[INFO] Configure new restore config to :%s\n", restore->toString().c_str());
	restore_input = restore;
	printf("[INFO] Assign the global restoreConfig to :%s\n", restore_input->toString().c_str());


	Optional<RestorableFileSet> restorable = wait(bc->getRestoreSet(restoreVersion));
	if(!restorable.present()) {
		printf("[WARNING] restoreVersion:%ld (%lx) is not restorable!\n", restoreVersion, restoreVersion);
		throw restore_missing_data();
	}

//	state std::vector<RestoreFile> files;
	if (!rd->files.empty()) {
		printf("[WARNING] global files are not empty! files.size()=%d. We forcely clear files\n", rd->files.size());
		rd->files.clear();
	}

	printf("[INFO] Found backup files: num of range files:%d, num of log files:%d\n",
			restorable.get().ranges.size(), restorable.get().logs.size());
 	for(const RangeFile &f : restorable.get().ranges) {
// 		TraceEvent("FoundRangeFileMX").detail("FileInfo", f.toString());
 		printf("[INFO] FoundRangeFile, fileInfo:%s\n", f.toString().c_str());
		RestoreFile file = {f.version, f.fileName, true, f.blockSize, f.fileSize};
 		rd->files.push_back(file);
 	}
 	for(const LogFile &f : restorable.get().logs) {
// 		TraceEvent("FoundLogFileMX").detail("FileInfo", f.toString());
		printf("[INFO] FoundLogFile, fileInfo:%s\n", f.toString().c_str());
		RestoreFile file = {f.beginVersion, f.fileName, false, f.blockSize, f.fileSize, f.endVersion};
		rd->files.push_back(file);
 	}

	return Void();

 }

 // MX: To revise the parser later
 ACTOR static Future<Void> _parseRangeFileToMutationsOnLoader(Reference<RestoreData> rd,
 									Reference<IBackupContainer> bc, Version version,
 									std::string fileName, int64_t readOffset_input, int64_t readLen_input,
 									KeyRange restoreRange, Key addPrefix, Key removePrefix) {
//	state Reference<ReadYourWritesTransaction> tr(new ReadYourWritesTransaction(cx)); // Used to clear the range where the KV will be applied.

 	state int64_t readOffset = readOffset_input;
 	state int64_t readLen = readLen_input;

 	//MX: the set of key value version is rangeFile.version. the key-value set in the same range file has the same version
 	state Reference<IAsyncFile> inFile = wait(bc->readFile(fileName));

 	state Standalone<VectorRef<KeyValueRef>> blockData = wait(parallelFileRestore::decodeRangeFileBlock(inFile, readOffset, readLen));

	printf("[VERBOSE_DEBUG] Parse range file and get mutations\n");
	int tmpi = 0;
	for (tmpi = 0; tmpi < blockData.size(); tmpi++) {
		printf("\t[VERBOSE_DEBUG] mutation: key:%s value:%s\n", blockData[tmpi].key.toString().c_str(), blockData[tmpi].value.toString().c_str());
	}
	 

 	// First and last key are the range for this file
 	state KeyRange fileRange = KeyRangeRef(blockData.front().key, blockData.back().key);
 	printf("[INFO] RangeFile:%s KeyRange:%s, restoreRange:%s\n",
 			fileName.c_str(), fileRange.toString().c_str(), restoreRange.toString().c_str());

 	// If fileRange doesn't intersect restore range then we're done.
 	if(!fileRange.intersects(restoreRange)) {
 		TraceEvent("ExtractApplyRangeFileToDB_MX").detail("NoIntersectRestoreRange", "FinishAndReturn");
 		return Void();
 	}

 	// We know the file range intersects the restore range but there could still be keys outside the restore range.
 	// Find the subvector of kv pairs that intersect the restore range.  Note that the first and last keys are just the range endpoints for this file
	 // The blockData's first and last entries are metadata, not the real data
 	int rangeStart = 1; //1
 	int rangeEnd = blockData.size() -1; //blockData.size() - 1 // Q: the rangeStart and rangeEnd is [,)?
	printf("[VERBOSE_DEBUG] Range file decoded blockData\n");
	for (auto& data : blockData ) {
		printf("\t[VERBOSE_DEBUG] data key:%s val:%s\n", data.key.toString().c_str(), data.value.toString().c_str());
	}

 	// Slide start forward, stop if something in range is found
	// Move rangeStart and rangeEnd until they is within restoreRange
 	while(rangeStart < rangeEnd && !restoreRange.contains(blockData[rangeStart].key)) {
		printf("[VERBOSE_DEBUG] rangeStart:%d key:%s is not in the range:%s\n", rangeStart, blockData[rangeStart].key.toString().c_str(), restoreRange.toString().c_str());
		++rangeStart;
	 }
 	// Side end backward, stop if something in range is found
 	while(rangeEnd > rangeStart && !restoreRange.contains(blockData[rangeEnd - 1].key)) {
		printf("[VERBOSE_DEBUG] (rangeEnd:%d - 1) key:%s is not in the range:%s\n", rangeEnd, blockData[rangeStart].key.toString().c_str(), restoreRange.toString().c_str());
		--rangeEnd;
	 }

 	// MX: now data only contains the kv mutation within restoreRange
 	state VectorRef<KeyValueRef> data = blockData.slice(rangeStart, rangeEnd);
 	printf("[INFO] RangeFile:%s blockData entry size:%d recovered data size:%d\n", fileName.c_str(), blockData.size(), data.size());

 	// Shrink file range to be entirely within restoreRange and translate it to the new prefix
 	// First, use the untranslated file range to create the shrunk original file range which must be used in the kv range version map for applying mutations
 	state KeyRange originalFileRange = KeyRangeRef(std::max(fileRange.begin, restoreRange.begin), std::min(fileRange.end,   restoreRange.end));

 	// Now shrink and translate fileRange
 	Key fileEnd = std::min(fileRange.end,   restoreRange.end);
 	if(fileEnd == (removePrefix == StringRef() ? normalKeys.end : strinc(removePrefix)) ) {
 		fileEnd = addPrefix == StringRef() ? normalKeys.end : strinc(addPrefix);
 	} else {
 		fileEnd = fileEnd.removePrefix(removePrefix).withPrefix(addPrefix);
 	}
 	fileRange = KeyRangeRef(std::max(fileRange.begin, restoreRange.begin).removePrefix(removePrefix).withPrefix(addPrefix),fileEnd);

 	state int start = 0;
 	state int end = data.size();
 	state int dataSizeLimit = BUGGIFY ? g_random->randomInt(256 * 1024, 10e6) : CLIENT_KNOBS->RESTORE_WRITE_TX_SIZE;
 	state int kvCount = 0;

 	//MX: This is where the key-value pair in range file is applied into DB
	loop {

		state int i = start;
		state int txBytes = 0;
		state int iend = start;

		// find iend that results in the desired transaction size
		for(; iend < end && txBytes < dataSizeLimit; ++iend) {
			txBytes += data[iend].key.expectedSize();
			txBytes += data[iend].value.expectedSize();
		}


		for(; i < iend; ++i) {
			//MXX: print out the key value version, and operations.
				printf("RangeFile [key:%s, value:%s, version:%ld, op:set]\n", data[i].key.printable().c_str(), data[i].value.printable().c_str(), version);
// 				TraceEvent("PrintRangeFile_MX").detail("Key", data[i].key.printable()).detail("Value", data[i].value.printable())
// 					.detail("Version", rangeFile.version).detail("Op", "set");
////				printf("PrintRangeFile_MX: mType:set param1:%s param2:%s param1_size:%d, param2_size:%d\n",
////						getHexString(data[i].key.c_str(), getHexString(data[i].value).c_str(), data[i].key.size(), data[i].value.size());

			//NOTE: Should NOT removePrefix and addPrefix for the backup data!
			// In other words, the following operation is wrong:  data[i].key.removePrefix(removePrefix).withPrefix(addPrefix)
			MutationRef m(MutationRef::Type::SetValue, data[i].key, data[i].value); //ASSUME: all operation in range file is set.
			++kvCount;

			// TODO: we can commit the kv operation into DB.
			// Right now, we cache all kv operations into kvOps, and apply all kv operations later in one place
			if ( rd->kvOps.find(version) == rd->kvOps.end() ) { // Create the map's key if mutation m is the first on to be inserted
				//kvOps.insert(std::make_pair(rangeFile.version, Standalone<VectorRef<MutationRef>>(VectorRef<MutationRef>())));
				rd->kvOps.insert(std::make_pair(version, VectorRef<MutationRef>()));
			}

			ASSERT(rd->kvOps.find(version) != rd->kvOps.end());
			rd->kvOps[version].push_back_deep(rd->kvOps[version].arena(), m);

		}

		// Commit succeeded, so advance starting point
		start = i;

		if(start == end) {
			//TraceEvent("ExtraApplyRangeFileToDB_MX").detail("Progress", "DoneApplyKVToDB");
			printf("[INFO][Loader] NodeID:%s Parse RangeFile:%s: the number of kv operations = %d\n",
					 rd->describeNode().c_str(), fileName.c_str(), kvCount);
			return Void();
		}
 	}

 }


 ACTOR static Future<Void> _parseLogFileToMutationsOnLoader(Reference<RestoreData> rd,
 									Reference<IBackupContainer> bc, Version version,
 									std::string fileName, int64_t readOffset, int64_t readLen,
 									KeyRange restoreRange, Key addPrefix, Key removePrefix,
 									Key mutationLogPrefix) {

	// Step: concatenate the backuped param1 and param2 (KV) at the same version.
 	//state Key mutationLogPrefix = mutationLogPrefix;
 	//TraceEvent("ReadLogFileStart").detail("LogFileName", fileName);
 	state Reference<IAsyncFile> inFile = wait(bc->readFile(fileName));
 	//TraceEvent("ReadLogFileFinish").detail("LogFileName", fileName);


 	printf("Parse log file:%s readOffset:%d readLen:%d\n", fileName.c_str(), readOffset, readLen);
 	//TODO: NOTE: decodeLogFileBlock() should read block by block! based on my serial version. This applies to decode range file as well
 	state Standalone<VectorRef<KeyValueRef>> data = wait(parallelFileRestore::decodeLogFileBlock(inFile, readOffset, readLen));
 	//state Standalone<VectorRef<MutationRef>> data = wait(fileBackup::decodeLogFileBlock_MX(inFile, readOffset, readLen)); //Decode log file
 	TraceEvent("ReadLogFileFinish").detail("LogFileName", fileName).detail("DecodedDataSize", data.contents().size());
 	printf("ReadLogFile, raw data size:%d\n", data.size());

 	state int start = 0;
 	state int end = data.size();
 	state int dataSizeLimit = BUGGIFY ? g_random->randomInt(256 * 1024, 10e6) : CLIENT_KNOBS->RESTORE_WRITE_TX_SIZE;
	state int kvCount = 0;
	state int numConcatenated = 0;
	loop {
 		try {
// 			printf("Process start:%d where end=%d\n", start, end);
 			if(start == end) {
 				printf("ReadLogFile: finish reading the raw data and concatenating the mutation at the same version\n");
 				break;
 			}

 			state int i = start;
 			state int txBytes = 0;
 			for(; i < end && txBytes < dataSizeLimit; ++i) {
 				Key k = data[i].key.withPrefix(mutationLogPrefix);
 				ValueRef v = data[i].value;
 				txBytes += k.expectedSize();
 				txBytes += v.expectedSize();
 				//MXX: print out the key value version, and operations.
 				//printf("LogFile [key:%s, value:%s, version:%ld, op:NoOp]\n", k.printable().c_str(), v.printable().c_str(), logFile.version);
 //				printf("LogFile [KEY:%s, VALUE:%s, VERSION:%ld, op:NoOp]\n", getHexString(k).c_str(), getHexString(v).c_str(), logFile.version);
 //				printBackupMutationRefValueHex(v, " |\t");
 /*
 				printf("||Register backup mutation:file:%s, data:%d\n", logFile.fileName.c_str(), i);
 				registerBackupMutation(data[i].value, logFile.version);
 */
 //				printf("[DEBUG]||Concatenate backup mutation:fileInfo:%s, data:%d\n", logFile.toString().c_str(), i);
 				bool concatenated = concatenateBackupMutationForLogFile(rd, data[i].value, data[i].key);
 				numConcatenated += ( concatenated ? 1 : 0);
 //				//TODO: Decode the value to get the mutation type. Use NoOp to distinguish from range kv for now.
 //				MutationRef m(MutationRef::Type::NoOp, data[i].key, data[i].value); //ASSUME: all operation in log file is NoOp.
 //				if ( rd->kvOps.find(logFile.version) == rd->kvOps.end() ) {
 //					rd->kvOps.insert(std::make_pair(logFile.version, std::vector<MutationRef>()));
 //				} else {
 //					rd->kvOps[logFile.version].push_back(m);
 //				}
 			}

 			start = i;

 		} catch(Error &e) {
 			if(e.code() == error_code_transaction_too_large)
 				dataSizeLimit /= 2;
 		}
 	}

 	printf("[INFO] raw kv number:%d parsed from log file, concatenated:%d kv, num_log_versions:%d\n", data.size(), numConcatenated, rd->mutationMap.size());

	return Void();
 }


 // Parse the kv pair (version, serialized_mutation), which are the results parsed from log file.
 void parseSerializedMutation(Reference<RestoreData> rd, bool isSampling) {
	// Step: Parse the concatenated KV pairs into (version, <K, V, mutationType>) pair
 	printf("[INFO] Parse the concatenated log data\n");
 	std::string prefix = "||\t";
	std::stringstream ss;
	const int version_size = 12;
	const int header_size = 12;
	int kvCount = 0;

	for ( auto& m : rd->mutationMap ) {
		StringRef k = m.first.contents();
		StringRefReaderMX readerVersion(k, restore_corrupted_data());
		uint64_t commitVersion = readerVersion.consume<uint64_t>(); // Consume little Endian data


		StringRef val = m.second.contents();
		StringRefReaderMX reader(val, restore_corrupted_data());

		int count_size = 0;
		// Get the include version in the batch commit, which is not the commitVersion.
		// commitVersion is in the key
		uint64_t includeVersion = reader.consume<uint64_t>();
		count_size += 8;
		uint32_t val_length_decode = reader.consume<uint32_t>(); //Parse little endian value, confirmed it is correct!
		count_size += 4;

		if ( rd->kvOps.find(commitVersion) == rd->kvOps.end() ) {
			rd->kvOps.insert(std::make_pair(commitVersion, VectorRef<MutationRef>()));
		}

		if ( debug_verbose ) {
			printf("----------------------------------------------------------Register Backup Mutation into KVOPs version:%08lx\n", commitVersion);
			printf("To decode value:%s\n", getHexString(val).c_str());
		}
		// In sampling, the last mutation vector may be not complete, we do not concatenate for performance benefit
		if ( val_length_decode != (val.size() - 12) ) {
			//IF we see val.size() == 10000, It means val should be concatenated! The concatenation may fail to copy the data
			if (isSampling) {
				printf("[PARSE WARNING]!!! val_length_decode:%d != val.size:%d version:%ld(0x%lx)\n",  val_length_decode, val.size(),
					commitVersion, commitVersion);
				printf("[PARSE WARNING] Skipped the mutation! OK for sampling workload but WRONG for restoring the workload\n");
				continue;
			} else {
				printf("[PARSE ERROR]!!! val_length_decode:%d != val.size:%d version:%ld(0x%lx)\n",  val_length_decode, val.size(),
					commitVersion, commitVersion);
			}
		} else {
			if ( debug_verbose ) {
				printf("[PARSE SUCCESS] val_length_decode:%d == (val.size:%d - 12)\n", val_length_decode, val.size());
			}
		}

		// Get the mutation header
		while (1) {
			// stop when reach the end of the string
			if(reader.eof() ) { //|| *reader.rptr == 0xFF
				//printf("Finish decode the value\n");
				break;
			}


			uint32_t type = reader.consume<uint32_t>();//reader.consumeNetworkUInt32();
			uint32_t kLen = reader.consume<uint32_t>();//reader.consumeNetworkUInkvOps[t32();
			uint32_t vLen = reader.consume<uint32_t>();//reader.consumeNetworkUInt32();
			const uint8_t *k = reader.consume(kLen);
			const uint8_t *v = reader.consume(vLen);
			count_size += 4 * 3 + kLen + vLen;

			MutationRef mutation((MutationRef::Type) type, KeyRef(k, kLen), KeyRef(v, vLen));
			rd->kvOps[commitVersion].push_back_deep(rd->kvOps[commitVersion].arena(), mutation);
			kvCount++;

			if ( kLen < 0 || kLen > val.size() || vLen < 0 || vLen > val.size() ) {
				printf("%s[PARSE ERROR]!!!! kLen:%d(0x%04x) vLen:%d(0x%04x)\n", prefix.c_str(), kLen, kLen, vLen, vLen);
			}

			//if ( debug_verbose ) {
			if ( true ) {
				printf("%s---LogFile parsed mutations. Prefix:[%d]: Version:%016lx Type:%d K:%s V:%s k_size:%d v_size:%d\n", prefix.c_str(),
					   kvCount,
					   commitVersion, type,  getHexString(KeyRef(k, kLen)).c_str(), getHexString(KeyRef(v, vLen)).c_str(), kLen, vLen);
			}

		}
		//	printf("----------------------------------------------------------\n");
	}

	printf("[INFO] Produces %d mutation operations from concatenated kv pairs that are parsed from log\n",  kvCount);

}

// TODO: The operation may be applied more than once due to network duplicate delivery!
 ACTOR Future<Void> applyKVOpsToDB(Reference<RestoreData> rd, Database cx) {
 	state bool isPrint = false; //Debug message
 	state std::string typeStr = "";

 	if ( debug_verbose ) {
		TraceEvent("ApplyKVOPsToDB").detail("MapSize", rd->kvOps.size());
		printf("ApplyKVOPsToDB num_of_version:%d\n", rd->kvOps.size());
 	}
 	state std::map<Version, Standalone<VectorRef<MutationRef>>>::iterator it = rd->kvOps.begin();
 	state int count = 0;
 	for ( ; it != rd->kvOps.end(); ++it ) {

 		if ( debug_verbose ) {
			TraceEvent("ApplyKVOPsToDB\t").detail("Version", it->first).detail("OpNum", it->second.size());
 		}
		//printf("ApplyKVOPsToDB Version:%08lx num_of_ops:%d\n", it->first, it->second.size());


 		state MutationRef m;
 		state int index = 0;
 		for ( ; index < it->second.size(); ++index ) {
 			m = it->second[index];
 			if (  m.type >= MutationRef::Type::SetValue && m.type <= MutationRef::Type::MAX_ATOMIC_OP )
 				typeStr = typeString[m.type];
 			else {
 				printf("ApplyKVOPsToDB MutationType:%d is out of range\n", m.type);
 			}

 			if ( count % 1000 == 1 ) {
 				printf("ApplyKVOPsToDB Node:%s num_mutation:%d Version:%08lx num_of_ops:%d\n",
 						rd->describeNode().c_str(), count, it->first, it->second.size());
 			}

 			// Mutation types SetValue=0, ClearRange, AddValue, DebugKeyRange, DebugKey, NoOp, And, Or,
			//		Xor, AppendIfFits, AvailableForReuse, Reserved_For_LogProtocolMessage /* See fdbserver/LogProtocolMessage.h */, Max, Min, SetVersionstampedKey, SetVersionstampedValue,
			//		ByteMin, ByteMax, MinV2, AndV2, MAX_ATOMIC_OP

			printf("[VERBOSE_DEBUG] Node:%s apply mutation:%s\n", rd->describeNode().c_str(), m.toString().c_str());
 			loop {
 				try {
					state Reference<ReadYourWritesTransaction> tr(new ReadYourWritesTransaction(cx));
 					tr->reset();
 					tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
 					tr->setOption(FDBTransactionOptions::LOCK_AWARE);

 					if ( m.type == MutationRef::SetValue ) {
 						tr->set(m.param1, m.param2);
 					} else if ( m.type == MutationRef::ClearRange ) {
 						KeyRangeRef mutationRange(m.param1, m.param2);
 						tr->clear(mutationRange);
 					} else if ( isAtomicOp((MutationRef::Type) m.type) ) {
 						//// Now handle atomic operation from this if statement
 						// TODO: Have not de-duplicated the mutations for multiple network delivery
 						// ATOMIC_MASK = (1 << AddValue) | (1 << And) | (1 << Or) | (1 << Xor) | (1 << AppendIfFits) | (1 << Max) | (1 << Min) | (1 << SetVersionstampedKey) | (1 << SetVersionstampedValue) | (1 << ByteMin) | (1 << ByteMax) | (1 << MinV2) | (1 << AndV2),
 						//atomicOp( const KeyRef& key, const ValueRef& operand, uint32_t operationType )
 						tr->atomicOp(m.param1, m.param2, m.type);
 					} else {
 						printf("[WARNING] mtype:%d (%s) unhandled\n", m.type, typeStr.c_str());
 					}

 					wait(tr->commit());
					++count;
 					break;
 				} catch(Error &e) {
 					printf("ApplyKVOPsToDB transaction error:%s. Type:%d, Param1:%s, Param2:%s\n", e.what(),
 							m.type, getHexString(m.param1).c_str(), getHexString(m.param2).c_str());
 					wait(tr->onError(e));
 				}
 			}

 			if ( isPrint ) {
 				printf("\tApplyKVOPsToDB Version:%016lx MType:%s K:%s, V:%s K_size:%d V_size:%d\n", it->first, typeStr.c_str(),
 					   getHexString(m.param1).c_str(), getHexString(m.param2).c_str(), m.param1.size(), m.param2.size());

 				TraceEvent("ApplyKVOPsToDB\t\t").detail("Version", it->first)
 						.detail("MType", m.type).detail("MTypeStr", typeStr)
 						.detail("MKey", getHexString(m.param1))
 						.detail("MValueSize", m.param2.size())
 						.detail("MValue", getHexString(m.param2));
 			}
 		}
 	}

 	rd->kvOps.clear();
 	printf("Node:%s ApplyKVOPsToDB number of kv mutations:%d\n", rd->describeNode().c_str(), count);

 	return Void();
}

ACTOR Future<Void> setWorkerInterface(Reference<RestoreData> rd, Database cx) {
 	state Transaction tr(cx);

	state vector<RestoreCommandInterface> agents; // agents is cmdsInterf
	printf("[INFO][Worker] Node:%s Get the interface for all workers\n", rd->describeNode().c_str());
	loop {
		try {
			tr.reset();
			tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			tr.setOption(FDBTransactionOptions::LOCK_AWARE);
			Standalone<RangeResultRef> agentValues = wait(tr.getRange(restoreWorkersKeys, CLIENT_KNOBS->TOO_MANY));
			ASSERT(!agentValues.more);
			if(agentValues.size()) {
				for(auto& it : agentValues) {
					agents.push_back(BinaryReader::fromStringRef<RestoreCommandInterface>(it.value, IncludeVersion()));
					// Save the RestoreCommandInterface for the later operations
					rd->workers_interface.insert(std::make_pair(agents.back().id(), agents.back()));
				}
				break;
			}
			wait( delay(5.0) );
		} catch( Error &e ) {
			printf("[WARNING] Node:%s setWorkerInterface() transaction error:%s\n", rd->describeNode().c_str(), e.what());
			wait( tr.onError(e) );
		}
		printf("[WARNING] Node:%s setWorkerInterface should always succeed in the first loop! Something goes wrong!\n", rd->describeNode().c_str());
	};

	return Void();
 }


////--- Restore Functions for the master role
//// --- Configure roles ---
// Set roles (Loader or Applier) for workers
// The master node's localNodeStatus has been set outside of this function
ACTOR Future<Void> configureRoles(Reference<RestoreData> rd, Database cx)  { //, VectorRef<RestoreInterface> ret_agents
	state Transaction tr(cx);

	state vector<RestoreCommandInterface> agents; // agents is cmdsInterf
	printf("%s:Start configuring roles for workers\n", rd->describeNode().c_str());
	loop {
		try {
			tr.reset();
			tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			tr.setOption(FDBTransactionOptions::LOCK_AWARE);
			Standalone<RangeResultRef> agentValues = wait(tr.getRange(restoreWorkersKeys, CLIENT_KNOBS->TOO_MANY));
			ASSERT(!agentValues.more);
			// If agentValues.size() < min_num_workers, we should wait for coming workers to register their interface before we read them once for all
			if(agentValues.size() >= min_num_workers) {
				for(auto& it : agentValues) {
					agents.push_back(BinaryReader::fromStringRef<RestoreCommandInterface>(it.value, IncludeVersion()));
					// Save the RestoreCommandInterface for the later operations
					rd->workers_interface.insert(std::make_pair(agents.back().id(), agents.back()));
				}
				break;
			}
			printf("%s:Wait for enough workers. Current num_workers:%d target num_workers:%d\n",
					rd->describeNode().c_str(), agentValues.size(), min_num_workers);
			wait( delay(5.0) );
		} catch( Error &e ) {
			printf("[WARNING]%s: configureRoles transaction error:%s\n", rd->describeNode().c_str(), e.what());
			wait( tr.onError(e) );
		}
	}
	ASSERT(agents.size() >= min_num_workers); // ASSUMPTION: We must have at least 1 loader and 1 applier
	// Set up the role, and the global status for each node
	int numNodes = agents.size();
	int numLoader = numNodes * ratio_loader_to_applier / (ratio_loader_to_applier + 1);
	int numApplier = numNodes - numLoader;
	if (numLoader <= 0 || numApplier <= 0) {
		ASSERT( numLoader > 0 ); // Quick check in correctness
		ASSERT( numApplier > 0 );
		fprintf(stderr, "[ERROR] not enough nodes for loader and applier. numLoader:%d, numApplier:%d, ratio_loader_to_applier:%d, numAgents:%d\n", numLoader, numApplier, ratio_loader_to_applier, numNodes);
	} else {
		printf("Node%s: Configure roles numWorkders:%d numLoader:%d numApplier:%d\n", rd->describeNode().c_str(), numNodes, numLoader, numApplier);
	}

	rd->localNodeStatus.nodeIndex = 0; // Master has nodeIndex = 0

	// The first numLoader nodes will be loader, and the rest nodes will be applier
	int nodeIndex = 1;
	for (int i = 0; i < numLoader; ++i) {
		rd->globalNodeStatus.push_back(RestoreNodeStatus());
		rd->globalNodeStatus.back().init(RestoreRole::Loader);
		rd->globalNodeStatus.back().nodeID = agents[i].id();
		rd->globalNodeStatus.back().nodeIndex = nodeIndex;
		nodeIndex++;
	}

	for (int i = numLoader; i < numNodes; ++i) {
		rd->globalNodeStatus.push_back(RestoreNodeStatus());
		rd->globalNodeStatus.back().init(RestoreRole::Applier);
		rd->globalNodeStatus.back().nodeID = agents[i].id();
		rd->globalNodeStatus.back().nodeIndex = nodeIndex;
		nodeIndex++;
	}

	// Set the last Applier as the master applier
	rd->masterApplier = rd->globalNodeStatus.back().nodeID;
	printf("masterApplier ID:%s\n", rd->masterApplier.toString().c_str());

	state int index = 0;
	state RestoreRole role;
	state UID nodeID;
	printf("Node:%s Start configuring roles for workers\n", rd->describeNode().c_str());
	rd->cmdID.initPhase(RestoreCommandEnum::Set_Role);

	loop {
		try {
			wait(delay(1.0));
			std::vector<Future<RestoreCommandReply>> cmdReplies;
			for(auto& cmdInterf : agents) {
				role = rd->globalNodeStatus[index].role;
				nodeID = rd->globalNodeStatus[index].nodeID;
				rd->cmdID.nextCmd();
				printf("[CMD:%s] Node:%s Set role (%s) to node (index=%d uid=%s)\n", rd->cmdID.toString().c_str(), rd->describeNode().c_str(),
						getRoleStr(role).c_str(), index, nodeID.toString().c_str());
				cmdReplies.push_back( cmdInterf.cmd.getReply(RestoreCommand(RestoreCommandEnum::Set_Role, rd->cmdID, nodeID, role, index, rd->masterApplier)));
				index++;
			}
			std::vector<RestoreCommandReply> reps = wait( timeoutError(getAll(cmdReplies), FastRestore_Failure_Timeout) );
			for (int i = 0; i < reps.size(); ++i) {
				printf("[INFO] Node:%s, CMDReply for CMD:%s, node:%s\n", rd->describeNode().c_str(), reps[i].cmdID.toString().c_str(),
						reps[i].id.toString().c_str());
			}

			break;
		} catch (Error &e) {
			// TODO: Handle the command reply timeout error
			if (e.code() != error_code_io_timeout) {
				fprintf(stderr, "[ERROR] Node:%s, Commands before cmdID:%s timeout\n", rd->describeNode().c_str(), rd->cmdID.toString().c_str());
			} else {
				fprintf(stderr, "[ERROR] Node:%s, Commands before cmdID:%s error. error code:%d, error message:%s\n", rd->describeNode().c_str(),
						rd->cmdID.toString().c_str(), e.code(), e.what());
			}

			printf("Node:%s waits on replies time out. Current phase: Set_Role, Retry all commands.\n", rd->describeNode().c_str());
		}
	}

	// Notify node that all nodes' roles have been set
	printf("[INFO][Master] Notify all workers their roles have been set\n");
	rd->cmdID.initPhase(RestoreCommandEnum::Set_Role_Done);
	ASSERT( rd->cmdID.getPhase() == RestoreCommandEnum::Set_Role_Done );
	ASSERT( rd->cmdID.getIndex() == 0 );
	
	loop {
		try {
			wait(delay(1.0));
			index = 0;

			std::vector<Future<RestoreCommandReply>> cmdReplies;
			printf("Number of agents:%d\n", agents.size());
			for(auto& cmdInterf : agents) {
				role = rd->globalNodeStatus[index].role;
				nodeID = rd->globalNodeStatus[index].nodeID;
				rd->cmdID.nextCmd();
				printf("Node:%s, Notify the finish of set role %s(%d) to node (index=%d uid=%s), CMDID:%s\n", rd->describeNode().c_str(),
						getRoleStr(role).c_str(), role, index, nodeID.toString().c_str(), rd->cmdID.toString().c_str());
				cmdReplies.push_back( cmdInterf.cmd.getReply(RestoreCommand(RestoreCommandEnum::Set_Role_Done, rd->cmdID, nodeID, role)));
				index++;
			}
			std::vector<RestoreCommandReply> reps = wait( timeoutError( getAll(cmdReplies), FastRestore_Failure_Timeout ) );
			printf("Node:%s Got all replies for Set_Role_Done\n", rd->describeNode().c_str());
			
			// TODO: Write to DB the worker's roles

			break;

		} catch (Error &e) {
			// TODO: Handle the command reply timeout error
			if (e.code() != error_code_io_timeout) {
				fprintf(stderr, "[ERROR] Commands before cmdID:%s timeout\n", rd->cmdID.toString().c_str());
			} else {
				fprintf(stderr, "[ERROR] Commands before cmdID:%s error. error code:%d, error message:%s\n",
						rd->cmdID.toString().c_str(), e.code(), e.what());
			}

			printf("Node:%s waits on replies time out. Current phase: Set_Role_Done, Retry all commands.\n", rd->describeNode().c_str());
		}
	}

	// Sanity check roles configuration
	std::pair<int, int> numWorkers = getNumLoaderAndApplier(rd);
	int numLoaders = numWorkers.first;
	int numAppliers = numWorkers.second;
	ASSERT( rd->globalNodeStatus.size() > 0 );
	ASSERT( numLoaders > 0 );
	ASSERT( numAppliers > 0 );

	printf("Node:%s finish configure roles\n", rd->describeNode().c_str());
	return Void();
}

// MX: Function is refactored
// Handle restore command request on workers
ACTOR Future<Void> configureRolesHandler(Reference<RestoreData> rd, RestoreCommandInterface interf) {
	printf("[Worker] Node::%s yet, starts configureRolesHandler\n", rd->describeNode().c_str());
	loop {
		choose {
			when(RestoreCommand req = waitNext(interf.cmd.getFuture())) {
				printf("[Worker][Node:%s] Got Restore Command: CMDId:%s\n",
						rd->describeNode().c_str(), req.cmdID.toString().c_str());
				ASSERT( interf.id() == req.id );

				if ( req.cmd == RestoreCommandEnum::Set_Role ) {
					ASSERT(req.cmd == (RestoreCommandEnum) req.cmdID.phase);
					rd->localNodeStatus.init(req.role);
					rd->localNodeStatus.nodeID = interf.id();
					rd->localNodeStatus.nodeIndex = req.nodeIndex;
					rd->masterApplier = req.masterApplier;
					printf("[INFO][Worker] Node:%s get role %s\n", rd->describeNode().c_str(),
							getRoleStr(rd->localNodeStatus.role).c_str());
					req.reply.send(RestoreCommandReply(interf.id(), req.cmdID));
				} else if (req.cmd == RestoreCommandEnum::Set_Role_Done) {
					printf("[INFO][Worker] Node:%s Set_Role_Done.\n",
							rd->describeNode().c_str());
					ASSERT(req.cmd == (RestoreCommandEnum) req.cmdID.phase);
					req.reply.send(RestoreCommandReply(interf.id(), req.cmdID)); // master node is waiting
					break;
				} else {
					if ( IsCmdInPreviousPhase(RestoreCommandEnum::Set_Role_Done, req.cmd) ) {
						logExpectedOldCmd(rd, RestoreCommandEnum::Set_Role_Done, req.cmd, req.cmdID);
						req.reply.send(RestoreCommandReply(interf.id(), req.cmdID));
					} else {
						logUnexpectedCmd(rd, RestoreCommandEnum::Set_Role_Done, req.cmd, req.cmdID);
					}
				}
			}
		}
	}

	// This actor never returns. You may cancel it in master
	return Void();
}



void printApplierKeyRangeInfo(std::map<UID, Standalone<KeyRangeRef>>  appliers) {
	printf("[INFO] appliers num:%d\n", appliers.size());
	int index = 0;
	for(auto &applier : appliers) {
		printf("\t[INFO][Applier:%d] ID:%s --> KeyRange:%s\n", index, applier.first.toString().c_str(), applier.second.toString().c_str());
	}
}

// MXNOTE: Refactor Done
ACTOR Future<Void> assignKeyRangeToAppliers(Reference<RestoreData> rd, Database cx)  { //, VectorRef<RestoreInterface> ret_agents
	//construct the key range for each applier
	std::vector<KeyRef> lowerBounds;
	std::vector<Standalone<KeyRangeRef>> keyRanges;
	std::vector<UID> applierIDs;

	printf("[INFO] Node:%s, Assign key range to appliers. num_appliers:%d\n", rd->describeNode().c_str(), rd->range2Applier.size());
	for (auto& applier : rd->range2Applier) {
		lowerBounds.push_back(applier.first);
		applierIDs.push_back(applier.second);
		printf("\t[INFO] ApplierID:%s lowerBound:%s\n",
				applierIDs.back().toString().c_str(),
				lowerBounds.back().toString().c_str());
	}
	for (int i  = 0; i < lowerBounds.size(); ++i) {
		KeyRef startKey = lowerBounds[i];
		KeyRef endKey;
		if ( i < lowerBounds.size() - 1) {
			endKey = lowerBounds[i+1];
		} else {
			endKey = normalKeys.end;
		}

		keyRanges.push_back(KeyRangeRef(startKey, endKey));
	}

	ASSERT( applierIDs.size() == keyRanges.size() );
	state std::map<UID, Standalone<KeyRangeRef>> appliers;
	appliers.clear(); // If this function is called more than once in multiple version batches, appliers may carry over the data from earlier version batch
	for (int i = 0; i < applierIDs.size(); ++i) {
		if (appliers.find(applierIDs[i]) != appliers.end()) {
			printf("[ERROR] ApplierID appear more than once!appliers size:%d applierID: %s\n",
					appliers.size(), applierIDs[i].toString().c_str());
			printApplierKeyRangeInfo(appliers);
		}
		ASSERT( appliers.find(applierIDs[i]) == appliers.end() ); // we should not have a duplicate applierID respoinsbile for multiple key ranges
		appliers.insert(std::make_pair(applierIDs[i], keyRanges[i]));
	}


	state std::vector<Future<RestoreCommandReply>> cmdReplies;
	loop {
		try {
			cmdReplies.clear();
			rd->cmdID.initPhase(RestoreCommandEnum::Assign_Applier_KeyRange);
			for (auto& applier : appliers) {
				KeyRangeRef keyRange = applier.second;
				UID nodeID = applier.first;
				ASSERT(rd->workers_interface.find(nodeID) != rd->workers_interface.end());
				RestoreCommandInterface& cmdInterf = rd->workers_interface[nodeID];
				printf("[CMD] Node:%s, Assign KeyRange:%s [begin:%s end:%s] to applier ID:%s\n", rd->describeNode().c_str(),
						keyRange.toString().c_str(),
						getHexString(keyRange.begin).c_str(), getHexString(keyRange.end).c_str(),
						nodeID.toString().c_str());
				rd->cmdID.nextCmd();
				cmdReplies.push_back( cmdInterf.cmd.getReply(RestoreCommand(RestoreCommandEnum::Assign_Applier_KeyRange, rd->cmdID, nodeID, keyRange)) );

			}
			printf("[INFO] Wait for %d applier to accept the cmd Assign_Applier_KeyRange\n", appliers.size());
			std::vector<RestoreCommandReply> reps = wait( timeoutError(getAll(cmdReplies), FastRestore_Failure_Timeout) );
			for (int i = 0; i < reps.size(); ++i) {
				printf("[INFO] Get reply:%s for Assign_Applier_KeyRange\n",
						reps[i].toString().c_str());
			}

			break;
		} catch (Error &e) {
			// TODO: Handle the command reply timeout error
			if (e.code() != error_code_io_timeout) {
				fprintf(stderr, "[ERROR] Node:%s, Commands before cmdID:%s timeout\n", rd->describeNode().c_str(), rd->cmdID.toString().c_str());
			} else {
				fprintf(stderr, "[ERROR] Node:%s, Commands before cmdID:%s error. error code:%d, error message:%s\n", rd->describeNode().c_str(),
						rd->cmdID.toString().c_str(), e.code(), e.what());
			}
			//fprintf(stderr, "[ERROR] WE STOP HERE FOR DEBUG\n");
			//break;
		}
	}

	loop {
		//wait(delay(1.0));
		try {
			cmdReplies.clear();
			rd->cmdID.initPhase(RestoreCommandEnum::Assign_Applier_KeyRange_Done);
			for (auto& applier : appliers) {
				KeyRangeRef keyRange = applier.second;
				UID nodeID = applier.first;
				RestoreCommandInterface& cmdInterf = rd->workers_interface[nodeID];
				rd->cmdID.nextCmd();
				printf("[CMD] Node:%s Finish assigning KeyRange %s to applier ID:%s\n",rd->describeNode().c_str(), keyRange.toString().c_str(), nodeID.toString().c_str());
				cmdReplies.push_back( cmdInterf.cmd.getReply(RestoreCommand(RestoreCommandEnum::Assign_Applier_KeyRange_Done, rd->cmdID, nodeID)) );

			}
			std::vector<RestoreCommandReply> reps = wait( timeoutError(getAll(cmdReplies), FastRestore_Failure_Timeout) );
			for (int i = 0; i < reps.size(); ++i) {
				printf("[INFO] Assign_Applier_KeyRange_Done: Get reply:%s\n",
						reps[i].toString().c_str());
			}

			break;
		} catch (Error &e) {
			// TODO: Handle the command reply timeout error
			if (e.code() != error_code_io_timeout) {
				fprintf(stderr, "[ERROR] Node:%s, Commands before cmdID:%s timeout\n", rd->describeNode().c_str(), rd->cmdID.toString().c_str());
			} else {
				fprintf(stderr, "[ERROR] Node:%s, Commands before cmdID:%s error. error code:%d, error message:%s\n", rd->describeNode().c_str(),
						rd->cmdID.toString().c_str(), e.code(), e.what());
			}
			//fprintf(stderr, "[ERROR] WE STOP HERE FOR DEBUG\n");
			//break;
		}
	}

	return Void();
}

// MXNOTE: Revise Done
// Handle restore command request on workers
ACTOR Future<Void> assignKeyRangeToAppliersHandler(Reference<RestoreData> rd, RestoreCommandInterface interf) {
	if ( rd->localNodeStatus.role != RestoreRole::Applier) {
		printf("[ERROR] non-applier node:%s (role:%d) is waiting for cmds for appliers\n",
				rd->describeNode().c_str(), rd->localNodeStatus.role);
	} else {
		printf("[INFO][Applier] nodeID:%s (interface id:%s) waits for Assign_Applier_KeyRange cmd\n",
				rd->describeNode().c_str(), interf.id().toString().c_str());
	}

	loop {
		choose {
			when(RestoreCommand req = waitNext(interf.cmd.getFuture())) {
				printf("[INFO] Node:%s Got Restore Command: CMDID:%s KeyRange:%s\n", rd->describeNode().c_str(),
						req.cmdID.toString().c_str(), req.keyRange.toString().c_str());
				if ( rd->localNodeStatus.nodeID != req.id ) {
						printf("[ERROR] CMDID:%s node:%s receive request with a different id:%s\n",
								req.cmdID.toString().c_str(), rd->describeNode().c_str(), req.id.toString().c_str());
				}
				if ( req.cmd == RestoreCommandEnum::Assign_Applier_KeyRange ) {
					// Idempodent operation. OK to re-execute the duplicate cmd
					// The applier should remember the key range it is responsible for
					ASSERT(req.cmd == (RestoreCommandEnum) req.cmdID.phase);
					rd->applierStatus.id = req.id;
					rd->applierStatus.keyRange = req.keyRange;
					req.reply.send(RestoreCommandReply(interf.id(), req.cmdID));
				} else if (req.cmd == RestoreCommandEnum::Assign_Applier_KeyRange_Done) {
					printf("[INFO] Node:%s CMDID:%s Node:%s finish configure its key range:%s.\n", rd->describeNode().c_str(),
							req.cmdID.toString().c_str(), rd->describeNode().c_str(), rd->applierStatus.keyRange.toString().c_str());
					ASSERT(req.cmd == (RestoreCommandEnum) req.cmdID.phase);
					req.reply.send(RestoreCommandReply(interf.id(), req.cmdID)); // master node is waiting
					break;
				} else {
					if ( IsCmdInPreviousPhase(RestoreCommandEnum::Assign_Applier_KeyRange_Done, req.cmd) ) {
						printf("Applier Node:%s receive commands from last phase. Check if this node is master applier\n", rd->describeNode().c_str());
						logExpectedOldCmd(rd, RestoreCommandEnum::Assign_Applier_KeyRange_Done, req.cmd, req.cmdID);
						req.reply.send(RestoreCommandReply(interf.id(), req.cmdID));
					} else {
						logUnexpectedCmd(rd, RestoreCommandEnum::Assign_Applier_KeyRange_Done, req.cmd, req.cmdID);
					}
				}
			}
		}
	}

	return Void();
}

// MXNOTE: Revise done
// Notify loader about appliers' responsible key range
ACTOR Future<Void> notifyAppliersKeyRangeToLoader(Reference<RestoreData> rd, Database cx)  {
	state std::vector<UID> loaders = getLoaderIDs(rd);
	state std::vector<Future<RestoreCommandReply>> cmdReplies;
	loop {
		try {

			rd->cmdID.initPhase( RestoreCommandEnum::Notify_Loader_ApplierKeyRange );
			for (auto& nodeID : loaders) {
				ASSERT(rd->workers_interface.find(nodeID) != rd->workers_interface.end());
				RestoreCommandInterface& cmdInterf = rd->workers_interface[nodeID];
				printf("[CMD] Node:%s Notify node:%s about appliers key range\n", rd->describeNode().c_str(), nodeID.toString().c_str());
				state std::map<Standalone<KeyRef>, UID>::iterator applierRange;
				for (applierRange = rd->range2Applier.begin(); applierRange != rd->range2Applier.end(); applierRange++) {
					rd->cmdID.nextCmd();
					cmdReplies.push_back( cmdInterf.cmd.getReply(RestoreCommand(RestoreCommandEnum::Notify_Loader_ApplierKeyRange, rd->cmdID, nodeID, applierRange->first, applierRange->second)) );
				}
			}
			printf("[INFO] Wait for %d loaders to accept the cmd Notify_Loader_ApplierKeyRange\n", loaders.size());
			std::vector<RestoreCommandReply> reps = wait( timeoutError( getAll(cmdReplies), FastRestore_Failure_Timeout ) );
			for (int i = 0; i < reps.size(); ++i) {
				printf("[INFO] Get reply:%s from Notify_Loader_ApplierKeyRange cmd for node.\n",
						reps[i].toString().c_str());
			}

			cmdReplies.clear();
			rd->cmdID.initPhase( RestoreCommandEnum::Notify_Loader_ApplierKeyRange_Done );
			for (auto& nodeID : loaders) {
				RestoreCommandInterface& cmdInterf = rd->workers_interface[nodeID];
				rd->cmdID.nextCmd();
				printf("[CMD] Node:%s Notify node:%s cmd Notify_Loader_ApplierKeyRange_Done\n", rd->describeNode().c_str(), nodeID.toString().c_str());
				cmdReplies.push_back( cmdInterf.cmd.getReply(RestoreCommand(RestoreCommandEnum::Notify_Loader_ApplierKeyRange_Done, rd->cmdID, nodeID)) );

			}
			std::vector<RestoreCommandReply> reps = wait( timeoutError( getAll(cmdReplies), FastRestore_Failure_Timeout) );
			for (int i = 0; i < reps.size(); ++i) {
				printf("[INFO] Node:%s, Get reply from Notify_Loader_ApplierKeyRange_Done cmd for CMDUID:%s\n", rd->describeNode().c_str(),
						reps[i].cmdID.toString().c_str());
			}

			break;
		} catch (Error &e) {
			// TODO: Handle the command reply timeout error
			if (e.code() != error_code_io_timeout) {
				fprintf(stderr, "[ERROR] Node:%s, Commands before cmdID:%s timeout\n", rd->describeNode().c_str(), rd->cmdID.toString().c_str());
			} else {
				fprintf(stderr, "[ERROR] Node:%s, Commands before cmdID:%s error. error code:%d, error message:%s\n", rd->describeNode().c_str(),
						rd->cmdID.toString().c_str(), e.code(), e.what());
			}
			fprintf(stderr, "[ERROR] WE STOP HERE FOR DEBUG\n");
			break;
		}
	}

	return Void();
}

// MXNOTE: revise doen
// Handle  Notify_Loader_ApplierKeyRange cmd
ACTOR Future<Void> notifyAppliersKeyRangeToLoaderHandler(Reference<RestoreData> rd, RestoreCommandInterface interf) {
	if ( rd->localNodeStatus.role != RestoreRole::Loader) {
		printf("[ERROR] non-loader node:%s (role:%d) is waiting for cmds for Loader\n",
				rd->describeNode().c_str(), rd->localNodeStatus.role);
	} else {
		printf("[INFO][Loader] nodeID:%s (interface id:%s) waits for Notify_Loader_ApplierKeyRange cmd\n",
				rd->describeNode().c_str(), interf.id().toString().c_str());
	}

	loop {
		choose {
			when(RestoreCommand req = waitNext(interf.cmd.getFuture())) {
				printf("[INFO] Node:%s, Got Restore Command CmdID:%s \n", rd->describeNode().c_str(), req.cmdID.toString().c_str());
				if ( rd->localNodeStatus.nodeID != req.id ) {
						printf("[ERROR] CmdID:%s node:%s receive request with a different id:%s\n", req.cmdID.toString().c_str(),
								rd->describeNode().c_str(), req.id.toString().c_str());
				}
				if ( req.cmd == RestoreCommandEnum::Notify_Loader_ApplierKeyRange ) {
					ASSERT(req.cmd == (RestoreCommandEnum) req.cmdID.phase);
					KeyRef applierKeyRangeLB = req.applierKeyRangeLB;
					UID applierID = req.applierID;
					if (rd->range2Applier.find(applierKeyRangeLB) != rd->range2Applier.end()) {
						if ( rd->range2Applier[applierKeyRangeLB] != applierID) {
							printf("[WARNING] key range to applier may be wrong for range:%s on applierID:%s!",
									getHexString(applierKeyRangeLB).c_str(), applierID.toString().c_str());
						}
						rd->range2Applier[applierKeyRangeLB] = applierID;//always use the newest one
					} else {
						rd->range2Applier.insert(std::make_pair(applierKeyRangeLB, applierID));
					}
					req.reply.send(RestoreCommandReply(interf.id(), req.cmdID));
				} else if (req.cmd == RestoreCommandEnum::Notify_Loader_ApplierKeyRange_Done) {
					printf("[INFO] Node:%s CmdId finish Notify_Loader_ApplierKeyRange, has range2Applier size:%d.\n",
							rd->describeNode().c_str(), req.cmdID.toString().c_str(), rd->range2Applier.size());
					ASSERT(req.cmd == (RestoreCommandEnum) req.cmdID.phase);
					printAppliersKeyRange(rd);
					req.reply.send(RestoreCommandReply(interf.id(), req.cmdID)); // master node is waiting
					break;
				} else {
					printf("[WARNING]notifyAppliersKeyRangeToLoaderHandler() master is wating on cmd:%d for node:%s due to message lost, we reply to it.\n", req.cmd, rd->describeNode().c_str());
					req.reply.send(RestoreCommandReply(interf.id(), req.cmdID)); // master node is waiting
				}
			}
		}
	}

	return Void();
}



void printLowerBounds(std::vector<Standalone<KeyRef>> lowerBounds) {
	printf("[INFO] Print out %d keys in the lowerbounds\n", lowerBounds.size());
	for (int i = 0; i < lowerBounds.size(); i++) {
		printf("\t[INFO][%d] %s\n", i, getHexString(lowerBounds[i]).c_str());
	}
}

std::vector<Standalone<KeyRef>> _calculateAppliersKeyRanges(Reference<RestoreData> rd, int numAppliers) {
	ASSERT(numAppliers > 0);
	std::vector<Standalone<KeyRef>> lowerBounds;
	//intervalLength = (numSampledMutations - remainder) / (numApplier - 1)
	int intervalLength = std::max(rd->numSampledMutations / numAppliers, 1); // minimal length is 1
	int curCount = 0;
	int curInterval = 0;



	printf("[INFO] Node:%s calculateAppliersKeyRanges(): numSampledMutations:%d numAppliers:%d intervalLength:%d\n",
			rd->describeNode().c_str(),
			rd->numSampledMutations, numAppliers, intervalLength);
	for (auto &count : rd->keyOpsCount) {
		if (curInterval <= curCount / intervalLength) {
			printf("[INFO] Node:%s calculateAppliersKeyRanges(): Add a new key range %d: curCount:%d\n",
					rd->describeNode().c_str(), curInterval, curCount);
			lowerBounds.push_back(count.first); // The lower bound of the current key range
			curInterval++;
		}
		curCount += count.second;
	}

	if ( lowerBounds.size() != numAppliers ) {
		printf("[WARNING] calculateAppliersKeyRanges() WE MAY NOT USE ALL APPLIERS efficiently! num_keyRanges:%d numAppliers:%d\n",
				lowerBounds.size(), numAppliers);
		printLowerBounds(lowerBounds);
	}

	//ASSERT(lowerBounds.size() <= numAppliers + 1); // We may have at most numAppliers + 1 key ranges
	if ( lowerBounds.size() >= numAppliers ) {
		printf("[WARNING] Key ranges number:%d > numAppliers:%d. Merge the last ones\n", lowerBounds.size(), numAppliers);
	}

	while ( lowerBounds.size() >= numAppliers ) {
		printf("[WARNING] Key ranges number:%d > numAppliers:%d. Merge the last ones\n", lowerBounds.size(), numAppliers);
		lowerBounds.pop_back();
	}

	return lowerBounds;
}

// MXNOTE: Revise done
// Master applier calculate the key range for appliers
ACTOR Future<Void> calculateApplierKeyRange(Reference<RestoreData> rd, RestoreCommandInterface interf) {
	if ( rd->localNodeStatus.role != RestoreRole::Applier) {
		printf("[ERROR] non-applier node:%s (role:%d) is waiting for cmds for appliers\n",
				rd->describeNode().c_str(), rd->localNodeStatus.role);
	} else {
		printf("[INFO][Applier] nodeID:%s (interface id:%s) waits for Calculate_Applier_KeyRange cmd\n",
				rd->describeNode().c_str(), interf.id().toString().c_str());
	}

	state int numMutations = 0;
	state std::vector<Standalone<KeyRef>> keyRangeLowerBounds;

	loop {
		choose {
			when(RestoreCommand req = waitNext(interf.cmd.getFuture())) {
				if ( rd->localNodeStatus.nodeID != req.id ) {
						printf("[ERROR] CMD:%s Node:%s receive request with a different node id:%s\n",
								rd->cmdID.toString().c_str(), rd->describeNode().c_str(), req.id.toString().c_str());
				}
				// Handle duplicate message
				if (rd->isCmdProcessed(req.cmdID) ) {
					printf("[DEBUG] Node:%s skip duplicate cmd:%s\n", rd->describeNode().c_str(), req.cmdID.toString().c_str());
					req.reply.send(RestoreCommandReply(interf.id(), req.cmdID));
					continue;
				}
				if ( req.cmd == RestoreCommandEnum::Calculate_Applier_KeyRange ) {
					// Applier will calculate applier key range
					printf("[INFO][Applier] CMD:%s, Node:%s Calculate key ranges for %d appliers\n",
							req.cmdID.toString().c_str(), rd->describeNode().c_str(), req.keyRangeIndex);
					ASSERT(req.cmd == (RestoreCommandEnum) req.cmdID.phase);
					if ( keyRangeLowerBounds.empty() ) {
						keyRangeLowerBounds = _calculateAppliersKeyRanges(rd, req.keyRangeIndex); // keyRangeIndex is the number of key ranges requested
					}
					printf("[INFO][Applier] CMD:%s, NodeID:%s: num of key ranges:%d\n",
							rd->cmdID.toString().c_str(), rd->describeNode().c_str(), keyRangeLowerBounds.size());
					req.reply.send(RestoreCommandReply(interf.id(), req.cmdID, keyRangeLowerBounds.size()));
					//rd->processedCmd[req.cmdID] = 1; // We should not skip this command in the following phase. Otherwise, the handler in other phases may return a wrong number of appliers
				} else if ( req.cmd == RestoreCommandEnum::Get_Applier_KeyRange ) {
					if ( req.keyRangeIndex < 0 || req.keyRangeIndex >= keyRangeLowerBounds.size() ) {
						printf("[INFO][Applier] NodeID:%s Get_Applier_KeyRange keyRangeIndex is out of range. keyIndex:%d keyRagneSize:%d\n",
								rd->describeNode().c_str(), req.keyRangeIndex,  keyRangeLowerBounds.size());
					}
					ASSERT(req.cmd == (RestoreCommandEnum) req.cmdID.phase);

					printf("[INFO][Applier] NodeID:%s replies Get_Applier_KeyRange. keyRangeIndex:%d lower_bound_of_keyRange:%s\n",
							rd->describeNode().c_str(), req.keyRangeIndex, getHexString(keyRangeLowerBounds[req.keyRangeIndex]).c_str());

					req.reply.send(RestoreCommandReply(interf.id(), req.cmdID, keyRangeLowerBounds[req.keyRangeIndex]));
				} else if ( req.cmd == RestoreCommandEnum::Get_Applier_KeyRange_Done ) {
					printf("[INFO][Applier] NodeID:%s replies Get_Applier_KeyRange_Done\n",
							rd->describeNode().c_str());
					ASSERT(req.cmd == (RestoreCommandEnum) req.cmdID.phase);
					req.reply.send(RestoreCommandReply(interf.id(), req.cmdID));
					rd->processedCmd[req.cmdID] = 1;
					break;
				} else {
					if ( IsCmdInPreviousPhase(RestoreCommandEnum::Get_Applier_KeyRange_Done, req.cmd) ) {
						logExpectedOldCmd(rd, RestoreCommandEnum::Get_Applier_KeyRange_Done, req.cmd, req.cmdID);
						req.reply.send(RestoreCommandReply(interf.id(), req.cmdID));
					} else {
						logUnexpectedCmd(rd, RestoreCommandEnum::Get_Applier_KeyRange_Done, req.cmd, req.cmdID);
					}
				}
			}
		}
	}

	return Void();
}


// Receive mutations sent from loader
ACTOR Future<Void> receiveMutations(Reference<RestoreData> rd, RestoreCommandInterface interf) {
	if ( rd->localNodeStatus.role != RestoreRole::Applier) {
		printf("[ERROR] non-applier node:%s (role:%d) is waiting for cmds for appliers\n",
				rd->describeNode().c_str(), rd->localNodeStatus.role);
	} else {
		printf("[INFO][Applier] nodeID:%s (interface id:%s) waits for Loader_Send_Mutations_To_Applier cmd\n",
				rd->describeNode().c_str(), interf.id().toString().c_str());
	}

	//printf("[WARNING!!!] The receiveMutations() May receive the same mutation more than once! BAD for atomic operations!\n");

	state int numMutations = 0;

	loop {
		choose {
			when(RestoreCommand req = waitNext(interf.cmd.getFuture())) {
//				printf("[INFO][Applier] Got Restore Command: cmd:%d UID:%s\n",
//						req.cmd, req.id.toString().c_str());
				if ( rd->localNodeStatus.nodeID != req.id ) {
						printf("[ERROR] Node:%s receive request with a different id:%s\n",
								rd->describeNode().c_str(), req.id.toString().c_str());
				}
				if ( req.cmd == RestoreCommandEnum::Loader_Send_Mutations_To_Applier ) {
					ASSERT(req.cmd == (RestoreCommandEnum) req.cmdID.phase);
					printf("[VERBOSE_DEBUG] Node:%s receive mutation:%s\n", rd->describeNode().c_str(), req.mutation.toString().c_str());
					// Handle duplicat cmd
					if ( rd->isCmdProcessed(req.cmdID) ) {
						printf("[DEBUG] NODE:%s skip duplicate cmd:%s\n", rd->describeNode().c_str(), req.cmdID.toString().c_str());
						printf("[DEBUG] Skipped mutation:%s\n", req.mutation.toString().c_str());
						req.reply.send(RestoreCommandReply(interf.id(), req.cmdID));	
						continue;
					}
					// Applier will cache the mutations at each version. Once receive all mutations, applier will apply them to DB
					state uint64_t commitVersion = req.commitVersion;
					MutationRef mutation(req.mutation);
					if ( rd->kvOps.find(commitVersion) == rd->kvOps.end() ) {
						rd->kvOps.insert(std::make_pair(commitVersion, VectorRef<MutationRef>()));
					}
					rd->kvOps[commitVersion].push_back_deep(rd->kvOps[commitVersion].arena(), mutation);
					numMutations++;
					if ( numMutations % 100000 == 1 ) { // Should be different value in simulation and in real mode
						printf("[INFO][Applier] Node:%s Receives %d mutations. cur_mutation:%s\n",
								rd->describeNode().c_str(), numMutations, mutation.toString().c_str());
					}

					req.reply.send(RestoreCommandReply(interf.id(), req.cmdID));
					rd->processedCmd[req.cmdID] = 1;
				} else if ( req.cmd == RestoreCommandEnum::Loader_Send_Mutations_To_Applier_Done ) {
					printf("[INFO][Applier] NodeID:%s receive all mutations, num_versions:%d\n", rd->describeNode().c_str(), rd->kvOps.size());
					ASSERT(req.cmd == (RestoreCommandEnum) req.cmdID.phase);
					req.reply.send(RestoreCommandReply(interf.id(), req.cmdID));
					break;
				} else {
					if ( IsCmdInPreviousPhase(RestoreCommandEnum::Loader_Send_Mutations_To_Applier_Done, req.cmd) ) {
						logExpectedOldCmd(rd, RestoreCommandEnum::Loader_Send_Mutations_To_Applier_Done, req.cmd, req.cmdID);
						req.reply.send(RestoreCommandReply(interf.id(), req.cmdID));
					} else {
						logUnexpectedCmd(rd, RestoreCommandEnum::Loader_Send_Mutations_To_Applier_Done, req.cmd, req.cmdID);
					}
					
				}
			}
		}
	}

	return Void();
}

// MXINFO: Revise done
ACTOR Future<Void> applyMutationToDB(Reference<RestoreData> rd, RestoreCommandInterface interf, Database cx) {
	if ( rd->localNodeStatus.role != RestoreRole::Applier) {
		printf("[ERROR] non-applier node:%s (role:%d) is waiting for cmds for appliers\n",
				rd->describeNode().c_str(), rd->localNodeStatus.role);
	} else {
		printf("[INFO][Applier] nodeID:%s (interface id:%s) waits for Loader_Notify_Appler_To_Apply_Mutation cmd\n",
				rd->describeNode().c_str(), interf.id().toString().c_str());
	}

	//printf("[WARNING!!!] The applyKVOpsToDB() May be applied multiple times! BAD for atomic operations!\n");

	state int numMutations = 0;

	loop {
		choose {
			when(state RestoreCommand req = waitNext(interf.cmd.getFuture())) {
//				printf("[INFO][Applier] Got Restore Command: cmd:%d UID:%s\n",
//						req.cmd, req.id.toString().c_str());
				if ( rd->localNodeStatus.nodeID != req.id ) {
						printf("[ERROR] node:%s receive request with a different id:%s\n",
								rd->describeNode().c_str(), req.id.toString().c_str());
				}
				if ( req.cmd == RestoreCommandEnum::Loader_Notify_Appler_To_Apply_Mutation ) {
					printf("[INFO][Applier] node:%s sanity check mutations to be applied...\n", rd->describeNode().c_str());
					ASSERT(req.cmd == (RestoreCommandEnum) req.cmdID.phase);
					if ( rd->isCmdProcessed(req.cmdID) ) {
						printf("[DEBUG] NODE:%s skip duplicate cmd:%s\n", rd->describeNode().c_str(), req.cmdID.toString().c_str());
						req.reply.send(RestoreCommandReply(interf.id(), req.cmdID));
						continue;
					}
					sanityCheckMutationOps(rd);
					// Applier apply mutations to DB
					printf("[INFO][Applier] apply KV ops to DB starts...\n");
					wait( applyKVOpsToDB(rd, cx) );
					printf("[INFO][Applier] apply KV ops to DB finishes...\n");
					req.reply.send(RestoreCommandReply(interf.id(), req.cmdID));
					printf("[INFO][Applier] Node: %s, At the end of its functionality! Hang here to make sure master proceeds!\n",
								rd->describeNode().c_str());
					rd->processedCmd[req.cmdID] = 1;
					// Applier should wait in the loop in case the send message is lost. This actor will be cancelled when the test finishes
					break;
				} else {
					if ( IsCmdInPreviousPhase(RestoreCommandEnum::Loader_Notify_Appler_To_Apply_Mutation, req.cmd) ) {
						logExpectedOldCmd(rd, RestoreCommandEnum::Loader_Send_Mutations_To_Applier_Done, req.cmd, req.cmdID);
						req.reply.send(RestoreCommandReply(interf.id(), req.cmdID)); // master is waiting on the previous command
					} else {
						logUnexpectedCmd(rd, RestoreCommandEnum::Loader_Notify_Appler_To_Apply_Mutation, req.cmd, req.cmdID);
					}
					
				}
			}
		}
	}

	return Void();
}

//MXNOTE: Revise Done
//DONE: collectRestoreRequests
ACTOR Future<Standalone<VectorRef<RestoreRequest>>> collectRestoreRequests(Database cx) {
	state int restoreId = 0;
	state int checkNum = 0;
	state Standalone<VectorRef<RestoreRequest>> restoreRequests;

	//wait for the restoreRequestTriggerKey to be set by the client/test workload
	state ReadYourWritesTransaction tr2(cx);

	loop {
		try {
			tr2.reset(); // The transaction may fail! Must full reset the transaction
			tr2.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			tr2.setOption(FDBTransactionOptions::LOCK_AWARE);
			// Assumption: restoreRequestTriggerKey has not been set
			// Question: What if  restoreRequestTriggerKey has been set? we will stuck here?
			// Question: Can the following code handle the situation?
			// Note: restoreRequestTriggerKey may be set before the watch is set or may have a conflict when the client sets the same key
			// when it happens, will we  stuck at wait on the watch?

			state Future<Void> watch4RestoreRequest = tr2.watch(restoreRequestTriggerKey);
			wait(tr2.commit());
			printf("[INFO][Master] Finish setting up watch for restoreRequestTriggerKey\n");
			break;
		} catch(Error &e) {
			printf("[WARNING] Transaction for restore request. Error:%s\n", e.name());
			wait(tr2.onError(e));
		}
	};


	loop {
		try {
			tr2.reset(); // The transaction may fail! Must full reset the transaction
			tr2.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			tr2.setOption(FDBTransactionOptions::LOCK_AWARE);
			// Assumption: restoreRequestTriggerKey has not been set
			// Before we wait on the watch, we must make sure the key is not there yet!
			printf("[INFO][Master] Make sure restoreRequestTriggerKey does not exist before we wait on the key\n");
			Optional<Value> triggerKey = wait( tr2.get(restoreRequestTriggerKey) );
			if ( triggerKey.present() ) {
				printf("!!! restoreRequestTriggerKey (and restore requests) is set before restore agent waits on the request. Restore agent can immediately proceed\n");
				break;
			}
			wait(watch4RestoreRequest);
			printf("[INFO][Master] restoreRequestTriggerKey watch is triggered\n");
			break;
		} catch(Error &e) {
			printf("[WARNING] Transaction for restore request. Error:%s\n", e.name());
			wait(tr2.onError(e));
		}
	};

	loop {
		try {
			tr2.reset();
			tr2.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			tr2.setOption(FDBTransactionOptions::LOCK_AWARE);

			state Optional<Value> numRequests = wait(tr2.get(restoreRequestTriggerKey));
			int num = decodeRestoreRequestTriggerValue(numRequests.get());
			//TraceEvent("RestoreRequestKey").detail("NumRequests", num);
			printf("[INFO] RestoreRequestNum:%d\n", num);

			state Standalone<RangeResultRef> restoreRequestValues = wait(tr2.getRange(restoreRequestKeys, CLIENT_KNOBS->TOO_MANY));
			printf("Restore worker get restoreRequest: %sn", restoreRequestValues.toString().c_str());

			ASSERT(!restoreRequestValues.more);

			if(restoreRequestValues.size()) {
				for ( auto &it : restoreRequestValues ) {
					printf("Now decode restore request value...\n");
					restoreRequests.push_back(restoreRequests.arena(), decodeRestoreRequestValue(it.value));
				}
			}
			break;
		} catch(Error &e) {
			printf("[WARNING] Transaction error: collect restore requests. Error:%s\n", e.name());
			wait(tr2.onError(e));
		}
	};


	return restoreRequests;
}

void printRestorableFileSet(Optional<RestorableFileSet> files) {

	printf("[INFO] RestorableFileSet num_of_range_files:%d num_of_log_files:%d\n",
			files.get().ranges.size(), files.get().logs.size());
	int index = 0;
 	for(const RangeFile &f : files.get().ranges) {
 		printf("\t[INFO] [RangeFile:%d]:%s\n", index, f.toString().c_str());
 		++index;
 	}
 	index = 0;
 	for(const LogFile &f : files.get().logs) {
		printf("\t[INFO], [LogFile:%d]:%s\n", index, f.toString().c_str());
		++index;
 	}

 	return;
}

std::vector<RestoreFile> getRestoreFiles(Optional<RestorableFileSet> fileSet) {
	std::vector<RestoreFile> files;

 	for(const RangeFile &f : fileSet.get().ranges) {
 		files.push_back({f.version, f.fileName, true, f.blockSize, f.fileSize});
 	}
 	for(const LogFile &f : fileSet.get().logs) {
 		files.push_back({f.beginVersion, f.fileName, false, f.blockSize, f.fileSize, f.endVersion});
 	}

 	return files;
}
// MX: This function is refactored
// NOTE: This function can now get the backup file descriptors
ACTOR static Future<Void> collectBackupFiles(Reference<RestoreData> rd, Database cx, RestoreRequest request) {
	state Key tagName = request.tagName;
	state Key url = request.url;
	state bool waitForComplete = request.waitForComplete;
	state Version targetVersion = request.targetVersion;
	state bool verbose = request.verbose;
	state KeyRange range = request.range;
	state Key addPrefix = request.addPrefix;
	state Key removePrefix = request.removePrefix;
	state bool lockDB = request.lockDB;
	state UID randomUid = request.randomUid;
	//state VectorRef<RestoreFile> files; // return result

	ASSERT( lockDB == true );

	state Reference<IBackupContainer> bc = IBackupContainer::openContainer(url.toString());
	state BackupDescription desc = wait(bc->describeBackup());

	wait(desc.resolveVersionTimes(cx));

	printf("[INFO] Backup Description\n%s", desc.toString().c_str());
	printf("[INFO] Restore for url:%s, lockDB:%d\n", url.toString().c_str(), lockDB);
	if(targetVersion == invalidVersion && desc.maxRestorableVersion.present())
		targetVersion = desc.maxRestorableVersion.get();

	printf("[INFO] collectBackupFiles: now getting backup files for restore request: %s\n", request.toString().c_str());
	Optional<RestorableFileSet> restorable = wait(bc->getRestoreSet(targetVersion));

	if(!restorable.present()) {
		printf("[WARNING] restoreVersion:%ld (%lx) is not restorable!\n", targetVersion, targetVersion);
		throw restore_missing_data();
	}

	if (!rd->files.empty()) {
		printf("[WARNING] global files are not empty! files.size()=%d. We forcely clear files\n", rd->files.size());
		rd->files.clear();
	}

	printf("[INFO] Found backup files: num of files:%d\n", rd->files.size());
 	for(const RangeFile &f : restorable.get().ranges) {
 		TraceEvent("FoundRangeFileMX").detail("FileInfo", f.toString());
 		printf("[INFO] FoundRangeFile, fileInfo:%s\n", f.toString().c_str());
		RestoreFile file = {f.version, f.fileName, true, f.blockSize, f.fileSize, 0};
 		rd->files.push_back(file);
 	}
 	for(const LogFile &f : restorable.get().logs) {
 		TraceEvent("FoundLogFileMX").detail("FileInfo", f.toString());
		printf("[INFO] FoundLogFile, fileInfo:%s\n", f.toString().c_str());
		RestoreFile file = {f.beginVersion, f.fileName, false, f.blockSize, f.fileSize, f.endVersion, 0};
		rd->files.push_back(file);
 	}

	printf("[INFO] Restoring backup to version: %lld\n", (long long) targetVersion);

	return Void();
}

// MXNOTE: Revise Done
// The manager that manage the control of sampling workload
ACTOR static Future<Void> sampleWorkload(Reference<RestoreData> rd, RestoreRequest request, Reference<RestoreConfig> restoreConfig, int64_t sampleMB_input) {
	state Key tagName = request.tagName;
	state Key url = request.url;
	state bool waitForComplete = request.waitForComplete;
	state Version targetVersion = request.targetVersion;
	state bool verbose = request.verbose;
	state KeyRange restoreRange = request.range;
	state Key addPrefix = request.addPrefix;
	state Key removePrefix = request.removePrefix;
	state bool lockDB = request.lockDB;
	state UID randomUid = request.randomUid;
	state Key mutationLogPrefix = restoreConfig->mutationLogPrefix();

	state bool allLoadReqsSent = false;
	state std::vector<UID> loaderIDs = getLoaderIDs(rd);
	state std::vector<UID> applierIDs = getApplierIDs(rd);
	state std::vector<UID> finishedLoaderIDs;
	state int64_t sampleMB = sampleMB_input; //100;
	state int64_t sampleB = sampleMB * 1024 * 1024; // Sample a block for every sampleB bytes. // Should adjust this value differently for simulation mode and real mode
	state int64_t curFileIndex = 0;
	state int64_t curFileOffset = 0;
	state int64_t loadSizeB = 0;
	state int64_t loadingCmdIndex = 0;
	state int64_t sampleIndex = 0;
	state double totalBackupSizeB = 0;
	state double samplePercent = 0.05; // sample 1 data block per samplePercent (0.01) of data. num_sample = 1 / samplePercent

	// We should sample 1% data
	for (int i = 0; i < rd->files.size(); i++) {
		totalBackupSizeB += rd->files[i].fileSize;
	}
	sampleB = std::max((int) (samplePercent * totalBackupSizeB), 10 * 1024 * 1024); // The minimal sample size is 10MB
	printf("Node:%s totalBackupSizeB:%.1fB (%.1fMB) samplePercent:%.2f, sampleB:%d\n", rd->describeNode().c_str(),
			totalBackupSizeB,  totalBackupSizeB / 1024 / 1024, samplePercent, sampleB);

	// Step: Distribute sampled file blocks to loaders to sample the mutations
	rd->cmdID.initPhase(RestoreCommandEnum::Sample_Range_File);
	curFileIndex = 0;
	state CMDUID checkpointCMDUID = rd->cmdID;
	state int checkpointCurFileIndex = curFileIndex;
	state int64_t checkpointCurFileOffset = 0;
	state std::vector<Future<RestoreCommandReply>> cmdReplies;
	state RestoreCommandEnum cmdType = RestoreCommandEnum::Sample_Range_File;
	loop { // For retry on timeout
		try {
			if ( allLoadReqsSent ) {
				break; // All load requests have been handled
			}
			wait(delay(1.0));

			cmdReplies.clear();

			printf("[Sampling] Node:%s We will sample the workload among %d backup files.\n", rd->describeNode().c_str(), rd->files.size());
			printf("[Sampling] Node:%s totalBackupSizeB:%.1fB (%.1fMB) samplePercent:%.2f, sampleB:%d, loadSize:%dB sampleIndex:%d\n", rd->describeNode().c_str(),
				totalBackupSizeB,  totalBackupSizeB / 1024 / 1024, samplePercent, sampleB, loadSizeB, sampleIndex);
			for (auto &loaderID : loaderIDs) {
				// Find the sample file
				while ( rd->files[curFileIndex].fileSize == 0 && curFileIndex < rd->files.size()) {
					// NOTE: && rd->files[curFileIndex].cursor >= rd->files[curFileIndex].fileSize
					printf("[Sampling] File %d:%s filesize:%d skip the file\n", curFileIndex,
							rd->files[curFileIndex].fileName.c_str(), rd->files[curFileIndex].fileSize);
					curFileOffset = 0;
					curFileIndex++;
				}
				// Find the next sample point
				while ( loadSizeB / sampleB < sampleIndex && curFileIndex < rd->files.size() ) {
					if (rd->files[curFileIndex].fileSize == 0) {
						// NOTE: && rd->files[curFileIndex].cursor >= rd->files[curFileIndex].fileSize
						printf("[Sampling] File %d:%s filesize:%d skip the file\n", curFileIndex,
								rd->files[curFileIndex].fileName.c_str(), rd->files[curFileIndex].fileSize);
						curFileIndex++;
						curFileOffset = 0;
						continue;
					}
					if ( loadSizeB / sampleB >= sampleIndex ) {
						break;
					}
					if (curFileIndex >= rd->files.size()) {
						break;
					}
					loadSizeB += std::min( rd->files[curFileIndex].blockSize, std::max(rd->files[curFileIndex].fileSize - curFileOffset * rd->files[curFileIndex].blockSize, (int64_t) 0) );
					curFileOffset++;
					if ( rd->files[curFileIndex].blockSize == 0 || curFileOffset >= rd->files[curFileIndex].fileSize / rd->files[curFileIndex].blockSize ) {
						curFileOffset = 0;
						curFileIndex++;
					}
				}
				if ( curFileIndex >= rd->files.size() ) {
					allLoadReqsSent = true;
					break;
				}

				//sampleIndex++;

				// Notify loader to sample the file
				LoadingParam param;
				param.url = request.url;
				param.version = rd->files[curFileIndex].version;
				param.filename = rd->files[curFileIndex].fileName;
				param.offset = curFileOffset * rd->files[curFileIndex].blockSize; // The file offset in bytes
				//param.length = std::min(rd->files[curFileIndex].fileSize - rd->files[curFileIndex].cursor, loadSizeB);
				param.length = std::min(rd->files[curFileIndex].blockSize, std::max((int64_t)0, rd->files[curFileIndex].fileSize - param.offset));
				loadSizeB += param.length;
				sampleIndex = std::ceil(loadSizeB / sampleB);
				curFileOffset++;

				//loadSizeB = param.length;
				param.blockSize = rd->files[curFileIndex].blockSize;
				param.restoreRange = restoreRange;
				param.addPrefix = addPrefix;
				param.removePrefix = removePrefix;
				param.mutationLogPrefix = mutationLogPrefix;
				if ( !(param.length > 0  &&  param.offset >= 0 && param.offset < rd->files[curFileIndex].fileSize) ) {
					printf("[ERROR] param: length:%d offset:%d fileSize:%d for %dth file:%s\n",
							param.length, param.offset, rd->files[curFileIndex].fileSize, curFileIndex,
							rd->files[curFileIndex].toString().c_str());
				}


				printf("[Sampling][File:%d] filename:%s offset:%d blockSize:%d filesize:%d loadSize:%dB sampleIndex:%d\n",
						curFileIndex, rd->files[curFileIndex].fileName.c_str(), curFileOffset,
						rd->files[curFileIndex].blockSize, rd->files[curFileIndex].fileSize,
						loadSizeB, sampleIndex);


				ASSERT( param.length > 0 );
				ASSERT( param.offset >= 0 );
				ASSERT( param.offset <= rd->files[curFileIndex].fileSize );
				UID nodeID = loaderID;

				ASSERT(rd->workers_interface.find(nodeID) != rd->workers_interface.end());
				RestoreCommandInterface& cmdInterf = rd->workers_interface[nodeID];
				printf("[Sampling][CMD] Node:%s Loading %s on node %s\n", rd->describeNode().c_str(), param.toString().c_str(), nodeID.toString().c_str());

				if (!rd->files[curFileIndex].isRange) {
					cmdType = RestoreCommandEnum::Sample_Log_File;
					rd->cmdID.setPhase(RestoreCommandEnum::Sample_Log_File);
				} else {
					cmdType = RestoreCommandEnum::Sample_Range_File;
					rd->cmdID.setPhase(RestoreCommandEnum::Sample_Range_File);
				}

				rd->cmdID.nextCmd(); // The cmd index is the i^th file (range or log file) to be processed
				printf("[Sampling] Master cmdType:%d cmdUID:%s isRange:%d destinationNode:%s\n", 
						(int) cmdType, rd->cmdID.toString().c_str(), (int) rd->files[curFileIndex].isRange,
						nodeID.toString().c_str());
				cmdReplies.push_back( cmdInterf.cmd.getReply(RestoreCommand(cmdType, rd->cmdID, nodeID, param)) );
				if (param.offset + param.length >= rd->files[curFileIndex].fileSize) { // Reach the end of the file
					curFileIndex++;
					curFileOffset = 0;
				}
				if ( curFileIndex >= rd->files.size() ) {
					allLoadReqsSent = true;
					break;
				}
				++loadingCmdIndex;
			}

			printf("[Sampling] Wait for %d loaders to accept the cmd Sample_Range_File or Sample_Log_File\n", cmdReplies.size());

			if ( !cmdReplies.empty() ) {
				std::vector<RestoreCommandReply> reps = wait( timeoutError( getAll(cmdReplies), FastRestore_Failure_Timeout ) ); //TODO: change to getAny. NOTE: need to keep the still-waiting replies

				finishedLoaderIDs.clear();
				for (int i = 0; i < reps.size(); ++i) {
					printf("[Sampling] Get reply:%s for  Sample_Range_File or Sample_Log_File\n",
							reps[i].toString().c_str());
					finishedLoaderIDs.push_back(reps[i].id);
					//int64_t repLoadingCmdIndex = reps[i].cmdIndex;
				}
				loaderIDs = finishedLoaderIDs;
				checkpointCMDUID = rd->cmdID;
				checkpointCurFileIndex = curFileIndex;
				checkpointCurFileOffset = curFileOffset;
			}

			if (allLoadReqsSent) {
				break; // NOTE: need to change when change to wait on any cmdReplies
			}

		} catch (Error &e) {
			// TODO: Handle the command reply timeout error
			if (e.code() != error_code_io_timeout) {
				fprintf(stderr, "[ERROR] Node:%s, Commands before cmdID:%s timeout.\n", rd->describeNode().c_str(), rd->cmdID.toString().c_str());
			} else {
				fprintf(stderr, "[ERROR] Node:%s, Commands before cmdID:%s error. error code:%d, error message:%s\n", rd->describeNode().c_str(),
						rd->cmdID.toString().c_str(), e.code(), e.what());
			}
			rd->cmdID = checkpointCMDUID;
			curFileIndex = checkpointCurFileIndex;
			curFileOffset = checkpointCurFileOffset;
			printf("[Sampling][Waring] Retry at CMDID:%s curFileIndex:%d\n", rd->cmdID.toString().c_str(), curFileIndex);
		}
	}

	// Step: Signal the end of sampling for loaders
	rd->cmdID.initPhase(RestoreCommandEnum::Sample_File_Done);
	loaderIDs = getLoaderIDs(rd); // Reset loaderIDs
	loop {
		try {
			cmdReplies.clear();
			for (auto &loaderID : loaderIDs) {
				UID nodeID = loaderID;

				ASSERT(rd->workers_interface.find(nodeID) != rd->workers_interface.end());
				RestoreCommandInterface& cmdInterf = rd->workers_interface[nodeID];
				printf("[Sampling][CMD] Node:%s Signal the end of sampling to node %s\n", rd->describeNode().c_str(), nodeID.toString().c_str());
				RestoreCommandEnum cmdType = RestoreCommandEnum::Sample_File_Done;

				rd->cmdID.nextCmd();
				cmdReplies.push_back( cmdInterf.cmd.getReply(RestoreCommand(cmdType, rd->cmdID, nodeID)) );
			}

			printf("[Sampling] Node:%s Wait for %d loaders to accept the cmd Sample_File_Done\n", rd->describeNode().c_str(),  cmdReplies.size());

			if ( !cmdReplies.empty() ) {
				std::vector<RestoreCommandReply> reps = wait( timeoutError( getAll(cmdReplies), FastRestore_Failure_Timeout ) ); //TODO: change to getAny. NOTE: need to keep the still-waiting replies

				for (int i = 0; i < reps.size(); ++i) {
					printf("[Sampling] Get reply:%s for Sample_File_Done\n",
							reps[i].toString().c_str());
				}
			}

			break;

		} catch (Error &e) {
			// TODO: Handle the command reply timeout error
			if (e.code() != error_code_io_timeout) {
				fprintf(stderr, "[ERROR] Node:%s, Commands before cmdID:%s timeout\n", rd->describeNode().c_str(), rd->cmdID.toString().c_str());
			} else {
				fprintf(stderr, "[ERROR] Node:%s, Commands before cmdID:%s error. error code:%d, error message:%s\n", rd->describeNode().c_str(),
						rd->cmdID.toString().c_str(), e.code(), e.what());
			}
			printf("[Sampling] [Warning] Retry on Sample_File_Done\n");
		}
	}

	printf("[Sampling][Master] Finish sampling the backup workload. Next: Ask the master applier for appliers key range boundaries.\n");

	// Notify master applier that all sampled mutations have been sent to it
	loop {
		try {
			cmdReplies.clear();
			ASSERT(rd->workers_interface.find(rd->masterApplier) != rd->workers_interface.end());
			RestoreCommandInterface& cmdInterf = rd->workers_interface[rd->masterApplier];
			rd->cmdID.initPhase(RestoreCommandEnum::Loader_Send_Sample_Mutation_To_Applier_Done);
			rd->cmdID.nextCmd();
			printf("[Sampling] Node:%s Signal master applier %s Loader_Send_Sample_Mutation_To_Applier_Done\n", rd->describeNode().c_str(), rd->masterApplier.toString().c_str());

			RestoreCommandReply rep = wait( timeoutError( cmdInterf.cmd.getReply(
					RestoreCommand(RestoreCommandEnum::Loader_Send_Sample_Mutation_To_Applier_Done, rd->cmdID, rd->masterApplier, applierIDs.size())),
					FastRestore_Failure_Timeout) );
			
			printf("[Sampling][CMDRep] Ack from master applier: %s  for Loader_Send_Sample_Mutation_To_Applier_Done\n",  rd->masterApplier.toString().c_str());
			break;
		} catch (Error &e) {
			// TODO: Handle the command reply timeout error
			if (e.code() != error_code_io_timeout) {
				fprintf(stderr, "[ERROR] Node:%s, Commands before cmdID:%s timeout\n", rd->describeNode().c_str(), rd->cmdID.toString().c_str());
			} else {
				fprintf(stderr, "[ERROR] Node:%s, Commands before cmdID:%s error. error code:%d, error message:%s\n", rd->describeNode().c_str(),
						rd->cmdID.toString().c_str(), e.code(), e.what());
			}
			printf("[Sampling] [Warning] Retry on Loader_Send_Sample_Mutation_To_Applier_Done\n");
		}
	}

	// Ask master applier to calculate the key ranges for appliers
	state int numKeyRanges = 0;
	loop {
		try {
			RestoreCommandInterface& cmdInterf = rd->workers_interface[rd->masterApplier];
			printf("[Sampling][CMD] Ask master applier %s for the key ranges for appliers\n", rd->masterApplier.toString().c_str());
			ASSERT(applierIDs.size() > 0);
			rd->cmdID.initPhase(RestoreCommandEnum::Calculate_Applier_KeyRange);
			rd->cmdID.nextCmd();
			RestoreCommandReply rep = wait( timeoutError(  cmdInterf.cmd.getReply(RestoreCommand(RestoreCommandEnum::Calculate_Applier_KeyRange, rd->cmdID, rd->masterApplier, applierIDs.size())),  FastRestore_Failure_Timeout) );
			printf("[Sampling][CMDRep] number of key ranges calculated by master applier:%d\n", rep.num);
			numKeyRanges = rep.num;

			if (numKeyRanges <= 0 || numKeyRanges >= applierIDs.size() ) {
				printf("[WARNING] Calculate_Applier_KeyRange receives wrong reply (numKeyRanges:%d) from other phases. applierIDs.size:%d Retry Calculate_Applier_KeyRange\n", numKeyRanges, applierIDs.size());
				continue;
			}

			if ( numKeyRanges < applierIDs.size() ) {
				printf("[WARNING][Sampling] numKeyRanges:%d < appliers number:%d. %d appliers will not be used!\n",
						numKeyRanges, applierIDs.size(), applierIDs.size() - numKeyRanges);
			}

			break;
		} catch (Error &e) {
			// TODO: Handle the command reply timeout error
			if (e.code() != error_code_io_timeout) {
				fprintf(stderr, "[ERROR] Node:%s, Commands before cmdID:%s timeout\n", rd->describeNode().c_str(), rd->cmdID.toString().c_str());
			} else {
				fprintf(stderr, "[ERROR] Node:%s, Commands before cmdID:%s error. error code:%d, error message:%s\n", rd->describeNode().c_str(),
						rd->cmdID.toString().c_str(), e.code(), e.what());
			}
			printf("[Sampling] [Warning] Retry on Calculate_Applier_KeyRange\n");
		}
	}

	// Ask master applier to return the key range for appliers
	loop {
		try {
			rd->cmdID.initPhase(RestoreCommandEnum::Get_Applier_KeyRange);
			rd->cmdID.nextCmd();
			for (int i = 0; i < applierIDs.size() && i < numKeyRanges; ++i) {
				UID applierID = applierIDs[i];
				rd->cmdID.nextCmd();
				printf("[Sampling][Master] Node:%s, CMDID:%s Ask masterApplier:%s for the lower boundary of the key range for applier:%s\n",
						rd->describeNode().c_str(), rd->cmdID.toString().c_str(),
						rd->masterApplier.toString().c_str(), applierID.toString().c_str());
				ASSERT(rd->workers_interface.find(rd->masterApplier) != rd->workers_interface.end());
				RestoreCommandInterface& masterApplierCmdInterf = rd->workers_interface[rd->masterApplier];
				cmdReplies.push_back( masterApplierCmdInterf.cmd.getReply(RestoreCommand(RestoreCommandEnum::Get_Applier_KeyRange, rd->cmdID, rd->masterApplier, i)) );
			}
			std::vector<RestoreCommandReply> reps = wait( timeoutError( getAll(cmdReplies), FastRestore_Failure_Timeout) );

			for (int i = 0; i < applierIDs.size() && i < numKeyRanges; ++i) {
				UID applierID = applierIDs[i];
				Standalone<KeyRef> lowerBound;
				if (i < numKeyRanges) {
					lowerBound = reps[i].lowerBound;
				} else {
					lowerBound = normalKeys.end;
				}

				if (i == 0) {
					lowerBound = LiteralStringRef("\x00"); // The first interval must starts with the smallest possible key
				}
				printf("[INFO] Node:%s Assign key-to-applier map: Key:%s -> applierID:%s\n", rd->describeNode().c_str(),
						getHexString(lowerBound).c_str(), applierID.toString().c_str());
				rd->range2Applier.insert(std::make_pair(lowerBound, applierID));
			}

			break;
		} catch (Error &e) {
			// TODO: Handle the command reply timeout error
			if (e.code() != error_code_io_timeout) {
				fprintf(stderr, "[ERROR] Node:%s, Commands before cmdID:%s timeout\n", rd->describeNode().c_str(), rd->cmdID.toString().c_str());
			} else {
				fprintf(stderr, "[ERROR] Node:%s, Commands before cmdID:%s error. error code:%d, error message:%s\n", rd->describeNode().c_str(),
						rd->cmdID.toString().c_str(), e.code(), e.what());
			}
			printf("[Sampling] [Warning] Retry on Get_Applier_KeyRange\n");
		}
	}

	// Notify master applier the end of sampling. 
	loop {
		try {
			rd->cmdID.initPhase(RestoreCommandEnum::Get_Applier_KeyRange_Done);
			rd->cmdID.nextCmd();
			printf("[Sampling] Node:%s Singal master applier the end of sampling\n", rd->describeNode().c_str());
			RestoreCommandInterface& cmdInterf = rd->workers_interface[rd->masterApplier];
			RestoreCommandReply rep = wait( timeoutError( cmdInterf.cmd.getReply(
					RestoreCommand(RestoreCommandEnum::Get_Applier_KeyRange_Done, rd->cmdID, rd->masterApplier, applierIDs.size())), FastRestore_Failure_Timeout) );
			printf("[Sampling] Node:%s master applier has acked the cmd Get_Applier_KeyRange_Done\n", rd->describeNode().c_str());

			break;
		} catch (Error &e) {
			// TODO: Handle the command reply timeout error
			if (e.code() != error_code_io_timeout) {
				fprintf(stderr, "[ERROR] Node:%s, Commands before cmdID:%s timeout\n", rd->describeNode().c_str(), rd->cmdID.toString().c_str());
			} else {
				fprintf(stderr, "[ERROR] Node:%s, Commands before cmdID:%s error. error code:%d, error message:%s\n", rd->describeNode().c_str(),
						rd->cmdID.toString().c_str(), e.code(), e.what());
			}
			printf("[Sampling] [Warning] Retry on Get_Applier_KeyRange_Done\n");
		}
	}

	return Void();

}

bool isBackupEmpty(Reference<RestoreData> rd) {
	for (int i = 0; i < rd->files.size(); ++i) {
		if (rd->files[i].fileSize > 0) {
			return false;
		}
	}
	return true;
}

// Distribution workload per version batch
ACTOR static Future<Void> distributeWorkloadPerVersionBatch(RestoreCommandInterface interf, Reference<RestoreData> rd, Database cx, RestoreRequest request, Reference<RestoreConfig> restoreConfig) {
	state Key tagName = request.tagName;
	state Key url = request.url;
	state bool waitForComplete = request.waitForComplete;
	state Version targetVersion = request.targetVersion;
	state bool verbose = request.verbose;
	state KeyRange restoreRange = request.range;
	state Key addPrefix = request.addPrefix;
	state Key removePrefix = request.removePrefix;
	state bool lockDB = request.lockDB;
	state UID randomUid = request.randomUid;
	state Key mutationLogPrefix = restoreConfig->mutationLogPrefix();

	if ( isBackupEmpty(rd) ) {
		printf("[WARNING] Node:%s distributeWorkloadPerVersionBatch() load an empty batch of backup. Print out the empty backup files info.\n", rd->describeNode().c_str());
		printBackupFilesInfo(rd);
		return Void();
	}

	printf("[INFO] Node:%s mutationLogPrefix:%s (hex value:%s)\n", rd->describeNode().c_str(), mutationLogPrefix.toString().c_str(), getHexString(mutationLogPrefix).c_str());

	// Determine the key range each applier is responsible for
	std::pair<int, int> numWorkers = getNumLoaderAndApplier(rd);
	int numLoaders = numWorkers.first;
	int numAppliers = numWorkers.second;
	ASSERT( rd->globalNodeStatus.size() > 0 );
	ASSERT( numLoaders > 0 );
	ASSERT( numAppliers > 0 );

	state int loadingSizeMB = 0; //numLoaders * 1000; //NOTE: We want to load the entire file in the first version, so we want to make this as large as possible
	int64_t sampleSizeMB = 0; //loadingSizeMB / 100; // Will be overwritten. The sampleSizeMB will be calculated based on the batch size

	state double startTimeSampling = now();
	// TODO: WiP Sample backup files to determine the key range for appliers
	wait( sampleWorkload(rd, request, restoreConfig, sampleSizeMB) );

	printf("------[Progress] distributeWorkloadPerVersionBatch sampling time:%.2f seconds------\n", now() - startTimeSampling);


	state double startTime = now();

	// Notify each applier about the key range it is responsible for, and notify appliers to be ready to receive data
	wait( assignKeyRangeToAppliers(rd, cx) );

	wait( notifyAppliersKeyRangeToLoader(rd, cx) );

	// Determine which backup data block (filename, offset, and length) each loader is responsible for and
	// Notify the loader about the data block and send the cmd to the loader to start loading the data
	// Wait for the ack from loader and repeats

	// Prepare the file's loading status
	for (int i = 0; i < rd->files.size(); ++i) {
		rd->files[i].cursor = 0;
	}

	// Send loading cmd to available loaders whenever loaders become available
	// NOTE: We must split the workload in the correct boundary:
	// For range file, it's the block boundary;
	// For log file, it is the version boundary.
	// This is because
	// (1) The set of mutations at a version may be encoded in multiple KV pairs in log files.
	// We need to concatenate the related KVs to a big KV before we can parse the value into a vector of mutations at that version
	// (2) The backuped KV are arranged in blocks in range file.
	// For simplicity, we distribute at the granularity of files for now.

	state int loadSizeB = loadingSizeMB * 1024 * 1024;
	state int loadingCmdIndex = 0;
	state bool allLoadReqsSent = false;
	state std::vector<UID> loaderIDs = getLoaderIDs(rd);
	state std::vector<UID> applierIDs;
	state std::vector<UID> finishedLoaderIDs = loaderIDs;


	state int checkpointCurFileIndex = 0;

	state int curFileIndex = 0; // The smallest index of the files that has not been FULLY loaded
	loop {
		try {
			if ( allLoadReqsSent ) {
				break; // All load requests have been handled
			}
			wait(delay(1.0));

			state std::vector<Future<RestoreCommandReply>> cmdReplies;
			printf("[INFO] Number of backup files:%d\n", rd->files.size());
			rd->cmdID.initPhase(RestoreCommandEnum::Assign_Loader_Log_File);
			for (auto &loaderID : loaderIDs) {
				while ( rd->files[curFileIndex].fileSize == 0 && curFileIndex < rd->files.size()) {
					// NOTE: && rd->files[curFileIndex].cursor >= rd->files[curFileIndex].fileSize
					printf("[INFO] File %d:%s filesize:%d skip the file\n", curFileIndex,
							rd->files[curFileIndex].fileName.c_str(), rd->files[curFileIndex].fileSize);
					curFileIndex++;
				}
				if ( curFileIndex >= rd->files.size() ) {
					allLoadReqsSent = true;
					break;
				}
				LoadingParam param;
				param.url = request.url;
				param.version = rd->files[curFileIndex].version;
				param.filename = rd->files[curFileIndex].fileName;
				param.offset = rd->files[curFileIndex].cursor;
				//param.length = std::min(rd->files[curFileIndex].fileSize - rd->files[curFileIndex].cursor, loadSizeB);
				param.length = rd->files[curFileIndex].fileSize;
				loadSizeB = param.length;
				param.blockSize = rd->files[curFileIndex].blockSize;
				param.restoreRange = restoreRange;
				param.addPrefix = addPrefix;
				param.removePrefix = removePrefix;
				param.mutationLogPrefix = mutationLogPrefix;
				if ( !(param.length > 0  &&  param.offset >= 0 && param.offset < rd->files[curFileIndex].fileSize) ) {
					printf("[ERROR] param: length:%d offset:%d fileSize:%d for %dth filename:%s\n",
							param.length, param.offset, rd->files[curFileIndex].fileSize, curFileIndex,
							rd->files[curFileIndex].fileName.c_str());
				}
				ASSERT( param.length > 0 );
				ASSERT( param.offset >= 0 );
				ASSERT( param.offset < rd->files[curFileIndex].fileSize );
				rd->files[curFileIndex].cursor = rd->files[curFileIndex].cursor +  param.length;
				UID nodeID = loaderID;
				// record the loading status
				LoadingStatus loadingStatus(rd->files[curFileIndex], param.offset, param.length, nodeID);
				rd->loadingStatus.insert(std::make_pair(loadingCmdIndex, loadingStatus));

				ASSERT(rd->workers_interface.find(nodeID) != rd->workers_interface.end());
				RestoreCommandInterface& cmdInterf = rd->workers_interface[nodeID];

				printf("[CMD] Loading fileIndex:%d fileInfo:%s loadingParam:%s on node %s\n",
						curFileIndex, rd->files[curFileIndex].toString().c_str(), 
						param.toString().c_str(), nodeID.toString().c_str()); // VERY USEFUL INFO

				RestoreCommandEnum cmdType = RestoreCommandEnum::Assign_Loader_Range_File;
				rd->cmdID.setPhase(RestoreCommandEnum::Assign_Loader_Range_File);
				if (!rd->files[curFileIndex].isRange) {
					cmdType = RestoreCommandEnum::Assign_Loader_Log_File;
					rd->cmdID.setPhase(RestoreCommandEnum::Assign_Loader_Log_File);
				}
				rd->cmdID.nextCmd();
				printf("[INFO] Node:%s CMDUID:%s cmdType:%d isRange:%d loaderNode:%s\n", rd->describeNode().c_str(), rd->cmdID.toString().c_str(),
						(int) cmdType, (int) rd->files[curFileIndex].isRange, nodeID.toString().c_str());
				cmdReplies.push_back( cmdInterf.cmd.getReply(RestoreCommand(cmdType, rd->cmdID, nodeID, param)) );
				if (param.length <= loadSizeB) { // Reach the end of the file
					ASSERT( rd->files[curFileIndex].cursor == rd->files[curFileIndex].fileSize );
					curFileIndex++;
				}
				if ( curFileIndex >= rd->files.size() ) {
					allLoadReqsSent = true;
					break;
				}
				++loadingCmdIndex; // Replaced by cmdUID
			}

			printf("[INFO] Wait for %d loaders to accept the cmd Assign_Loader_File\n", cmdReplies.size());

			// Question: How to set reps to different value based on cmdReplies.empty()?
			if ( !cmdReplies.empty() ) {
				std::vector<RestoreCommandReply> reps = wait( timeoutError( getAll(cmdReplies), FastRestore_Failure_Timeout ) ); //TODO: change to getAny. NOTE: need to keep the still-waiting replies

				finishedLoaderIDs.clear();
				for (int i = 0; i < reps.size(); ++i) {
					printf("[INFO] Get Ack reply:%s for Assign_Loader_File\n",
							reps[i].toString().c_str());
					finishedLoaderIDs.push_back(reps[i].id);
					//int64_t repLoadingCmdIndex = reps[i].cmdIndex;
					//rd->loadingStatus[repLoadingCmdIndex].state = LoadingState::Assigned;
				}
				loaderIDs = finishedLoaderIDs;
				checkpointCurFileIndex = curFileIndex; // Save the previous success point
			}

			// TODO: Let master print all nodes status. Note: We need a function to print out all nodes status

			if (allLoadReqsSent) {
				break; // NOTE: need to change when change to wait on any cmdReplies
			}

		} catch (Error &e) {
			// TODO: Handle the command reply timeout error
			if (e.code() != error_code_io_timeout) {
				fprintf(stderr, "[ERROR] Node:%s, Commands before cmdID:%s timeout\n", rd->describeNode().c_str(), rd->cmdID.toString().c_str());
			} else {
				fprintf(stderr, "[ERROR] Node:%s, Commands before cmdID:%s error. error code:%d, error message:%s\n", rd->describeNode().c_str(),
						rd->cmdID.toString().c_str(), e.code(), e.what());
			}
			curFileIndex = checkpointCurFileIndex;
		}
	}


	
	loop {
		try {
			// Notify loaders the end of the loading
			printf("[INFO][Master] Notify loaders the end of loading\n");
			loaderIDs = getLoaderIDs(rd);
			cmdReplies.clear();
			rd->cmdID.initPhase(RestoreCommandEnum::Assign_Loader_File_Done);
			for (auto& loaderID : loaderIDs) {
				UID nodeID = loaderID;
				RestoreCommandInterface& cmdInterf = rd->workers_interface[nodeID];
				printf("[CMD] Assign_Loader_File_Done for node ID:%s\n", nodeID.toString().c_str());
				rd->cmdID.nextCmd();
				cmdReplies.push_back( cmdInterf.cmd.getReply(RestoreCommand(RestoreCommandEnum::Assign_Loader_File_Done, rd->cmdID, nodeID)) );
			}
			std::vector<RestoreCommandReply> reps = wait( timeoutError( getAll(cmdReplies), FastRestore_Failure_Timeout) );
			for (int i = 0; i < reps.size(); ++i) {
				printf("[INFO] Node:%s CMDUID:%s Get reply:%s for Assign_Loader_File_Done\n",
						rd->describeNode().c_str(), reps[i].cmdID.toString().c_str(),
						reps[i].toString().c_str());
			}

			break;
		} catch (Error &e) {
			// TODO: Handle the command reply timeout error
			if (e.code() != error_code_io_timeout) {
				fprintf(stderr, "[ERROR] Node:%s, Commands before cmdID:%s timeout\n", rd->describeNode().c_str(), rd->cmdID.toString().c_str());
			} else {
				fprintf(stderr, "[ERROR] Node:%s, Commands before cmdID:%s error. error code:%d, error message:%s\n", rd->describeNode().c_str(),
						rd->cmdID.toString().c_str(), e.code(), e.what());
			}
			printf("Retry notifying loaders the end of loading ");
		}
	}

	loop {
		try {
			// Notify appliers the end of the loading
			printf("[INFO][Master] Notify appliers the end of loading\n");
			//applierIDs = getApplierIDs(rd);
			// Only the appliers that are responsible for a key range should be sent result
			applierIDs = rd->getBusyAppliers();
			cmdReplies.clear();
			rd->cmdID.initPhase(RestoreCommandEnum::Loader_Send_Mutations_To_Applier_Done);
			for (auto& id : applierIDs) {
				UID nodeID = id;
				RestoreCommandInterface& cmdInterf = rd->workers_interface[nodeID];
				rd->cmdID.nextCmd();
				printf("[CMD] Loader_Send_Mutations_To_Applier_Done for node ID:%s\n", nodeID.toString().c_str());
				cmdReplies.push_back( cmdInterf.cmd.getReply(RestoreCommand(RestoreCommandEnum::Loader_Send_Mutations_To_Applier_Done, rd->cmdID, nodeID)) );
			}
			std::vector<RestoreCommandReply> reps = wait( timeoutError( getAll(cmdReplies), FastRestore_Failure_Timeout) );
			for (int i = 0; i < reps.size(); ++i) {
				printf("[INFO] Node:%s CMDUID:%s Get reply:%s for Loader_Send_Mutations_To_Applier_Done\n",
						rd->describeNode().c_str(), reps[i].cmdID.toString().c_str(),
						reps[i].toString().c_str());
			}

			break;
		} catch (Error &e) {
			// TODO: Handle the command reply timeout error
			if (e.code() != error_code_io_timeout) {
				fprintf(stderr, "[ERROR] Node:%s, Commands before cmdID:%s timeout\n", rd->describeNode().c_str(), rd->cmdID.toString().c_str());
			} else {
				fprintf(stderr, "[ERROR] Node:%s, Commands before cmdID:%s error. error code:%d, error message:%s\n", rd->describeNode().c_str(),
						rd->cmdID.toString().c_str(), e.code(), e.what());
			}
			printf("Retry notifying appliers the end of loading ");
		}
	}

	// Notify the applier to applly mutation to DB
	wait( notifyApplierToApplyMutations(rd) );

	state double endTime = now();

	double runningTime = endTime - startTime;
	printf("------[Progress] Node:%s distributeWorkloadPerVersionBatch runningTime without sampling time:%.2f seconds, with sampling time:%.2f seconds------\n",
			rd->describeNode().c_str(),
			runningTime, endTime - startTimeSampling);

	return Void();

}

// loadingHandler: Loader will load file from blob and send mutations directly to appliers
// It is the command executor for master, and also the command initializer for applier
ACTOR Future<Void> loadingHandler(Reference<RestoreData> rd, RestoreCommandInterface interf, RestoreCommandInterface leaderInter) {
	printf("[INFO] Worker Node:%s starts loadingHandler\n", rd->describeNode().c_str());

	state LoadingParam param;
	state int64_t beginBlock = 0;
	state int64_t j = 0;
	state int64_t readLen = 0;
	state int64_t readOffset = 0;
	state Reference<IBackupContainer> bc;
	loop {
		try {
			choose {
				when(state RestoreCommand req = waitNext(interf.cmd.getFuture())) {
					printf("Node:%s Got Restore Command: CMDUID:%s\n",
							rd->describeNode().c_str(), req.cmdID.toString().c_str());
					if ( interf.id() != req.id ) {
							printf("[WARNING] node:%s receive request with a different id:%s\n",
								rd->describeNode().c_str(), req.id.toString().c_str());
					}

					param = req.loadingParam;
					beginBlock = 0;
					j = 0;
					readLen = 0;
					readOffset = 0;
					readOffset = param.offset;
					if ( req.cmd == RestoreCommandEnum::Assign_Loader_Range_File ) {
						printf("[INFO][Loader] Node:%s, CMDUID:%s Execute: Assign_Loader_Range_File, role: %s, loading param:%s\n",
								rd->describeNode().c_str(), req.cmdID.toString().c_str(),
								getRoleStr(rd->localNodeStatus.role).c_str(),
								param.toString().c_str());
						ASSERT(req.cmd == (RestoreCommandEnum) req.cmdID.phase); // NOTE: Very useful to catch subtle bugs that cause inconsistent restored data!

						//Note: handle duplicate message delivery
						if (rd->processedFiles.find(param.filename) != rd->processedFiles.end()) {
							printf("[WARNING]Node:%s, CMDUID:%s file:%s is delivered more than once! Reply directly without loading the file\n",
									rd->describeNode().c_str(), req.cmdID.toString().c_str(),
									param.filename.c_str());
							req.reply.send(RestoreCommandReply(interf.id(),req.cmdID));
							continue;
						}

						bc = IBackupContainer::openContainer(param.url.toString());
						printf("[INFO] Node:%s CMDUID:%s open backup container for url:%s\n",
								rd->describeNode().c_str(), req.cmdID.toString().c_str(),
								param.url.toString().c_str());


						rd->kvOps.clear(); //Clear kvOps so that kvOps only hold mutations for the current data block. We will send all mutations in kvOps to applier
						rd->mutationMap.clear();
						rd->mutationPartMap.clear();

						ASSERT( param.blockSize > 0 );
						//state std::vector<Future<Void>> fileParserFutures;
						if (param.offset % param.blockSize != 0) {
							printf("[WARNING] Parse file not at block boundary! param.offset:%ld param.blocksize:%ld, remainder\n",param.offset, param.blockSize, param.offset % param.blockSize);
						}
						for (j = param.offset; j < param.length; j += param.blockSize) {
							readOffset = j;
							readLen = std::min<int64_t>(param.blockSize, param.length - j);
							wait( _parseRangeFileToMutationsOnLoader(rd, bc, param.version, param.filename, readOffset, readLen, param.restoreRange, param.addPrefix, param.removePrefix) );
							++beginBlock;
						}

						printf("[INFO][Loader] Node:%s CMDUID:%s finishes process Range file:%s\n",
								rd->describeNode().c_str(), rd->cmdID.toString().c_str(),
								param.filename.c_str());
						// TODO: Send to applier to apply the mutations
						printf("[INFO][Loader] Node:%s CMDUID:%s will send range mutations to applier\n",
								rd->describeNode().c_str(), rd->cmdID.toString().c_str());
						wait( registerMutationsToApplier(rd) ); // Send the parsed mutation to applier who will apply the mutation to DB

						//Send ack to master that loader has finished loading the data
						req.reply.send(RestoreCommandReply(interf.id(), req.cmdID));
						rd->processedFiles[param.filename] =  1;
						rd->processedCmd[req.cmdID] = 1;
					} else if (req.cmd == RestoreCommandEnum::Assign_Loader_Log_File) {
						printf("[INFO][Loader] Node:%s CMDUID:%s Assign_Loader_Log_File Node: %s, role: %s, loading param:%s\n",
								rd->describeNode().c_str(), req.cmdID.toString().c_str(),
								getRoleStr(rd->localNodeStatus.role).c_str(),
								param.toString().c_str());
						ASSERT(req.cmd == (RestoreCommandEnum) req.cmdID.phase);

						//Note: handle duplicate message delivery
						if (rd->processedFiles.find(param.filename) != rd->processedFiles.end()) {
							printf("[WARNING] Node:%s CMDUID file:%s is delivered more than once! Reply directly without loading the file\n",
									rd->describeNode().c_str(), req.cmdID.toString().c_str(),
									param.filename.c_str());
							req.reply.send(RestoreCommandReply(interf.id(), req.cmdID));
							continue;
						}

						bc = IBackupContainer::openContainer(param.url.toString());
						printf("[INFO][Loader] Node:%s CMDUID:%s open backup container for url:%s\n",
								rd->describeNode().c_str(), req.cmdID.toString().c_str(),
								param.url.toString().c_str());
						printf("[INFO][Loader] Node:%s CMDUID:%s filename:%s blockSize:%d\n",
								rd->describeNode().c_str(), req.cmdID.toString().c_str(),
								param.filename.c_str(), param.blockSize);

						rd->kvOps.clear(); //Clear kvOps so that kvOps only hold mutations for the current data block. We will send all mutations in kvOps to applier
						rd->mutationMap.clear();
						rd->mutationPartMap.clear();

						ASSERT( param.blockSize > 0 );
						//state std::vector<Future<Void>> fileParserFutures;
						if (param.offset % param.blockSize != 0) {
							printf("[WARNING] Parse file not at block boundary! param.offset:%ld param.blocksize:%ld, remainder\n",param.offset, param.blockSize, param.offset % param.blockSize);
						}
						for (j = param.offset; j < param.length; j += param.blockSize) {
							readOffset = j;
							readLen = std::min<int64_t>(param.blockSize, param.length - j);
							// NOTE: Log file holds set of blocks of data. We need to parse the data block by block and get the kv pair(version, serialized_mutations)
							// The set of mutations at the same version may be splitted into multiple kv pairs ACROSS multiple data blocks when the size of serialized_mutations is larger than 20000.
							wait( _parseLogFileToMutationsOnLoader(rd, bc, param.version, param.filename, readOffset, readLen, param.restoreRange, param.addPrefix, param.removePrefix, param.mutationLogPrefix) );
							++beginBlock;
						}
						printf("[INFO][Loader] Node:%s CMDUID:%s finishes parsing the data block into kv pairs (version, serialized_mutations) for file:%s\n",
								rd->describeNode().c_str(), req.cmdID.toString().c_str(),
								param.filename.c_str());
						parseSerializedMutation(rd, false);

						printf("[INFO][Loader] Node:%s CMDUID:%s finishes process Log file:%s\n",
								rd->describeNode().c_str(), req.cmdID.toString().c_str(),
								param.filename.c_str());
						printf("[INFO][Loader] Node:%s CMDUID:%s will send log mutations to applier\n",
								rd->describeNode().c_str(), req.cmdID.toString().c_str());
						wait( registerMutationsToApplier(rd) ); // Send the parsed mutation to applier who will apply the mutation to DB

						req.reply.send(RestoreCommandReply(interf.id(), req.cmdID)); // master node is waiting
						rd->processedFiles[param.filename] =  1;
						rd->processedCmd[req.cmdID] = 1;
					} else if (req.cmd == RestoreCommandEnum::Assign_Loader_File_Done) {
							printf("Node: %s CMDUID:%s,  loading param:%s\n",
								rd->describeNode().c_str(), req.cmdID.toString().c_str(),
								param.toString().c_str());
							ASSERT(req.cmd == (RestoreCommandEnum) req.cmdID.phase);

							req.reply.send(RestoreCommandReply(interf.id(), req.cmdID)); // master node is waiting
							printf("[INFO][Loader] Node: %s, CMDUID:%s role: %s, At the end of its functionality! Hang here to make sure master proceeds!\n",
								rd->describeNode().c_str(), req.cmdID.toString().c_str(),
								getRoleStr(rd->localNodeStatus.role).c_str());
							break;
					} else {
						if ( IsCmdInPreviousPhase(RestoreCommandEnum::Assign_Loader_File_Done, req.cmd) ) {
							logExpectedOldCmd(rd, RestoreCommandEnum::Assign_Loader_File_Done, req.cmd, req.cmdID);
							req.reply.send(RestoreCommandReply(interf.id(), req.cmdID)); // master node is waiting
						} else {
							logUnexpectedCmd(rd, RestoreCommandEnum::Assign_Loader_File_Done, req.cmd, req.cmdID);
						}
//						printf("[ERROR][Loader] Expecting command:%d, %d, %d. Receive unexpected restore command %d. Directly reply to master to avoid stucking master\n",
//								RestoreCommandEnum::Assign_Loader_Range_File, RestoreCommandEnum::Assign_Loader_Log_File, RestoreCommandEnum::Assign_Loader_File_Done, req.cmd);
						
					}
				}
			}

		} catch (Error &e) {
			// TODO: Handle the command reply timeout error
			if (e.code() != error_code_io_timeout) {
				fprintf(stderr, "[ERROR] Node:%s, Commands before cmdID:%s timeout\n", rd->describeNode().c_str(), rd->cmdID.toString().c_str());
			} else {
				fprintf(stderr, "[ERROR] Node:%s, Commands before cmdID:%s error. error code:%d, error message:%s\n", rd->describeNode().c_str(),
						rd->cmdID.toString().c_str(), e.code(), e.what());
			}
		}
		//wait(delay(1.0));
	}

	return Void();
}



// Loader: sample's loading handler
ACTOR Future<Void> sampleHandler(Reference<RestoreData> rd, RestoreCommandInterface interf, RestoreCommandInterface leaderInter) {
	printf("[sampleHandler] Worker Node:%s starts\n",
			rd->describeNode().c_str());

	loop {
		state LoadingParam param;
		state int64_t beginBlock = 0;
		state int64_t j = 0;
		state int64_t readLen = 0;
		state int64_t readOffset = 0;
		state Reference<IBackupContainer> bc;
		//wait(delay(1.0));
		choose {
			when(state RestoreCommand req = waitNext(interf.cmd.getFuture())) {
				printf("[INFO] Node:%s Got Restore Command: cmdID:%s.\n", rd->describeNode().c_str(), req.cmdID.toString().c_str());
				if ( interf.id() != req.id ) {
						printf("[WARNING] node:%s receive request with a different id:%s\n",
							rd->describeNode().c_str(), req.id.toString().c_str());
				}

				param = req.loadingParam;
				beginBlock = 0;
				j = 0;
				readLen = 0;
				readOffset = 0;
				readOffset = param.offset;
				if ( req.cmd == RestoreCommandEnum::Sample_Range_File ) {
					printf("[Sample_Range_File][Loader] Node: %s, loading param:%s\n",
							rd->describeNode().c_str(), param.toString().c_str());
					ASSERT(req.cmd == (RestoreCommandEnum) req.cmdID.phase);

					// Handle duplicate, assuming cmdUID is always unique for the same workload
					if ( rd->isCmdProcessed(req.cmdID) ) {
						printf("[DEBUG] NODE:%s skip duplicate cmd:%s\n", rd->describeNode().c_str(), req.cmdID.toString().c_str());
						req.reply.send(RestoreCommandReply(interf.id(), req.cmdID));
						continue;
					}

					// TODO: This can be expensive
					bc = IBackupContainer::openContainer(param.url.toString());
					printf("[INFO] node:%s open backup container for url:%s\n",
							rd->describeNode().c_str(),
							param.url.toString().c_str());


					rd->kvOps.clear(); //Clear kvOps so that kvOps only hold mutations for the current data block. We will send all mutations in kvOps to applier
					rd->mutationMap.clear();
					rd->mutationPartMap.clear();

					ASSERT( param.blockSize > 0 );
					//state std::vector<Future<Void>> fileParserFutures;
					if (param.offset % param.blockSize != 0) {
						printf("[WARNING] Parse file not at block boundary! param.offset:%ld param.blocksize:%ld, remainder\n",param.offset, param.blockSize, param.offset % param.blockSize);
					}

					ASSERT( param.offset + param.blockSize >= param.length ); // We only sample one data block or less (at the end of the file) of a file.
					for (j = param.offset; j < param.length; j += param.blockSize) {
						readOffset = j;
						readLen = std::min<int64_t>(param.blockSize, param.length - j);
						wait( _parseRangeFileToMutationsOnLoader(rd, bc, param.version, param.filename, readOffset, readLen, param.restoreRange, param.addPrefix, param.removePrefix) );
						++beginBlock;
					}

					printf("[Sampling][Loader] Node:%s finishes sample Range file:%s\n", rd->describeNode().c_str(), param.filename.c_str());
					// TODO: Send to applier to apply the mutations
					printf("[Sampling][Loader] Node:%s will send sampled mutations to applier\n", rd->describeNode().c_str());
					wait( registerMutationsToMasterApplier(rd) ); // Send the parsed mutation to applier who will apply the mutation to DB

					//rd->processedFiles.insert(std::make_pair(param.filename, 1));

					//TODO: Send ack to master that loader has finished loading the data
					req.reply.send(RestoreCommandReply(interf.id(), req.cmdID));
					rd->processedCmd[req.cmdID] = 1; // Record the processed comand to handle duplicate command
				} else if (req.cmd == RestoreCommandEnum::Sample_Log_File) {
					printf("[Sample_Log_File][Loader]  Node: %s, loading param:%s\n",
							rd->describeNode().c_str(), param.toString().c_str());
					ASSERT(req.cmd == (RestoreCommandEnum) req.cmdID.phase);

					// Handle duplicate message
					if ( rd->isCmdProcessed(req.cmdID) ) {
						printf("[DEBUG] NODE:%s skip duplicate cmd:%s\n", rd->describeNode().c_str(), req.cmdID.toString().c_str());
						req.reply.send(RestoreCommandReply(interf.id(), req.cmdID));
						continue;
					}

					// TODO: Expensive operation
					bc = IBackupContainer::openContainer(param.url.toString());
					printf("[Sampling][Loader] Node:%s open backup container for url:%s\n",
							rd->describeNode().c_str(),
							param.url.toString().c_str());
					printf("[Sampling][Loader] Node:%s filename:%s blockSize:%d\n",
							rd->describeNode().c_str(),
							param.filename.c_str(), param.blockSize);

					rd->kvOps.clear(); //Clear kvOps so that kvOps only hold mutations for the current data block. We will send all mutations in kvOps to applier
					rd->mutationMap.clear();
					rd->mutationPartMap.clear();

					ASSERT( param.blockSize > 0 );
					//state std::vector<Future<Void>> fileParserFutures;
					if (param.offset % param.blockSize != 0) {
						printf("[WARNING] Parse file not at block boundary! param.offset:%ld param.blocksize:%ld, remainder\n",param.offset, param.blockSize, param.offset % param.blockSize);
					}
					ASSERT( param.offset + param.blockSize >= param.length ); // Assumption: Only sample one data block or less
					for (j = param.offset; j < param.length; j += param.blockSize) {
						readOffset = j;
						readLen = std::min<int64_t>(param.blockSize, param.length - j);
						// NOTE: Log file holds set of blocks of data. We need to parse the data block by block and get the kv pair(version, serialized_mutations)
						// The set of mutations at the same version may be splitted into multiple kv pairs ACROSS multiple data blocks when the size of serialized_mutations is larger than 20000.
						wait( _parseLogFileToMutationsOnLoader(rd, bc, param.version, param.filename, readOffset, readLen, param.restoreRange, param.addPrefix, param.removePrefix, param.mutationLogPrefix) );
						++beginBlock;
					}
					printf("[Sampling][Loader] Node:%s finishes parsing the data block into kv pairs (version, serialized_mutations) for file:%s\n", rd->describeNode().c_str(), param.filename.c_str());
					parseSerializedMutation(rd, true);

					printf("[Sampling][Loader] Node:%s finishes process Log file:%s\n", rd->describeNode().c_str(), param.filename.c_str());
					printf("[Sampling][Loader] Node:%s will send log mutations to applier\n", rd->describeNode().c_str());
					wait( registerMutationsToMasterApplier(rd) ); // Send the parsed mutation to applier who will apply the mutation to DB

					req.reply.send(RestoreCommandReply(interf.id(), req.cmdID)); // master node is waiting
					rd->processedFiles.insert(std::make_pair(param.filename, 1));
					rd->processedCmd[req.cmdID] = 1;
				} else if (req.cmd == RestoreCommandEnum::Sample_File_Done) {
						printf("[Sampling][Loader] Node: %s, loading param:%s\n",
							rd->describeNode().c_str(), param.toString().c_str());
						ASSERT(req.cmd == (RestoreCommandEnum) req.cmdID.phase);

						req.reply.send(RestoreCommandReply(interf.id(), req.cmdID)); // master node is waiting
						printf("[Sampling][Loader] Node: %s, role: %s, At the end of sampling. Proceed to the next step!\n",
							rd->describeNode().c_str(),
							getRoleStr(rd->localNodeStatus.role).c_str());
						break; // Break the loop and return
				} else {
					if ( IsCmdInPreviousPhase(RestoreCommandEnum::Sample_File_Done, req.cmd) ) {
						logExpectedOldCmd(rd, RestoreCommandEnum::Sample_File_Done, req.cmd, req.cmdID);
						req.reply.send(RestoreCommandReply(interf.id(), req.cmdID)); // master node is waiting
					} else {
						logUnexpectedCmd(rd, RestoreCommandEnum::Sample_File_Done, req.cmd, req.cmdID);
					}
					//printf("[ERROR][Loader] Expecting command:%d, %d, %d. Receive unexpected restore command %d. Directly reply to master to avoid stucking master\n",
					//		RestoreCommandEnum::Assign_Loader_Range_File, RestoreCommandEnum::Assign_Loader_Log_File, RestoreCommandEnum::Assign_Loader_File_Done, req.cmd);
					// NOTE: For debug benefit, we let master block in case error
					//req.reply.send(RestoreCommandReply(interf.id(), req.cmdID)); // master node is waiting
				}
			}
		}
	}
	return Void();
}


ACTOR Future<Void> applyToDBHandler(Reference<RestoreData> rd, RestoreCommandInterface interf, RestoreCommandInterface leaderInter) {
	printf("[INFO] Worker Node:%s Role:%s starts applyToDBHandler\n",
			rd->describeNode().c_str(),
			getRoleStr(rd->localNodeStatus.role).c_str());
	
	loop {
		try {
			//wait(delay(1.0));
			choose {
				when(state RestoreCommand req = waitNext(interf.cmd.getFuture())) {
					printf("Node:%s Got Restore Command: cmdID:%d \n", rd->describeNode().c_str(),
							req.cmdID.toString().c_str());
					if ( interf.id() != req.id ) {
							printf("[WARNING] node:%s receive request with a different id:%s\n",
								rd->describeNode().c_str(), req.id.toString().c_str());
					}

					if (req.cmd == RestoreCommandEnum::Apply_Mutation_To_DB) {
							printf("Node: %s, role: %s, receive cmd Apply_Mutation_To_DB \n",
								rd->describeNode().c_str());
							ASSERT(req.cmd == (RestoreCommandEnum) req.cmdID.phase);

							if ( rd->isCmdProcessed(req.cmdID) ) {
								printf("[DEBUG] NODE:%s skip duplicate cmd:%s\n", rd->describeNode().c_str(), req.cmdID.toString().c_str());
								req.reply.send(RestoreCommandReply(interf.id(), req.cmdID));
								break;
							}

							wait( notifyApplierToApplyMutations(rd) );

							req.reply.send(RestoreCommandReply(interf.id(), req.cmdID)); // master node is waiting
							rd->processedCmd[req.cmdID] = 1;
							break;
					} else if (req.cmd == RestoreCommandEnum::Apply_Mutation_To_DB_Skip) {
						printf("Node: %s, role: %s, receive cmd Apply_Mutation_To_DB_Skip \n",
								rd->describeNode().c_str());
						ASSERT(req.cmd == (RestoreCommandEnum) req.cmdID.phase);

						req.reply.send(RestoreCommandReply(interf.id(), req.cmdID)); // master node is waiting
						break;
					} else {
						if (req.cmd == RestoreCommandEnum::Loader_Send_Mutations_To_Applier_Done) {
							ASSERT(req.cmd == (RestoreCommandEnum) req.cmdID.phase);
							req.reply.send(RestoreCommandReply(interf.id(), req.cmdID)); // master node is waiting
						} else {
							printf("[ERROR] applyToDBHandler() Restore command %d is invalid. Master will be stuck at configuring roles\n", req.cmd);
						}
					}
				}
			}
		} catch(Error &e) {
			if(e.code() != error_code_end_of_stream) {
				printf("[ERROR] cmd: Apply_Mutation_To_DB has error:%s(code:%d)\n", e.what(), e.code());
			}
		}
	}

	return Void();
}

void sanityCheckMutationOps(Reference<RestoreData> rd) {
	 //	printf("Now print KVOps\n");
	 //	printKVOps();

	 //	printf("Now sort KVOps in increasing order of commit version\n");
	 //	sort(kvOps.begin(), kvOps.end()); //sort in increasing order of key using default less_than comparator

	if ( isKVOpsSorted(rd) ) {
 		printf("[CORRECT] KVOps is sorted by version\n");
 	} else {
 		printf("[ERROR]!!! KVOps is NOT sorted by version\n");
 //		assert( 0 );
 	}

 	if ( allOpsAreKnown(rd) ) {
 		printf("[CORRECT] KVOps all operations are known.\n");
 	} else {
 		printf("[ERROR]!!! KVOps has unknown mutation op. Exit...\n");
 //		assert( 0 );
 	}
}

ACTOR Future<Void> sanityCheckRestoreOps(Reference<RestoreData> rd, Database cx, UID uid) {
	sanityCheckMutationOps(rd);

	state Reference<ReadYourWritesTransaction> tr(new ReadYourWritesTransaction(cx));
	tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
	tr->setOption(FDBTransactionOptions::LOCK_AWARE);

 	printf("Now apply KVOps to DB. start...\n");
 	printf("DB lock status:%d\n");
 	tr->reset();
 	wait(checkDatabaseLock(tr, uid));
	wait(tr->commit());

	return Void();

}

ACTOR Future<Void> applyRestoreOpsToDB(Reference<RestoreData> rd, Database cx) {
	//Apply the kv operations to DB
	wait( applyKVOpsToDB(rd, cx) );
	printf("Now apply KVOps to DB, Done\n");

	return Void();
}


//TODO: distribute every k MB backup data to loader to parse the data.
// Note: before let loader to send data to applier, notify applier to receive loader's data
// Also wait for the ACKs from all loaders and appliers that
// (1) loaders have parsed all backup data and send the mutations to applier, and
// (2) applier have received all mutations and are ready to apply them to DB


//TODO: Wait for applier to apply mutations to DB

//TODO: sanity check the status of loader and applier

//TODO: notify the user (or test workload) that restore has finished






////--- Functions for both loader and applier role



////--- Restore Functions for the loader role

////--- Restore Functions for the applier role



static Future<Version> processRestoreRequest(RestoreCommandInterface const &interf, Reference<RestoreData> const &rd, Database const &cx, RestoreRequest const &request);


ACTOR Future<Void> _restoreWorker(Database cx_input, LocalityData locality) {
	state Database cx = cx_input;
	state RestoreCommandInterface interf;
	interf.initEndpoints();
	state Optional<RestoreCommandInterface> leaderInterf;
	//Global data for the worker
	state Reference<RestoreData> rd = Reference<RestoreData>(new RestoreData());

	state Transaction tr(cx);
	loop {
		try {
			tr.reset();
			tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			tr.setOption(FDBTransactionOptions::LOCK_AWARE);
			Optional<Value> leader = wait(tr.get(restoreLeaderKey));
			if(leader.present()) {
				leaderInterf = BinaryReader::fromStringRef<RestoreCommandInterface>(leader.get(), IncludeVersion());
				// NOTE: Handle the situation that the leader's commit of its key causes error(commit_unknown_result)
				// In this situation, the leader will try to register its key again, which will never succeed.
				// We should let leader escape from the infinite loop
				if ( leaderInterf.get().id() == interf.id() ) {
					printf("[Worker] NodeID:%s is the leader and has registered its key in commit_unknown_result error. Let it set the key again\n",
							leaderInterf.get().id().toString().c_str());
					tr.set(restoreLeaderKey, BinaryWriter::toValue(interf, IncludeVersion()));
					wait(tr.commit());
					 // reset leaderInterf to invalid for the leader process
					 // because a process will not execute leader's logic unless leaderInterf is invalid
					leaderInterf = Optional<RestoreCommandInterface>();
					break;
				}
				printf("[Worker] Leader key exists:%s. Worker registers its restore interface id:%s\n",
						leaderInterf.get().id().toString().c_str(), interf.id().toString().c_str());
				tr.set(restoreWorkerKeyFor(interf.id()), restoreCommandInterfaceValue(interf));
				wait(tr.commit());
				break;
			}
			printf("[Worker] NodeID:%s tries to register its interface as leader\n", interf.id().toString().c_str());
			tr.set(restoreLeaderKey, BinaryWriter::toValue(interf, IncludeVersion()));
			wait(tr.commit());
			break;
		} catch( Error &e ) {
			// ATTENTION: We may have error commit_unknown_result, the commit may or may not succeed!
			// We must handle this error, otherwise, if the leader does not know its key has been registered, the leader will stuck here!
			printf("[INFO] NodeID:%s restoreWorker select leader error, error code:%d error info:%s\n",
					interf.id().toString().c_str(), e.code(), e.what());
			wait( tr.onError(e) );
		}
	}

	//we are not the leader, so put our interface in the agent list
	if(leaderInterf.present()) {
		// Step: configure its role
		printf("[INFO][Worker] NodeID:%s Configure its role\n", interf.id().toString().c_str());
		wait( configureRolesHandler(rd, interf));

		//TODO: Log restore status to DB

		printf("[INFO][Worker] NodeID:%s is configure to %s\n",
				rd->describeNode().c_str(), getRoleStr(rd->localNodeStatus.role).c_str());

		// Step: Find other worker's interfaces
		// NOTE: This must be after wait(configureRolesHandler()) because we must ensure all workers have registered their interfaces into DB before we can read the interface.
		wait( setWorkerInterface(rd, cx) );

		// Step: prepare restore info: applier waits for the responsible keyRange,
		// loader waits for the info of backup block it needs to load
		state int restoreBatch = 0;
		loop {
			printf("[Batch:%d] Node:%s Start...\n", restoreBatch, rd->describeNode().c_str());
			rd->resetPerVersionBatch();
			if ( rd->localNodeStatus.role == RestoreRole::Applier ) {
				if ( rd->masterApplier.toString() == rd->localNodeStatus.nodeID.toString() ) {
					printf("[Batch:%d][INFO][Master Applier] Node:%s Waits for the mutations from the sampled backup data\n", restoreBatch, rd->describeNode().c_str(), restoreBatch);
					wait(receiveSampledMutations(rd, interf));
					wait(calculateApplierKeyRange(rd, interf));
				}

				printf("[Batch:%d][INFO][Applier] Node:%s Waits for the assignment of key range\n", restoreBatch, rd->describeNode().c_str(), restoreBatch);
				wait( assignKeyRangeToAppliersHandler(rd, interf) );

				printf("[Batch:%d][INFO][Applier] Waits for the mutations parsed from loaders\n", restoreBatch);
				wait( receiveMutations(rd, interf) );

				printf("[Batch:%d][INFO][Applier] Waits for the cmd to apply mutations\n", restoreBatch);
				wait( applyMutationToDB(rd, interf, cx) );
			} else if ( rd->localNodeStatus.role == RestoreRole::Loader ) {
				printf("[Batch:%d][INFO][Loader] Waits to sample backup data\n", restoreBatch);
				wait( sampleHandler(rd, interf, leaderInterf.get()) );

				printf("[Batch:%d][INFO][Loader] Waits for appliers' key range\n", restoreBatch);
				wait( notifyAppliersKeyRangeToLoaderHandler(rd, interf) );
				printAppliersKeyRange(rd);

				printf("[Batch:%d][INFO][Loader] Waits for the backup file assignment after reset processedFiles\n", restoreBatch);
				rd->processedFiles.clear();
				wait( loadingHandler(rd, interf, leaderInterf.get()) );

				//printf("[INFO][Loader] Waits for the command to ask applier to apply mutations to DB\n");
				//wait( applyToDBHandler(rd, interf, leaderInterf.get()) );
			} else {
				printf("[Batch:%d][ERROR][Worker] In an invalid role:%d\n", restoreBatch, rd->localNodeStatus.role);
			}

			restoreBatch++;
		};

		// The workers' logic ends here. Should not proceed
//		printf("[INFO][Worker:%s] LocalNodeID:%s Role:%s will exit now\n", interf.id().toString().c_str(),
//				rd->describeNode().c_str(), getRoleStr(rd->localNodeStatus.role).c_str());
//		return Void();
	}

	//we are the leader
	// We must wait for enough time to make sure all restore workers have registered their interfaces into the DB
	printf("[INFO][Master] NodeID:%s Restore master waits for agents to register their workerKeys\n",
			interf.id().toString().c_str());
	wait( delay(10.0) );

	//state vector<RestoreInterface> agents;
	//state VectorRef<RestoreInterface> agents;

	rd->localNodeStatus.init(RestoreRole::Master);
	rd->localNodeStatus.nodeID = interf.id();
	printf("[INFO][Master]  NodeID:%s starts configuring roles for workers\n", interf.id().toString().c_str());
	wait( configureRoles(rd, cx) );

	state int restoreId = 0;
	state int checkNum = 0;
	loop {
		printf("Node:%s---Wait on restore requests...---\n", rd->describeNode().c_str());
		state Standalone<VectorRef<RestoreRequest>> restoreRequests = wait( collectRestoreRequests(cx) );

		printf("Node:%s ---Received  restore requests as follows---\n", rd->describeNode().c_str());
		// Print out the requests info
		for ( auto &it : restoreRequests ) {
			printf("\t[INFO][Master]Node:%s RestoreRequest info:%s\n", rd->describeNode().c_str(), it.toString().c_str());
		}

		// Step: Perform the restore requests
		for ( auto &it : restoreRequests ) {
			TraceEvent("LeaderGotRestoreRequest").detail("RestoreRequestInfo", it.toString());
			printf("Node:%s Got RestoreRequestInfo:%s\n", rd->describeNode().c_str(), it.toString().c_str());
			Version ver = wait( processRestoreRequest(interf, rd, cx, it) );
		}

		// Step: Notify all restore requests have been handled by cleaning up the restore keys
		wait( finishRestore(cx, restoreRequests) ); 

		printf("[INFO] MXRestoreEndHere RestoreID:%d\n", restoreId);
		TraceEvent("MXRestoreEndHere").detail("RestoreID", restoreId++);
		wait( delay(5.0) );
		//NOTE: we have to break the loop so that the tester.actor can receive the return of this test workload.
		//Otherwise, this special workload never returns and tester will think the test workload is stuck and the tester will timesout
		break; //TODO: this break will be removed later since we need the restore agent to run all the time!
	}

	return Void();
}

ACTOR Future<Void> restoreWorker(Reference<ClusterConnectionFile> ccf, LocalityData locality) {
	Database cx = Database::createDatabase(ccf->getFilename(), Database::API_VERSION_LATEST,locality);
	wait(_restoreWorker(cx, locality));
	return Void();
}

ACTOR static Future<Void> finishRestore(Database cx, Standalone<VectorRef<RestoreRequest>> restoreRequests) {
	state ReadYourWritesTransaction tr3(cx);
	loop {
		try {
			tr3.reset();
			tr3.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			tr3.setOption(FDBTransactionOptions::LOCK_AWARE);
			tr3.clear(restoreRequestTriggerKey);
			tr3.clear(restoreRequestKeys);
			tr3.set(restoreRequestDoneKey, restoreRequestDoneValue(restoreRequests.size()));
			wait(tr3.commit());
			TraceEvent("LeaderFinishRestoreRequest");
			printf("[INFO] RestoreLeader write restoreRequestDoneKey\n");

			break;
		}  catch( Error &e ) {
			TraceEvent("RestoreAgentLeaderErrorTr3").detail("ErrorCode", e.code()).detail("ErrorName", e.name());
			printf("[Error] RestoreLead operation on restoreRequestDoneKey, error:%s\n", e.what());
			wait( tr3.onError(e) );
		}
	};


	// TODO: Clean up the fields in restore data structure
 	//state RestoreConfig restore(task);
// 	state RestoreConfig restore(uid);
 //	restore.stateEnum().set(tr, ERestoreState::COMPLETED);
 	// Clear the file map now since it could be huge.
 //	restore.fileSet().clear(tr);

 	// TODO:  Validate that the range version map has exactly the restored ranges in it.  This means that for any restore operation
 	// the ranges to restore must be within the backed up ranges, otherwise from the restore perspective it will appear that some
 	// key ranges were missing and so the backup set is incomplete and the restore has failed.
 	// This validation cannot be done currently because Restore only supports a single restore range but backups can have many ranges.

 	// Clear the applyMutations stuff, including any unapplied mutations from versions beyond the restored version.
 //	restore.clearApplyMutationsKeys(tr);

	printf("[INFO] Notify the end of the restore\n");
	TraceEvent("NotifyRestoreFinished");

	return Void();
}

////--- Restore functions
ACTOR static Future<Void> unlockDB(Reference<ReadYourWritesTransaction> tr, UID uid) {
	 loop {
		try {
			tr->reset();
			tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			tr->setOption(FDBTransactionOptions::LOCK_AWARE);
			printf("CheckDBlock:%s START\n", uid.toString().c_str());
			wait(checkDatabaseLock(tr, uid));
			printf("CheckDBlock:%s DONE\n", uid.toString().c_str());

			printf("UnlockDB now. Start.\n");
			wait(unlockDatabase(tr, uid)); //NOTE: unlockDatabase didn't commit inside the function!

			printf("CheckDBlock:%s START\n", uid.toString().c_str());
			wait(checkDatabaseLock(tr, uid));
			printf("CheckDBlock:%s DONE\n", uid.toString().c_str());

			printf("UnlockDB now. Commit.\n");
			wait( tr->commit() );

			printf("UnlockDB now. Done.\n");
			break;
		} catch( Error &e ) {
			printf("Error when we unlockDB. Error:%s\n", e.what());
			wait(tr->onError(e));
		}
	 };

 	return Void();
 }

 struct FastRestoreStatus {
	double curWorkloadSize;
	double curRunningTime;
	double curSpeed;

	double totalWorkloadSize;
	double totalRunningTime;
	double totalSpeed;
};

int restoreStatusIndex = 0;
 ACTOR static Future<Void> registerStatus(Database cx, struct FastRestoreStatus status) {
 	state Reference<ReadYourWritesTransaction> tr(new ReadYourWritesTransaction(cx));
	loop {
		try {
			printf("[Restore_Status][%d] curWorkload:%.2f curRunningtime:%.2f curSpeed:%.2f totalWorkload:%.2f totalRunningTime:%.2f totalSpeed:%.2f\n",
					restoreStatusIndex, status.curWorkloadSize, status.curRunningTime, status.curSpeed, status.totalWorkloadSize, status.totalRunningTime, status.totalSpeed);

			tr->reset();
			tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			tr->setOption(FDBTransactionOptions::LOCK_AWARE);

			tr->set(restoreStatusKeyFor(StringRef(std::string("curWorkload") + std::to_string(restoreStatusIndex))), restoreStatusValue(status.curWorkloadSize));
			tr->set(restoreStatusKeyFor(StringRef(std::string("curRunningTime") + std::to_string(restoreStatusIndex))), restoreStatusValue(status.curRunningTime));
			tr->set(restoreStatusKeyFor(StringRef(std::string("curSpeed") + std::to_string(restoreStatusIndex))), restoreStatusValue(status.curSpeed));

			tr->set(restoreStatusKeyFor(StringRef(std::string("totalWorkload"))), restoreStatusValue(status.totalWorkloadSize));
			tr->set(restoreStatusKeyFor(StringRef(std::string("totalRunningTime"))), restoreStatusValue(status.totalRunningTime));
			tr->set(restoreStatusKeyFor(StringRef(std::string("totalSpeed"))), restoreStatusValue(status.totalSpeed));

			wait( tr->commit() );
			restoreStatusIndex++;

			break;
		} catch( Error &e ) {
			printf("Transaction Error when we registerStatus. Error:%s\n", e.what());
			wait(tr->onError(e));
		}
	 };

	return Void();
}


ACTOR static Future<Void> _lockDB(Database cx, UID uid, bool lockDB) {
	printf("[Lock] DB will be locked, uid:%s, lockDB:%d\n", uid.toString().c_str(), lockDB);
	
	ASSERT( lockDB );

	loop {
		try {
			wait(lockDatabase(cx, uid));
			break;
		} catch( Error &e ) {
			printf("Transaction Error when we lockDB. Error:%s\n", e.what());
			wait(tr->onError(e));
		}
	}

	state Reference<ReadYourWritesTransaction> tr(new ReadYourWritesTransaction(cx));
	loop {
		try {
			tr->reset();
			tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			tr->setOption(FDBTransactionOptions::LOCK_AWARE);

			wait(checkDatabaseLock(tr, uid));

			tr->commit();
			break;
		} catch( Error &e ) {
			printf("Transaction Error when we lockDB. Error:%s\n", e.what());
			wait(tr->onError(e));
		}
	}


	return Void();
}

// MXTODO: Change name to restoreProcessor()
ACTOR static Future<Version> processRestoreRequest(RestoreCommandInterface interf, Reference<RestoreData> rd, Database cx, RestoreRequest request) {
	state Key tagName = request.tagName;
	state Key url = request.url;
	state bool waitForComplete = request.waitForComplete;
	state Version targetVersion = request.targetVersion;
	state bool verbose = request.verbose;
	state KeyRange range = request.range;
	state Key addPrefix = request.addPrefix;
	state Key removePrefix = request.removePrefix;
	state bool lockDB = request.lockDB;
	state UID randomUid = request.randomUid;

	//MX: Lock DB if it is not locked
	printf("RestoreRequest lockDB:%d\n", lockDB);
	if ( lockDB == false ) {
		printf("[WARNING] RestoreRequest lockDB:%d; we will overwrite request.lockDB to true and forcely lock db\n", lockDB);
		lockDB = true;
		request.lockDB = true;
	}

	state long curBackupFilesBeginIndex = 0;
	state long curBackupFilesEndIndex = 0;

	state double totalWorkloadSize = 0;
	state double totalRunningTime = 0; // seconds
	state double curRunningTime = 0; // seconds
	state double curStartTime = 0;
	state double curEndTime = 0;
	state double curWorkloadSize = 0; //Bytes

	state double loadBatchSizeMB = 1.0;
	state double loadBatchSizeThresholdB = loadBatchSizeMB * 1024 * 1024;
	state int restoreBatchIndex = 0;
	state Reference<ReadYourWritesTransaction> tr(new ReadYourWritesTransaction(cx));
	state Reference<RestoreConfig> restoreConfig(new RestoreConfig(randomUid));

	// lock DB for restore
	wait( _lockDB(cx, randomUid, lockDB) );

	loop {
		try {
			tr->reset();
			tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			tr->setOption(FDBTransactionOptions::LOCK_AWARE);
			tr->clear(normalKeys);
			tr->commit();
			break;
		} catch(Error &e) {
			printf("[ERROR] At clean up DB before restore. error code:%d message:%s. Retry...\n", e.code(), e.what());
			if(e.code() != error_code_restore_duplicate_tag) {
				wait(tr->onError(e));
			}
		}
	}

	// Step: Collect all backup files
	loop {
		try {
			tr->reset();
			tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			tr->setOption(FDBTransactionOptions::LOCK_AWARE);

			printf("===========Restore request start!===========\n");
			state double startTime = now();
			wait( collectBackupFiles(rd, cx, request) );
			printf("[Perf] Node:%s collectBackupFiles takes %.2f seconds\n", rd->describeNode().c_str(), now() - startTime);
			constructFilesWithVersionRange(rd);
			

			// Sort the backup files based on end version.
			sort(rd->allFiles.begin(), rd->allFiles.end());
			printAllBackupFilesInfo(rd);

			buildForbiddenVersionRange(rd);
			printForbiddenVersionRange(rd);
			if ( isForbiddenVersionRangeOverlapped(rd) ) {
				printf("[ERROR] forbidden version ranges are overlapped! Check out the forbidden version range above\n");
				ASSERT( 0 );
			}

			break;
		} catch(Error &e) {
			printf("[ERROR] At collect all backup files. error code:%d message:%s. Retry...\n", e.code(), e.what());
			if(e.code() != error_code_restore_duplicate_tag) {
				wait(tr->onError(e));
			}
		}
	}

	loop {
		try {
			rd->files.clear();
			curWorkloadSize = 0;
			state Version endVersion = -1;
			state bool isRange = false;
			state bool validVersion = false;
			// Step: Find backup files in each version batch and restore them.
			while ( curBackupFilesBeginIndex < rd->allFiles.size() ) {
				// Find the curBackupFilesEndIndex, such that the to-be-loaded files size (curWorkloadSize) is as close to loadBatchSizeThresholdB as possible,
				// and curBackupFilesEndIndex must not belong to the forbidden version range!
				if ( curBackupFilesEndIndex < rd->allFiles.size() ) {
					endVersion =  rd->allFiles[curBackupFilesEndIndex].endVersion;
					isRange = rd->allFiles[curBackupFilesEndIndex].isRange;
					validVersion = !isVersionInForbiddenRange(rd, endVersion, isRange);
					curWorkloadSize += rd->allFiles[curBackupFilesEndIndex].fileSize;
					printf("[DEBUG][Batch:%d] Calculate backup files for a version batch: endVersion:%lld isRange:%d validVersion:%d curWorkloadSize:%.2fB curBackupFilesBeginIndex:%d curBackupFilesEndIndex:%d, files.size:%d\n",
						restoreBatchIndex, endVersion, isRange, validVersion, curWorkloadSize, curBackupFilesBeginIndex, curBackupFilesEndIndex, rd->allFiles.size());
				}
				if ( (validVersion && curWorkloadSize >= loadBatchSizeThresholdB) || curBackupFilesEndIndex >= rd->allFiles.size() )  {
					if ( curBackupFilesEndIndex >= rd->allFiles.size() && curWorkloadSize <= 0 ) {
						printf("Restore finishes: curBackupFilesEndIndex:%d, allFiles.size:%d, curWorkloadSize:%.2f\n",
								curBackupFilesEndIndex, rd->allFiles.size(), curWorkloadSize);
						break;
					}
					//TODO: Construct the files [curBackupFilesBeginIndex, curBackupFilesEndIndex]
					rd->files.clear();
					rd->resetPerVersionBatch();
					if ( curBackupFilesBeginIndex < rd->allFiles.size()) {
						for (int fileIndex = curBackupFilesBeginIndex; fileIndex <= curBackupFilesEndIndex && fileIndex < rd->allFiles.size(); fileIndex++) {
							rd->files.push_back(rd->allFiles[fileIndex]);
						}
					}
					printBackupFilesInfo(rd);

					curStartTime = now();

					printf("------[Progress] Node:%s, restoreBatchIndex:%d, curWorkloadSize:%.2f------\n", rd->describeNode().c_str(), restoreBatchIndex, curWorkloadSize);
					rd->resetPerVersionBatch();
					rd->cmdID.setBatch(restoreBatchIndex);
					wait( distributeWorkloadPerVersionBatch(interf, rd, cx, request, restoreConfig) );

					curEndTime = now();
					curRunningTime = curEndTime - curStartTime;
					ASSERT(curRunningTime >= 0);
					totalRunningTime += curRunningTime;
					totalWorkloadSize += curWorkloadSize;

					struct FastRestoreStatus status;
					status.curRunningTime = curRunningTime;
					status.curWorkloadSize = curWorkloadSize;
					status.curSpeed = curWorkloadSize /  curRunningTime;
					status.totalRunningTime = totalRunningTime;
					status.totalWorkloadSize = totalWorkloadSize;
					status.totalSpeed = totalWorkloadSize / totalRunningTime;

					printf("------[Progress] restoreBatchIndex:%d, curWorkloadSize:%.2f B, curWorkload:%.2f B curRunningtime:%.2f s curSpeed:%.2f B/s  totalWorkload:%.2f B totalRunningTime:%.2f s totalSpeed:%.2f B/s\n",
							restoreBatchIndex, curWorkloadSize,
							status.curWorkloadSize, status.curRunningTime, status.curSpeed, status.totalWorkloadSize, status.totalRunningTime, status.totalSpeed);

					wait( registerStatus(cx, status) );

					curBackupFilesBeginIndex = curBackupFilesEndIndex + 1;
					curBackupFilesEndIndex++;
					curWorkloadSize = 0;
					restoreBatchIndex++;
				} else if (validVersion && curWorkloadSize < loadBatchSizeThresholdB) {
					curBackupFilesEndIndex++;
				} else if (!validVersion && curWorkloadSize < loadBatchSizeThresholdB) {
					curBackupFilesEndIndex++;
				} else if (!validVersion && curWorkloadSize >= loadBatchSizeThresholdB) {
					// Now: just move to the next file. We will eventually find a valid version but load more than loadBatchSizeThresholdB
					printf("[WARNING] The loading batch size will be larger than expected! curBatchSize:%.2fB, expectedBatchSize:%2.fB, endVersion:%lld\n",
							curWorkloadSize, loadBatchSizeThresholdB, endVersion);
					curBackupFilesEndIndex++;
					//TODO: Roll back to find a valid version
				} else {
					ASSERT( 0 ); // Never happend!
				}
			}


			printf("Finish my restore now!\n");
			// MX: Unlock DB after restore
			state Reference<ReadYourWritesTransaction> tr_unlockDB(new ReadYourWritesTransaction(cx));
			printf("Finish restore cleanup. Start\n");
			wait( unlockDB(tr_unlockDB, randomUid) );
			printf("Finish restore cleanup. Done\n");

			TraceEvent("ProcessRestoreRequest").detail("UnlockDB", "Done");

			break;
		} catch(Error &e) {
			fprintf(stderr, "ERROR: Stop at Error when we process version batch at the top level. error:%s\n", e.what());
			if(e.code() != error_code_restore_duplicate_tag) {
				wait(tr->onError(e));
			}
			break;
		}
	}

	return targetVersion;
}

//-------Helper functions
std::string getHexString(StringRef input) {
	std::stringstream ss;
	for (int i = 0; i<input.size(); i++) {
		if ( i % 4 == 0 )
			ss << " ";
		if ( i == 12 ) { //The end of 12bytes, which is the version size for value
			ss << "|";
		}
		if ( i == (12 + 12) ) { //The end of version + header
			ss << "@";
		}
		ss << std::setfill('0') << std::setw(2) << std::hex << (int) input[i]; // [] operator moves the pointer in step of unit8
	}
	return ss.str();
}

std::string getHexKey(StringRef input, int skip) {
	std::stringstream ss;
	for (int i = 0; i<skip; i++) {
		if ( i % 4 == 0 )
			ss << " ";
		ss << std::setfill('0') << std::setw(2) << std::hex << (int) input[i]; // [] operator moves the pointer in step of unit8
	}
	ss << "||";

	//hashvalue
	ss << std::setfill('0') << std::setw(2) << std::hex << (int) input[skip]; // [] operator moves the pointer in step of unit8
	ss << "|";

	// commitversion in 64bit
	int count = 0;
	for (int i = skip+1; i<input.size() && i < skip+1+8; i++) {
		if ( count++ % 4 == 0 )
			ss << " ";
		ss << std::setfill('0') << std::setw(2) << std::hex << (int) input[i]; // [] operator moves the pointer in step of unit8
	}
	// part value
	count = 0;
	for (int i = skip+1+8; i<input.size(); i++) {
		if ( count++ % 4 == 0 )
			ss << " ";
		ss << std::setfill('0') << std::setw(2) << std::hex << (int) input[i]; // [] operator moves the pointer in step of unit8
	}
	return ss.str();
}


void printMutationListRefHex(MutationListRef m, std::string prefix) {
	MutationListRef::Iterator iter = m.begin();
	for ( ;iter != m.end(); ++iter) {
		printf("%s mType:%04x param1:%s param2:%s param1_size:%d, param2_size:%d\n", prefix.c_str(), iter->type,
			   getHexString(iter->param1).c_str(), getHexString(iter->param2).c_str(), iter->param1.size(), iter->param2.size());
	}
}

//TODO: Print out the backup mutation log value. The backup log value (i.e., the value in the kv pair) has the following format
//version(12B)|mutationRef|MutationRef|....
//A mutationRef has the format: |type_4B|param1_size_4B|param2_size_4B|param1|param2.
//Note: The data is stored in little endian! You need to convert it to BigEndian so that you know how long the param1 and param2 is and how to format them!
void printBackupMutationRefValueHex(Standalone<StringRef> val_input, std::string prefix) {
	std::stringstream ss;
	const int version_size = 12;
	const int header_size = 12;
	StringRef val = val_input.contents();
	StringRefReaderMX reader(val, restore_corrupted_data());

	int count_size = 0;
	// Get the version
	uint64_t version = reader.consume<uint64_t>();
	count_size += 8;
	uint32_t val_length_decode = reader.consume<uint32_t>();
	count_size += 4;

	printf("----------------------------------------------------------\n");
	printf("To decode value:%s\n", getHexString(val).c_str());
	if ( val_length_decode != (val.size() - 12) ) {
		fprintf(stderr, "%s[PARSE ERROR]!!! val_length_decode:%d != val.size:%d\n", prefix.c_str(), val_length_decode, val.size());
	} else {
		if ( debug_verbose ) {
			printf("%s[PARSE SUCCESS] val_length_decode:%d == (val.size:%d - 12)\n", prefix.c_str(), val_length_decode, val.size());
		}
	}

	// Get the mutation header
	while (1) {
		// stop when reach the end of the string
		if(reader.eof() ) { //|| *reader.rptr == 0xFFCheckRestoreRequestDoneErrorMX
			//printf("Finish decode the value\n");
			break;
		}


		uint32_t type = reader.consume<uint32_t>();//reader.consumeNetworkUInt32();
		uint32_t kLen = reader.consume<uint32_t>();//reader.consumeNetworkUInt32();
		uint32_t vLen = reader.consume<uint32_t>();//reader.consumeNetworkUInt32();
		const uint8_t *k = reader.consume(kLen);
		const uint8_t *v = reader.consume(vLen);
		count_size += 4 * 3 + kLen + vLen;

		if ( kLen < 0 || kLen > val.size() || vLen < 0 || vLen > val.size() ) {
			fprintf(stderr, "%s[PARSE ERROR]!!!! kLen:%d(0x%04x) vLen:%d(0x%04x)\n", prefix.c_str(), kLen, kLen, vLen, vLen);
		}

		if ( debug_verbose ) {
			printf("%s---DedodeBackupMutation: Type:%d K:%s V:%s k_size:%d v_size:%d\n", prefix.c_str(),
				   type,  getHexString(KeyRef(k, kLen)).c_str(), getHexString(KeyRef(v, vLen)).c_str(), kLen, vLen);
		}

	}
	if ( debug_verbose ) {
		printf("----------------------------------------------------------\n");
	}
}

void printBackupLogKeyHex(Standalone<StringRef> key_input, std::string prefix) {
	std::stringstream ss;
	const int version_size = 12;
	const int header_size = 12;
	StringRef val = key_input.contents();
	StringRefReaderMX reader(val, restore_corrupted_data());

	int count_size = 0;
	// Get the version
	uint64_t version = reader.consume<uint64_t>();
	count_size += 8;
	uint32_t val_length_decode = reader.consume<uint32_t>();
	count_size += 4;

	printf("----------------------------------------------------------\n");
	printf("To decode value:%s\n", getHexString(val).c_str());
	if ( val_length_decode != (val.size() - 12) ) {
		fprintf(stderr, "%s[PARSE ERROR]!!! val_length_decode:%d != val.size:%d\n", prefix.c_str(), val_length_decode, val.size());
	} else {
		printf("%s[PARSE SUCCESS] val_length_decode:%d == (val.size:%d - 12)\n", prefix.c_str(), val_length_decode, val.size());
	}

	// Get the mutation header
	while (1) {
		// stop when reach the end of the string
		if(reader.eof() ) { //|| *reader.rptr == 0xFF
			//printf("Finish decode the value\n");
			break;
		}


		uint32_t type = reader.consume<uint32_t>();//reader.consumeNetworkUInt32();
		uint32_t kLen = reader.consume<uint32_t>();//reader.consumeNetworkUInt32();
		uint32_t vLen = reader.consume<uint32_t>();//reader.consumeNetworkUInt32();
		const uint8_t *k = reader.consume(kLen);
		const uint8_t *v = reader.consume(vLen);
		count_size += 4 * 3 + kLen + vLen;

		if ( kLen < 0 || kLen > val.size() || vLen < 0 || vLen > val.size() ) {
			printf("%s[PARSE ERROR]!!!! kLen:%d(0x%04x) vLen:%d(0x%04x)\n", prefix.c_str(), kLen, kLen, vLen, vLen);
		}

		printf("%s---DedoceBackupMutation: Type:%d K:%s V:%s k_size:%d v_size:%d\n", prefix.c_str(),
			   type,  getHexString(KeyRef(k, kLen)).c_str(), getHexString(KeyRef(v, vLen)).c_str(), kLen, vLen);

	}
	printf("----------------------------------------------------------\n");
}

void printKVOps(Reference<RestoreData> rd) {
	std::string typeStr = "MSet";
	TraceEvent("PrintKVOPs").detail("MapSize", rd->kvOps.size());
	printf("PrintKVOPs num_of_version:%d\n", rd->kvOps.size());
	for ( auto it = rd->kvOps.begin(); it != rd->kvOps.end(); ++it ) {
		TraceEvent("PrintKVOPs\t").detail("Version", it->first).detail("OpNum", it->second.size());
		printf("PrintKVOPs Version:%08lx num_of_ops:%d\n",  it->first, it->second.size());
		for ( auto m = it->second.begin(); m != it->second.end(); ++m ) {
			if (  m->type >= MutationRef::Type::SetValue && m->type <= MutationRef::Type::MAX_ATOMIC_OP )
				typeStr = typeString[m->type];
			else {
				printf("PrintKVOPs MutationType:%d is out of range\n", m->type);
			}

			printf("\tPrintKVOPs Version:%016lx MType:%s K:%s, V:%s K_size:%d V_size:%d\n", it->first, typeStr.c_str(),
				   getHexString(m->param1).c_str(), getHexString(m->param2).c_str(), m->param1.size(), m->param2.size());

			TraceEvent("PrintKVOPs\t\t").detail("Version", it->first)
					.detail("MType", m->type).detail("MTypeStr", typeStr)
					.detail("MKey", getHexString(m->param1))
					.detail("MValueSize", m->param2.size())
					.detail("MValue", getHexString(m->param2));
		}
	}
}

// Sanity check if KVOps is sorted
bool isKVOpsSorted(Reference<RestoreData> rd) {
	bool ret = true;
	auto prev = rd->kvOps.begin();
	for ( auto it = rd->kvOps.begin(); it != rd->kvOps.end(); ++it ) {
		if ( prev->first > it->first ) {
			ret = false;
			break;
		}
		prev = it;
	}
	return ret;
}

bool allOpsAreKnown(Reference<RestoreData> rd) {
	bool ret = true;
	for ( auto it = rd->kvOps.begin(); it != rd->kvOps.end(); ++it ) {
		for ( auto m = it->second.begin(); m != it->second.end(); ++m ) {
			if ( m->type == MutationRef::SetValue || m->type == MutationRef::ClearRange
			    || isAtomicOp((MutationRef::Type) m->type) )
				continue;
			else {
				printf("[ERROR] Unknown mutation type:%d\n", m->type);
				ret = false;
			}
		}

	}

	return ret;
}



//version_input is the file version
void registerBackupMutation(Reference<RestoreData> rd, Standalone<StringRef> val_input, Version file_version) {
	std::string prefix = "||\t";
	std::stringstream ss;
	const int version_size = 12;
	const int header_size = 12;
	StringRef val = val_input.contents();
	StringRefReaderMX reader(val, restore_corrupted_data());

	int count_size = 0;
	// Get the version
	uint64_t version = reader.consume<uint64_t>();
	count_size += 8;
	uint32_t val_length_decode = reader.consume<uint32_t>();
	count_size += 4;

	if ( rd->kvOps.find(file_version) == rd->kvOps.end() ) {
		//kvOps.insert(std::make_pair(rangeFile.version, Standalone<VectorRef<MutationRef>>(VectorRef<MutationRef>())));
		rd->kvOps.insert(std::make_pair(file_version, VectorRef<MutationRef>()));
	}

	printf("----------------------------------------------------------Register Backup Mutation into KVOPs version:%08lx\n", file_version);
	printf("To decode value:%s\n", getHexString(val).c_str());
	if ( val_length_decode != (val.size() - 12) ) {
		printf("[PARSE ERROR]!!! val_length_decode:%d != val.size:%d\n",  val_length_decode, val.size());
	} else {
		printf("[PARSE SUCCESS] val_length_decode:%d == (val.size:%d - 12)\n", val_length_decode, val.size());
	}

	// Get the mutation header
	while (1) {
		// stop when reach the end of the string
		if(reader.eof() ) { //|| *reader.rptr == 0xFF
			//printf("Finish decode the value\n");
			break;
		}


		uint32_t type = reader.consume<uint32_t>();//reader.consumeNetworkUInt32();
		uint32_t kLen = reader.consume<uint32_t>();//reader.consumeNetworkUInkvOps[t32();
		uint32_t vLen = reader.consume<uint32_t>();//reader.consumeNetworkUInt32();
		const uint8_t *k = reader.consume(kLen);
		const uint8_t *v = reader.consume(vLen);
		count_size += 4 * 3 + kLen + vLen;

		MutationRef m((MutationRef::Type) type, KeyRef(k, kLen), KeyRef(v, vLen)); //ASSUME: all operation in range file is set.
		rd->kvOps[file_version].push_back_deep(rd->kvOps[file_version].arena(), m);

		//		if ( kLen < 0 || kLen > val.size() || vLen < 0 || vLen > val.size() ) {
		//			printf("%s[PARSE ERROR]!!!! kLen:%d(0x%04x) vLen:%d(0x%04x)\n", prefix.c_str(), kLen, kLen, vLen, vLen);
		//		}
		//
		if ( debug_verbose ) {
			printf("%s---RegisterBackupMutation: Type:%d K:%s V:%s k_size:%d v_size:%d\n", prefix.c_str(),
				   type,  getHexString(KeyRef(k, kLen)).c_str(), getHexString(KeyRef(v, vLen)).c_str(), kLen, vLen);
		}

	}
	//	printf("----------------------------------------------------------\n");
}


//key_input format: [logRangeMutation.first][hash_value_of_commit_version:1B][bigEndian64(commitVersion)][bigEndian32(part)]
bool concatenateBackupMutationForLogFile(Reference<RestoreData> rd, Standalone<StringRef> val_input, Standalone<StringRef> key_input) {
	std::string prefix = "||\t";
	std::stringstream ss;
	const int version_size = 12;
	const int header_size = 12;
	StringRef val = val_input.contents();
	StringRefReaderMX reader(val, restore_corrupted_data());
	StringRefReaderMX readerKey(key_input, restore_corrupted_data()); //read key_input!
	int logRangeMutationFirstLength = key_input.size() - 1 - 8 - 4;
	bool concatenated = false;

	if ( logRangeMutationFirstLength < 0 ) {
		printf("[ERROR]!!! logRangeMutationFirstLength:%d < 0, key_input.size:%d\n", logRangeMutationFirstLength, key_input.size());
	}

	if ( debug_verbose ) {
		printf("[DEBUG] Process key_input:%s\n", getHexKey(key_input, logRangeMutationFirstLength).c_str());
	}

	//PARSE key
	Standalone<StringRef> id_old = key_input.substr(0, key_input.size() - 4); //Used to sanity check the decoding of key is correct
	Standalone<StringRef> partStr = key_input.substr(key_input.size() - 4, 4); //part
	StringRefReaderMX readerPart(partStr, restore_corrupted_data());
	uint32_t part_direct = readerPart.consumeNetworkUInt32(); //Consume a bigEndian value
	if ( debug_verbose  ) {
		printf("[DEBUG] Process prefix:%s and partStr:%s part_direct:%08x fromm key_input:%s, size:%d\n",
			   getHexKey(id_old, logRangeMutationFirstLength).c_str(),
			   getHexString(partStr).c_str(),
			   part_direct,
			   getHexKey(key_input, logRangeMutationFirstLength).c_str(),
			   key_input.size());
	}

	StringRef longRangeMutationFirst;

	if ( logRangeMutationFirstLength > 0 ) {
		printf("readerKey consumes %dB\n", logRangeMutationFirstLength);
		longRangeMutationFirst = StringRef(readerKey.consume(logRangeMutationFirstLength), logRangeMutationFirstLength);
	}

	uint8_t hashValue = readerKey.consume<uint8_t>();
	uint64_t commitVersion = readerKey.consumeNetworkUInt64(); // Consume big Endian value encoded in log file, commitVersion is in littleEndian
	uint64_t commitVersionBE = bigEndian64(commitVersion);
	uint32_t part = readerKey.consumeNetworkUInt32(); //Consume big Endian value encoded in log file
	uint32_t partBE = bigEndian32(part);
	Standalone<StringRef> id2 = longRangeMutationFirst.withSuffix(StringRef(&hashValue,1)).withSuffix(StringRef((uint8_t*) &commitVersion, 8));

	//Use commitVersion as id
	Standalone<StringRef> id = StringRef((uint8_t*) &commitVersion, 8);

	if ( debug_verbose ) {
		printf("[DEBUG] key_input_size:%d longRangeMutationFirst:%s hashValue:%02x commitVersion:%016lx (BigEndian:%016lx) part:%08x (BigEndian:%08x), part_direct:%08x mutationMap.size:%d\n",
			   key_input.size(), longRangeMutationFirst.printable().c_str(), hashValue,
			   commitVersion, commitVersionBE,
			   part, partBE,
			   part_direct, rd->mutationMap.size());
	}

	if ( rd->mutationMap.find(id) == rd->mutationMap.end() ) {
		rd->mutationMap.insert(std::make_pair(id, val_input));
		if ( part_direct != 0 ) {
			printf("[ERROR]!!! part:%d != 0 for key_input:%s\n", part_direct, getHexString(key_input).c_str());
		}
		rd->mutationPartMap.insert(std::make_pair(id, part_direct));
	} else { // concatenate the val string
//		printf("[INFO] Concatenate the log's val string at version:%ld\n", id.toString().c_str());
		rd->mutationMap[id] = rd->mutationMap[id].contents().withSuffix(val_input.contents()); //Assign the new Areana to the map's value
		if ( part_direct != (rd->mutationPartMap[id] + 1) ) {
			printf("[ERROR]!!! current part id:%d new part_direct:%d is not the next integer of key_input:%s\n", rd->mutationPartMap[id], part_direct, getHexString(key_input).c_str());
			printf("[HINT] Check if the same range or log file has been processed more than once!\n");
		}
		if ( part_direct != part ) {
			printf("part_direct:%08x != part:%08x\n", part_direct, part);
		}
		rd->mutationPartMap[id] = part_direct;
		concatenated = true;
	}

	return concatenated;
}

bool isRangeMutation(MutationRef m) {
	if (m.type == MutationRef::Type::ClearRange) {
		if (m.type == MutationRef::Type::DebugKeyRange) {
			printf("[ERROR] DebugKeyRange mutation is in backup data unexpectedly. We still handle it as a range mutation; the suspicious mutation:%s\n", m.toString().c_str());
		}
		return true;
	} else {
		if ( !(m.type == MutationRef::Type::SetValue ||
				isAtomicOp((MutationRef::Type) m.type)) ) {
			printf("[ERROR] %s mutation is in backup data unexpectedly. We still handle it as a key mutation; the suspicious mutation:%s\n", typeString[m.type], m.toString().c_str());

		}
		return false;
	}
}

void splitMutation(Reference<RestoreData> rd,  MutationRef m, Arena& mvector_arena, VectorRef<MutationRef> mvector, Arena& nodeIDs_arena, VectorRef<UID> nodeIDs) {
	// mvector[i] should be mapped to nodeID[i]
	ASSERT(mvector.empty());
	ASSERT(nodeIDs.empty());
	// key range [m->param1, m->param2)
	//std::map<Standalone<KeyRef>, UID>;
	std::map<Standalone<KeyRef>, UID>::iterator itlow, itup; //we will return [itlow, itup)
	itlow = rd->range2Applier.lower_bound(m.param1); // lower_bound returns the iterator that is >= m.param1
	if ( itlow != rd->range2Applier.begin()) { // m.param1 is not the smallest key \00
		// (itlow-1) is the node whose key range includes m.param1
		--itlow;
	} else {
		if (m.param1 != LiteralStringRef("\00")) {
			printf("[ERROR] splitMutation has bug on range mutation:%s\n", m.toString().c_str());
		}
	}

	itup = rd->range2Applier.upper_bound(m.param2); // upper_bound returns the iterator that is > m.param2; return rmap::end if no keys are considered to go after m.param2.
	ASSERT( itup == rd->range2Applier.end() || itup->first >= m.param2 );
	// Now adjust for the case: example: mutation range is [a, d); we have applier's ranges' inclusive lower bound values are: a, b, c, d, e; upper_bound(d) returns itup to e, but we want itup to d.
	--itup;
	ASSERT( itup->first <= m.param2 );
	if ( itup->first < m.param2 ) {
		++itup; //make sure itup is >= m.param2, that is, itup is the next key range >= m.param2
	}

	while (itlow->first < itup->first) {
		MutationRef curm; //current mutation
		curm.type = m.type;
		curm.param1 = itlow->first;
		itlow++;
		if (itlow == rd->range2Applier.end()) {
			curm.param2 = normalKeys.end;
		} else {
			curm.param2 = itlow->first;
		}
		mvector.push_back(mvector_arena, curm);

		nodeIDs.push_back(nodeIDs_arena, itlow->second);
	}

	return;
}


// MXNOTE: revise done
ACTOR Future<Void> registerMutationsToApplier(Reference<RestoreData> rd) {
	printf("[INFO][Loader] Node:%s rd->masterApplier:%s, hasApplierInterface:%d registerMutationsToApplier\n",
			rd->describeNode().c_str(), rd->masterApplier.toString().c_str(),
			rd->workers_interface.find(rd->masterApplier) != rd->workers_interface.end());

	state RestoreCommandInterface applierCmdInterf; // = rd->workers_interface[rd->masterApplier];
	state int packMutationNum = 0;
	state int packMutationThreshold = 1;
	state int kvCount = 0;
	state std::vector<Future<RestoreCommandReply>> cmdReplies;

	state int splitMutationIndex = 0;

	printAppliersKeyRange(rd);

	loop {
		try {
			packMutationNum = 0;
			splitMutationIndex = 0;
			kvCount = 0;
			state std::map<Version, Standalone<VectorRef<MutationRef>>>::iterator kvOp;
			rd->cmdID.initPhase(RestoreCommandEnum::Loader_Send_Mutations_To_Applier);
			for ( kvOp = rd->kvOps.begin(); kvOp != rd->kvOps.end(); kvOp++) {
				state uint64_t commitVersion = kvOp->first;
				state int mIndex;
				state MutationRef kvm;
				for (mIndex = 0; mIndex < kvOp->second.size(); mIndex++) {
					kvm = kvOp->second[mIndex];
					printf("[VERBOSE_DEBUG] mutation to sent to applier, mutation:%s\n", kvm.toString().c_str());
					// Send the mutation to applier
					if (isRangeMutation(kvm)) {
						// Because using a vector of mutations causes overhead, and the range mutation should happen rarely;
						// We handle the range mutation and key mutation differently for the benefit of avoiding memory copy
						state Standalone<VectorRef<MutationRef>> mvector;
						state Standalone<VectorRef<UID>> nodeIDs;
						// '' Bug may be here! The splitMutation() may be wrong!
						splitMutation(rd, kvm, mvector.arena(), mvector.contents(), nodeIDs.arena(), nodeIDs.contents());
						ASSERT(mvector.size() == nodeIDs.size());

						for (splitMutationIndex = 0; splitMutationIndex < mvector.size(); splitMutationIndex++ ) {
							MutationRef mutation = mvector[splitMutationIndex];
							UID applierID = nodeIDs[splitMutationIndex];
							applierCmdInterf = rd->workers_interface[applierID];

							rd->cmdID.nextCmd();
							printf("[VERBOSE_DEBUG] mutation:%s\n", mutation.toString().c_str());
							cmdReplies.push_back(applierCmdInterf.cmd.getReply(
									RestoreCommand(RestoreCommandEnum::Loader_Send_Mutations_To_Applier, rd->cmdID, applierID, commitVersion, mutation)));

							packMutationNum++;
							kvCount++;
							if (packMutationNum >= packMutationThreshold) {
								ASSERT( packMutationNum == packMutationThreshold );
								printf("[INFO][Loader] Waits for applier to receive %d mutations\n", cmdReplies.size());
								std::vector<RestoreCommandReply> reps = wait( timeoutError( getAll(cmdReplies), FastRestore_Failure_Timeout ) );
								cmdReplies.clear();
								packMutationNum = 0;
							}
						}
					} else { // mutation operates on a particular key
						std::map<Standalone<KeyRef>, UID>::iterator itlow = rd->range2Applier.lower_bound(kvm.param1); // lower_bound returns the iterator that is >= m.param1
						// make sure itlow->first <= m.param1
						if ( itlow == rd->range2Applier.end() || itlow->first > kvm.param1 ) {
							--itlow;
						}
						ASSERT( itlow->first <= kvm.param1 );
						MutationRef mutation = kvm;
						UID applierID = itlow->second;
						applierCmdInterf = rd->workers_interface[applierID];

						rd->cmdID.nextCmd();
						cmdReplies.push_back(applierCmdInterf.cmd.getReply(
								RestoreCommand(RestoreCommandEnum::Loader_Send_Mutations_To_Applier, rd->cmdID, applierID, commitVersion, mutation)));
						packMutationNum++;
						kvCount++;
						if (packMutationNum >= packMutationThreshold) {
							ASSERT( packMutationNum == packMutationThreshold );
							printf("[INFO][Loader] Waits for applier to receive %d mutations\n", cmdReplies.size());
							std::vector<RestoreCommandReply> reps = wait( timeoutError( getAll(cmdReplies), FastRestore_Failure_Timeout ) );
							cmdReplies.clear();
							packMutationNum = 0;
						}
					}
				}

			}

			if (!cmdReplies.empty()) {
				std::vector<RestoreCommandReply> reps =  wait( timeoutError( getAll(cmdReplies), FastRestore_Failure_Timeout ) );
				cmdReplies.clear();
			}
			printf("[Summary][Loader] Node:%s Last CMDUID:%s produces %d mutation operations\n",
					rd->describeNode().c_str(), rd->cmdID.toString().c_str(), kvCount);

			break;

		} catch (Error &e) {
			// TODO: Handle the command reply timeout error
			if (e.code() != error_code_io_timeout) {
				fprintf(stderr, "[ERROR] Node:%s, Commands before cmdID:%s timeout\n", rd->describeNode().c_str(), rd->cmdID.toString().c_str());
			} else {
				fprintf(stderr, "[ERROR] Node:%s, Commands before cmdID:%s error. error code:%d, error message:%s\n", rd->describeNode().c_str(),
						rd->cmdID.toString().c_str(), e.code(), e.what());
			}
			fprintf(stderr, "[ERROR] WE STOP HERE FOR DEBUG\n");
			break;
		}
	};

	return Void();
}

// Loader: Register sampled mutations
ACTOR Future<Void> registerMutationsToMasterApplier(Reference<RestoreData> rd) {
	printf("[Sampling] Node:%s registerMutationsToMaster() rd->masterApplier:%s, hasApplierInterface:%d\n",
			rd->describeNode().c_str(), rd->masterApplier.toString().c_str(),
			rd->workers_interface.find(rd->masterApplier) != rd->workers_interface.end());
	//printAppliersKeyRange(rd);

	state RestoreCommandInterface applierCmdInterf = rd->workers_interface[rd->masterApplier];
	state UID applierID = rd->masterApplier;
	state int packMutationNum = 0;
	state int packMutationThreshold = 1;
	state int kvCount = 0;
	state std::vector<Future<RestoreCommandReply>> cmdReplies;

	state int splitMutationIndex = 0;
	state std::map<Version, Standalone<VectorRef<MutationRef>>>::iterator kvOp;

	loop {
		try {
			rd->cmdID.initPhase(RestoreCommandEnum::Loader_Send_Sample_Mutation_To_Applier); 
			// TODO: Consider using a different EndPoint for loader and applier communication.
			// Otherwise, applier may receive loader's message while applier is waiting for master to assign key-range
			for ( kvOp = rd->kvOps.begin(); kvOp != rd->kvOps.end(); kvOp++) {
				state uint64_t commitVersion = kvOp->first;
				state int mIndex;
				state MutationRef kvm;
				for (mIndex = 0; mIndex < kvOp->second.size(); mIndex++) {
					kvm = kvOp->second[mIndex];
					rd->cmdID.nextCmd();
					printf("[VERBOSE_DEBUG] send mutation to applier, mutation:%s\n", kvm.toString().c_str());
					cmdReplies.push_back(applierCmdInterf.cmd.getReply(
							RestoreCommand(RestoreCommandEnum::Loader_Send_Sample_Mutation_To_Applier, rd->cmdID, applierID, commitVersion, kvm)));
					packMutationNum++;
					kvCount++;
					if (packMutationNum >= packMutationThreshold) {
						ASSERT( packMutationNum == packMutationThreshold );
						//printf("[INFO][Loader] Waits for applier to receive %d mutations\n", cmdReplies.size());
						std::vector<RestoreCommandReply> reps = wait( timeoutError( getAll(cmdReplies), FastRestore_Failure_Timeout) );
						cmdReplies.clear();
						packMutationNum = 0;
					}
				}
			}

			if (!cmdReplies.empty()) {
				std::vector<RestoreCommandReply> reps = wait( timeoutError( getAll(cmdReplies), FastRestore_Failure_Timeout) );
				cmdReplies.clear();
			}

			printf("[Sample Summary][Loader] Node:%s produces %d mutation operations\n", rd->describeNode().c_str(), kvCount);
			break;
		} catch (Error &e) {
			// TODO: Handle the command reply timeout error
			if (e.code() != error_code_io_timeout) {
				fprintf(stderr, "[ERROR] Node:%s, Commands before cmdID:%s timeout\n", rd->describeNode().c_str(), rd->cmdID.toString().c_str());
			} else {
				fprintf(stderr, "[ERROR] Node:%s, Commands before cmdID:%s error. error code:%d, error message:%s\n", rd->describeNode().c_str(),
						rd->cmdID.toString().c_str(), e.code(), e.what());
			}
			printf("[WARNING] Node:%s timeout at waiting on replies of Loader_Send_Sample_Mutation_To_Applier. Retry...\n", rd->describeNode().c_str());
		}
	}

	return Void();
}

// Master applier: Receive sampled mutations sent from loader
ACTOR Future<Void> receiveSampledMutations(Reference<RestoreData> rd, RestoreCommandInterface interf) {
	if ( rd->localNodeStatus.role != RestoreRole::Applier) {
		printf("[ERROR] non-applier node:%s (role:%d) is waiting for cmds for appliers\n",
				rd->describeNode().c_str(), rd->localNodeStatus.role);
	} else {
		printf("[Sampling][Loader_Send_Sample_Mutation_To_Applier] nodeID:%s starts \n",
				rd->describeNode().c_str());
	}

	state int numMutations = 0;
	rd->numSampledMutations = 0;

	loop {
		choose {
			when(RestoreCommand req = waitNext(interf.cmd.getFuture())) {
				//printf("[INFO][Applier] Got Restore Command: cmd:%d UID:%s\n",
				//		req.cmd, req.id.toString().c_str());
				if ( rd->localNodeStatus.nodeID != req.id ) {
						printf("[ERROR]CMDID:%s Node:%s receive request with a different nodeId:%s\n",
								req.cmdID.toString().c_str(), rd->describeNode().c_str(), req.id.toString().c_str());
				}
				if ( req.cmd == RestoreCommandEnum::Loader_Send_Sample_Mutation_To_Applier ) {
					ASSERT(req.cmd == (RestoreCommandEnum) req.cmdID.phase);
					// Handle duplicate message
					if (rd->isCmdProcessed(req.cmdID)) {
						printf("[DEBUG] NODE:%s skip duplicate cmd:%s\n", rd->describeNode().c_str(), req.cmdID.toString().c_str());
						req.reply.send(RestoreCommandReply(interf.id(), req.cmdID));
						continue;
					}

					// Applier will cache the mutations at each version. Once receive all mutations, applier will apply them to DB
					state uint64_t commitVersion = req.commitVersion;
					// TODO: Change the req.mutation to a vector of mutations
					MutationRef mutation(req.mutation);

					if ( rd->keyOpsCount.find(mutation.param1) == rd->keyOpsCount.end() ) {
						rd->keyOpsCount.insert(std::make_pair(mutation.param1, 0));
					}
					// NOTE: We may receive the same mutation more than once due to network package lost.
					// Since sampling is just an estimation and the network should be stable enough, we do NOT handle the duplication for now
					// In a very unreliable network, we may get many duplicate messages and get a bad key-range splits for appliers. But the restore should still work except for running slower.
					rd->keyOpsCount[mutation.param1]++;
					rd->numSampledMutations++;

					if ( rd->numSampledMutations % 1000 == 1 ) {
						printf("[Sampling][Applier] Node:%s Receives %d sampled mutations. cur_mutation:%s\n",
								rd->describeNode().c_str(), rd->numSampledMutations, mutation.toString().c_str());
					}

					req.reply.send(RestoreCommandReply(interf.id(), req.cmdID));
					rd->processedCmd[req.cmdID] = 1;
				} else if ( req.cmd == RestoreCommandEnum::Loader_Send_Sample_Mutation_To_Applier_Done ) {
					printf("[Sampling][Applier] NodeID:%s receive all sampled mutations, num_of_total_sampled_muations:%d\n",
							rd->describeNode().c_str(), rd->numSampledMutations);
					ASSERT(req.cmd == (RestoreCommandEnum) req.cmdID.phase);
					req.reply.send(RestoreCommandReply(interf.id(), req.cmdID));
					break;
				} else {
					if ( IsCmdInPreviousPhase(RestoreCommandEnum::Loader_Send_Sample_Mutation_To_Applier_Done, req.cmd) ) {
						logExpectedOldCmd(rd, RestoreCommandEnum::Loader_Send_Sample_Mutation_To_Applier_Done, req.cmd, req.cmdID);
						req.reply.send(RestoreCommandReply(interf.id(), req.cmdID));
					} else {
						logUnexpectedCmd(rd, RestoreCommandEnum::Loader_Send_Sample_Mutation_To_Applier_Done, req.cmd, req.cmdID);
					}
				}
			}
		}
	}

	return Void();
}

// MXNODE: revise done
ACTOR Future<Void> notifyApplierToApplyMutations(Reference<RestoreData> rd) {
	loop {
		try {
			printf("[INFO]Node:%s rd->masterApplier:%s, hasApplierInterface:%d\n",
					rd->describeNode().c_str(),
					rd->masterApplier.toString().c_str(),
					rd->workers_interface.find(rd->masterApplier) != rd->workers_interface.end());

			state int packMutationNum = 0;
			state int packMutationThreshold = 1;
			state int kvCount = 0;
			state std::vector<Future<RestoreCommandReply>> cmdReplies;
			//state std::vector<UID> applierIDs = getApplierIDs(rd);
			state std::vector<UID> applierIDs = rd->getBusyAppliers();
			state int applierIndex = 0;
			state UID applierID;
			state RestoreCommandInterface applierCmdInterf;

			rd->cmdID.initPhase(RestoreCommandEnum::Loader_Notify_Appler_To_Apply_Mutation);
			printf("Num_ApplierID:%d\n", applierIDs.size());
			for (applierIndex = 0; applierIndex < applierIDs.size(); applierIndex++) {
				applierID = applierIDs[applierIndex];
				applierCmdInterf = rd->workers_interface[applierID];
				rd->cmdID.nextCmd();
				cmdReplies.push_back(applierCmdInterf.cmd.getReply(RestoreCommand(RestoreCommandEnum::Loader_Notify_Appler_To_Apply_Mutation, rd->cmdID, applierID)));
			}

			// Q: Maybe we should not timeout at apply-to-DB because apply-to-DB can take a long time
			//std::vector<RestoreCommandReply> reps = wait( timeoutError( getAll(cmdReplies), FastRestore_Failure_Timeout ) );
			std::vector<RestoreCommandReply> reps = wait( getAll(cmdReplies) );
			//wait( waitForAny(cmdReplies) ); //TODO: I wait for any insteal of wait for all! This is NOT TESTED IN SIMULATION!

			printf("[INFO] Node:%s Finish Loader_Notify_Appler_To_Apply_Mutation cmd\n", rd->describeNode().c_str());

			break;
		} catch (Error &e) {
			// TODO: Handle the command reply timeout error
			if (e.code() != error_code_io_timeout) {
				fprintf(stderr, "[ERROR] Node:%s, Commands before cmdID:%s timeout\n", rd->describeNode().c_str(), rd->cmdID.toString().c_str());
			} else {
				fprintf(stderr, "[ERROR] Node:%s, Commands before cmdID:%s error. error code:%d, error message:%s\n", rd->describeNode().c_str(),
						rd->cmdID.toString().c_str(), e.code(), e.what());
			}
			printf("Retry notifying appliers to apply mutations\n");
		}
	}

	return Void();
}




////---------------Helper Functions and Class copied from old file---------------

// This function is copied from RestoreConfig. It is not used now. May use it later. 
ACTOR Future<std::string> RestoreConfig::getProgress_impl(Reference<RestoreConfig> restore, Reference<ReadYourWritesTransaction> tr) {
	tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
	tr->setOption(FDBTransactionOptions::LOCK_AWARE);

	state Future<int64_t> fileCount = restore->fileCount().getD(tr);
	state Future<int64_t> fileBlockCount = restore->fileBlockCount().getD(tr);
	state Future<int64_t> fileBlocksDispatched = restore->filesBlocksDispatched().getD(tr);
	state Future<int64_t> fileBlocksFinished = restore->fileBlocksFinished().getD(tr);
	state Future<int64_t> bytesWritten = restore->bytesWritten().getD(tr);
	state Future<StringRef> status = restore->stateText(tr);
	state Future<Version> lag = restore->getApplyVersionLag(tr);
	state Future<std::string> tag = restore->tag().getD(tr);
	state Future<std::pair<std::string, Version>> lastError = restore->lastError().getD(tr);

	// restore might no longer be valid after the first wait so make sure it is not needed anymore.
	state UID uid = restore->getUid();
	wait(success(fileCount) && success(fileBlockCount) && success(fileBlocksDispatched) && success(fileBlocksFinished) && success(bytesWritten) && success(status) && success(lag) && success(tag) && success(lastError));

	std::string errstr = "None";
	if(lastError.get().second != 0)
		errstr = format("'%s' %llds ago.\n", lastError.get().first.c_str(), (tr->getReadVersion().get() - lastError.get().second) / CLIENT_KNOBS->CORE_VERSIONSPERSECOND );

	TraceEvent("FileRestoreProgress")
		.detail("RestoreUID", uid)
		.detail("Tag", tag.get())
		.detail("State", status.get().toString())
		.detail("FileCount", fileCount.get())
		.detail("FileBlocksFinished", fileBlocksFinished.get())
		.detail("FileBlocksTotal", fileBlockCount.get())
		.detail("FileBlocksInProgress", fileBlocksDispatched.get() - fileBlocksFinished.get())
		.detail("BytesWritten", bytesWritten.get())
		.detail("ApplyLag", lag.get())
		.detail("TaskInstance", (uint64_t)this);


	return format("Tag: %s  UID: %s  State: %s  Blocks: %lld/%lld  BlocksInProgress: %lld  Files: %lld  BytesWritten: %lld  ApplyVersionLag: %lld  LastError: %s",
					tag.get().c_str(),
					uid.toString().c_str(),
					status.get().toString().c_str(),
					fileBlocksFinished.get(),
					fileBlockCount.get(),
					fileBlocksDispatched.get() - fileBlocksFinished.get(),
					fileCount.get(),
					bytesWritten.get(),
					lag.get(),
					errstr.c_str()
				);
}