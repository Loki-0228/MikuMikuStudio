#pragma once
#include "state.hpp"
#include "host_api.hpp"
namespace mme_re::mmhack {
// The original has undefined output after a singular inverse. This recovered
// interface reports that failure explicitly and never fabricates matrices.
HRESULT light_view_projection(State&, const HostApi&, D3DMATRIX& view, D3DMATRIX& projection, D3DMATRIX& view_projection);
bool detect_playback(State&);
}
