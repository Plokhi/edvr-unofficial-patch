#pragma once
#include "../../src/d3d11/flat_diagnostic.h"
#include "../../src/common/runtime_profile.h"
#include <fstream>
#include <iterator>
#include <string>

namespace flat_diagnostic_tests {
inline std::string source(const char* n) {
    std::ifstream f(n,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};
}
inline bool has(const std::string& s,const char* text,unsigned count=1) {
    size_t p=0;unsigned n=0;while((p=s.find(text,p))!=std::string::npos){++n;p+=std::string(text).size();}return n==count;
}
template<class Check>void run(Check check) {
    using namespace edvr::flatdiag;
    const auto previousProfile=edvr::g_runtimeProfile.load();
    const char* keys[]={"diagnostic.flat_session","diagnostic.flat_motion","diagnostic.flat_preparation","diagnostic.flat_composition","diagnostic.flat_backend"};
    for(auto profile:{edvr::RuntimeProfile::Flat,edvr::RuntimeProfile::Invalid}) {
        edvr::g_runtimeProfile=profile;
        for(const char* key:keys)check(edvr::runtimeProfileAllowsKey(key)==(profile==edvr::RuntimeProfile::Flat),"actual profile getter gate recognizes all diagnostic controls and rejects invalid installs");
    }
    edvr::g_runtimeProfile=previousProfile;
    publish(0);clearCounts();
    check(!active() && enabled(Motion) && enabled(Preparation) && enabled(Composition) && enabled(Backend),"normal rendering enables every stage");
    const uint32_t initial=epoch.load();
    check(!publish(0) && epoch.load()==initial,"unchanged selection never resets resources/history");
    note(Resolve);check(counts[Resolve]==0,"normal mode incurs no diagnostic work counting");
    for(uint32_t disabled=0;disabled<16;++disabled) {
        const bool m=!(disabled&1),p=!(disabled&2),c=!(disabled&4),b=!(disabled&8);
        check(selection(false,m,p,c,b)==0,"stage choices inert without explicit diagnostic session");
        const uint32_t next=selection(true,m,p,c,b),oldEpoch=epoch.load();
        check(next==16+disabled && publish(next) && epoch.load()==oldEpoch+1,"all 16 live experiments have distinct boundary epochs");
        check(active() && enabled(Motion)==m && enabled(Preparation)==p && enabled(Composition)==c && enabled(Backend)==b,"each subsystem remains independently selectable");
        check(foregroundEnabled()==(m&&c),"foreground capture needs both producer ownership and composition");
        check(!publish(next) && epoch.load()==oldEpoch+1,"same live configuration does not reinitialize per frame");
        clearCounts();note(Resolve);note(Prepared);note(Resolve);
        check(counts[Resolve]==2 && counts[Prepared]==1 && counts[Evaluated]==0,"executed-work counters distinguish absence from an empty stage");
    }
    check(publish(0) && !active() && foregroundEnabled(),"leaving session restores normal policy");
    publish(Session|Motion|Preparation|Composition|Backend);
    note(Resolve);const auto switchEpoch=epoch.load();
    profileSwitch();
    check(!active() && foregroundEnabled() && enabled(Preparation) && enabled(Backend),
          "profile switch cannot carry any flat isolation cut into the shared VR resolver");
    check(counts[Resolve]==0 && epoch.load()==switchEpoch+1,
          "profile switch discards diagnostic counters and advances the frozen-input epoch");
    profileSwitch();check(epoch.load()==switchEpoch+1,"normal profile switches do not invent diagnostic epochs");
    const auto watch=source("src/d3d11/mode_watch.cpp");
    const auto clearAt=watch.find("flatdiag::profileSwitch();"),commitAt=watch.find("runtimeProfileSet(target);");
    check(clearAt!=std::string::npos && clearAt<commitAt,
          "actual profile commit clears flat isolation before publishing the new profile");
    clearCounts();
    const auto runtime=source("src/d3d11/flat_runtime.cpp"),resolve=source("src/d3d11/flat_mono_resolve.cpp"),
        ui=source("src/d3d11/flat_ui_layer.cpp"),refresh=source("src/d3d11/temporal_pass.cpp");
    check(!runtime.empty() && !resolve.empty() && !ui.empty() && !refresh.empty(),"diagnostic production sources readable from repository root");
    const char* gates[]={
        "cfg.getBool(\"diagnostic.flat_session\",false)",
        "flatCameraInjectDisarm();",
        "if(flatdiag::publish(next))",
        "s.census.idle();gpuReset(s);flatdiag::clearCounts();",
        "const bool phaseWanted = !flatdiag::active()",
        "if(alternateNominee && flatdiag::enabled(flatdiag::Motion))",
        "if(sel.selected() && flatdiag::enabled(flatdiag::Motion))",
        "if (continuesRun && flatdiag::enabled(flatdiag::Motion))",
        "if(flatdiag::foregroundEnabled() && foregroundCandidate",
        "if(flatdiag::enabled(flatdiag::Composition) && domainForeign && domainPool && phaseKnown)",
        "overlayPlanned=flatdiag::enabled(flatdiag::Composition);",
        "protectedOverlay=flatdiag::enabled(flatdiag::Composition);"
    };
    for(const char* gate:gates) {
        check(has(runtime,gate),"diagnostic stage wired to actual production boundary");
        auto removed=runtime;const auto at=removed.find(gate);if(at!=std::string::npos)removed.erase(at,std::string(gate).size());
        check(!has(removed,gate),"wiring regression rejects a missing gate");
    }
    check(runtime.find("flatCameraInjectDisarm();")<runtime.find("if(flatdiag::publish(next))") &&
          runtime.find("if(flatdiag::publish(next))")<runtime.find("engineVelocityConfigure(enabled"),"configuration applies only between completed frame and next producers");
    check(has(runtime,"flatdiag::enabled(flatdiag::Motion) && !engineVelocitySourceViews(",2),"both resolve routes deliberately accept empty motion seam when bypassed");
    check(has(runtime,"selected.sourceFree && flatdiag::enabled(flatdiag::Motion)",2),"both routes omit source-free producer allocation when bypassed");
    check(has(refresh,"temporalModeEnabled(cfg.requestedTemporalMode()) && flatdiag::enabled(flatdiag::Motion)"),"regular config refresh cannot reenable a disabled producer");
    check(has(ui,"runtimeFlatProfile() && flatdiag::enabled(flatdiag::Composition) && uiLayerCrispOn()"),"all flat UI layer entry points use composition gate");
    check(has(resolve,"const bool requestedReset=flatdiag::active()") && has(resolve,"g.history=!flatdiag::active();"),"all diagnostics including all-on control invalidate history");
    check(has(resolve,"if(prepareEnabled) {") && has(resolve,"!prepareEnabled && backendEnabled && g.neutralEpoch!=flatdiag::epoch.load()"),"preparation bypass is independent and seeds only once per epoch");
    check(has(resolve,"if(!g.inputsPrepared) {") && has(resolve,"if(g.expected.uav)context->ClearUnorderedAccessViewFloat"),"cold inputs initialized and SDKs with no expected texture never clear a null UAV");
    check(has(resolve,"if(!prepareEnabled && g.inputsPrepared)depthIndex=g.preparedDepth;"),"freeze uses the last depth actually written even in alternating-depth modes");
    check(has(resolve,"if(!backendEnabled) {") && has(resolve,"context->CSSetShader(g.spatial.Get(),nullptr,0);context->Dispatch((evalW+7)/8,(evalH+7)/8,1);"),"backend bypass writes valid spatial output instead of declining treatment");
    check(has(resolve,"g.output[hdr?index:1].uav.Get()") && has(resolve,"Image& second=g.output[(taa && backendEnabled)?index:1];"),"spatial bypass returns its written image and preserves SDR SRGB-capable output");
    check(has(resolve,"constants.route[1]=(hdr&&(taa || !backendEnabled))?1u:0u;"),"HDR finish reads raw spatial result without compress/expand mismatch");
}
}
