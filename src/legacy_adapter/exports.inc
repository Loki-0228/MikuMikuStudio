// MMSAdapter export table, as an X-macro list.
//
// Included several times by adapter.cpp: once to declare the host function
// pointer types, once for the table, once for the symbol lookup, once for the
// wrapper bodies, and once to count the entries. Keep the parameter lists and
// return types identical to the host definitions in src/exports/effect_api.cpp;
// keep names and ordinals identical to exports/MikuMikuDance.def. Every entry is
// one of the 37 host exports, so a line may not be dropped or duplicated.
//
// Signature: ordinal, name, return type, parameter list, argument list.
// The argument list forwards every parameter unchanged and in order.

MMS_ADAPTER_EXPORT(21, ExpGetFrameTime, float, (), ())
MMS_ADAPTER_EXPORT(33, ExpGetPmdNum, int, (), ())
MMS_ADAPTER_EXPORT(26, ExpGetPmdFilename, char*, (int index), (index))
MMS_ADAPTER_EXPORT(34, ExpGetPmdOrder, int, (int index), (index))
MMS_ADAPTER_EXPORT(28, ExpGetPmdMatNum, int, (int index), (index))
MMS_ADAPTER_EXPORT(29, ExpGetPmdMaterial, float*, (float* out, int index, int mat), (out, index, mat))
MMS_ADAPTER_EXPORT(23, ExpGetPmdBoneNum, int, (int index), (index))
MMS_ADAPTER_EXPORT(22, ExpGetPmdBoneName, char*, (int index, int bone), (index, bone))
MMS_ADAPTER_EXPORT(24, ExpGetPmdBoneWorldMat, float*, (float* out, int index, int bone), (out, index, bone))
MMS_ADAPTER_EXPORT(31, ExpGetPmdMorphNum, int, (int index), (index))
MMS_ADAPTER_EXPORT(30, ExpGetPmdMorphName, char*, (int index, int morph), (index, morph))
MMS_ADAPTER_EXPORT(32, ExpGetPmdMorphValue, float, (int index, int morph), (index, morph))
MMS_ADAPTER_EXPORT(25, ExpGetPmdDisp, int, (int index), (index))
MMS_ADAPTER_EXPORT(27, ExpGetPmdID, std::uint32_t, (int index), (index))
MMS_ADAPTER_EXPORT(6, ExpGetAcsNum, int, (), ())
MMS_ADAPTER_EXPORT(35, ExpGetPreAcsNum, int, (), ())
MMS_ADAPTER_EXPORT(2, ExpGetAcsFilename, char*, (int index), (index))
MMS_ADAPTER_EXPORT(7, ExpGetAcsOrder, int, (int index), (index))
MMS_ADAPTER_EXPORT(13, ExpGetAcsWorldMat, float*, (float* out, int index), (out, index))
MMS_ADAPTER_EXPORT(14, ExpGetAcsX, float, (int index), (index))
MMS_ADAPTER_EXPORT(15, ExpGetAcsY, float, (int index), (index))
MMS_ADAPTER_EXPORT(16, ExpGetAcsZ, float, (int index), (index))
MMS_ADAPTER_EXPORT(8, ExpGetAcsRx, float, (int index), (index))
MMS_ADAPTER_EXPORT(9, ExpGetAcsRy, float, (int index), (index))
MMS_ADAPTER_EXPORT(10, ExpGetAcsRz, float, (int index), (index))
MMS_ADAPTER_EXPORT(11, ExpGetAcsSi, float, (int index), (index))
MMS_ADAPTER_EXPORT(12, ExpGetAcsTr, float, (int index), (index))
MMS_ADAPTER_EXPORT(3, ExpGetAcsID, std::uint32_t, (int index), (index))
MMS_ADAPTER_EXPORT(4, ExpGetAcsMatNum, int, (int index), (index))
MMS_ADAPTER_EXPORT(5, ExpGetAcsMaterial, float*, (float* out, int index, int mat), (out, index, mat))
MMS_ADAPTER_EXPORT(1, ExpGetAcsDisp, int, (int index), (index))
MMS_ADAPTER_EXPORT(18, ExpGetCurrentObject, int, (), ())
MMS_ADAPTER_EXPORT(17, ExpGetCurrentMaterial, int, (), ())
MMS_ADAPTER_EXPORT(19, ExpGetCurrentTechnic, int, (), ())
MMS_ADAPTER_EXPORT(20, ExpGetEnglishMode, int, (), ())
MMS_ADAPTER_EXPORT(36, ExpGetRenderRepeatCount, int, (), ())
MMS_ADAPTER_EXPORT(37, ExpSetRenderRepeatCount, void, (int count), (count))
