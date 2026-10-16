#ifndef MMS_PROPOSAL_EDIT_H
#define MMS_PROPOSAL_EDIT_H
#include "plugin.h"
#pragma pack(push, 8)
typedef struct MmsTransactionParticipantV1 {
	MmsHeader h;
	void* context;
	MmsString pluginId;
	MmsResult (MMS_CALL *prepare)(void*);
	void (MMS_CALL *commit)(void*);
	void (MMS_CALL *cancel)(void*);
	MmsResult (MMS_CALL *getUndoState)(void* context, MmsBytes* before, MmsBytes* after, uint32_t* schema, MmsHandle* lease);
	void (MMS_CALL *releaseUndoState)(void* context, MmsHandle lease);
} MmsTransactionParticipantV1;
typedef struct MmsEditApiV1 {
	MmsHeader h;
	void* context;
	/* One successful transaction is one undo step, including all participants.
	   Separate successful transactions remain separate undo steps. */
	MmsResult (MMS_CALL *begin)(void* context, MmsString label, uint64_t expectedSceneRevision, MmsHandle* transaction);
	MmsResult (MMS_CALL *prepare)(void* context, MmsHandle transaction);
	MmsResult (MMS_CALL *join)(void* context, MmsHandle transaction, const MmsTransactionParticipantV1* participant);
	MmsResult (MMS_CALL *commit)(void* context, MmsHandle transaction);
	void (MMS_CALL *cancel)(void* context, MmsHandle transaction);
	MmsResult (MMS_CALL *undo)(void*);
	MmsResult (MMS_CALL *redo)(void*);
} MmsEditApiV1;
#pragma pack(pop)
#ifdef __cplusplus
static_assert(sizeof(MmsTransactionParticipantV1) == 72, "MMS transaction layout");
static_assert(sizeof(MmsEditApiV1) == 72, "MMS edit API layout");
#endif
#endif
