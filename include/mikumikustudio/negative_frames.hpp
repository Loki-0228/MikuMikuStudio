#pragma once
#include <cstdint>
#include "mikumikustudio/pmm_extension.hpp"

namespace mikumikustudio {
class MMDApp;
namespace negative_frames {
enum class Global { Camera=2,Light,Shadow,Gravity };
bool RegisterGlobal(MMDApp* app,Global kind,int frame);
bool RegisterAccessory(MMDApp* app,int slot,int frame);
bool RegisterDisplay(unsigned char* model,int frame);
void EvaluateGlobals(MMDApp* app,int frame);
inline constexpr char kPmmKey[]="org.mikudancestudio.timeline.negative_keys.v1";
bool RegisterBone(unsigned char* model,int bone,int frame);
bool RegisterSelectedBones(unsigned char* model,int frame);
bool RegisterMorph(unsigned char* model,int morph,int frame,float value);
void EvaluateModel(unsigned char* model,int frame);
void Reset(MMDApp* app);
void ForgetModel(unsigned char* model);
void RefreshControls(MMDApp* app);
void Capture(const MMDApp& app,pmm_extension::Document& document);
bool Load(MMDApp& app,const pmm_extension::Document& document);
void SelectRange(MMDApp* app,int first,int last);
bool Command(MMDApp* app,unsigned id);
void Draw(MMDApp* app,void* dc,void* icons,int columns,int height);
bool SelectCell(MMDApp* app,int frame,int row,bool additive);
bool CellSelected(MMDApp* app,int frame,int row);
void BeginDrag(MMDApp* app);
void Drag(MMDApp* app,int offset);
void EndDrag(MMDApp* app);
void SelectBox(MMDApp* app,std::int64_t first,std::int64_t last,int top,int bottom,bool selected,bool additive);
void SelectColumns(MMDApp* app);
std::size_t Count(const MMDApp* app);
int Earliest(const MMDApp* app);
}
}
