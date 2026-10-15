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
	MmsResult (MMS_CALL *getUndoState)(void*, MmsBytes*, MmsBytes*, uint32_t*, MmsHandle*);
	void (MMS_CALL *releaseUndoState)(void*, MmsHandle);
} MmsTransactionParticipantV1;
typedef struct MmsEditApiV1 {
	MmsHeader h;
	void* context;
	MmsResult (MMS_CALL *begin)(void*, MmsString, uint64_t, MmsHandle*);
	MmsResult (MMS_CALL *prepare)(void*, MmsHandle);
	MmsResult (MMS_CALL *join)(void*, MmsHandle, const MmsTransactionParticipantV1*);
	MmsResult (MMS_CALL *commit)(void*, MmsHandle);
	void (MMS_CALL *cancel)(void*, MmsHandle);
	MmsResult (MMS_CALL *undo)(void*);
	MmsResult (MMS_CALL *redo)(void*);
} MmsEditApiV1;
#pragma pack(pop)
#ifdef __cplusplus
static_assert(sizeof(MmsTransactionParticipantV1) == 72, "MMS transaction layout");
static_assert(sizeof(MmsEditApiV1) == 72, "MMS edit API layout");
#endif
#endif
