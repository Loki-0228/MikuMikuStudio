#include <mms/cpp/ui.hpp>
#include <cstring>
#include <new>

using mms::sdk::Header;
using mms::sdk::Text;
struct Plugin {
	MmsHostV1 host{};
	mms::sdk::CommandUi controls;
	bool checked = false;
	Plugin(const MmsHostV1& source, mms::sdk::UiServices services) noexcept
		: host(source), controls(std::move(services)) {}
};
static MmsResult MMS_CALL getState(void* context, MmsObjectRef, MmsCommandStateV1* out) noexcept {
	if (!context || !out || out->h.size < sizeof(*out)) return MMS_E_ARGUMENT;
	*out = {};
	out->h = Header<MmsCommandStateV1>();
	out->enabled = out->visible = 1;
	out->checked = static_cast<Plugin*>(context)->checked ? 1u : 0u;
	return MMS_OK;
}
static MmsResult MMS_CALL invoke(void* context, MmsObjectRef, MmsSpan args, MmsHandle tx) noexcept {
	if (args.count != 0 || tx != 0) return MMS_E_ARGUMENT;
	auto* p = static_cast<Plugin*>(context);
	p->checked = !p->checked;
	p->host.log(p->host.context, 1u, Text("Hello from org.example.hello"));
	return MMS_OK;
}
static MmsResult MMS_CALL create(const MmsHostV1* host, const MmsCreateInfoV1* info, void** out) noexcept {
	if (!out) return MMS_E_ARGUMENT;
	*out = nullptr;
	if (!host || !info || host->h.size < sizeof(*host) || info->h.size < sizeof(*info)) return MMS_E_ARGUMENT;
	if (host->h.major != 1 || info->h.major != 1) return MMS_E_VERSION;
	if (!host->log) return MMS_E_ARGUMENT;
	mms::sdk::UiServices services;
	const auto result = mms::sdk::UiServices::Query(*host, services);
	if (result != MMS_OK) return result;
	auto* p = new (std::nothrow) Plugin(*host, std::move(services));
	if (!p) return MMS_E_MEMORY;
	*out = p;
	return MMS_OK;
}
static MmsResult MMS_CALL start(void* instance) noexcept {
	auto* p = static_cast<Plugin*>(instance);
	MmsUiLayoutHintsV1 hints{};
	hints.h = Header<MmsUiLayoutHintsV1>();
	hints.minimum[0] = 60; hints.minimum[1] = 24;
	hints.preferred[0] = 160; hints.preferred[1] = 24;
	hints.shrink[0] = 1;
	mms::sdk::CommandUiSpec spec;
	spec.id = "org.example.hello.say";
	spec.owner = "org.example.hello";
	spec.label = "示例：输出日志";
	spec.description = "写入插件日志，并切换菜单选中状态";
	spec.context = p; spec.getState = getState; spec.invoke = invoke;
	spec.menu.id = "org.example.hello.menu";
	spec.button.id = "org.example.hello.toolbar";
	spec.button.hints = &hints;
	spec.order = 10;
	return p->controls.Register(spec);
}
static void MMS_CALL onEvent(void*, const MmsEventV1*) noexcept {}
static MmsResult MMS_CALL stop(void* instance) noexcept {
	// Returning a cleanup error keeps the instance/context alive for retry.
	return static_cast<Plugin*>(instance)->controls.Close();
}
static void MMS_CALL destroy(void* instance) noexcept { delete static_cast<Plugin*>(instance); }
extern "C" MmsResult MMS_CALL MMSPluginGetApi(uint32_t major, uint32_t minor,
	uint32_t outBytes, MmsPluginApiV1* out) {
	if (!out || outBytes < sizeof(*out)) return MMS_E_ARGUMENT;
	if (major != 1 || minor != 0) return MMS_E_VERSION;
	const MmsPluginApiV1 api{Header<MmsPluginApiV1>(), create, start, onEvent, stop, destroy};
	std::memcpy(out, &api, sizeof(api));
	return MMS_OK;
}
