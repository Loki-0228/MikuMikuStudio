#pragma once

#include "effect_save_capture.hpp"

namespace mikumikustudio::effect_save {

// Created by the host only after the paired disk writer verifies all members.
// A compatible provider must acknowledge this particular committed request,
// rather than just report that it has processed some save notification.
struct PublishedPair {
    Request request;
    std::string pmmSha256;
    EffectDigest emmSha256;
};

struct ProviderReceipt {
    std::uint64_t requestId;
    CaptureIdentity identity;
    std::wstring pmmPath, emmPath;
    std::string pmmSha256;
    EffectDigest emmSha256;
};

struct LiveIdentity {
    CaptureIdentity identity;
    bool providerAvailable = false;
};

inline bool MatchesPublication(const PublishedPair& pair, const ProviderReceipt& receipt) noexcept {
    const auto& request=pair.request;
    return receipt.requestId==request.requestId && receipt.identity==request.identity &&
        receipt.pmmPath==request.pmmPath && receipt.emmPath==request.emmPath &&
        receipt.pmmSha256==pair.pmmSha256 && receipt.emmSha256==pair.emmSha256;
}

// Main thread only. StartWrite belongs to capture; a worker does not touch
// the tracker, live identity, provider, or UI. An exception, missing receipt,
// stale scene or provider retirement can never authorize dirty clearing/close.
inline bool CompleteFrozenPublication(CompletionTracker& tracker,const CapturedEffects& frozen,
    const PublishedPair& pair,const std::function<LiveIdentity()>& observe,
    const std::function<std::optional<ProviderReceipt>(const PublishedPair&)>& acknowledge) noexcept {
    const auto& expected=tracker.GetRequest();
    const auto sameRequest=[&](const Request& request) {
        return request.requestId==expected.requestId && request.identity==expected.identity &&
            request.pmmPath==expected.pmmPath && request.emmPath==expected.emmPath &&
            request.kind==expected.kind && request.saveEffects==expected.saveEffects;
    };
    if (!sameRequest(frozen.GetRequest()) || !sameRequest(pair.request) ||
        frozen.Digest()!=pair.emmSha256 || pair.pmmSha256.size()!=64 ||
        pair.pmmSha256.find_first_not_of("0123456789abcdef")!=std::string::npos)
        return false;
    try {
        const auto before=observe();
        if (!tracker.FinishWrite(expected.requestId,true,before.identity,before.providerAvailable) ||
            !tracker.BeginAcknowledgement(expected.requestId,before.identity,before.providerAvailable))
            return false;
        bool matches=false;
        try {
            const auto receipt=acknowledge(pair);
            matches=receipt && MatchesPublication(pair,*receipt);
        } catch (...) { /* A provider error is an unsuccessful acknowledgement. */ }
        LiveIdentity after;
        try { after=observe(); }
        catch (...) { /* The current scene/provider can no longer be confirmed. */ }
        return tracker.FinishAcknowledgement(expected.requestId,matches,after.identity,after.providerAvailable) &&
            tracker.GetStage()==Stage::Completed;
    } catch (...) {
        // The durable file result survives failure to observe the live scene.
        tracker.FinishWrite(expected.requestId,true,{},false);
        return false;
    }
}

} // namespace mikumikustudio::effect_save
