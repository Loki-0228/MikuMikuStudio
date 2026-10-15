#include <mms/ui.h>
#include <cstring>
#include <new>

template<class T> static MmsHeader header() noexcept {
	return {static_cast<uint32_t>(sizeof(T)), 1u, 0u};
}
template<size_t N> static MmsString text(const char (&s)[N]) noexcept {
	return {s, N - 1};
}
struct Plugin {
	MmsHostV1 host{};
	MmsUiApiV1 ui{};
	MmsHandle command = 0, element = 0, toolbarButton = 0;
};
static MmsResult MMS_CALL getState(void*, MmsObjectRef, MmsCommandStateV1* out) noexcept {
	if (!out || out->h.size < sizeof(*out)) return MMS_E_ARGUMENT;
	*out = {};
	out->h = header<MmsCommandStateV1>();
	out->enabled = out->visible = 1;
	return MMS_OK;
}
static MmsResult MMS_CALL invoke(void* context, MmsObjectRef, MmsSpan args, MmsHandle tx) noexcept {
	if (args.count != 0 || tx != 0) return MMS_E_ARGUMENT;
	auto* p = static_cast<Plugin*>(context);
	p->host.log(p->host.context, 1u, text("Hello from org.example.hello"));
	return MMS_OK;
}
static MmsResult MMS_CALL create(const MmsHostV1* host, const MmsCreateInfoV1* info, void** out) noexcept {
	if (!out) return MMS_E_ARGUMENT;
	*out = nullptr;
	if (!host || !info || host->h.size < sizeof(*host) || info->h.size < sizeof(*info))
		return MMS_E_ARGUMENT;
	if (host->h.major != 1 || info->h.major != 1) return MMS_E_VERSION;
	if (!host->queryService || !host->registerProvider || !host->unregisterProvider ||
		!host->log || !host->enqueueUi || !host->cancelUi) return MMS_E_ARGUMENT;
	auto* p = new (std::nothrow) Plugin;
	if (!p) return MMS_E_MEMORY;
	p->host = *host;
	p->host.h = header<MmsHostV1>();
	auto result = host->queryService(host->context, text("mms.ui"), 1, 0, sizeof(p->ui), &p->ui);
	if (result == MMS_OK && (p->ui.h.major != 1 || p->ui.h.size < sizeof(p->ui)))
		result = MMS_E_VERSION;
	if (result == MMS_OK && (!p->ui.registerCommand || !p->ui.registerElement ||
		!p->ui.registerProperty || !p->ui.updateCommand || !p->ui.updateElement ||
		!p->ui.updateProperty || !p->ui.unregisterItem || !p->ui.acquireSnapshot ||
		!p->ui.releaseSnapshot || !p->ui.getCommandState || !p->ui.invoke ||
		!p->ui.readProperty || !p->ui.releaseValue || !p->ui.stageProperty ||
		!p->ui.beginLayout || !p->ui.applyLayout || !p->ui.commitLayout || !p->ui.cancelLayout))
		result = MMS_E_ARGUMENT;
	if (result != MMS_OK) { delete p; return result; }
	*out = p;
	return MMS_OK;
}
static MmsResult MMS_CALL start(void* instance) noexcept {
	auto* p = static_cast<Plugin*>(instance);
	MmsCommandDescV1 command{};
	command.info.h = header<MmsCommandDescV1>();
	command.info.id = text("org.example.hello.say");
	command.info.ownerPlugin = text("org.example.hello");
	command.info.label = text("示例：输出日志");
	command.info.description = text("在 MMS 插件日志中写入一条消息");
	command.context = p;
	command.getState = getState;
	command.invoke = invoke;
	auto result = p->ui.registerCommand(p->ui.context, &command, &p->command);
	if (result != MMS_OK) return result;
	MmsUiElementV1 item{};
	item.h = header<MmsUiElementV1>();
	item.id = text("org.example.hello.menu");
	item.ownerPlugin = command.info.ownerPlugin;
	item.parentId = text("mms.menu.plugins");
	item.label = command.info.label;
	item.commandId = command.info.id;
	item.kind = MMS_UI_MENU_ITEM;
	item.capabilities = MMS_UI_MOVE | MMS_UI_HIDE | MMS_UI_SHORTCUT;
	// If this fails, the host still calls stop to roll back the command registration.
	result = p->ui.registerElement(p->ui.context, &item, &p->element);
	if (result != MMS_OK) return result;
	item.id = text("org.example.hello.toolbar");
	item.parentId = text("mms.toolbar.main.extensions");
	item.kind = MMS_UI_BUTTON;
	return p->ui.registerElement(p->ui.context, &item, &p->toolbarButton);
}
static void MMS_CALL onEvent(void*, const MmsEventV1*) noexcept {}
static MmsResult MMS_CALL stop(void* instance) noexcept {
	auto* p = static_cast<Plugin*>(instance);
	MmsHandle* handles[] = {&p->toolbarButton, &p->element, &p->command};
	for (MmsHandle* handle : handles) {
		if (!*handle) continue;
		auto result = p->ui.unregisterItem(p->ui.context, *handle);
		if (result != MMS_OK && result != MMS_E_NOT_FOUND) return result;
		*handle = 0;
	}
	return MMS_OK;
}
static void MMS_CALL destroy(void* instance) noexcept { delete static_cast<Plugin*>(instance); }
extern "C" MmsResult MMS_CALL MMSPluginGetApi(uint32_t major, uint32_t minor,
	uint32_t outBytes, MmsPluginApiV1* out) {
	if (!out || outBytes < sizeof(*out)) return MMS_E_ARGUMENT;
	if (major != 1 || minor != 0) return MMS_E_VERSION;
	const MmsPluginApiV1 api{header<MmsPluginApiV1>(), create, start, onEvent, stop, destroy};
	std::memcpy(out, &api, sizeof(api));
	return MMS_OK;
}
